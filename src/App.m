#import <AppKit/AppKit.h>
#import "DoubaoMonitor.h"
#import "hal_probe.h"
#import "control.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

static NSString *const AutoPreference = @"AutomaticWarmupEnabled";
static const CGFloat SettingsWidth = 480;
static const CGFloat SettingsInset = 24;

static NSTextField *SettingsLabel(NSString *text, CGFloat size, NSFontWeight weight, NSColor *color) {
    NSTextField *label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont systemFontOfSize:size weight:weight];
    label.textColor = color;
    return label;
}

static NSArray<NSRunLoopMode> *WarmupUIModes(void) {
    return @[NSRunLoopCommonModes, NSEventTrackingRunLoopMode, NSModalPanelRunLoopMode];
}

@interface SettingsCard : NSView
@end

@implementation SettingsCard
- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 0.5, 0.5)
                                                      xRadius:13 yRadius:13];
    [NSColor.controlBackgroundColor setFill];
    [path fill];
    [NSColor.separatorColor setStroke];
    [path stroke];
}
- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    self.needsDisplay = YES;
}
@end

@interface WarmupApp : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate> {
    MWPhase phase;
    MWAutoState automatic;
    uint64_t deadline;
}
@property (strong) NSWindow *window;
@property (strong) NSStackView *settings;
@property (strong) NSTextField *status;
@property (strong) NSTextField *probe;
@property (strong) NSTextField *autoStatus;
@property (strong) NSButton *autoButton;
@property (strong) NSButton *startButton;
@property (strong) NSButton *stopButton;
@property (strong) NSStatusItem *statusItem;
@property (strong) NSMenuItem *menuStatus;
@property (strong) NSMenuItem *menuAuto;
@property (strong) NSMenuItem *menuStart;
@property (strong) NSMenuItem *menuStop;
@property (copy) NSString *automaticSummary;
@property (copy) NSString *problemTitle;
@property (copy) NSString *countdown;
@property (copy) NSString *statusSymbol;
@property (strong) NSTask *task;
@property (strong) NSFileHandle *control;
@property (strong) NSFileHandle *helperOutput;
@property (strong) NSMutableData *controlBuffer;
@property uint64_t requestedDeadline;
@property uint64_t pendingAutomaticDeadline;
@property BOOL controlFailed;
@property (strong) NSTimer *timer;
@property (strong) MWDoubaoMonitor *monitor;
@property uint64_t autoEpoch;
@property uint64_t autoBinding;
@property uint64_t idleSince;
@property BOOL checkPending;
@property BOOL exitPending;
@property BOOL autoEnabled;
@property BOOL sleeping;
@property BOOL sessionAutomatic;
@property BOOL stoppingForRouteChange;
@end

@implementation WarmupApp

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    signal(SIGPIPE, SIG_IGN);
    NSNotificationCenter *workspace = NSWorkspace.sharedWorkspace.notificationCenter;
    [workspace addObserver:self selector:@selector(sleepOrWake:)
                      name:NSWorkspaceWillSleepNotification object:nil];
    [workspace addObserver:self selector:@selector(sleepOrWake:)
                      name:NSWorkspaceDidWakeNotification object:nil];

    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults registerDefaults:@{ AutoPreference: @YES }];
    self.autoEnabled = [defaults boolForKey:AutoPreference];
    self.idleSince = MWNow();

    [self buildSettings];
    [self buildStatusMenu];
    __weak WarmupApp *owner = self;
    self.monitor = [[MWDoubaoMonitor alloc] initWithUpdate:^(uint64_t epoch, uint64_t binding,
                                                            BOOL supported, MWAutoReading reading) {
        [owner observeAutomatic:reading supported:supported epoch:epoch binding:binding];
    }];
    [self resetAutomatic];
    [self checkInput];
}

