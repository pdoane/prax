// The Cocoa application backend: the application, its host's delegates, and the delivery of callbacks.

#include "pr/app_cocoa.h"
#include "app/backends/cocoa/src/app_cocoa.h"

#include "app/backends/cocoa/src/app_cocoa_data.h"
#include "app/backends/cocoa/src/app_cocoa_display.h"
#include "app/backends/cocoa/src/app_cocoa_input.h"
#include "app/backends/cocoa/src/app_cocoa_menu.h"
#include "app/backends/cocoa/src/app_cocoa_shell.h"
#include "app/backends/cocoa/src/app_cocoa_window.h"

namespace pr_impl {

namespace {

NSApplicationActivationPolicy activationPolicyOf(PrAppPresence presence) {
    if (presence == PrAppPresence_Accessory)
        return NSApplicationActivationPolicyAccessory;
    return NSApplicationActivationPolicyRegular;
}

// Delivers a PowerChanged event for a change of the power sources, on the main run loop.
void powerSourcesChanged(void* context) {
    [(__bridge PrCocoaHost*)context deliverKind:PrAppEventKind_PowerChanged];
}

void destroy(PrApp* app) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrCocoaHost* host = (__bridge_transfer PrCocoaHost*)cocoa->host;
    host.app = nullptr;
    cocoaHotkeysReset(cocoa);
    cocoaSetAwake(app, 0);
    cocoaKeyLabelsReset(cocoa);
    appDisplayListReset(cocoa->displays);
    appDataRequestsReset(cocoa->requests);
    appDataSourcesReset(cocoa->sources);
    // Only a process that has posted has touched the notification center. Any other process must not touch it, because
    // the center raises an exception outside an application bundle.
    UNUserNotificationCenter* center = nil;
    if (host.permission != CocoaPermission_Unasked)
        center = UNUserNotificationCenter.currentNotificationCenter;
    if (center != nil && center.delegate == host)
        center.delegate = nil;
    prMemoryFree(cocoa);
}

void stop(PrApp* app) {
    [cocoaHostOf(app) stop];
}

void run(PrApp* app, PrAppHandler handler) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    cocoa->handler = handler;
    @autoreleasepool {
        PrCocoaHost* host = cocoaHostOf(app);
        NSApplication* application = NSApplication.sharedApplication;
        // A second instance hands its request to the first, and its run ends without a callback.
        if (![host startServices])
            return;
        if (!host.headless) {
            [application setActivationPolicy:activationPolicyOf(host.presence)];
            if (host.menuBar == nil) {
                host.menuBar = [[PrCocoaMenuTree alloc] init];
                cocoaMenuBarOf(nullptr, host.menuBar, host, nil);
            }
            application.mainMenu = host.menuBar.root;
        }
        application.delegate = host;
        host.running = YES;
        [host beginCallback];
        if (!handler.start(handler.context, app))
            stop(app);
        [host endCallback];
        if (host.running)
            [application run];
        [host beginCallback];
        handler.shutdown(handler.context, app);
        [host endCallback];
        cocoaHotkeysReset(cocoa);
        [host stopServices];
        // The windows that the program left open close with the run.
        while (host.windows.count != 0)
            prAppWindowDestroy(host.windows.lastObject.record);
        if (application.delegate == host)
            application.delegate = nil;
    }
}

void wake(PrApp* app) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    if (cocoa->wakePending.exchange(true))
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        cocoa->wakePending.store(false);
        PrAppEvent event{};
        event.kind = PrAppEventKind_Wake;
        event.time = cocoaUptime();
        [(__bridge PrCocoaHost*)cocoa->host deliver:event to:nullptr];
    });
}

void quitCancel(PrApp* app) {
    PrCocoaHost* host = cocoaHostOf(app);
    if (host.quitDelivering)
        host.quitCancelled = YES;
}

void setHidden(PrApp* app, bool hidden) {
    if (cocoaHostOf(app).headless)
        return;
    if (hidden)
        [NSApp hide:nil];
    else
        [NSApp unhide:nil];
}

