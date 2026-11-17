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
- (void)requestExpiredStart:(uint64_t)expires { [super startAutomatic:YES until:expires]; }
- (void)startAutomatic:(BOOL)isAutomatic until:(uint64_t)expires {
    assert(isAutomatic);
    assert([[self valueForKey:@"phase"] intValue] == MWIdle);
    assert(!self.task);
    self.starts++;
    self.startedUntil = expires;
}
@end

@interface MWDoubaoMonitor (DeliveryTests)
- (void)publish:(MWAutoReading)reading supported:(BOOL)supported;
- (void)deliver:(NSArray *)message;
@end

// Buffer the monitor's normal delivery requests to simulate a busy main thread.
@interface QueuedMonitor : MWDoubaoMonitor
@property (strong) NSMutableArray<NSArray *> *messages;
@end
@implementation QueuedMonitor
- (void)performSelectorOnMainThread:(SEL)selector withObject:(id)object waitUntilDone:(BOOL)wait
                               modes:(NSArray<NSRunLoopMode> *)modes {
    assert(selector == @selector(deliver:) && !wait && modes.count);
    [self.messages addObject:object];
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

static void TestExpiryDuringHelperLaunch(void) {
    uint64_t expired = MWNow() - 1;
    TestWarmupApp *idle = New(MWIdle, 0);
    uint64_t previousFloor = idle.idleSince;
    [idle requestExpiredStart:expired];
    assert(!idle.task && !idle.problemTitle && idle.idleSince == previousFloor);

    TestWarmupApp *app = New(MWStarting, expired);
    NSPipe *pipe = [NSPipe pipe];
    app.control = pipe.fileHandleForWriting;
    [pipe.fileHandleForReading closeAndReturnError:NULL];
    assert(![app sendControlBytes:"S" length:1] && !app.control && !app.controlFailed);
    Observe(app, MWInputInactive, MWNow());
    uint64_t edge = MWNow();
    Observe(app, MWInputActive, edge);
    [app finish:MWExpired];
    assert(app.starts == 1 && app.startedUntil == edge + MW_LIMIT);
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

static void TestDelayedShortInputRenewsTheSameHelper(void) {
    uint64_t now = MWNow(), edge = now - 2 * MW_SECOND;
    TestWarmupApp *app = New(MWActive, now + 10 * MW_SECOND);
    app.idleSince = edge - MW_SECOND;
    NSPipe *pipe = [NSPipe pipe];
    app.control = pipe.fileHandleForWriting;
    int reader = pipe.fileHandleForReading.fileDescriptor;
    assert(fcntl(reader, F_SETFL, O_NONBLOCK) == 0);
    Observe(app, MWInputInactive, edge - 1);
    Observe(app, MWInputActive, edge);
    Observe(app, MWInputInactive, edge + MW_SECOND / 5);
    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    assert(read(reader, frame, sizeof(frame)) == sizeof(frame));
    assert(frame[0] == MW_CONTROL_RENEW && MWControlDecode(frame) == edge + MW_LIMIT);
    [app receivedControl:@[app.task, Ack(edge + MW_LIMIT)]];
    assert([[app valueForKey:@"deadline"] unsignedLongLongValue] == edge + MW_LIMIT);
    assert(!app.starts && read(reader, frame, sizeof(frame)) == -1 && errno == EAGAIN);
    [app stop:nil]; [app finish:MWStopped];
}

static void TestQueuedDeviceSwitch(void) {
    uint64_t base = MWNow() - 4 * MW_SECOND;
    __block TestWarmupApp *app = New(MWIdle, 0);
    app.idleSince = base - 1;
    app.autoEpoch = app.autoBinding = 0;
    QueuedMonitor *monitor = [[QueuedMonitor alloc] initWithUpdate:^(
        uint64_t epoch, uint64_t binding, BOOL supported, MWAutoReading reading) {
        [app observeAutomatic:reading supported:supported epoch:epoch binding:binding];
    }];
    monitor.messages = [NSMutableArray array];
    MWAutoReading readings[] = {
        { 11, 21, MWInputInactive, MWRouteUnclear, base },
        { 11, 21, MWInputActive, MWRouteBuiltIn, base + MW_SECOND },
        { 11, 21, MWInputActive, MWRouteOther, base + 2 * MW_SECOND },
        { 11, 21, MWInputInactive, MWRouteUnclear, base + 3 * MW_SECOND },
        { 11, 21, MWInputActive, MWRouteBuiltIn, base + 3 * MW_SECOND + 1 },
        { 11, 21, MWInputInactive, MWRouteUnclear, base + 3 * MW_SECOND + 2 },
    };
    for (size_t i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
        [monitor publish:readings[i] supported:YES];
    }
    // The old helper can expire before the already-observed external switch reaches AppKit.
    TestWarmupApp *expiring = New(MWActive, base);
    expiring.monitor = monitor;
    expiring.autoEpoch = expiring.autoBinding = 0;
    expiring.pendingAutomaticDeadline = readings[1].at + MW_LIMIT;
    [expiring finish:MWExpired];
    assert(!expiring.starts);
    for (NSArray *message in monitor.messages) [monitor deliver:message];
    assert(app.starts == 1 && app.startedUntil == readings[4].at + MW_LIMIT);

    enum { FinishEarly, FinishLate, ExplicitStop, StopFailed };
    for (int scenario = FinishEarly; scenario <= StopFailed; scenario++) {
        app = New(MWActive, MWNow() + 10 * MW_SECOND);
        app.monitor = monitor;
        app.autoEpoch = app.autoBinding = 0;
        app.idleSince = base - 1;
        app.sessionAutomatic = YES;
        for (NSUInteger i = 0; i < monitor.messages.count; i++) {
            [monitor deliver:monitor.messages[i]];
            if (i == 2) {
                assert([[app valueForKey:@"phase"] intValue] == MWStopping);
                if (scenario == FinishEarly) [app finish:MWStopped];
            }
        }
        if (scenario != FinishEarly) {
            assert(!app.starts && app.pendingAutomaticDeadline == readings[4].at + MW_LIMIT);
            if (scenario == ExplicitStop) [app stop:nil];
            [app finish:scenario == StopFailed ? MWStopFailed : MWStopped];
        }
        if (scenario == ExplicitStop || scenario == StopFailed) assert(!app.starts);
        else assert(app.starts == 1 && app.startedUntil == readings[4].at + MW_LIMIT);
    }
    app.monitor = nil;

    for (NSUInteger automatic = 0; automatic <= 1; automatic++) {
        TestWarmupApp *running = New(MWActive, MWNow() + 20 * MW_SECOND);
        running.sessionAutomatic = (BOOL)automatic;
        running.pendingAutomaticDeadline = MWNow() + MW_LIMIT;
        [running observeAutomatic:readings[2] supported:YES epoch:1 binding:1];
        assert(!running.pendingAutomaticDeadline);
        assert([[running valueForKey:@"phase"] intValue] == (automatic ? MWStopping : MWActive));
        [running stop:nil]; [running finish:MWStopped];
    }
}

static void ExpectResultAfterControlFailure(MWOutcome result, NSString *title) {
    TestWarmupApp *app = New(MWActive, MWNow() + 30 * MW_SECOND);
    [app receivedControl:@[app.task, Ack(app.requestedDeadline + 1)]];
    assert(app.controlFailed && [[app valueForKey:@"phase"] intValue] == MWStopping);
    [app finish:result];
    assert(!app.starts && [app.problemTitle isEqualToString:title]);
}

int main(void) {
    @autoreleasepool {
        signal(SIGPIPE, SIG_IGN);
        TestActiveRenewalAndSplitAck();
        TestStartingAckAndInvalidFrame();
        TestExpiryBoundaryAndExplicitStop();
        TestVeryOldEdgeDoesNotRestart();
        TestExpiryDuringHelperLaunch();
        TestPendingRouteCannotSurviveFailure();
        TestDelayedShortInputRenewsTheSameHelper();
        TestQueuedDeviceSwitch();
        ExpectResultAfterControlFailure(MWStopFailed, @"停止未确认");
        ExpectResultAfterControlFailure(MWWatchdog, @"停止未确认");
        ExpectResultAfterControlFailure(MWStartTimedOut, @"启动超时，状态未确认");
        ExpectResultAfterControlFailure(MWStartTimedOutStopped, @"启动超时，预热已停止");
        ExpectResultAfterControlFailure(MWSessionQueryFailed, @"设备状态无法确认");
        ExpectResultAfterControlFailure(MWStopped, @"辅助进程通信异常");
        puts("App control tests passed: renewal, delayed edges, route changes, explicit stops and error reporting.");
    }
    return 0;
}