- (void)buildSettings {
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, SettingsWidth + 2 * SettingsInset, 500)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"麦克风预热";
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.titlebarAppearsTransparent = YES;
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;

    NSImageView *cover = [NSImageView new];
    NSURL *coverURL = [NSBundle.mainBundle URLForResource:@"cover" withExtension:@"png"];
    if (coverURL) cover.image = [[NSImage alloc] initWithContentsOfURL:coverURL];
    cover.imageScaling = NSImageScaleProportionallyUpOrDown;
    cover.wantsLayer = YES;
    cover.layer.cornerRadius = 14;
    cover.layer.masksToBounds = YES;
    [cover.widthAnchor constraintEqualToConstant:SettingsWidth].active = YES;
    [cover.heightAnchor constraintEqualToConstant:SettingsWidth / 3].active = YES;

    NSTextField *title = SettingsLabel(@"麦克风预热", 27, NSFontWeightSemibold, NSColor.labelColor);
    NSTextField *description = SettingsLabel(@"每次豆包开始使用内置麦克风，预热重新计时 2 分钟。\n主要帮助后续输入，首次启动可能仍需等待。",
                                             13, NSFontWeightRegular, NSColor.secondaryLabelColor);
    self.autoButton = [NSButton checkboxWithTitle:@"自动预热" target:self action:@selector(toggleAutomatic:)];
    [self.autoButton setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [self.autoButton setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                              forOrientation:NSLayoutConstraintOrientationHorizontal];
    self.status = SettingsLabel(@"当前未预热", 16, NSFontWeightSemibold, NSColor.labelColor);
    self.status.preferredMaxLayoutWidth = 336;
    self.autoStatus = SettingsLabel(@"正在检查豆包的麦克风状态…", 12, NSFontWeightRegular, NSColor.secondaryLabelColor);
    self.probe = SettingsLabel(@"正在检查内置麦克风…", 11, NSFontWeightRegular, NSColor.secondaryLabelColor);
    NSTextField *privacy = SettingsLabel(@"不读取、保存或上传录音。关闭窗口后仍在菜单栏待命。",
                                         11, NSFontWeightRegular, NSColor.secondaryLabelColor);

    NSStackView *statusRow = [NSStackView stackViewWithViews:@[self.status, self.autoButton]];
    statusRow.alignment = NSLayoutAttributeCenterY;
    statusRow.spacing = 12;
    statusRow.distribution = NSStackViewDistributionFill;
    NSStackView *cardContents = [NSStackView stackViewWithViews:@[statusRow, self.autoStatus]];
    cardContents.orientation = NSUserInterfaceLayoutOrientationVertical;
    cardContents.alignment = NSLayoutAttributeLeading;
    cardContents.spacing = 5;
    cardContents.translatesAutoresizingMaskIntoConstraints = NO;
    SettingsCard *card = [SettingsCard new];
    [card addSubview:cardContents];
    [NSLayoutConstraint activateConstraints:@[
        [cardContents.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:14],
        [cardContents.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-14],
        [cardContents.topAnchor constraintEqualToAnchor:card.topAnchor constant:12],
        [cardContents.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-12],
        [statusRow.widthAnchor constraintEqualToAnchor:cardContents.widthAnchor],
        [self.autoStatus.widthAnchor constraintEqualToAnchor:cardContents.widthAnchor],
    ]];
    self.autoStatus.preferredMaxLayoutWidth = SettingsWidth - 28;

    self.startButton = [NSButton buttonWithTitle:@"手动预热 2 分钟" target:self action:@selector(start:)];
    self.stopButton = [NSButton buttonWithTitle:@"停止本次" target:self action:@selector(stop:)];
    self.startButton.refusesFirstResponder = YES;
    NSStackView *buttons = [NSStackView stackViewWithViews:@[self.startButton, self.stopButton]];
    buttons.spacing = 10;

    NSStackView *stack = [NSStackView stackViewWithViews:@[cover, title, description, card, buttons, privacy, self.probe]];
    self.settings = stack;
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 11;
    [stack setCustomSpacing:17 afterView:cover];
    [stack setCustomSpacing:5 afterView:title];
    [stack setCustomSpacing:17 afterView:description];
    [stack setCustomSpacing:13 afterView:card];
    [stack setCustomSpacing:14 afterView:buttons];
    [stack setCustomSpacing:3 afterView:privacy];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *content = self.window.contentView;
    [content addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:SettingsInset],
        [stack.widthAnchor constraintEqualToConstant:SettingsWidth],
        [stack.topAnchor constraintEqualToAnchor:content.topAnchor constant:16],
        [card.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
    ]];
    for (NSTextField *label in @[title, description, self.probe, privacy]) {
        label.preferredMaxLayoutWidth = SettingsWidth;
        [label.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    }

    // Key equivalents while a settings window is focused. The app remains an accessory.
    NSMenu *menu = [NSMenu new];
    NSMenuItem *appItem = [menu addItemWithTitle:@"麦克风预热" action:NULL keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"麦克风预热"];
    [appMenu addItemWithTitle:@"关闭设置" action:@selector(performClose:) keyEquivalent:@"w"];
    [appMenu addItemWithTitle:@"退出麦克风预热" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSApp.mainMenu = menu;
}

- (void)buildStatusMenu {
    self.statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    NSStatusBarButton *button = self.statusItem.button;
    button.imagePosition = NSImageLeading;
    button.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium];

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"麦克风预热"];
    menu.autoenablesItems = NO;
    menu.delegate = self;
    self.menuStatus = [menu addItemWithTitle:@"麦克风预热" action:NULL keyEquivalent:@""];
    self.menuStatus.enabled = NO;
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:@"打开设置…" action:@selector(showWindow:) keyEquivalent:@","].target = self;
    self.menuAuto = [menu addItemWithTitle:@"自动预热" action:@selector(toggleAutomatic:) keyEquivalent:@""];
    self.menuAuto.target = self;
    [menu addItem:NSMenuItem.separatorItem];
    self.menuStart = [menu addItemWithTitle:@"手动预热 2 分钟" action:@selector(start:) keyEquivalent:@""];
    self.menuStart.target = self;
    self.menuStop = [menu addItemWithTitle:@"停止本次" action:@selector(stop:) keyEquivalent:@""];
    self.menuStop.target = self;
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:@"退出麦克风预热" action:@selector(terminate:) keyEquivalent:@"q"].target = NSApp;
    self.statusItem.menu = menu;
}