bool isHidden(const PrApp*) {
    return NSApp.isHidden;
}

void setPresence(PrApp* app, PrAppPresence presence) {
    [cocoaHostOf(app) setPresence:presence];
}

PrAppLifecycle lifecycle(const PrApp* app) {
    return [cocoaHostOf(app) lifecycle];
}

void timerSet(PrApp* app, uint64_t tag, double delay, double interval) {
    [cocoaHostOf(app) setTimer:tag delay:delay interval:interval];
}

void timerCancel(PrApp* app, uint64_t tag) {
    [cocoaHostOf(app) cancelTimer:tag];
}

// Calls the window's handler with an event. A window that the program destroyed during the call is freed.
void deliverEvent(PrAppWindow* window, const PrAppEvent& event) {
    if (!appWindowDeliverEvent(static_cast<CocoaWindow*>(window)->delivery, window, &event, cocoaFrameState))
        cocoaWindowDestroy(window);
}

PrAppWindowModel windowModel(const PrApp* app) {
    return static_cast<const CocoaApp*>(app)->windowModel;
}

} // namespace

PrCocoaHost* cocoaHostOf(const PrApp* app) {
    return (__bridge PrCocoaHost*)static_cast<const CocoaApp*>(app)->host;
}

PrCocoaWindow* cocoaObjectOf(const PrAppWindow* window) {
    return (__bridge PrCocoaWindow*)static_cast<const CocoaWindow*>(window)->object;
}

double cocoaUptime() {
    return NSProcessInfo.processInfo.systemUptime;
}

bool cocoaIsBundled() {
    NSBundle* bundle = NSBundle.mainBundle;
    return bundle.bundleIdentifier != nil && [bundle.bundleURL.pathExtension isEqualToString:@"app"];
}

void cocoaDeliverFrame(PrAppWindow* window, const PrAppFrame& frame) {
    PrAppOutput output{};
    if (!appWindowDeliverFrame(static_cast<CocoaWindow*>(window)->delivery, window, &frame, &output)) {
        cocoaWindowDestroy(window);
        return;
    }
    cocoaSetCursor(window, output.cursor);
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(window);
    // The text input applies to the window that the output names, which may be another than the one before.
    PrAppWindow* before = cocoa->textInputWindow;
    PrAppWindow* named = output.textInputWindow != nullptr ? output.textInputWindow : window;
    cocoa->textInputWindow = output.hasTextInput ? named : nullptr;
    cocoa->textInputKind = output.textInputKind;
    cocoa->textInput = output.textInput;
    cocoaApplyTextInput(window);
    if (before != nullptr && before != window)
        cocoaApplyTextInput(before);
    if (cocoa->textInputWindow != nullptr && cocoa->textInputWindow != window && cocoa->textInputWindow != before)
        cocoaApplyTextInput(cocoa->textInputWindow);
    appTitleBarRegionsSet(cocoa->titleBar, output);
    cocoa->dropOperation = output.dropOperation;
    if (output.wantsFrame)
        cocoaRequestFrame(window);
    cocoaRequestFrameAt(window, output.frameTime);
}

} // namespace pr_impl

using namespace pr_impl;

