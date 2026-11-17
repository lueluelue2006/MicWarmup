#import "DoubaoMonitor.h"
#import "hal_probe.h"
#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>

static const AudioObjectPropertyAddress Processes = { kAudioHardwarePropertyProcessObjectList,
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress PIDToProcess = { kAudioHardwarePropertyTranslatePIDToProcessObject,
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress RunningInput = { kAudioProcessPropertyIsRunningInput,
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress InputDevices = { kAudioProcessPropertyDevices,
    kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain };

static BOOL SupportsProcessProperties(void) {
    if (@available(macOS 14.2, *)) return YES;
    return NO;
}

static pid_t DoubaoPID(void) {
    pid_t pid = 0;
    for (NSRunningApplication *app in [NSRunningApplication
             runningApplicationsWithBundleIdentifier:@"com.bytedance.inputmethod.doubaoime"]) {
        if (app.terminated || ![app.executableURL.path isEqualToString:
                @"/Library/Input Methods/DoubaoIme.app/Contents/MacOS/DoubaoIme"]) continue;
        if (pid) return 0;
        pid = app.processIdentifier;
    }
    return pid > 0 ? pid : 0;
}

static BOOL ProcessForPID(pid_t pid, AudioObjectID *object) {
    *object = kAudioObjectUnknown;
    UInt32 size = sizeof(*object);
    return pid == 0 || (AudioObjectGetPropertyData(kAudioObjectSystemObject, &PIDToProcess,
           sizeof(pid), &pid, &size, object) == noErr && size == sizeof(*object));
}

static MWInputActivity ReadActivity(AudioObjectID object) {
    UInt32 value;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(object, &RunningInput, 0, NULL, &size, &value) != noErr ||
        size != sizeof(value) || value > 1) return MWInputUnclear;
    return value ? MWInputActive : MWInputInactive;
}

static MWInputRoute ReadRoute(AudioObjectID object) {
    MWInput input = MWProbeBuiltInInput(false);
    if (input.selection != MWInputOK) return MWRouteUnclear;
    UInt32 size;
    if (AudioObjectGetPropertyDataSize(object, &InputDevices, 0, NULL, &size) != noErr ||
        size == 0 || size % sizeof(AudioObjectID)) return MWRouteUnclear;
    if (size != sizeof(AudioObjectID)) return MWRouteOther;
    AudioObjectID device;
    if (AudioObjectGetPropertyData(object, &InputDevices, 0, NULL, &size, &device) != noErr ||
        size != sizeof(device) || device == kAudioObjectUnknown) return MWRouteUnclear;
    return device == input.id ? MWRouteBuiltIn : MWRouteOther;
}

@interface MWDoubaoMonitor ()
@property (copy) MWMonitorUpdate update;
@property (strong) dispatch_queue_t worker;
@property (strong) dispatch_source_t health;
@property (copy) AudioObjectPropertyListenerBlock systemListener;
@property (copy) AudioObjectPropertyListenerBlock processListener;
// Only the worker writes these counters. Main reads them to reject queued stale updates.
@property (atomic) uint64_t epoch;
@property (atomic) uint64_t binding;
@property (atomic) uint64_t lastOtherRouteAt;
@property BOOL watching;
@property BOOL confirmed;
@property BOOL refreshQueued;
@property BOOL retryQueued;
@property pid_t pid;
@property AudioObjectID object;
@end

@implementation MWDoubaoMonitor
- (instancetype)initWithUpdate:(MWMonitorUpdate)update {
    self = [super init];
    if (self) {
        _update = [update copy];
        _worker = dispatch_queue_create("local.doubaolab.MicWarmup.monitor", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)publish:(MWAutoReading)reading supported:(BOOL)supported {
    if (reading.activity == MWInputActive && reading.route == MWRouteOther) {
        self.lastOtherRouteAt = reading.at;
    }
    uint64_t epoch = self.epoch, binding = self.binding;
    NSArray *message = @[@(epoch), @(binding), @(supported),
        [NSData dataWithBytes:&reading length:sizeof(reading)]];
    [self performSelectorOnMainThread:@selector(deliver:) withObject:message waitUntilDone:NO
        modes:@[NSRunLoopCommonModes, NSEventTrackingRunLoopMode, NSModalPanelRunLoopMode]];
}

- (void)deliver:(NSArray *)message {
    uint64_t epoch = [message[0] unsignedLongLongValue], binding = [message[1] unsignedLongLongValue];
    NSData *data = message[3];
    if (data.length != sizeof(MWAutoReading)) return;
    MWAutoReading reading;
    [data getBytes:&reading length:sizeof(reading)];
    // An observed switch away from the built-in mic takes precedence over older
    // queued readings. Simply finishing a short utterance preserves its start.
    if (reading.at && reading.at <= self.lastOtherRouteAt && reading.route != MWRouteOther) return;
    if (self.binding != binding || self.epoch != epoch) return;
    self.update(epoch, binding, [message[2] boolValue], reading);
}

- (BOOL)canResumeFrom:(uint64_t)edgeAt epoch:(uint64_t)epoch binding:(uint64_t)binding {
    return edgeAt > self.lastOtherRouteAt && self.binding == binding && self.epoch == epoch;
}

- (void)sample {
    MWAutoReading reading = { .pid = self.pid, .object = self.object, .at = MWNow() };
    if (self.confirmed && reading.object != kAudioObjectUnknown) {
        reading.activity = ReadActivity(reading.object);
        if (reading.activity == MWInputActive) reading.route = ReadRoute(reading.object);
    }
    [self publish:reading supported:YES];
}

- (void)unbind {
    self.binding++;
    self.lastOtherRouteAt = 0;
    self.confirmed = NO;
    self.retryQueued = NO;
    if (self.processListener) {
        AudioObjectRemovePropertyListenerBlock(self.object, &RunningInput, self.worker, self.processListener);
        AudioObjectRemovePropertyListenerBlock(self.object, &InputDevices, self.worker, self.processListener);
        self.processListener = nil;
    }
    self.pid = 0;
    self.object = kAudioObjectUnknown;
}

- (BOOL)bindPID:(pid_t)pid object:(AudioObjectID)object {
    uint64_t epoch = self.epoch, binding = self.binding;
    __weak MWDoubaoMonitor *weakSelf = self;
    AudioObjectPropertyListenerBlock listener = [^(UInt32 count, const AudioObjectPropertyAddress *addresses) {
        (void)count; (void)addresses;
        MWDoubaoMonitor *monitor = weakSelf;
        if (monitor && monitor.watching && monitor.epoch == epoch && monitor.binding == binding) {
            [monitor sample];
        }
    } copy];
    if (AudioObjectAddPropertyListenerBlock(object, &RunningInput, self.worker, listener) != noErr) return NO;
    if (AudioObjectAddPropertyListenerBlock(object, &InputDevices, self.worker, listener) != noErr) {
        AudioObjectRemovePropertyListenerBlock(object, &RunningInput, self.worker, listener);
        return NO;
    }
    self.pid = pid;
    self.object = object;
    self.processListener = listener;
    self.confirmed = YES;
    return YES;
}

- (BOOL)observeProcessList {
    if (self.systemListener) return YES;
    uint64_t epoch = self.epoch;
    __weak MWDoubaoMonitor *weakSelf = self;
    AudioObjectPropertyListenerBlock listener = [^(UInt32 count, const AudioObjectPropertyAddress *addresses) {
        (void)count; (void)addresses;
        MWDoubaoMonitor *monitor = weakSelf;
        if (!monitor || !monitor.watching || monitor.epoch != epoch || monitor.refreshQueued) return;
        monitor.refreshQueued = YES;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 35 * NSEC_PER_MSEC), monitor.worker, ^{
            MWDoubaoMonitor *current = weakSelf;
            if (!current || !current.watching || current.epoch != epoch) return;
            current.refreshQueued = NO;
            [current refresh:NO];
        });
    } copy];
    if (AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &Processes, self.worker, listener) != noErr) return NO;
    self.systemListener = listener;
    return YES;
}

- (void)refresh:(BOOL)healthCheck {
    if (![self observeProcessList]) {
        [self publish:(MWAutoReading){0} supported:YES];
        return;
    }
    pid_t pid = DoubaoPID();
    AudioObjectID object;
    if (!ProcessForPID(pid, &object)) {
        self.confirmed = NO;
        [self sample];
        return;
    }
    // Other clients (including our own helper) can change the system process list.
    // Keep the same binding and its edge history when that doesn't change our target.
    if (self.processListener && self.pid == pid && self.object == object) {
        BOOL shouldSample = healthCheck || !self.confirmed;
        self.confirmed = YES;
        if (shouldSample) [self sample];
        return;
    }
    if (self.processListener || (pid && object != kAudioObjectUnknown)) [self unbind];
    if (pid && object != kAudioObjectUnknown) [self bindPID:pid object:object];
    [self sample];
}

- (void)setWatching:(BOOL)watching epoch:(uint64_t)epoch {
    dispatch_async(self.worker, ^{
        self.epoch = epoch;
        self.watching = watching;
        self.refreshQueued = NO;
        [self unbind];
        if (self.health) { dispatch_source_cancel(self.health); self.health = nil; }
        if (self.systemListener) {
            AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &Processes,
                                                   self.worker, self.systemListener);
            self.systemListener = nil;
        }
        if (!watching) return;
        if (!SupportsProcessProperties()) { [self publish:(MWAutoReading){0} supported:NO]; return; }
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.worker);
        self.health = timer;
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                                  5 * NSEC_PER_SEC, NSEC_PER_SEC);
        __weak MWDoubaoMonitor *weakSelf = self;
        dispatch_source_set_event_handler(timer, ^{
            MWDoubaoMonitor *monitor = weakSelf;
            if (monitor && monitor.watching && monitor.epoch == epoch) [monitor refresh:YES];
        });
        dispatch_resume(timer);
        [self refresh:YES];
    });
}

