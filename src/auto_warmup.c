#include "auto_warmup.h"

MWAutoDecision MWAutoObserve(MWAutoState *state, MWAutoReading reading, uint64_t now, bool mayStart) {
    const MWAutoDecision nothing = {0};
    if (state->pid != reading.pid || state->object != reading.object) {
        // New binding: its first successful reading is a baseline, even if active.
        *state = (MWAutoState){ .pid = reading.pid, .object = reading.object };
    }
    if (!reading.pid || !reading.object || !reading.at || !now || reading.at > now) {
        state->last = MWInputUnclear;
        state->verifyBy = 0;
        state->edgeAt = 0;
        return nothing;
    }

    if (!mayStart || (state->verifyBy && now >= state->verifyBy)) {
        state->verifyBy = 0;
        state->edgeAt = 0;
    }
    if (reading.at <= state->lastReadingAt) {
        return (MWAutoDecision){ .verifyBy = state->verifyBy };
    }
    state->lastReadingAt = reading.at;
    MWInputActivity prior = state->last;
    state->last = reading.activity;
    if (reading.activity == MWInputInactive ||
        (reading.activity == MWInputActive && reading.route == MWRouteOther)) {
        state->verifyBy = 0;
        state->edgeAt = 0;
        return nothing;
    }
    if (reading.activity != MWInputActive) {
        // An unknown query is not evidence that recording stopped. We can only
        // continue verifying an edge already observed before this query failed.
        return (MWAutoDecision){ .verifyBy = state->verifyBy };
    }
    if (mayStart && prior == MWInputInactive) {
        if (reading.at > UINT64_MAX - MW_LIMIT || now - reading.at >= MW_LIMIT) return nothing;
        if (reading.route == MWRouteBuiltIn) {
            return (MWAutoDecision){ .trigger = true, .triggerAt = reading.at };
        }
        if (reading.route != MWRouteUnclear) return nothing;
        // A fresh observation can resolve this route until either deadline.
        uint64_t expires = reading.at + MW_LIMIT;
        state->verifyFrom = now;
        state->verifyBy = expires - now < MW_ROUTE_WINDOW ? expires : now + MW_ROUTE_WINDOW;
        state->edgeAt = reading.at;
    }
    if (!state->verifyBy) return nothing;
    if (reading.at < state->verifyFrom || reading.route == MWRouteUnclear) {
        return (MWAutoDecision){ .verifyBy = state->verifyBy };
    }

    state->verifyBy = 0;
    uint64_t edgeAt = state->edgeAt;
    state->edgeAt = 0;
    return (MWAutoDecision){ .trigger = reading.route == MWRouteBuiltIn, .triggerAt = edgeAt };
}
