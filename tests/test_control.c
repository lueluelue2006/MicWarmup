#include "control.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t Base = MW_SECOND;
static const uint64_t Millisecond = MW_SECOND / 1000;

typedef struct {
    uint64_t deadline[4];
    size_t count;
} Acks;

static bool RecordAck(void *ctx, uint64_t deadline) {
    Acks *acks = ctx;
    assert(acks->count < sizeof(acks->deadline) / sizeof(*acks->deadline));
    acks->deadline[acks->count++] = deadline;
    return true;
}

static int Step(MWControl *control, uint64_t now) {
    return MWControlAdvance(control, now, MWContinue, NULL, 0, NULL, NULL);
}

static int Renew(MWControl *control, Acks *acks, uint64_t now, uint64_t deadline) {
    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    MWControlEncode(frame, MW_CONTROL_RENEW, deadline);
    return MWControlAdvance(control, now, MWContinue, frame, sizeof(frame), RecordAck, acks);
}

typedef struct {
    MWControl control;
    Acks acks;
    uint64_t now, stopAt;
    const uint64_t *renewAt;
    size_t renewCount, renewIndex;
    int starts, stops, reports;
    bool renewDuringStart;
} MockAudio;

static void SimulateSupervisor(MockAudio *audio) {
    if (audio->renewIndex < audio->renewCount && audio->now == audio->renewAt[audio->renewIndex]) {
        assert(audio->starts == 1 && audio->stops == 0);
        assert(Renew(&audio->control, &audio->acks, audio->now, audio->now + MW_LIMIT) == 0);
        assert(audio->acks.count == ++audio->renewIndex);
    } else {
        assert(Step(&audio->control, audio->now) == 0);
    }
    if (audio->now == Base + MW_LIMIT) {
        assert(audio->starts == 1 && audio->stops == 0 && audio->reports == 1);
        assert(MWControlInterruption(&audio->control) == MWContinue);
    }
}

static MWInput Probe(void *ctx) { (void)ctx; return (MWInput){ MWInputOK, 17, MWBeforeStopped }; }
static int32_t Start(void *ctx, MWDeviceID id) {
    MockAudio *audio = ctx;
    assert(id == 17 && ++audio->starts == 1);
    if (audio->renewDuringStart) {
        assert(audio->renewAt[0] > audio->now && !audio->reports);
        audio->now = audio->renewAt[0];
        SimulateSupervisor(audio);
        assert(audio->acks.count == 1 && !audio->reports && !audio->stops);
    }
    return 0;
}
static int32_t Stop(void *ctx, MWDeviceID id) {
    MockAudio *audio = ctx;
    assert(id == 17 && audio->starts == 1 && ++audio->stops == 1);
    MWControlAudioStopping(&audio->control, audio->now);
    audio->stopAt = audio->now;
    return 0;
}
static MWDeviceState Available(void *ctx, MWDeviceID id) {
    (void)ctx;
    return id == 17 ? MWDeviceAvailable : MWDeviceUnavailable;
}
static bool Report(void *ctx, MWBefore before) {
    MockAudio *audio = ctx;
    assert(MWControlAudioReady(&audio->control, audio->now));
    assert(before == MWBeforeStopped && ++audio->reports == 1);
    return true;
}
static MWInterrupt Interrupt(void *ctx) {
    MockAudio *audio = ctx;
    return MWControlAudioInterruption(&audio->control, audio->now);
}
static uint64_t Now(void *ctx) { return ((MockAudio *)ctx)->now; }
static void Pause(void *ctx, uint64_t duration) {
    MockAudio *audio = ctx;
    audio->now += duration;
    SimulateSupervisor(audio);
}

static void TestRenewedAudio(const uint64_t *events, size_t count, bool duringStart) {
    MockAudio audio = { .now = Base, .renewAt = events, .renewCount = count, .renewDuringStart = duringStart };
    assert(MWControlInit(&audio.control, Base, Base + MW_LIMIT));
    assert(MWControlBeginStartup(&audio.control, Base));
    MWOps ops = { &audio, Probe, Start, Stop, Available, Report, Interrupt, Now, Pause };
    assert(MWRunSupervised(&ops) == MWExpired);
    assert(audio.starts == 1 && audio.stops == 1 && audio.reports == 1);
    assert(audio.renewIndex == count && audio.acks.count == count);
    assert(audio.stopAt == events[count - 1] + 30 * MW_SECOND);
    assert(MWControlNextCheck(&audio.control) == audio.stopAt + MW_STOP_GRACE);
}