- (void)fitSettings {
    [self.window.contentView layoutSubtreeIfNeeded];
    CGFloat height = ceil(self.settings.fittingSize.height) + 16 + SettingsInset;
    if (fabs(NSHeight(self.window.contentView.frame) - height) < 1) return;
    NSRect old = self.window.frame;
    NSRect rect = [self.window frameRectForContentRect:NSMakeRect(0, 0, SettingsWidth + 2 * SettingsInset, height)];
    rect.origin = NSMakePoint(NSMinX(old), NSMaxY(old) - NSHeight(rect));
    [self.window setFrame:rect display:self.window.visible];
}

- (void)showWindow:(id)sender {
    (void)sender;
    if (self.exitPending) return;
    [self fitSettings];
    if (!self.window.visible) [self.window center];
    if (@available(macOS 14.0, *)) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];
    [self.window makeKeyAndOrderFront:nil];
}

- (void)refreshPresentation {
    BOOL canStart = phase == MWIdle && !self.sleeping && !self.exitPending;
    BOOL canStop = (phase == MWStarting || phase == MWActive) && !self.exitPending;
    self.startButton.enabled = self.menuStart.enabled = canStart;
    self.stopButton.enabled = self.menuStop.enabled = canStop;
    self.autoButton.state = self.menuAuto.state = self.autoEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    self.autoButton.enabled = self.menuAuto.enabled = !self.exitPending;

    NSString *symbol = (!self.autoEnabled || self.sleeping) ? @"mic.slash" : @"mic";
    NSString *caption = self.automaticSummary ?: @"自动待命";
    NSString *counter = @"";
    switch (phase) {
        case MWStarting: caption = @"正在请求预热…"; break;
        case MWStopping: caption = @"正在停止…"; break;
        case MWActive:
            symbol = @"mic.fill";
            counter = self.countdown ?: @"";
            caption = [NSString stringWithFormat:@"预热中 · 剩余 %@", counter];
            break;
        case MWIdle:
            if (self.problemTitle) { caption = self.problemTitle; symbol = @"exclamationmark.circle"; }
            break;
    }
    NSStatusBarButton *button = self.statusItem.button;
    if (![self.statusSymbol isEqualToString:symbol]) {
        NSImageSymbolConfiguration *style = [NSImageSymbolConfiguration configurationWithPointSize:17
                                                                                             weight:NSFontWeightMedium];
        NSImage *image = [[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil]
                          imageWithSymbolConfiguration:style];
        image.template = YES;
        button.image = image;
        self.statusSymbol = symbol;
    }
    if (![button.title isEqualToString:counter]) button.title = counter;
    if (![self.menuStatus.title isEqualToString:caption]) {
        self.menuStatus.title = caption;
        NSString *description = [@"麦克风预热 · " stringByAppendingString:caption];
        button.toolTip = description;
        [button setAccessibilityLabel:description];
    }
    if (self.window.visible) [self fitSettings];
}

