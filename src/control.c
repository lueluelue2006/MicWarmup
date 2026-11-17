#include "control.h"

enum { AudioStarting = MWControlError + 1, AudioStopping, LateStartupStopping, StartupTimedOut };

static bool Live(int state) {
    return state == AudioStarting || state == MWContinue;
}

static uint64_t After(uint64_t now, uint64_t duration) {
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

static bool ValidDeadline(uint64_t now, uint64_t deadline) {
    return now && deadline > now &&
           deadline - now <= MW_LIMIT && deadline <= UINT64_MAX - MW_STOP_GRACE;
}

bool MWControlInit(MWControl *control, uint64_t now, uint64_t deadline) {
    if (!ValidDeadline(now, deadline)) return false;
    atomic_init(&control->state, MWContinue);
    control->deadline = deadline;
    control->startupAt = 0;
    control->watchdogAt = After(deadline, MW_STOP_GRACE);
    control->used = 0;
    return true;
}

bool MWControlBeginStartup(MWControl *control, uint64_t now) {
    if (!now || control->startupAt || atomic_load(&control->state) != MWContinue) return false;
    control->startupAt = After(now, MW_START_LIMIT);
    atomic_store(&control->state, AudioStarting);
    return true;
}

static uint64_t WatchdogAt(const MWControl *control, int state) {
    // The worker publishes its stop time before committing either cleanup state.
    if (state == AudioStopping || state == LateStartupStopping) {
        uint64_t workerLimit = After(control->audioStopAt, MW_STOP_GRACE);
        if (workerLimit < control->watchdogAt) return workerLimit;
    }
    return control->watchdogAt;
}

uint64_t MWControlNextCheck(const MWControl *control) {
    int state = atomic_load(&control->state);
    if (state == StartupTimedOut) return control->startupAt;
    if (state == AudioStarting && control->startupAt < control->deadline) return control->startupAt;
    return Live(state) ? control->deadline : WatchdogAt(control, state);
}

static MWInterrupt InterruptionFor(int state) {
    if (Live(state)) return MWContinue;
    return state > MWControlError ? MWCancel : (MWInterrupt)state;
}

MWInterrupt MWControlInterruption(const MWControl *control) {
    return InterruptionFor(atomic_load(&control->state));
}

MWInterrupt MWControlAudioInterruption(MWControl *control, uint64_t now) {
    int state = atomic_load(&control->state);
    if (!now && Live(state)) return MWControlError;
    if (state == AudioStarting && now >= control->startupAt) {
        // A HAL call returned after the cutoff. Skip further startup work, bound
        // the paired stop if needed, and retain why the worker is unwinding.
        control->audioStopAt = now;
        if (atomic_compare_exchange_strong(&control->state, &state, LateStartupStopping)) {
            return MWCancel;
        }
    }
    return InterruptionFor(state);
}

bool MWControlAudioReady(MWControl *control, uint64_t now) {
    int starting = AudioStarting;
    return MWControlAudioInterruption(control, now) == MWContinue &&
           atomic_compare_exchange_strong(&control->state, &starting, MWContinue);
}

MWOutcome MWControlCompletedOutcome(const MWControl *control, MWOutcome outcome) {
    if (outcome != MWStopped) return outcome;
    int state = atomic_load(&control->state);
    if (state == LateStartupStopping && control->deadline <= control->startupAt) return MWExpired;
    if (state == LateStartupStopping || state == StartupTimedOut) return MWStartTimedOutStopped;
    return outcome;
}

void MWControlAudioStopping(MWControl *control, uint64_t now) {
    int state = atomic_load(&control->state);
    if (!Live(state)) return;
    control->audioStopAt = now;
    // Only this worker can transition between the two live states.
    atomic_compare_exchange_strong(&control->state, &state, AudioStopping);
}

static MWInterrupt CheckTags(const MWControl *control, const unsigned char *bytes, size_t size) {
    // Check the complete batch for cancellation before applying any renewals.
    size_t firstTag = control->used ? MW_CONTROL_FRAME_SIZE - control->used : 0;
    for (size_t i = firstTag; i < size; i += MW_CONTROL_FRAME_SIZE) {
        if (bytes[i] == MW_CONTROL_CANCEL) return MWCancel;
        if (bytes[i] != MW_CONTROL_RENEW) return MWControlError;
    }
    return MWContinue;
}

int MWControlAdvance(MWControl *control, uint64_t now, MWInterrupt outside,
                     const unsigned char *bytes, size_t size, MWControlAck ack, void *ctx) {
    int state = atomic_load(&control->state);
    if (state == StartupTimedOut) return MWStartTimedOut;
    if (!now) return MWWatchdog;
    MWInterrupt stop = outside != MWContinue ? outside : CheckTags(control, bytes, size);
    if (state == AudioStarting && now >= control->startupAt && stop == MWContinue) {
        // If the lease ended first, retain its original stop grace. Otherwise the
        // startup cutoff wins over every renewal, including a frame in this batch.
        int reason = control->deadline <= control->startupAt ? MWTimeUp : StartupTimedOut;
        if (atomic_compare_exchange_strong(&control->state, &state, reason)) {
            if (reason == StartupTimedOut) return MWStartTimedOut;
            return now < control->watchdogAt ? 0 : MWWatchdog;
        }
        // The worker confirmed startup or began cleanup while the supervisor woke.
    }
    if (now >= WatchdogAt(control, state)) return MWWatchdog;
    if (!Live(state)) return 0;

    for (size_t i = 0; stop == MWContinue && i < size; i++) {
        control->partial[control->used++] = bytes[i];
        if (control->used != MW_CONTROL_FRAME_SIZE) continue;
        control->used = 0;
        uint64_t deadline = MWControlDecode(control->partial);
        if (deadline > control->deadline && ValidDeadline(now, deadline)) {
            // The live-state load commits this renewal relative to a worker stop.
            if (!Live(atomic_load(&control->state))) break;
            control->deadline = deadline;
            control->watchdogAt = After(deadline, MW_STOP_GRACE);
            if (!ack(ctx, deadline)) stop = MWControlError;
        }
    }

    // A complete renewal already in the pipe gets considered before soft expiry.
    if (stop == MWContinue && now >= control->deadline) stop = MWTimeUp;
    state = atomic_load(&control->state);
    while (stop != MWContinue && Live(state)) {
        if (atomic_compare_exchange_strong(&control->state, &state, stop)) {
            if (stop != MWTimeUp) control->watchdogAt = After(now, MW_STOP_GRACE);
            return 0;
        }
    }
    return now < WatchdogAt(control, state) ? 0 : MWWatchdog;
}