- (void)retryBefore:(uint64_t)verifyBy epoch:(uint64_t)epoch binding:(uint64_t)binding {
    dispatch_async(self.worker, ^{
        const uint64_t delay = 50 * NSEC_PER_MSEC;
        uint64_t now = MWNow();
        if (!self.watching || self.epoch != epoch || self.binding != binding || self.retryQueued ||
            !now || now >= verifyBy || verifyBy - now <= delay) return;
        self.retryQueued = YES;
        __weak MWDoubaoMonitor *weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay), self.worker, ^{
            MWDoubaoMonitor *monitor = weakSelf;
            if (!monitor || !monitor.watching || monitor.epoch != epoch || monitor.binding != binding) return;
            monitor.retryQueued = NO;
            [monitor sample];
        });
    });
}
@end

int MWCheckDoubaoMonitor(void) {
    BOOL supported = SupportsProcessProperties();
    pid_t pid = DoubaoPID();
    AudioObjectID object = kAudioObjectUnknown;
    BOOL bound = supported && pid && ProcessForPID(pid, &object) && object != kAudioObjectUnknown;
    dispatch_queue_t worker = dispatch_queue_create("local.doubaolab.MicWarmup.monitor.check", DISPATCH_QUEUE_SERIAL);
    AudioObjectPropertyListenerBlock listener = ^(UInt32 count, const AudioObjectPropertyAddress *addresses) {
        (void)count; (void)addresses;
    };
    BOOL system = supported && AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &Processes, worker, listener) == noErr;
    BOOL input = bound && AudioObjectAddPropertyListenerBlock(object, &RunningInput, worker, listener) == noErr;
    BOOL route = bound && AudioObjectAddPropertyListenerBlock(object, &InputDevices, worker, listener) == noErr;
    if (route) AudioObjectRemovePropertyListenerBlock(object, &InputDevices, worker, listener);
    if (input) AudioObjectRemovePropertyListenerBlock(object, &RunningInput, worker, listener);
    if (system) AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &Processes, worker, listener);
    printf("api_supported=%s\ntarget_visible=%s\nbinding_available=%s\n"
           "process_list_listener_available=%s\ninput_listener_available=%s\ninput_devices_listener_available=%s\n",
           supported ? "true" : "false", pid ? "true" : "false", bound ? "true" : "false",
           system ? "true" : "false", input ? "true" : "false", route ? "true" : "false");
    return 0;
}

#ifdef MW_CHECK_AUTO_STANDALONE
int main(void) {
    @autoreleasepool { return MWCheckDoubaoMonitor(); }
}
#endif