- (void)menuNeedsUpdate:(NSMenu *)menu { (void)menu; [self updateTime:nil]; [self refreshPresentation]; }

- (void)showAutomatic:(NSString *)text menuTitle:(NSString *)menuTitle {
    self.autoStatus.stringValue = text;
    self.automaticSummary = menuTitle;
    [self refreshPresentation];
}

- (void)resetAutomatic {
    automatic = (MWAutoState){0};
    self.pendingAutomaticDeadline = 0;
    self.stoppingForRouteChange = NO;
    self.autoBinding = 0;
    self.autoEpoch++;
    BOOL watching = self.autoEnabled && !self.sleeping && !self.exitPending;
    if (!self.autoEnabled) [self showAutomatic:@"自动预热已暂停。" menuTitle:@"自动预热已暂停"];
    else if (self.sleeping) [self showAutomatic:@"休眠期间暂停自动预热。" menuTitle:@"休眠期间暂停"];
    else [self showAutomatic:@"正在检查豆包的麦克风状态。" menuTitle:@"自动待命"];
    [self.monitor setWatching:watching epoch:self.autoEpoch];
}

- (void)toggleAutomatic:(id)sender {
    if (self.exitPending) return;
    self.autoEnabled = sender == self.menuAuto ? !self.autoEnabled : self.autoButton.state == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:self.autoEnabled forKey:AutoPreference];
    if (!self.autoEnabled) [self stop:nil];
    [self resetAutomatic];
}

- (void)observeAutomatic:(MWAutoReading)reading supported:(BOOL)supported
                    epoch:(uint64_t)epoch binding:(uint64_t)binding {
    if (self.autoEpoch != epoch || !self.autoEnabled || self.sleeping || self.exitPending) return;
    if (!supported) {
        [self showAutomatic:@"自动预热需要 macOS 14.2 或更新版本，可手动预热。" menuTitle:@"仅支持手动预热"];
        return;
    }
    if (self.autoBinding != binding) {
        automatic = (MWAutoState){0}; self.autoBinding = binding;
        self.pendingAutomaticDeadline = 0;
    }
    if (reading.at && reading.at <= automatic.lastReadingAt) return;
    BOOL otherInput = reading.activity == MWInputActive && reading.route == MWRouteOther;
    if (otherInput) {
        self.pendingAutomaticDeadline = 0;
        if (self.sessionAutomatic && (phase == MWStarting || phase == MWActive || self.stoppingForRouteChange)) {
            [self stopAt:reading.at forRouteChange:YES];
        }
    }
    switch (reading.activity) {
        case MWInputInactive:
            [self showAutomatic:@"自动待命，等豆包使用内置麦克风。" menuTitle:@"自动待命"]; break;
        case MWInputActive:
            if (reading.route == MWRouteBuiltIn) {
                [self showAutomatic:@"豆包正在使用内置麦克风，下次开始输入会重新计时。" menuTitle:@"每次新输入续时"];
            } else if (reading.route == MWRouteOther) {
                [self showAutomatic:@"豆包正在使用其他输入，暂不自动预热。" menuTitle:@"豆包正在使用其他输入"];
            } else {
                [self showAutomatic:@"等待确认豆包使用的输入设备。" menuTitle:@"等待确认输入设备"];
            }
            break;
        default:
            [self showAutomatic:@"等待确认豆包的麦克风状态。" menuTitle:@"自动待命"]; break;
    }
    BOOL mayTrigger = !otherInput && (phase != MWStopping || self.stoppingForRouteChange) &&
                      reading.at >= self.idleSince;
    MWAutoDecision decision = MWAutoObserve(&automatic, reading, MWNow(), mayTrigger);
    if (decision.trigger && decision.triggerAt >= self.idleSince && decision.triggerAt &&
        decision.triggerAt <= UINT64_MAX - MW_LIMIT) {
        uint64_t expires = decision.triggerAt + MW_LIMIT;
        if (expires <= MWNow()) return;
        if (phase == MWIdle) [self startAutomatic:YES until:expires];
        // A new built-in input may arrive while the previous route's helper is stopping.
        else if (phase == MWStopping) self.pendingAutomaticDeadline = expires;
        else [self renewAutomaticUntil:expires];
    }
    else if (decision.verifyBy) [self.monitor retryBefore:decision.verifyBy epoch:epoch binding:binding];
}

