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

static bool Step(MWControl *control, uint64_t now) {
    return MWControlAdvance(control, now, MWContinue, NULL, 0, NULL, NULL);
}

static bool Renew(MWControl *control, Acks *acks, uint64_t now, uint64_t deadline) {
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
        assert(Renew(&audio->control, &audio->acks, audio->now, audio->now + MW_LIMIT));
        assert(audio->acks.count == ++audio->renewIndex);
    } else {
        assert(Step(&audio->control, audio->now));
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
static bool Available(void *ctx, MWDeviceID id) { (void)ctx; return id == 17; }
static bool Report(void *ctx, MWBefore before) {
    MockAudio *audio = ctx;
    assert(before == MWBeforeStopped && ++audio->reports == 1);
    return true;
}
static MWInterrupt Interrupt(void *ctx) { return MWControlInterruption(&((MockAudio *)ctx)->control); }
static uint64_t Now(void *ctx) { return ((MockAudio *)ctx)->now; }
static void Pause(void *ctx, uint64_t duration) {
    MockAudio *audio = ctx;
    audio->now += duration;
    SimulateSupervisor(audio);
}

static void TestRenewedAudio(const uint64_t *events, size_t count, bool duringStart) {
    MockAudio audio = { .now = Base, .renewAt = events, .renewCount = count, .renewDuringStart = duringStart };
    assert(MWControlInit(&audio.control, Base, Base + MW_LIMIT));
    MWOps ops = { &audio, Probe, Start, Stop, Available, Report, Interrupt, Now, Pause };
    assert(MWRunSupervised(&ops) == MWExpired);
    assert(audio.starts == 1 && audio.stops == 1 && audio.reports == 1);
    assert(audio.renewIndex == count && audio.acks.count == count);
    assert(audio.stopAt == events[count - 1] + MW_LIMIT);
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
    assert(MWControlAdvance(&control, now, MWContinue, commands, 4, RecordAck, &acks));
    assert(!acks.count && MWControlNextCheck(&control) == Base + MW_LIMIT);
    assert(MWControlAdvance(&control, now + Millisecond, MWContinue, commands + 4,
                            sizeof(commands) - 4, RecordAck, &acks));
    assert(acks.count == 2 && acks.deadline[0] == first && acks.deadline[1] == second);
    assert(MWControlNextCheck(&control) == second);
    assert(Renew(&control, &acks, now + Millisecond, second));
    assert(acks.count == 2); // Duplicate or shorter deadlines are not acknowledgments.

    assert(Renew(&control, &acks, now + Millisecond, second + 1));
    assert(Renew(&control, &acks, now + Millisecond, UINT64_MAX));
    assert(Renew(&control, &acks, now + Millisecond, now));
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
    assert(MWControlAdvance(&control, now, MWContinue, commands, sizeof(commands), RecordAck, &acks));
    assert(MWControlInterruption(&control) == MWCancel && !acks.count);
    assert(MWControlNextCheck(&control) == now + MW_STOP_GRACE);
    assert(Renew(&control, &acks, now + 1, now + 1 + MW_LIMIT) && !acks.count);

    // EOF, owner exit and signal cancellation reach the parser before queued commands.
    assert(MWControlInit(&control, Base, deadline));
    assert(MWControlAdvance(&control, now, MWCancel, commands, MW_CONTROL_FRAME_SIZE, RecordAck, &acks));
    assert(MWControlInterruption(&control) == MWCancel && !acks.count);
    assert(!Step(&control, now + MW_STOP_GRACE));

    commands[MW_CONTROL_FRAME_SIZE] = 'X';
    assert(MWControlInit(&control, Base, deadline));
    assert(MWControlAdvance(&control, now, MWContinue, commands, sizeof(commands), RecordAck, &acks));
    assert(MWControlInterruption(&control) == MWControlError && !acks.count);

    assert(MWControlInit(&control, Base, deadline));
    MWControlAudioStopping(&control, now);
    assert(Renew(&control, &acks, now, now + MW_LIMIT) && !acks.count);
    assert(MWControlNextCheck(&control) == now + MW_STOP_GRACE);
    assert(!Step(&control, now + MW_STOP_GRACE));

    assert(MWControlInit(&control, Base, deadline));
    MWControlAudioStopping(&control, deadline + MW_STOP_GRACE / 2);
    assert(MWControlNextCheck(&control) == deadline + MW_STOP_GRACE);
    assert(!Step(&control, deadline + MW_STOP_GRACE));
}

static void TestExpiryBoundary(void) {
    MWControl control;
    Acks acks = {0};
    uint64_t deadline = Base + MW_LIMIT;

    assert(MWControlInit(&control, Base, deadline));
    assert(Renew(&control, &acks, deadline, deadline + MW_LIMIT));
    assert(acks.count == 1 && MWControlInterruption(&control) == MWContinue);
    assert(MWControlNextCheck(&control) == deadline + MW_LIMIT);

    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    MWControlEncode(frame, MW_CONTROL_RENEW, deadline + MW_LIMIT);
    assert(MWControlInit(&control, Base, deadline));
    assert(MWControlAdvance(&control, deadline, MWContinue, frame, 8, RecordAck, &acks));
    assert(MWControlInterruption(&control) == MWTimeUp && acks.count == 1);
    assert(MWControlAdvance(&control, deadline + 1, MWContinue, frame + 8, 1, RecordAck, &acks));
    assert(acks.count == 1 && MWControlNextCheck(&control) == deadline + MW_STOP_GRACE);
    assert(Step(&control, deadline + MW_STOP_GRACE - 1));
    assert(!Step(&control, deadline + MW_STOP_GRACE));

    assert(MWControlInit(&control, Base, deadline));
    assert(!Renew(&control, &acks, deadline + MW_STOP_GRACE, deadline + MW_LIMIT));
    assert(acks.count == 1);
}

int main(void) {
    const uint64_t renewals[] = { Base + MW_LIMIT - 100 * Millisecond, Base + 200 * MW_SECOND };
    const uint64_t whileStarting[] = { Base + MW_SECOND };
    TestRenewedAudio(renewals, 1, false);
    TestRenewedAudio(renewals, 2, false);
    TestRenewedAudio(whileStarting, 1, true);
    TestStream();
    TestCancellationAndDeviceChange();
    TestExpiryBoundary();
    puts("Renewal simulations passed: continuous audio, stream framing, stop priority, and watchdog timing.");
    return 0;
}