@implementation PrCocoaHost {
    // The events that arrived during the running callback, in order. A callback that runs a run loop of its own, as a
    // modal panel does, has events delivered within it, and they wait here, so that no callback nests in another.
    AppEventQueue _held;
    BOOL _holding;              // events wait in `_held` while a key's input method runs (holdEvents)

    PrAppLifecycle _lifecycle;                         // the lifecycle that the last LifecycleChanged event reported
    NSMutableDictionary<NSNumber*, NSTimer*>* _timers; // the program's timers, by tag

    // The observers of the system's notifications, as pairs of a center and the token that it returned.
    NSMutableArray<NSArray*>* _observers;
    CFRunLoopSourceRef _powerSource;    // the notification of changes to the power sources, or null
    dispatch_source_t _memory;          // the notification of memory pressure, or nil

    // The pasteboard has no notification of its changes, so the host polls its change count.
    NSTimer* _pasteboardPoll;
    NSInteger _pasteboardChanges;

    int _instanceLock;          // the lock file that a single instance holds, or -1
    id _pressMonitor;           // the monitor of presses, which close the popups that they miss
}
- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _windows = [NSMutableArray array];
        _shares = [NSMutableArray array];
        _instanceLock = -1;
    }
    return self;
}
- (void)dealloc {
    appEventQueueReset(_held, [self sources]);
}
// The application's sources, or null once the application is gone.
- (AppDataSources*)sources {
    return self.app != nullptr ? &static_cast<CocoaApp*>(self.app)->sources : nullptr;
}
// Calls the program with an event, as no other callback runs. The run stops after a QuitRequested event that the
// program did not cancel.
- (void)call:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window {
    self.inCallback = YES;
    BOOL quit = event.kind == PrAppEventKind_QuitRequested;
    self.quitDelivering = quit;
    self.quitCancelled = NO;
    if (window != nullptr) {
        deliverEvent(window, event);
    } else {
        const PrAppHandler& handler = static_cast<CocoaApp*>(self.app)->handler;
        handler.event(handler.context, self.app, &event);
    }
    self.quitDelivering = NO;
    self.inCallback = NO;
    if (quit && !self.quitCancelled)
        [self stop];
}
- (void)beginCallback {
    self.inCallback = YES;
}
// Ends a callback, and delivers the events that arrived during it, and those that arrive during their own delivery, in
// order. The events of a run that stopped are dropped, and so are those of a window that closed (forget:).
- (void)endCallback {
    self.inCallback = NO;
    uint32_t at = 0;
    PrAppWindow* window = nullptr;
    PrAppEvent event{};
    AppAnswer* answer = nullptr;
    while (self.running && appEventQueueNext(_held, &at, &window, &event, &answer)) {
        if (answer != nullptr)
            [self call:*answer];
        else
            [self call:event to:window];
    }
    appEventQueueClear(_held, [self sources]);
}
// Calls the callback of an answer's request. No other callback may be running.
- (void)call:(AppAnswer&)answer {
    CocoaApp* app = static_cast<CocoaApp*>(self.app);
    self.inCallback = YES;
    appAnswerCall(answer, app->requests, app->sources);
    self.inCallback = NO;
}
- (void)answer:(void*)stored {
    AppAnswer* answer = static_cast<AppAnswer*>(stored);
    if (!self.running) {
        appAnswerDestroy(answer, [self sources]);
        return;
    }
    if (self.inCallback || _holding) {
        appEventQueueHoldAnswer(_held, answer);
        return;
    }
    [self call:*answer];
    appAnswerDestroy(answer, [self sources]);
    [self endCallback];
}
// Delivers an event now, or holds it if a callback is running or holdEvents is in effect. Held events are delivered in
// order when the callback returns or the hold ends. An event that hands over a source releases its reference once
// delivered.
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window {
    if (self.running && (self.inCallback || _holding)) {
        appEventQueueHold(_held, window, event);
        return;
    }
    if (self.running)
        [self call:event to:window];
    PrAppDataSource source = prAppEventSource(&event);
    if (source != kPrAppDataSourceNone && [self sources] != nullptr)
        appDataSourceRelease(*[self sources], source);
    if (self.running)
        [self endCallback];
}
// Holds the events that arrive from now on, and returns the mark of the first of them, for deliver:to:ahead:.
- (uint32_t)holdEvents {
    PR_ASSERT(!_holding);
    _holding = YES;
    return _held.held.count;
}
// Ends holdEvents, and delivers `event` ahead of the events that arrived since `mark`, and then those events.
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow*)window ahead:(uint32_t)mark {
    _holding = NO;
    if (!self.running) {
        appEventQueueDiscard(_held, mark, [self sources]);
        return;
    }
    appEventQueueHold(_held, window, event, mark);
    if (!self.inCallback)
        [self endCallback];
}
// Drops the held events of a window that is closing, and the window.
- (void)forget:(PrAppWindow*)window {
    appEventQueueForget(_held, window);
    for (PrCocoaWindow* object in self.windows) {
        if (object.record == window) {
            [self.windows removeObject:object];
            break;
        }
    }
}
- (void)stop {
    if (!self.running)
        return;
    self.running = NO;
    for (PrCocoaWindow* window in self.windows)
        [window stopFrames];
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
}
// Posts a notification once the user allows notifications. The first post asks for permission.
- (PrStatus)postNotification:(const PrAppNotificationDesc*)desc {
    if (self.permission == CocoaPermission_Refused)
        return PrStatus_NotAllowed;
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = cocoaStringOf(desc->title);
    content.body = cocoaStringOf(desc->body);
    content.userInfo = @{kNotificationTagKey: @(desc->tag)};
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:NSUUID.UUID.UUIDString content:content
        trigger:nil];
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    if (self.permission == CocoaPermission_Granted) {
        [center addNotificationRequest:request withCompletionHandler:nil];
        return PrStatus_Ok;
    }
    if (self.waiting == nil)
        self.waiting = [NSMutableArray array];
    if (self.waiting.count == kWaitingNotificationsMax)
        [self.waiting removeObjectAtIndex:0];
    [self.waiting addObject:request];
    if (self.permission == CocoaPermission_Unasked) {
        self.permission = CocoaPermission_Asking;
        center.delegate = self;
        __weak PrCocoaHost* weak = self;
        [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert completionHandler:^(BOOL granted, NSError*) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weak answered:granted];
            });
        }];
    }
    return PrStatus_Ok;
}
// Records the user's answer about notifications. If the user allowed them, posts the notifications that waited.
- (void)answered:(BOOL)granted {
    self.permission = granted ? CocoaPermission_Granted : CocoaPermission_Refused;
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    if (granted) {
        for (UNNotificationRequest* request in self.waiting)
            [center addNotificationRequest:request withCompletionHandler:nil];
    }
    [self.waiting removeAllObjects];
}
// Activates the application for a click on a notification, and delivers the click.
- (void)notificationClicked:(uint64_t)tag {
    if (!self.running)
        return;
    [NSApp activate];
    PrAppEvent event{};
    event.kind = PrAppEventKind_Notification;
    event.tag = tag;
    event.time = cocoaUptime();
    [self deliver:event to:nullptr];
}
// Shows a notification even while the application is active, because the program chose to post it.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center willPresentNotification:(UNNotification*)notification
        withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
        didReceiveNotificationResponse:(UNNotificationResponse*)response
        withCompletionHandler:(void (^)(void))completionHandler {
    (void)center;
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) {
        id tag = response.notification.request.content.userInfo[kNotificationTagKey];
        uint64_t value = [tag isKindOfClass:NSNumber.class] ? [tag unsignedLongLongValue] : 0;
        __weak PrCocoaHost* weak = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weak notificationClicked:value];
        });
    }
    completionHandler();
}
- (void)applicationDidChangeScreenParameters:(NSNotification*)note {
    (void)note;
    // The names that the last event's displays had stay valid until this event, which may change them.
    if (self.app != nullptr)
        appDisplayListReset(static_cast<CocoaApp*>(self.app)->displays);
    PrAppEvent event{};
    event.kind = PrAppEventKind_DisplaysChanged;
    event.time = cocoaUptime();
    [self deliver:event to:nullptr];
    for (PrCocoaWindow* window in self.windows)
        [window requestFrame];
}
- (void)applicationDidBecomeActive:(NSNotification*)note {
    (void)note;
    for (PrCocoaWindow* window in self.windows)
        [window requestFrame];
    [self lifecycleMayChange];
}
- (void)applicationDidChangeOcclusionState:(NSNotification*)note {
    (void)note;
    [self lifecycleMayChange];
}
- (void)applicationDidResignActive:(NSNotification*)note {
    (void)note;
    for (PrCocoaWindow* window in self.windows) {
        if (!window.overlay)
            [window freePointer];
        [window requestFrame];
    }
    [self closePopupsExcept:nil];
    [self lifecycleMayChange];
}
- (void)applicationDidHide:(NSNotification*)note {
    (void)note;
    for (PrCocoaWindow* window in self.windows)
        [window requestFrame];
    [self lifecycleMayChange];
}
- (void)applicationDidUnhide:(NSNotification*)note {
    (void)note;
    for (PrCocoaWindow* window in self.windows)
        [window requestFrame];
    [self lifecycleMayChange];
}
// Delivers the press of the hotkey whose Carbon id is `id`.
- (void)deliverHotkey:(uint32_t)id {
    if (self.app == nullptr || !self.running)
        return;
    const RawArray<CocoaHotkey>& hotkeys = static_cast<CocoaApp*>(self.app)->hotkeys;
    for (uint32_t i = 0; i < hotkeys.count; ++i) {
        if (hotkeys.data[i].id != id)
            continue;
        PrAppEvent event{};
        event.kind = PrAppEventKind_Hotkey;
        event.time = cocoaUptime();
        event.modifiers = cocoaModifiersOf(NSEvent.modifierFlags);
        event.tag = hotkeys.data[i].tag;
        [self deliver:event to:nullptr];
        return;
    }
}
// A request to quit delivers QuitRequested, after which the run stops itself unless the program cancels the quit. So
// AppKit's request is canceled either way.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)application {
    (void)application;
    PrAppEvent event{};
    event.kind = PrAppEventKind_QuitRequested;
    event.time = cocoaUptime();
    NSAppleEventDescriptor* request = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
    OSType reason = [request attributeDescriptorForKeyword:kAEQuitReason].enumCodeValue;
    event.quit.sessionEnding = reason == kAELogOut || reason == kAEReallyLogOut || reason == kAEShowRestartDialog ||
        reason == kAEShowShutdownDialog || reason == kAERestart || reason == kAEShutDown;
    [self deliver:event to:nullptr];
    return NSTerminateCancel;
}
// Shows the menu bar of the key window, if it has its own, or else the application's.
- (void)showMenuBar {
    if (self.headless || !self.running)
        return;
    NSMenu* bar = self.menuBar.root;
    for (PrCocoaWindow* object in self.windows) {
        if (object.window.isKeyWindow && object.menuBar != nil)
            bar = object.menuBar.root;
    }
    if (bar != nil && NSApp.mainMenu != bar)
        NSApp.mainMenu = bar;
}
// Asks each popup to close, but the one that shows `window`, as for a press elsewhere or the application's
// deactivation.
- (void)closePopupsExcept:(NSWindow* PR_NULLABLE)window {
    for (PrCocoaWindow* object in [self.windows copy]) {
        if (object.role != PrAppWindowRole_Popup || object.window == window || !object.window.isVisible)
            continue;
        if (object.record == nullptr)
            continue;
        PrAppEvent event{};
        event.kind = PrAppEventKind_CloseRequested;
        event.time = cocoaUptime();
        [self deliver:event to:object.record];
    }
}
// Shows a progress bar over the application's icon in the Dock, or the icon alone for None.
- (void)setDockProgress:(PrAppProgressState)state value:(float)value {
    NSDockTile* tile = NSApp.dockTile;
    if (state == PrAppProgressState_None) {
        tile.contentView = nil;
        [tile display];
        return;
    }
    PrCocoaDockProgress* progress = nil;
    if ([tile.contentView isKindOfClass:PrCocoaDockProgress.class])
        progress = static_cast<PrCocoaDockProgress*>(tile.contentView);
    if (progress == nil) {
        progress = [[PrCocoaDockProgress alloc] initWithFrame:NSMakeRect(0, 0, tile.size.width, tile.size.height)];
        tile.contentView = progress;
    }
    progress.state = state;
    progress.value = value;
    [tile display];
}
// Delivers an application event of `kind` that carries no details.
- (void)deliverKind:(PrAppEventKind)kind {
    if (!self.running)
        return;
    PrAppEvent event{};
    event.kind = kind;
    event.time = cocoaUptime();
    [self deliver:event to:nullptr];
}
- (void)setPresence:(PrAppPresence)presence {
    _presence = presence;
    if (self.running && !self.headless)
        [NSApp setActivationPolicy:activationPolicyOf(presence)];
}
- (PrAppLifecycle)computedLifecycle {
    if (self.headless)
        return PrAppLifecycle_Background;
    if (NSApp.isActive)
        return PrAppLifecycle_Active;
    if (NSApp.isHidden || (NSApp.occlusionState & NSApplicationOcclusionStateVisible) == 0)
        return PrAppLifecycle_Background;
    return PrAppLifecycle_Inactive;
}
- (PrAppLifecycle)lifecycle {
    return self.running ? _lifecycle : [self computedLifecycle];
}
- (void)lifecycleMayChange {
    PrAppLifecycle now = [self computedLifecycle];
    if (now == _lifecycle)
        return;
    _lifecycle = now;
    [self deliverKind:PrAppEventKind_LifecycleChanged];
}
// Timers fire in the common modes, so that they keep firing during a live resize or while a menu tracks.
- (NSTimer*)timerFor:(uint64_t)tag after:(double)seconds repeats:(BOOL)repeats interval:(double)interval {
    __weak PrCocoaHost* weak = self;
    NSTimer* timer = [NSTimer timerWithTimeInterval:MAX(seconds, 0.0) repeats:repeats block:^(NSTimer*) {
        [weak timerFired:tag repeating:repeats interval:interval];
    }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    return timer;
}
- (void)setTimer:(uint64_t)tag delay:(double)delay interval:(double)interval {
    [self cancelTimer:tag];
    if (_timers == nil)
        _timers = [NSMutableDictionary dictionary];
    _timers[@(tag)] = [self timerFor:tag after:delay repeats:NO interval:interval];
}
- (void)cancelTimer:(uint64_t)tag {
    [_timers[@(tag)] invalidate];
    [_timers removeObjectForKey:@(tag)];
}
// Delivers a timer's event. The first firing of a repeating timer replaces it with one that repeats at its interval,
// before the delivery, so that the program may set or cancel the tag during the event.
- (void)timerFired:(uint64_t)tag repeating:(BOOL)repeating interval:(double)interval {
    if (!repeating) {
        [_timers removeObjectForKey:@(tag)];
        if (interval > 0)
            _timers[@(tag)] = [self timerFor:tag after:interval repeats:YES interval:interval];
    }
    if (!self.running)
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_Timer;
    event.time = cocoaUptime();
    event.tag = tag;
    [self deliver:event to:nullptr];
}
// Delivers an event of `kind` for each notification `name` of `center`.
- (void)observe:(NSNotificationCenter*)center name:(NSString*)name kind:(PrAppEventKind)kind {
    __weak PrCocoaHost* weak = self;
    id token = [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
        PrCocoaHost* host = weak;
        if (host == nil || host.app == nullptr)
            return;
        if (kind == PrAppEventKind_KeyboardLayoutChanged)
            cocoaKeyLabelsReset(static_cast<CocoaApp*>(host.app));
        [host deliverKind:kind];
    }];
    [_observers addObject:@[ center, token ]];
}
// Takes the single instance's lock, or hands this launch's arguments to the instance that holds it and returns NO.
- (BOOL)claimSingleInstance {
    NSString* name = [@"org.prax.app.instance." stringByAppendingString:self.singleInstance];
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:[name stringByAppendingString:@".lock"]];
    int lock = open(path.fileSystemRepresentation, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    NSDistributedNotificationCenter* center = NSDistributedNotificationCenter.defaultCenter;
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0) {
        _instanceLock = lock;
        __weak PrCocoaHost* weak = self;
        id token = [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification* note) {
                [weak openArguments:note.userInfo[@"arguments"]];
            }];
        [_observers addObject:@[ center, token ]];
        return YES;
    }
    if (lock >= 0)
        close(lock);
    NSArray<NSString*>* arguments = NSProcessInfo.processInfo.arguments;
    NSArray<NSString*>* rest = @[];
    if (arguments.count > 1)
        rest = [arguments subarrayWithRange:NSMakeRange(1, arguments.count - 1)];
    [center postNotificationName:name object:nil userInfo:@{@"arguments": rest} deliverImmediately:YES];
    return NO;
}
// Delivers the request of a second launch, with its arguments, and brings the application forward.
- (void)openArguments:(id)arguments {
    if (!self.running || ![arguments isKindOfClass:NSArray.class])
        return;
    RawArray<PrRawString> texts;
    for (id argument in (NSArray*)arguments) {
        if ([argument isKindOfClass:NSString.class])
            texts.push(prAppleNSStringToRawString((NSString*)argument));
    }
    RawArray<PrStringView> views;
    for (uint32_t i = 0; i < texts.count; ++i)
        views.push(prRawStringView(&texts.data[i]));
    if (!self.headless)
        [NSApp activate];
    PrAppEvent event{};
    event.kind = PrAppEventKind_Open;
    event.time = cocoaUptime();
    event.open.arguments = views.data;
    event.open.argumentCount = views.count;
    [self deliver:event to:nullptr];
    views.reset();
    for (uint32_t i = 0; i < texts.count; ++i)
        prRawStringReset(&texts.data[i]);
    texts.reset();
}
- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    (void)application;
    if (self.app == nullptr)
        return;
    AppDataStore* store = allocZeroed<AppDataStore>();
    for (NSURL* url in urls) {
        if (url.isFileURL)
            cocoaAddFile(url, *store);
        else {
            PrRawString utf8 = prAppleNSStringToRawString(url.absoluteString);
            appDataStoreAddUrl(*store, prRawStringView(&utf8));
            prRawStringReset(&utf8);
        }
    }
    PrAppEvent event{};
    event.kind = PrAppEventKind_Open;
    event.time = cocoaUptime();
    event.open.source = appDataSourceCreate(static_cast<CocoaApp*>(self.app)->sources, store, nullptr);
    [self deliver:event to:nullptr];
}
- (BOOL)startServices {
    _observers = [NSMutableArray array];
    if (self.singleInstance != nil && ![self claimSingleInstance])
        return NO;
    _lifecycle = [self computedLifecycle];
    NSNotificationCenter* local = NSNotificationCenter.defaultCenter;
    NSNotificationCenter* workspace = NSWorkspace.sharedWorkspace.notificationCenter;
    NSNotificationCenter* distributed = NSDistributedNotificationCenter.defaultCenter;
    [self observe:local name:NSCurrentLocaleDidChangeNotification kind:PrAppEventKind_LocaleChanged];
    [self observe:local name:NSProcessInfoPowerStateDidChangeNotification kind:PrAppEventKind_PowerChanged];
    [self observe:workspace name:NSWorkspaceWillSleepNotification kind:PrAppEventKind_Suspend];
    [self observe:workspace name:NSWorkspaceDidWakeNotification kind:PrAppEventKind_Resume];
    [self observe:distributed name:@"com.apple.screenIsLocked" kind:PrAppEventKind_ScreenLocked];
    [self observe:distributed name:@"com.apple.screenIsUnlocked" kind:PrAppEventKind_ScreenUnlocked];
    [self observe:distributed name:(__bridge NSString*)kTISNotifySelectedKeyboardInputSourceChanged
        kind:PrAppEventKind_KeyboardLayoutChanged];
    _powerSource = IOPSNotificationCreateRunLoopSource(powerSourcesChanged, (__bridge void*)self);
    if (_powerSource != nullptr)
        CFRunLoopAddSource(CFRunLoopGetMain(), _powerSource, kCFRunLoopCommonModes);
    _memory = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
        DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, dispatch_get_main_queue());
    __weak PrCocoaHost* weak = self;
    dispatch_source_set_event_handler(_memory, ^{
        [weak deliverKind:PrAppEventKind_LowMemory];
    });
    dispatch_resume(_memory);
    _pasteboardChanges = NSPasteboard.generalPasteboard.changeCount;
    _pasteboardPoll = [NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) {
        [weak pollPasteboard];
    }];
    [NSRunLoop.mainRunLoop addTimer:_pasteboardPoll forMode:NSRunLoopCommonModes];
    NSEventMask presses = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown;
    _pressMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:presses handler:^NSEvent*(NSEvent* press) {
        [weak closePopupsExcept:press.window];
        return press;
    }];
    return YES;
}
- (void)pollPasteboard {
    NSInteger changes = NSPasteboard.generalPasteboard.changeCount;
    if (changes == _pasteboardChanges)
        return;
    _pasteboardChanges = changes;
    [self deliverKind:PrAppEventKind_ClipboardChanged];
}
- (void)stopServices {
    for (NSTimer* timer in _timers.allValues)
        [timer invalidate];
    [_timers removeAllObjects];
    for (NSArray* observer in _observers)
        [(NSNotificationCenter*)observer[0] removeObserver:observer[1]];
    [_observers removeAllObjects];
    if (_powerSource != nullptr) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), _powerSource, kCFRunLoopCommonModes);
        CFRelease(_powerSource);
        _powerSource = nullptr;
    }
    if (_memory != nil)
        dispatch_source_cancel(_memory);
    _memory = nil;
    [_pasteboardPoll invalidate];
    _pasteboardPoll = nil;
    if (_pressMonitor != nil)
        [NSEvent removeMonitor:_pressMonitor];
    _pressMonitor = nil;
    if (_instanceLock >= 0)
        close(_instanceLock);
    _instanceLock = -1;
}
@end