- (void)presentBefore:(MWBefore)before started:(BOOL)started {
    NSString *lead = started ? @"启动前：" : @"只读检查：";
    NSString *message;
    if (before == MWBeforeRunning) message = @"硬件已在运行，额外预热是否有帮助仍需对照测试。";
    else if (before == MWBeforeStopped) message = @"未检测到内置输入硬件运行。";
    else message = @"无法判断内置输入硬件是否在运行。";
    self.probe.stringValue = [lead stringByAppendingString:message];
}

- (void)checkInput {
    if (self.checkPending || phase != MWIdle || self.exitPending) return;
    self.checkPending = YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        MWInput input = MWProbeBuiltInInput(true);
        dispatch_async(dispatch_get_main_queue(), ^{
            self.checkPending = NO;
            if (self->phase != MWIdle) return;
            if (input.selection == MWInputOK) [self presentBefore:input.before started:NO];
            else if (input.selection == MWInputNone) self.probe.stringValue = @"未找到可用的内置输入设备。";
            else if (input.selection == MWInputAmbiguous) self.probe.stringValue = @"有多个可用的内置输入设备，无法确定目标。";
            else self.probe.stringValue = @"暂时无法确认可用的内置输入设备。";
            if (self.window.visible) [self fitSettings];
        });
    });
}

- (void)start:(id)sender {
    (void)sender;
    [self startAutomatic:NO];
}

- (void)startAutomatic:(BOOL)isAutomatic {
    uint64_t now = MWNow();
    [self startAutomatic:isAutomatic until:now && now <= UINT64_MAX - MW_LIMIT ? now + MW_LIMIT : 0];
}