static void TestStream(void) {
    unsigned char wire[MW_CONTROL_FRAME_SIZE];
    MWControlEncode(wire, MW_CONTROL_ACK, UINT64_C(0x0102030405060708));
    const unsigned char expected[] = { 'A', 8, 7, 6, 5, 4, 3, 2, 1 };
    assert(memcmp(wire, expected, sizeof(expected)) == 0);
    assert(MWControlDecode(wire) == UINT64_C(0x0102030405060708));

    MWControl control;
    Acks acks = {0};
    assert(MWControlInit(&control, Base, Base + MW_LIMIT));
    unsigned char commands[2 * MW_CONTROL_FRAME_SIZE];
    uint64_t now = Base + MW_SECOND;
    uint64_t first = now + MW_LIMIT;
    uint64_t second = first + Millisecond;
    MWControlEncode(commands, MW_CONTROL_RENEW, first);
    MWControlEncode(commands + MW_CONTROL_FRAME_SIZE, MW_CONTROL_RENEW, second);
    assert(MWControlAdvance(&control, now, MWContinue, commands, 4, RecordAck, &acks) == 0);
    assert(!acks.count && MWControlNextCheck(&control) == Base + MW_LIMIT);
    assert(MWControlAdvance(&control, now + Millisecond, MWContinue, commands + 4,
                            sizeof(commands) - 4, RecordAck, &acks) == 0);
    assert(acks.count == 2 && acks.deadline[0] == first && acks.deadline[1] == second);
    assert(MWControlNextCheck(&control) == second);
    assert(Renew(&control, &acks, now + Millisecond, second) == 0);
    assert(acks.count == 2); // Duplicate or shorter deadlines are not acknowledgments.

    assert(Renew(&control, &acks, now + Millisecond, second + 1) == 0);
    assert(Renew(&control, &acks, now + Millisecond, UINT64_MAX) == 0);
    assert(Renew(&control, &acks, now + Millisecond, now) == 0);
    assert(acks.count == 2 && MWControlNextCheck(&control) == second);
}

static void TestCancellationAndDeviceChange(void) {
    MWControl control;
    Acks acks = {0};
    uint64_t deadline = Base + MW_LIMIT;
    uint64_t now = Base + MW_SECOND;
    unsigned char commands[MW_CONTROL_FRAME_SIZE + 1];
    MWControlEncode(commands, MW_CONTROL_RENEW, now + MW_LIMIT);
    commands[MW_CONTROL_FRAME_SIZE] = MW_CONTROL_CANCEL;

    assert(MWControlInit(&control, Base, deadline));
    assert(MWControlAdvance(&control, now, MWContinue, commands, sizeof(commands), RecordAck, &acks) == 0);
    assert(MWControlInterruption(&control) == MWCancel && !acks.count);
    assert(MWControlNextCheck(&control) == now + MW_STOP_GRACE);
    assert(Renew(&control, &acks, now + 1, now + 1 + MW_LIMIT) == 0 && !acks.count);

    // EOF, owner exit and signal cancellation reach the parser before queued commands.
    assert(MWControlInit(&control, Base, deadline));
    assert(MWControlAdvance(&control, now, MWCancel, commands, MW_CONTROL_FRAME_SIZE, RecordAck, &acks) == 0);
    assert(MWControlInterruption(&control) == MWCancel && !acks.count);
    assert(Step(&control, now + MW_STOP_GRACE) == MWWatchdog);

    commands[MW_CONTROL_FRAME_SIZE] = 'X';
    assert(MWControlInit(&control, Base, deadline));
    assert(MWControlAdvance(&control, now, MWContinue, commands, sizeof(commands), RecordAck, &acks) == 0);
    assert(MWControlInterruption(&control) == MWControlError && !acks.count);

    assert(MWControlInit(&control, Base, deadline));
    MWControlAudioStopping(&control, now);
    assert(Renew(&control, &acks, now, now + MW_LIMIT) == 0 && !acks.count);
    assert(MWControlNextCheck(&control) == now + MW_STOP_GRACE);
    assert(Step(&control, now + MW_STOP_GRACE) == MWWatchdog);

    assert(MWControlInit(&control, Base, deadline));
    MWControlAudioStopping(&control, deadline + MW_STOP_GRACE / 2);
    assert(MWControlNextCheck(&control) == deadline + MW_STOP_GRACE);
    assert(Step(&control, deadline + MW_STOP_GRACE) == MWWatchdog);
}

static void TestExpiryBoundary(void) {
    MWControl control;
    Acks acks = {0};
    uint64_t deadline = Base + MW_LIMIT;

    assert(MWControlInit(&control, Base, deadline));
    assert(Renew(&control, &acks, deadline, deadline + MW_LIMIT) == 0);
    assert(acks.count == 1 && MWControlInterruption(&control) == MWContinue);
    assert(MWControlNextCheck(&control) == deadline + MW_LIMIT);

    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    MWControlEncode(frame, MW_CONTROL_RENEW, deadline + MW_LIMIT);
    assert(MWControlInit(&control, Base, deadline));
    assert(MWControlAdvance(&control, deadline, MWContinue, frame, 8, RecordAck, &acks) == 0);
    assert(MWControlInterruption(&control) == MWTimeUp && acks.count == 1);
    assert(MWControlAdvance(&control, deadline + 1, MWContinue, frame + 8, 1, RecordAck, &acks) == 0);
    assert(acks.count == 1 && MWControlNextCheck(&control) == deadline + MW_STOP_GRACE);
    assert(Step(&control, deadline + MW_STOP_GRACE - 1) == 0);
    assert(Step(&control, deadline + MW_STOP_GRACE) == MWWatchdog);

    assert(MWControlInit(&control, Base, deadline));
    assert(Renew(&control, &acks, deadline + MW_STOP_GRACE, deadline + MW_LIMIT) == MWWatchdog);
    assert(acks.count == 1);
}

