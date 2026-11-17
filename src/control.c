#include "control.h"

// A valid monotonic deadline leaves the small values available as terminal markers.
enum { AudioStopping = MWControlError + 1, FirstDeadline = AudioStopping + 1 };

static uint64_t WithGrace(uint64_t now) {
    return now > UINT64_MAX - MW_STOP_GRACE ? UINT64_MAX : now + MW_STOP_GRACE;
}

static bool ValidDeadline(uint64_t now, uint64_t deadline) {
    return now && deadline >= FirstDeadline && deadline > now &&
           deadline - now <= MW_LIMIT && deadline <= UINT64_MAX - MW_STOP_GRACE;
}

bool MWControlInit(MWControl *control, uint64_t now, uint64_t deadline) {
    if (!ValidDeadline(now, deadline)) return false;
    atomic_init(&control->state, deadline);
    control->watchdogAt = WithGrace(deadline);
    control->used = 0;
    return true;
}

static uint64_t WatchdogAt(const MWControl *control, uint64_t state) {
    // The worker publishes its stop time before committing AudioStopping.
    if (state == AudioStopping) {
        uint64_t workerLimit = WithGrace(control->audioStopAt);
        if (workerLimit < control->watchdogAt) return workerLimit;
    }
    return control->watchdogAt;
}

uint64_t MWControlNextCheck(const MWControl *control) {
    uint64_t state = atomic_load(&control->state);
    return state >= FirstDeadline ? state : WatchdogAt(control, state);
}

MWInterrupt MWControlInterruption(const MWControl *control) {
    uint64_t state = atomic_load(&control->state);
    if (state >= FirstDeadline) return MWContinue;
    return state == AudioStopping ? MWCancel : (MWInterrupt)state;
}

void MWControlAudioStopping(MWControl *control, uint64_t now) {
    uint64_t state = atomic_load(&control->state);
    if (state < FirstDeadline) return;
    control->audioStopAt = now;
    do {
        if (atomic_compare_exchange_strong(&control->state, &state, AudioStopping)) return;
    } while (state >= FirstDeadline);
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

bool MWControlAdvance(MWControl *control, uint64_t now, MWInterrupt outside,
                      const unsigned char *bytes, size_t size, MWControlAck ack, void *ctx) {
    uint64_t state = atomic_load(&control->state);
    if (!now || now >= WatchdogAt(control, state)) return false;
    if (state < FirstDeadline) return true;

    MWInterrupt stop = outside != MWContinue ? outside : CheckTags(control, bytes, size);
    for (size_t i = 0; stop == MWContinue && i < size; i++) {
        control->partial[control->used++] = bytes[i];
        if (control->used != MW_CONTROL_FRAME_SIZE) continue;
        control->used = 0;
        uint64_t deadline = MWControlDecode(control->partial);
        state = atomic_load(&control->state);
        if (state < FirstDeadline) break;
        if (deadline > state && ValidDeadline(now, deadline) &&
            atomic_compare_exchange_strong(&control->state, &state, deadline)) {
            control->watchdogAt = WithGrace(deadline);
            if (!ack(ctx, deadline)) stop = MWControlError;
        }
    }

    // A complete renewal already in the pipe gets considered before soft expiry.
    // Once either thread commits a stop, no frame can change the deadline.
    state = atomic_load(&control->state);
    if (state >= FirstDeadline) {
        if (stop == MWContinue && now >= state) stop = MWTimeUp;
        if (stop != MWContinue && atomic_compare_exchange_strong(&control->state, &state, stop)) {
            if (stop != MWTimeUp) control->watchdogAt = WithGrace(now);
            return true;
        }
    }
    return now < WatchdogAt(control, state);
}
