// Tests the actual UI controller with anonymous pipes and inert NSTask objects.
// No NSApplication is launched, no helper is spawned, and no microphone is opened.
#define main MWUnusedApplicationMain
#include "App.m"
#undef main
#include <assert.h>

@interface TestWarmupApp : WarmupApp
@property NSUInteger starts;
@property uint64_t startedUntil;
@end
@implementation TestWarmupApp
- (void)refreshPresentation {}
- (void)checkInput {}
- (void)startAutomatic:(BOOL)isAutomatic until:(uint64_t)expires {
    assert(isAutomatic);
    assert([[self valueForKey:@"phase"] intValue] == MWIdle);
    assert(!self.task);
    self.starts++;
    self.startedUntil = expires;
}
@end

static TestWarmupApp *New(MWPhase phase, uint64_t expires) {
    TestWarmupApp *app = [TestWarmupApp new];
    app.autoEnabled = YES;
    app.autoEpoch = app.autoBinding = 1;
    app.idleSince = MWNow() - MW_SECOND;
    app.controlBuffer = [NSMutableData data];
    app.requestedDeadline = expires;
    [app setValue:@(phase) forKey:@"phase"];
    [app setValue:@(expires) forKey:@"deadline"];
    if (phase != MWIdle) app.task = [NSTask new];
    return app;
}

static void Observe(TestWarmupApp *app, MWInputActivity activity, uint64_t at) {
    MWAutoReading reading = { 11, 21, activity, activity == MWInputActive ? MWRouteBuiltIn : MWRouteUnclear, at };
    [app observeAutomatic:reading supported:YES epoch:1 binding:1];
}

static NSData *Ack(uint64_t expires) {
    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    MWControlEncode(frame, MW_CONTROL_ACK, expires);
    return [NSData dataWithBytes:frame length:sizeof(frame)];
}

static void TestActiveRenewalAndSplitAck(void) {
    uint64_t old = MWNow() + 30 * MW_SECOND;
    TestWarmupApp *app = New(MWActive, old);
    NSPipe *pipe = [NSPipe pipe]; app.control = pipe.fileHandleForWriting;
    int reader = pipe.fileHandleForReading.fileDescriptor;
    assert(fcntl(reader, F_SETFL, O_NONBLOCK) == 0);
    NSTask *same = app.task;
    Observe(app, MWInputInactive, MWNow());
    uint64_t edge = MWNow(); Observe(app, MWInputActive, edge);
    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    assert(read(reader, frame, sizeof(frame)) == sizeof(frame));
    assert(frame[0] == MW_CONTROL_RENEW && MWControlDecode(frame) == edge + MW_LIMIT);
    assert(app.task == same && app.starts == 0);
    assert([[app valueForKey:@"deadline"] unsignedLongLongValue] == old); // No optimistic UI extension.
    NSData *ack = Ack(edge + MW_LIMIT);
    [app receivedControl:@[same, [ack subdataWithRange:NSMakeRange(0, 4)]]];
    assert([[app valueForKey:@"deadline"] unsignedLongLongValue] == old);
    [app receivedControl:@[same, [ack subdataWithRange:NSMakeRange(4, ack.length - 4)]]];
    assert([[app valueForKey:@"deadline"] unsignedLongLongValue] == edge + MW_LIMIT);
    assert(app.pendingAutomaticDeadline == 0);
    for (int i = 0; i < 4; i++) Observe(app, MWInputActive, MWNow());
    assert(read(reader, frame, sizeof(frame)) == -1 && errno == EAGAIN);
    [app receivedControl:@[[NSTask new], Ack(edge + MW_LIMIT + 1)]];
    assert(!app.controlFailed); // Old helper data cannot affect the current one.
    [app stop:nil]; [app finish:MWStopped];
}

static void TestStartingAckAndInvalidFrame(void) {
    uint64_t now = MWNow(), old = now + 20 * MW_SECOND, extended = now + MW_LIMIT;
    TestWarmupApp *app = New(MWStarting, old);
    app.requestedDeadline = extended;
    NSMutableData *message = [Ack(extended) mutableCopy];
    unsigned char running = MWBeforeRunning;
    [message appendBytes:&running length:1];
    [app receivedControl:@[app.task, message]]; // ACK can arrive while AudioDeviceStart is pending.
    assert([[app valueForKey:@"phase"] intValue] == MWActive);
    assert([[app valueForKey:@"deadline"] unsignedLongLongValue] == extended);
    [app receivedControl:@[app.task, Ack(extended + 1)]];
    assert(app.controlFailed && [[app valueForKey:@"phase"] intValue] == MWStopping);
    [app finish:MWStopped];
    assert(!app.starts);
}

static void TestExpiryBoundaryAndExplicitStop(void) {
    uint64_t now = MWNow();
    TestWarmupApp *app = New(MWActive, now - MW_SECOND);
    app.pendingAutomaticDeadline = now + 100 * MW_SECOND;
    [app finish:MWExpired];
    assert(app.starts == 1 && app.startedUntil == now + 100 * MW_SECOND);

    app = New(MWActive, now + 30 * MW_SECOND);
    app.pendingAutomaticDeadline = now + 100 * MW_SECOND;
    [app stop:nil]; [app finish:MWExpired];
    assert(!app.starts && !app.pendingAutomaticDeadline);

    app = New(MWActive, now - MW_SECOND);
    Observe(app, MWInputInactive, MWNow());
    uint64_t queued = MWNow();
    [app finish:MWExpired];
    Observe(app, MWInputActive, queued); // The callback was queued before the old task finished.
    assert(app.starts == 1 && app.startedUntil == queued + MW_LIMIT);

    app = New(MWActive, now + 30 * MW_SECOND);
    Observe(app, MWInputInactive, MWNow());
    queued = MWNow();
    [app stop:nil]; [app finish:MWExpired]; // User stop wins even if helper had already committed expiry.
    Observe(app, MWInputActive, queued);
    assert(!app.starts);
}

static void TestVeryOldEdgeDoesNotRestart(void) {
    uint64_t now = MWNow(), old = now - 3 * MW_LIMIT;
    TestWarmupApp *app = New(MWIdle, 0);
    app.idleSince = old - MW_SECOND;
    Observe(app, MWInputInactive, old);
    Observe(app, MWInputActive, old + MW_SECOND);
    Observe(app, MWInputActive, MWNow()); // Route recheck must retain the old edge's time.
    assert(!app.starts);
}

static void TestPendingRouteCannotSurviveFailure(void) {
    TestWarmupApp *app = New(MWActive, MWNow() + 30 * MW_SECOND);
    Observe(app, MWInputInactive, MWNow());
    MWAutoReading unresolved = { 11, 21, MWInputActive, MWRouteUnclear, MWNow() };
    [app observeAutomatic:unresolved supported:YES epoch:1 binding:1];
    [app finish:MWDeviceChanged];
    Observe(app, MWInputActive, MWNow());
    assert(!app.starts);
}

int main(void) {
    @autoreleasepool {
        signal(SIGPIPE, SIG_IGN);
        TestActiveRenewalAndSplitAck();
        TestStartingAckAndInvalidFrame();
        TestExpiryBoundaryAndExplicitStop();
        TestVeryOldEdgeDoesNotRestart();
        TestPendingRouteCannotSurviveFailure();
        puts("App control tests passed: same helper, acknowledged renewal, split frames, expiry races, explicit stops, stale events.");
    }
    return 0;
}
