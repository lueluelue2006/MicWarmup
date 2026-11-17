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
    MWInputActivity prior = state->last;
    state->last = reading.activity;
    if (reading.activity == MWInputInactive) {
        state->verifyBy = 0;
        state->edgeAt = 0;
        return nothing;
    }
    if (reading.activity == MWInputUnclear) {
        // An unknown query is not evidence that recording stopped. We can only
        // continue verifying an edge already observed before this query failed.
        return (MWAutoDecision){ .verifyBy = state->verifyBy };
    }
    if (mayStart && prior == MWInputInactive && reading.route != MWRouteOther) {
        if (reading.route == MWRouteBuiltIn && now - reading.at < MW_ROUTE_WINDOW) {
            return (MWAutoDecision){ .trigger = true, .triggerAt = reading.at };
        }
        // A delayed edge or an unresolved route needs a new observation begun
        // after delivery. This opens one bounded window, not a rolling retry.
        state->verifyFrom = now;
        state->verifyBy = now + MW_ROUTE_WINDOW;
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
