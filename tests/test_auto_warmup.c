#include "auto_warmup.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    MWAutoState state;
    uint64_t now;
    int32_t pid;
    uint32_t object;
} Fake;

static Fake New(void) { return (Fake){ .now = MW_SECOND, .pid = 11, .object = 21 }; }

static MWAutoDecision Send(Fake *fake, MWInputActivity activity, MWInputRoute route, bool mayStart) {
    uint64_t at = fake->now;
    fake->now += MW_SECOND / 100;
    MWAutoReading reading = { fake->pid, fake->object, activity, route, at };
    return MWAutoObserve(&fake->state, reading, fake->now, mayStart);
}

static MWAutoDecision Inactive(Fake *fake) { return Send(fake, MWInputInactive, MWRouteUnclear, true); }
static MWAutoDecision Active(Fake *fake) { return Send(fake, MWInputActive, MWRouteBuiltIn, true); }

static void TestSessions(void) {
    Fake fake = New();
    assert(!Inactive(&fake).trigger); // Login alone never requests warmup.
    assert(Active(&fake).trigger);
    assert(!Active(&fake).trigger); // Duplicate piri/pdv# notifications.
    MWPhase phase = MWIdle;
    assert(MWBegin(&phase));
    uint64_t deadline = fake.now + MW_LIMIT;
    assert(MWMarkActive(&phase));
    fake.now = deadline - MW_SECOND / 10;
    assert(!Send(&fake, MWInputInactive, MWRouteUnclear, phase != MWStopping).trigger);
    MWAutoDecision renewed = Send(&fake, MWInputActive, MWRouteBuiltIn, phase != MWStopping);
    assert(renewed.trigger && renewed.triggerAt < deadline);
    uint64_t extended = renewed.triggerAt + MW_LIMIT;
    assert(extended > deadline + 119 * MW_SECOND);
    assert(fake.state.verifyBy == 0 && !MWBegin(&phase));
    fake.now = deadline + MW_SECOND;
    assert(!Active(&fake).trigger); // Health/property notifications do not slide the deadline.
    assert(fake.now < extended);
    fake.now = extended;
    phase = MWIdle;
    assert(!Active(&fake).trigger); // Still active at expiry cannot start another session.
    assert(!Inactive(&fake).trigger && Active(&fake).trigger);
    assert(!Active(&fake).trigger); // Manual stop also preserves the activity history.

    assert(MWBegin(&phase)); // A separate real edge while HAL is still starting can also renew.
    assert(!Inactive(&fake).trigger);
    assert(Send(&fake, MWInputActive, MWRouteBuiltIn, phase != MWStopping).trigger);
    assert(MWEnd(&phase));
    assert(!Send(&fake, MWInputInactive, MWRouteUnclear, phase != MWStopping).trigger);
    assert(!Send(&fake, MWInputActive, MWRouteBuiltIn, phase != MWStopping).trigger);
    phase = MWIdle;
    assert(!Active(&fake).trigger); // A stop takes precedence over queued renewal observations.
}

static void TestBaselines(void) {
    Fake fake = New();
    assert(!Active(&fake).trigger); // Already active when launched.
    assert(!Inactive(&fake).trigger && Active(&fake).trigger);
    fake.state = (MWAutoState){0}; // Pause, resume or wake invalidates the old baseline.
    assert(!Send(&fake, MWInputActive, MWRouteBuiltIn, false).trigger);
    assert(!Send(&fake, MWInputInactive, MWRouteUnclear, false).trigger);
    assert(!Send(&fake, MWInputActive, MWRouteBuiltIn, false).trigger);
    fake.state = (MWAutoState){0};
    assert(!Active(&fake).trigger);
    assert(!Inactive(&fake).trigger);
    fake.pid++; // New process, including when it attaches while already active.
    assert(!Active(&fake).trigger);
    assert(!Inactive(&fake).trigger);
    fake.object++; // New HAL object on the same PID is also a new binding.
    assert(!Active(&fake).trigger);
    assert(!Inactive(&fake).trigger);
    assert(!Inactive(&fake).trigger); // A refresh of the same binding preserves the baseline.
    assert(Active(&fake).trigger);
}

