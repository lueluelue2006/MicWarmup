#ifndef MIC_WARMUP_AUTO_H
#define MIC_WARMUP_AUTO_H

#include "warmup.h"

#define MW_ROUTE_WINDOW (MW_SECOND * 3 / 4)

typedef enum { MWInputUnclear, MWInputInactive, MWInputActive } MWInputActivity;
typedef enum { MWRouteUnclear, MWRouteBuiltIn, MWRouteOther } MWInputRoute;

typedef struct {
    int32_t pid;
    uint32_t object;
    MWInputActivity activity;
    MWInputRoute route;
    uint64_t at;
} MWAutoReading;

typedef struct {
    int32_t pid;
    uint32_t object;
    MWInputActivity last;
    uint64_t lastReadingAt;
    uint64_t verifyFrom;
    uint64_t verifyBy;
    uint64_t edgeAt;
} MWAutoState;

typedef struct { bool trigger; uint64_t verifyBy; uint64_t triggerAt; } MWAutoDecision;

// Allow triggers while enabled and awake, including starting/active warmup.
// A trigger starts an idle session or renews the current one; stopping excludes it.
// Reset the state when pausing, resuming or waking, not when a session ends.
// Deliver current-epoch, current-binding observations after route/cancellation vetoes.
// triggerAt is the original edge time. MW_ROUTE_WINDOW bounds verification of an unknown route.
MWAutoDecision MWAutoObserve(MWAutoState *state, MWAutoReading reading, uint64_t now, bool mayStart);

#endif