- (void)startAutomatic:(BOOL)isAutomatic until:(uint64_t)expires {
    uint64_t now = MWNow();
    if (isAutomatic && expires && expires <= now) return;
    if (self.exitPending || self.sleeping || !MWBegin(&phase)) return;
    self.sessionAutomatic = isAutomatic;
    if (!now || expires <= now || expires - now > MW_LIMIT) { [self finish:MWControlFailed]; return; }
    deadline = expires;
    self.requestedDeadline = expires;
    self.pendingAutomaticDeadline = 0;
    self.controlFailed = NO;
    self.controlBuffer = [NSMutableData data];
    self.problemTitle = nil;
    self.countdown = nil;
    self.status.stringValue = isAutomatic ? @"正在请求自动预热…" : @"正在请求手动预热…";
    self.probe.stringValue = @"正在读取启动前的硬件状态…";
    [self refreshPresentation];

    NSTask *task = [NSTask new];
    task.executableURL = [NSBundle.mainBundle.bundleURL URLByAppendingPathComponent:
        @"Contents/Helpers/mic-warmup-helper" isDirectory:NO];
    task.arguments = @[@"--ui-session", [NSString stringWithFormat:@"%d", getpid()],
        [NSString stringWithFormat:@"%llu", (unsigned long long)deadline]];
    NSPipe *input = [NSPipe pipe];
    NSPipe *output = [NSPipe pipe];
    task.standardInput = input;
    task.standardOutput = output;
    task.standardError = NSFileHandle.fileHandleWithNullDevice;
    self.task = task;
    self.control = input.fileHandleForWriting;

    __weak WarmupApp *owner = self;
    __weak NSTask *watched = task;
    NSFileHandle *reader = output.fileHandleForReading;
    self.helperOutput = reader;
    reader.readabilityHandler = ^(NSFileHandle *handle) {
        NSError *error = nil;
        NSData *data = [handle readDataUpToLength:1024 error:&error];
        if (error || !data.length) handle.readabilityHandler = nil;
        NSTask *current = watched;
        if (current) {
            [owner performSelectorOnMainThread:@selector(receivedControl:)
                                 withObject:@[current, data ?: NSData.data, @(error != nil)]
                                 waitUntilDone:NO modes:WarmupUIModes()];
        }
    };
    task.terminationHandler = ^(NSTask *finished) {
        // AppKit can wait in its modal run loop after NSTerminateLater.
        [owner performSelectorOnMainThread:@selector(taskFinished:) withObject:finished
                           waitUntilDone:NO modes:WarmupUIModes()];
    };
    if (![task launchAndReturnError:NULL]) {
        reader.readabilityHandler = nil;
        [self finish:MWControlFailed];
        return;
    }
    int flags = fcntl(self.control.fileDescriptor, F_GETFL);
    if (flags == -1 || fcntl(self.control.fileDescriptor, F_SETFL, flags | O_NONBLOCK) == -1) {
        self.controlFailed = YES;
        [self stop:nil];
        return;
    }
    const char request = 'S';
    [self sendControlBytes:&request length:1];
}

- (BOOL)sendControlBytes:(const void *)bytes length:(size_t)length {
    if (!self.control) return NO;
    ssize_t sent;
    do { sent = write(self.control.fileDescriptor, bytes, length); } while (sent < 0 && errno == EINTR);
    if (sent == (ssize_t)length) return YES;
    if (sent < 0 && errno == EPIPE) {
        // The helper may have expired even before it received the first byte.
        // Its exit status determines whether a newer pending edge can resume.
        [self closeControl];
    } else {
        self.controlFailed = YES; [self stop:nil];
    }
    return NO;
}

- (void)renewAutomaticUntil:(uint64_t)expires {
    uint64_t now = MWNow();
    if (!self.autoEnabled || self.sleeping || self.exitPending || !self.task ||
        (phase != MWStarting && phase != MWActive) || !now || expires <= now ||
        expires - now > MW_LIMIT || expires <= self.requestedDeadline) return;
    self.pendingAutomaticDeadline = expires;
    unsigned char request[MW_CONTROL_FRAME_SIZE];
    MWControlEncode(request, MW_CONTROL_RENEW, expires);
    if ([self sendControlBytes:request length:sizeof(request)]) {
        self.requestedDeadline = expires;
        // The visible deadline moves only when this same helper acknowledges it.
    }
}

- (void)receivedControl:(NSArray *)message {
    NSTask *task = message[0];
    NSData *data = message[1];
    if (task != self.task || (phase != MWStarting && phase != MWActive)) return;
    BOOL failed = message.count > 2 && [message[2] boolValue];
    if (failed || (!data.length && self.controlBuffer.length)) {
        self.controlFailed = YES; [self stop:nil]; return;
    }
    if (data.length > 1024 || self.controlBuffer.length + data.length > 1024) {
        self.controlFailed = YES; [self stop:nil]; return;
    }
    [self.controlBuffer appendData:data];
    while (self.controlBuffer.length) {
        const unsigned char *bytes = self.controlBuffer.bytes;
        unsigned char tag = bytes[0];
        if (tag == MW_CONTROL_ACK) {
            if (self.controlBuffer.length < MW_CONTROL_FRAME_SIZE) break;
            uint64_t confirmed = MWControlDecode(bytes);
            if (!confirmed || confirmed > self.requestedDeadline) {
                self.controlFailed = YES; [self stop:nil]; return;
            }
            if (confirmed > deadline) deadline = confirmed;
            if (self.pendingAutomaticDeadline <= confirmed) self.pendingAutomaticDeadline = 0;
            [self.controlBuffer replaceBytesInRange:NSMakeRange(0, MW_CONTROL_FRAME_SIZE) withBytes:NULL length:0];
            [self updateTime:nil];
        } else if (tag == MWBeforeRunning || tag == MWBeforeStopped || tag == MWBeforeUnknown) {
            [self.controlBuffer replaceBytesInRange:NSMakeRange(0, 1) withBytes:NULL length:0];
            [self active:(MWBefore)tag task:task];
        } else {
            self.controlFailed = YES; [self stop:nil]; return;
        }
    }
}