static void TestQueriesAndRoutes(void) {
    Fake fake = New();
    assert(!Inactive(&fake).trigger);
    assert(!Send(&fake, MWInputUnclear, MWRouteUnclear, true).trigger);
    assert(!Active(&fake).trigger); // No edge fabricated from a failed query.
    assert(!Send(&fake, MWInputUnclear, MWRouteUnclear, true).trigger);
    assert(!Active(&fake).trigger);
    assert(!Inactive(&fake).trigger);
    assert(!Send(&fake, MWInputActive, MWRouteOther, true).trigger);
    assert(!Active(&fake).trigger); // Changing route mid-activity doesn't count as an edge.

    assert(!Inactive(&fake).trigger);
    MWAutoDecision needRoute = Send(&fake, MWInputActive, MWRouteUnclear, true);
    assert(!needRoute.trigger && needRoute.verifyBy > fake.now);
    assert(Send(&fake, MWInputUnclear, MWRouteUnclear, true).verifyBy == needRoute.verifyBy);
    assert(Active(&fake).trigger); // A known edge can be verified within the short window.
    assert(!Active(&fake).trigger);

    assert(!Inactive(&fake).trigger);
    needRoute = Send(&fake, MWInputActive, MWRouteUnclear, true);
    fake.now = needRoute.verifyBy;
    assert(!Active(&fake).trigger);

    assert(!Inactive(&fake).trigger);
    needRoute = Send(&fake, MWInputActive, MWRouteUnclear, true);
    assert(needRoute.verifyBy);
    assert(!Send(&fake, MWInputActive, MWRouteBuiltIn, false).trigger);
    assert(!Active(&fake).trigger); // Pending validation did not survive a blocked phase.
}

static void TestDelayedConfirmedShortInput(void) {
    const uint64_t lags[] = { MW_ROUTE_WINDOW, 2 * MW_SECOND, MW_LIMIT - 1 };
    for (size_t i = 0; i < sizeof(lags) / sizeof(lags[0]); i++) {
        Fake fake = New();
        assert(!Inactive(&fake).trigger);
        MWAutoReading edge = { fake.pid, fake.object, MWInputActive, MWRouteBuiltIn, fake.now };
        MWAutoReading ended = { fake.pid, fake.object, MWInputInactive, MWRouteUnclear,
                                 edge.at + MW_SECOND / 10 };
        uint64_t delivered = edge.at + lags[i]; // Both observations waited on the main queue.
        MWAutoDecision decision = MWAutoObserve(&fake.state, edge, delivered, true);
        assert(decision.trigger && !decision.verifyBy && decision.triggerAt == edge.at);
        assert(decision.triggerAt + MW_LIMIT - delivered == MW_LIMIT - lags[i]);
        assert(!MWAutoObserve(&fake.state, edge, delivered, true).trigger);
        assert(!MWAutoObserve(&fake.state, ended, delivered, true).trigger);
        assert(!MWAutoObserve(&fake.state, edge, delivered, true).trigger); // Replay after inactivity.
        edge.at = ended.at + 1;
        decision = MWAutoObserve(&fake.state, edge, delivered, true);
        assert(decision.trigger && decision.triggerAt == edge.at); // The next genuine edge can renew.
    }
}

static void TestOriginalDeadline(void) {
    Fake fake = New();
    assert(!Inactive(&fake).trigger);
    MWAutoReading edge = { fake.pid, fake.object, MWInputActive, MWRouteBuiltIn, fake.now };
    fake.now = edge.at + MW_LIMIT;
    MWAutoDecision expired = MWAutoObserve(&fake.state, edge, fake.now, true);
    assert(!expired.trigger && !expired.verifyBy);
    assert(!Active(&fake).trigger); // A duplicate cannot start a new 120 seconds.

    assert(!Inactive(&fake).trigger);
    edge.at = fake.now;
    edge.route = MWRouteUnclear;
    uint64_t expires = edge.at + MW_LIMIT;
    fake.now = expires - MW_SECOND / 10;
    MWAutoDecision pending = MWAutoObserve(&fake.state, edge, fake.now, true);
    assert(!pending.trigger && pending.verifyBy == expires);
    fake.now = expires;
    assert(!Active(&fake).trigger);

    fake.now = UINT64_MAX - MW_LIMIT + 1;
    assert(!Inactive(&fake).trigger);
    edge.at = fake.now;
    edge.route = MWRouteBuiltIn;
    expired = MWAutoObserve(&fake.state, edge, fake.now, true);
    assert(!expired.trigger && !expired.verifyBy); // The absolute deadline must be representable.
}