static void TestStartupDeadline(void) {
    MWControl control;
    Acks acks = {0};
    uint64_t began = Base + MW_SECOND, cutoff = began + 15 * MW_SECOND;
    uint64_t firstLease = began + MW_SECOND;
    assert(MWControlInit(&control, Base, firstLease));
    assert(MWControlBeginStartup(&control, began));
    assert(MWControlNextCheck(&control) == firstLease);

    const uint64_t times[] = { firstLease, cutoff - MW_SECOND, cutoff - 1 };
    for (size_t i = 0; i < sizeof(times) / sizeof(*times); i++) {
        assert(Renew(&control, &acks, times[i], times[i] + MW_LIMIT) == 0);
        assert(acks.count == i + 1 && MWControlNextCheck(&control) == cutoff);
    }
    assert(!MWControlBeginStartup(&control, cutoff - 1));
    assert(MWControlAudioInterruption(&control, cutoff - 1) == MWContinue); // Status write pending.
    assert(Renew(&control, &acks, cutoff, cutoff + MW_LIMIT) == MWStartTimedOut);
    assert(acks.count == 3 && !MWControlAudioReady(&control, cutoff - 1));
    assert(Step(&control, cutoff + MW_STOP_GRACE) == MWStartTimedOut);

    assert(MWControlInit(&control, Base, Base + MW_LIMIT));
    assert(MWControlBeginStartup(&control, began));
    assert(MWControlAudioReady(&control, cutoff - 1));
    assert(Step(&control, cutoff) == 0 && MWControlNextCheck(&control) == Base + MW_LIMIT);
    assert(Renew(&control, &acks, cutoff, cutoff + MW_LIMIT) == 0);
    assert(acks.count == 4 && MWControlNextCheck(&control) == cutoff + MW_LIMIT);
}

static void TestStartupStopRaces(void) {
    MWControl control;
    uint64_t cutoff = Base + MW_START_LIMIT, earlier = cutoff - Millisecond;

    assert(MWControlInit(&control, Base, earlier));
    assert(MWControlBeginStartup(&control, Base));
    assert(Step(&control, cutoff) == 0); // A delayed poll observed both deadlines.
    assert(MWControlInterruption(&control) == MWTimeUp && !MWControlAudioReady(&control, earlier));
    assert(Step(&control, earlier + MW_STOP_GRACE) == MWWatchdog);

    assert(MWControlInit(&control, Base, Base + MW_LIMIT));
    assert(MWControlBeginStartup(&control, Base));
    assert(MWControlAdvance(&control, earlier, MWCancel, NULL, 0, NULL, NULL) == 0);
    assert(!MWControlAudioReady(&control, earlier) && Step(&control, cutoff) == 0);
    assert(MWControlInterruption(&control) == MWCancel);
    assert(MWControlCompletedOutcome(&control, MWStopped) == MWStopped);
    assert(Step(&control, earlier + MW_STOP_GRACE) == MWWatchdog);

    assert(MWControlInit(&control, Base, Base + MW_LIMIT));
    assert(MWControlBeginStartup(&control, Base));
    assert(!MWControlAudioReady(&control, cutoff)); // A late worker must not announce active.
    MWControlAudioStopping(&control, cutoff); // The worker returned and is unwinding.
    assert(Step(&control, cutoff) == 0);
    assert(MWControlCompletedOutcome(&control, MWStopped) == MWStartTimedOutStopped);
    assert(Step(&control, cutoff + MW_STOP_GRACE) == MWWatchdog);
}

int main(void) {
    const uint64_t renewals[] = { Base + MW_LIMIT - 100 * Millisecond, Base + 45 * MW_SECOND };
    const uint64_t whileStarting[] = { Base + MW_SECOND };
    TestRenewedAudio(renewals, 1, false);
    TestRenewedAudio(renewals, 2, false);
    TestRenewedAudio(whileStarting, 1, true);
    TestStream();
    TestCancellationAndDeviceChange();
    TestExpiryBoundary();
    TestStartupDeadline();
    TestStartupStopRaces();
    puts("Renewal simulations passed, including fixed startup timing and stop races.");
    return 0;
}