- (void)taskFinished:(NSTask *)task {
    if (self.task != task) return;
    MWOutcome result = task.terminationReason == NSTaskTerminationReasonExit ?
        (MWOutcome)task.terminationStatus : MWWatchdog;
    [self finish:result];
}

- (void)active:(MWBefore)before task:(NSTask *)task {
    if (!task || self.task != task || !MWMarkActive(&phase)) return;
    [self presentBefore:before started:YES];
    [self updateTime:nil];
    self.timer = [NSTimer timerWithTimeInterval:0.25 target:self selector:@selector(updateTime:)
                                     userInfo:nil repeats:YES];
    for (NSRunLoopMode mode in WarmupUIModes()) [NSRunLoop.mainRunLoop addTimer:self.timer forMode:mode];
}

- (void)updateTime:(NSTimer *)timer {
    (void)timer;
    if (phase != MWActive) return;
    uint64_t now = MWNow();
    uint64_t seconds = now && deadline > now ? (deadline - now + MW_SECOND - 1) / MW_SECOND : 0;
    NSString *countdown = [NSString stringWithFormat:@"%llu:%02llu", (unsigned long long)(seconds / 60),
                                                    (unsigned long long)(seconds % 60)];
    if ([self.countdown isEqualToString:countdown]) return;
    self.countdown = countdown;
    self.status.stringValue = [NSString stringWithFormat:@"%@预热中 · 剩余 %@",
        self.sessionAutomatic ? @"自动" : @"手动", countdown];
    [self refreshPresentation];
}

- (void)closeControl {
    [self.control closeAndReturnError:NULL];
    self.control = nil;
}

- (void)stop:(id)sender {
    (void)sender;
    [self stopAt:MWNow() forRouteChange:NO];
}

- (void)stopAt:(uint64_t)at forRouteChange:(BOOL)routeChange {
    self.stoppingForRouteChange = routeChange;
    self.pendingAutomaticDeadline = 0;
    automatic.verifyBy = automatic.edgeAt = 0;
    self.idleSince = MAX(self.idleSince, at);
    if (!MWEnd(&phase)) return;
    [self.timer invalidate];
    self.timer = nil;
    [self closeControl];
    self.status.stringValue = @"正在停止…";
    [self refreshPresentation];
}