static void TestDelayedUnresolvedRoute(void) {
    Fake fake = New();
    assert(!Inactive(&fake).trigger);
    MWAutoReading late = { fake.pid, fake.object, MWInputActive, MWRouteUnclear, fake.now };
    fake.now += 3 * MW_SECOND / 2;
    MWAutoDecision pending = MWAutoObserve(&fake.state, late, fake.now, true);
    assert(!pending.trigger && pending.verifyBy == fake.now + MW_ROUTE_WINDOW);

    MWAutoReading queued = late;
    queued.route = MWRouteBuiltIn;
    queued.at = fake.now - MW_SECOND / 10;
    MWAutoDecision stillPending = MWAutoObserve(&fake.state, queued, fake.now, true);
    assert(!stillPending.trigger && stillPending.verifyBy == pending.verifyBy);
    MWAutoDecision confirmed = Active(&fake);
    assert(confirmed.trigger && confirmed.triggerAt == late.at); // Preserve the actual observed edge time.
    assert(!Active(&fake).trigger);

    assert(!Inactive(&fake).trigger);
    late.at = fake.now;
    fake.now += 3 * MW_SECOND / 2;
    pending = MWAutoObserve(&fake.state, late, fake.now, true);
    assert(!pending.trigger && pending.verifyBy);
    fake.now = pending.verifyBy;
    assert(!Active(&fake).trigger); // An expired confirmation cannot reopen it.
    assert(!Active(&fake).trigger);

    assert(!Inactive(&fake).trigger);
    late.at = fake.now;
    uint64_t idleSince = late.at + MW_SECOND;
    fake.now += 3 * MW_SECOND / 2;
    late.route = MWRouteBuiltIn;
    pending = MWAutoObserve(&fake.state, late, fake.now, late.at >= idleSince);
    assert(!pending.trigger && !pending.verifyBy); // Edge predates an explicit stop/pause.
    assert(!Active(&fake).trigger);

    assert(!Inactive(&fake).trigger);
    late.at = fake.now;
    late.route = MWRouteUnclear;
    fake.now += 3 * MW_SECOND / 2;
    pending = MWAutoObserve(&fake.state, late, fake.now, true);
    assert(!pending.trigger && pending.verifyBy);
    assert(!Inactive(&fake).trigger && !fake.state.verifyBy); // Input already ended before recheck.
    assert(Active(&fake).trigger); // The next real edge remains eligible.

    assert(!Inactive(&fake).trigger);
    late.at = fake.now;
    fake.now += MW_ROUTE_WINDOW - MW_SECOND / 100;
    pending = MWAutoObserve(&fake.state, late, fake.now, true);
    assert(!pending.trigger && pending.verifyBy == fake.now + MW_ROUTE_WINDOW);
    fake.now += MW_SECOND / 20;
    assert(Active(&fake).trigger); // A late device list still gets one short confirmation window.

    assert(!Inactive(&fake).trigger);
    late.at = fake.now;
    fake.now += 2 * MW_SECOND;
    assert(MWAutoObserve(&fake.state, late, fake.now, true).verifyBy);
    queued.route = MWRouteOther;
    queued.at = late.at + MW_SECOND;
    MWAutoDecision denied = MWAutoObserve(&fake.state, queued, fake.now, true);
    assert(!denied.trigger && !denied.verifyBy); // A queued external route already disproved the edge.
    assert(!Active(&fake).trigger);
}

int main(void) {
    TestSessions();
    TestBaselines();
    TestQueriesAndRoutes();
    TestDelayedConfirmedShortInput();
    TestOriginalDeadline();
    TestDelayedUnresolvedRoute();
    puts("Auto mock tests passed: new edges renew, duplicates do not, cancellation, routes, and original event times.");
    return 0;
}