PrApp* prAppCocoaCreate(const PrAppOptions* options) {
    CocoaApp* app = allocZeroed<CocoaApp>();
    app->destroyFn = destroy;
    app->runFn = run;
    app->stopFn = stop;
    app->wakeFn = wake;
    app->notificationPostFn = cocoaNotificationPost;
    app->openUrlFn = cocoaOpenUrl;
    app->cancelFn = cocoaDataCancel;
    app->quitCancelFn = quitCancel;
    app->dataRetainFn = cocoaDataRetain;
    app->dataReleaseFn = cocoaDataRelease;
    app->dataReadFn = cocoaDataRead;
    app->dataWriteFn = cocoaDataWrite;
    app->dataWriteFileFn = cocoaDataWriteFile;
    app->windowCreateFn = cocoaWindowCreate;
    app->displaysFn = cocoaDisplays;
    app->displayInfoFn = cocoaDisplayInfo;
    app->pointerPositionFn = cocoaPointerPosition;
    app->setHiddenFn = setHidden;
    app->isHiddenFn = isHidden;
    app->revealPathFn = cocoaRevealPath;
    app->hotkeyRegisterFn = cocoaHotkeyRegister;
    app->hotkeyUnregisterFn = cocoaHotkeyUnregister;
    app->fileDialogFn = cocoaFileDialog;
    app->setPresenceFn = setPresence;
    app->windowModelFn = windowModel;
    app->lifecycleFn = lifecycle;
    app->timerSetFn = timerSet;
    app->timerCancelFn = timerCancel;
    app->keyCodeLabelFn = cocoaKeyCodeLabel;
    app->trashPathFn = cocoaTrashPath;
    app->recentDocumentAddFn = cocoaRecentDocumentAdd;
    app->recentDocumentsClearFn = cocoaRecentDocumentsClear;
    app->setBadgeFn = cocoaSetBadge;
    app->powerFn = cocoaPower;
    app->setAwakeFn = cocoaSetAwake;
    app->hapticPlayFn = cocoaHapticPlay;
    app->setMenuBarFn = cocoaSetMenuBar;
    app->statusItemCreateFn = cocoaStatusItemCreate;
    app->displayAwake = kIOPMNullAssertionID;
    app->systemAwake = kIOPMNullAssertionID;
    appDataSourcesInit(app->sources);
    appDataRequestsInit(app->requests, app->sources);
    PrCocoaHost* host = [[PrCocoaHost alloc] init];
    host.app = app;
    host.headless = options->headless;
    host.presence = options->presence;
    app->windowModel = options->windowModel;
    if (options->singleInstance.size != 0)
        host.singleInstance = cocoaStringOf(options->singleInstance);
    app->host = (__bridge_retained void*)host;
    return app;
}
