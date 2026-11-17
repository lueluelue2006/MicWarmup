#include "warmup.h"

#include <time.h>

MWInput MWSelect(const MWCandidate *candidates, size_t count) {
    MWInput result = { MWInputNone, 0, MWBeforeUnknown };
    for (size_t i = 0; i < count; i++) {
        MWCandidate candidate = candidates[i];
        if (!candidate.id || !candidate.builtIn || !candidate.alive || !candidate.hasInput) continue;
        if (result.selection == MWInputOK) return (MWInput){ MWInputAmbiguous, 0, MWBeforeUnknown };
        result.selection = MWInputOK;
        result.id = candidate.id;
    }
    return result;
}

uint64_t MWNow(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &time) != 0) return 0;
    return (uint64_t)time.tv_sec * MW_SECOND + (uint64_t)time.tv_nsec;
}

bool MWBegin(MWPhase *phase) {
    if (*phase != MWIdle) return false;
    *phase = MWStarting;
    return true;
}

bool MWMarkActive(MWPhase *phase) {
    if (*phase != MWStarting) return false;
    *phase = MWActive;
    return true;
}

bool MWEnd(MWPhase *phase) {
    if (*phase != MWStarting && *phase != MWActive) return false;
    *phase = MWStopping;
    return true;
}

static MWInterrupt Check(const MWOps *ops, bool supervised, uint64_t stopAt) {
    MWInterrupt signal = ops->interruption(ops->ctx);
    if (signal != MWContinue || supervised) return signal;
    uint64_t now = ops->now(ops->ctx);
    if (now == 0) return MWControlError;
    return now >= stopAt ? MWTimeUp : MWContinue;
}

static MWOutcome Result(MWInterrupt signal) {
    if (signal == MWTimeUp) return MWExpired;
    if (signal == MWControlError) return MWControlFailed;
    return MWStopped;
}

static MWOutcome Run(const MWOps *ops, bool supervised, uint64_t stopAt) {
    void *ctx = ops->ctx;
    MWInterrupt signal = Check(ops, supervised, stopAt);
    if (signal != MWContinue) return Result(signal);
    MWInput input = ops->probe(ctx);
    signal = Check(ops, supervised, stopAt);
    if (signal != MWContinue) return Result(signal);
    if (input.selection == MWInputNone) return MWNoDevice;
    if (input.selection == MWInputAmbiguous) return MWMultipleDevices;
    if (input.selection != MWInputOK || !input.id) return MWQueryFailed;
    if (ops->start(ctx, input.id) != 0) return MWStartFailed;

    // There is exactly one exit through stop after any successful start.
    bool deviceChanged = false;
    signal = Check(ops, supervised, stopAt);
    if (signal == MWContinue) {
        deviceChanged = !ops->stillAvailable(ctx, input.id);
        signal = Check(ops, supervised, stopAt);
        if (!deviceChanged && signal == MWContinue && !ops->reportActive(ctx, input.before)) {
            signal = MWCancel;
        }
    }
    uint64_t nextProbe = ops->now(ctx) + MW_SECOND / 2;
    while (signal == MWContinue && !deviceChanged) {
        ops->pause(ctx, MW_SECOND / 20);
        signal = Check(ops, supervised, stopAt);
        uint64_t now = ops->now(ctx);
        if (signal == MWContinue && now >= nextProbe) {
            deviceChanged = !ops->stillAvailable(ctx, input.id);
            nextProbe = now + MW_SECOND / 2;
        }
    }
    if (ops->stop(ctx, input.id) != 0) return MWStopFailed;
    return deviceChanged ? MWDeviceChanged : Result(signal);
}

MWOutcome MWRun(const MWOps *ops, uint64_t stopAt) { return Run(ops, false, stopAt); }

MWOutcome MWRunSupervised(const MWOps *ops) { return Run(ops, true, 0); }