- (void)finish:(MWOutcome)outcome {
    BOOL naturalExpiry = outcome == MWExpired && phase != MWStopping && !self.controlFailed;
    BOOL cleanRouteStop = self.stoppingForRouteChange && !self.controlFailed &&
                          (outcome == MWStopped || outcome == MWExpired || outcome == MWDeviceChanged);
    uint64_t next = self.pendingAutomaticDeadline;
    BOOL resume = (cleanRouteStop || (naturalExpiry && next > deadline)) &&
                  self.autoEnabled && !self.sleeping && !self.exitPending && next > MWNow();
    if (resume && self.monitor) {
        resume = next > MW_LIMIT && [self.monitor canResumeFrom:next - MW_LIMIT
                                                         epoch:self.autoEpoch binding:self.autoBinding];
    }
    if (outcome == MWExpired && !naturalExpiry) outcome = MWStopped;
    // Keep the helper's specific result, especially when it could not confirm a stop.
    if (self.controlFailed && outcome == MWStopped) outcome = MWControlFailed;
    [self.timer invalidate];
    self.timer = nil;
    [self closeControl];
    self.helperOutput.readabilityHandler = nil;
    [self.helperOutput closeAndReturnError:NULL];
    self.helperOutput = nil;
    self.controlBuffer = nil;
    self.pendingAutomaticDeadline = 0;
    self.stoppingForRouteChange = NO;
    self.task = nil;
    phase = MWIdle;
    // Keep later input eligible across expiry or a confirmed route switch.
    // An explicit stop or a failure invalidates readings from that session.
    if (!naturalExpiry && !cleanRouteStop) {
        self.idleSince = MWNow();
        automatic.verifyBy = automatic.edgeAt = 0;
    }
    if (self.exitPending) { [NSApp replyToApplicationShouldTerminate:YES]; return; }
    if (resume) { [self startAutomatic:YES until:next]; return; }
    self.countdown = nil;
    switch (outcome) {
        case MWStopped: case MWExpired: case MWDeviceChanged: self.problemTitle = nil; break;
        case MWStopFailed: case MWWatchdog: self.problemTitle = @"停止未确认"; break;
        case MWStartTimedOut: self.problemTitle = @"启动超时，状态未确认"; break;
        case MWStartTimedOutStopped: self.problemTitle = @"启动超时，预热已停止"; break;
        case MWSessionQueryFailed: self.problemTitle = @"设备状态无法确认"; break;
        case MWControlFailed: self.problemTitle = @"辅助进程通信异常"; break;
        default: self.problemTitle = @"未能启动预热"; break;
    }
    NSString *message;
    switch (outcome) {
        case MWStopped: message = @"本次预热已停止。"; break;
        case MWExpired: message = self.autoEnabled ? @"两分钟内没有新输入，预热已结束。" : @"本次预热到时结束。"; break;
        case MWDeviceChanged: message = @"内置输入设备发生变化，预热已关闭。"; break;
        case MWNoDevice: message = @"未预热：没有可用的内置输入设备。"; break;
        case MWMultipleDevices: message = @"未预热：无法确定唯一的内置输入设备。"; break;
        case MWQueryFailed: message = @"未预热：无法读取内置输入设备状态。"; break;
        case MWStartFailed: message = @"系统未接受预热请求。"; break;
        case MWStartTimedOut: message = @"预热启动超时，系统未确认内置麦克风状态。"; break;
        case MWStartTimedOutStopped: message = @"预热启动超时，本次预热已正常停止。"; break;
        case MWSessionQueryFailed: message = @"无法确认内置输入设备状态，本次预热已停止。"; break;
        case MWStopFailed: message = @"辅助进程已退出，系统未确认正常停止。"; break;
        case MWWatchdog: message = @"辅助进程已结束，系统未确认正常停止。"; break;
        default: message = @"无法完成预热：辅助进程或通信异常。"; break;
    }
    self.status.stringValue = message;
    self.probe.stringValue = @"正在只读检查内置输入设备…";
    [self refreshPresentation];
    [self checkInput];
}

- (void)sleepOrWake:(NSNotification *)note {
    BOOL sleeping = [note.name isEqualToString:NSWorkspaceWillSleepNotification];
    void (^handle)(void) = ^{ self.sleeping = sleeping; [self stop:nil]; [self resetAutomatic]; };
    if (NSThread.isMainThread) handle();
    else dispatch_async(dispatch_get_main_queue(), handle);
}

- (void)applicationDidBecomeActive:(NSNotification *)note { (void)note; [self checkInput]; }
- (BOOL)windowShouldClose:(NSWindow *)window { [window orderOut:nil]; return NO; }
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible {
    (void)app; (void)visible;
    [self showWindow:nil];
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app;
    self.exitPending = YES;
    [self resetAutomatic];
    if (!self.task) return NSTerminateNow;
    [self stop:nil];
    return NSTerminateLater;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--check-auto") == 0) return MWCheckDoubaoMonitor();
        }
        NSApplication *app = NSApplication.sharedApplication;
        WarmupApp *delegate __attribute__((objc_precise_lifetime)) = [WarmupApp new];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app run];
    }
    return 0;
}
