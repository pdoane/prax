// The UIKit application backend: UIKit's scenes and windows, and the main run loop.

#include "app/backends/uikit/src/app_uikit.h"

#include "pr/alloc.h"
#include "pr/apple_string.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "app/backends/uikit/src/uikit_data.h"
#include "app/backends/uikit/src/uikit_device.h"
#include "app/backends/uikit/src/uikit_immersive.h"

#import <QuartzCore/QuartzCore.h>

// The delegate of the application, which starts the run once UIKit has launched, and gives each scene its delegate.
@interface PrUiKitAppDelegate : UIResponder <UIApplicationDelegate>
@end

// The delegate of each of the application's scenes.
@interface PrUiKitSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

namespace pr_impl {

PrUiKitHost* uiKitHostOf(const PrApp* app) {
    return (__bridge PrUiKitHost*)static_cast<const UiKitApp*>(app)->host;
}

PrUiKitWindow* uiKitObjectOf(const PrAppWindow* window) {
    return (__bridge PrUiKitWindow*)static_cast<const UiKitWindow*>(window)->object;
}

double uiKitUptime() {
    return NSProcessInfo.processInfo.systemUptime;
}

NSString* uiKitStringOf(PrStringView text) {
    NSString* string = prAppleNSString(text);
    return string != nil ? string : @"";
}

PrRawString uiKitTextOf(NSString* PR_NULLABLE string) {
    return prAppleNSStringToRawString(string != nil ? string : @"");
}

uint32_t uiKitUtf8Length(NSString* string, NSUInteger units) {
    uint32_t size = prAppleNSStringToUtf8([string substringToIndex:MIN(units, string.length)], nullptr, 0);
    return size != kPrAppleStringInvalid ? size : 0;
}

PrAppModifierFlags uiKitModifiersOf(UIKeyModifierFlags flags) {
    PrAppModifierFlags modifiers = 0;
    if ((flags & UIKeyModifierShift) != 0)
        modifiers |= PrAppModifierFlag_Shift;
    if ((flags & UIKeyModifierControl) != 0)
        modifiers |= PrAppModifierFlag_Control;
    if ((flags & UIKeyModifierAlternate) != 0)
        modifiers |= PrAppModifierFlag_Alt;
    if ((flags & UIKeyModifierCommand) != 0)
        modifiers |= PrAppModifierFlag_Super;
    if ((flags & UIKeyModifierAlphaShift) != 0)
        modifiers |= PrAppModifierFlag_CapsLock;
    return modifiers;
}

#if !TARGET_OS_VISION

UIScreen* PR_NULLABLE uiKitScreenOf(PrUiKitHost* host, UIView* PR_NULLABLE view) {
    UIWindowScene* scene = view.window.windowScene;
    if (scene == nil)
        scene = [host anyScene];
    return scene.screen;
}

#endif

namespace {

// The host of the run that UIKit drives, which its delegates reach, since UIKit makes them.
PrUiKitHost* runningHost;

// An event or an answer that arrived during a callback. The host delivers it after the callback returns. It owns a copy
// of its text, because the sender's text does not outlive the call.
struct HeldEvent {
    PrAppWindow* PR_NULLABLE window;    // the target window, or null for the application
    PrAppEvent event;
    RawArray<char> text;
    bool dropped;                       // true if the target window closed before delivery
    UiKitAnswer* PR_NULLABLE answer;    // an answer in place of an event
};

HeldEvent heldOf(PrAppWindow* PR_NULLABLE window, const PrAppEvent& event) {
    HeldEvent held{.window = window, .event = event};
    PrStringView text = prAppEventText(&event);
    if (text.size != 0)
        held.text.append(text.data, uint32_t(text.size));
    return held;
}

// Returns the source that `event` hands over, whose reference the event holds until its delivery ends, or None.
PrAppDataSource sourceOf(const PrAppEvent& event) {
    if (event.kind == PrAppEventKind_Drop)
        return event.drag.drop;
    if (event.kind == PrAppEventKind_Open)
        return event.open.source;
    return kPrAppDataSourceNone;
}

void heldReset(HeldEvent& held, AppDataSources* PR_NULLABLE sources) {
    held.text.reset();
    uiKitAnswerDestroy(held.answer);
    held.answer = nullptr;
    PrAppDataSource source = sourceOf(held.event);
    if (sources != nullptr && source != kPrAppDataSourceNone)
        appDataSourceRelease(*sources, source);
}

// ---------------------------------------------------------------------------------------------------------------------
// Displays

#if !TARGET_OS_VISION

// Takes the extended range of the screen into `display`. A television's layers show no extended range, so its display
// is SDR.
void takeHeadroom(PrAppDisplay& display, UIScreen* screen) {
#if TARGET_OS_TV
    (void)display;
    (void)screen;
#else
    display.headroom = MAX(float(screen.currentEDRHeadroom), 1.0f);
    display.potentialHeadroom = MAX(float(screen.potentialEDRHeadroom), display.headroom);
#endif
}

#endif

// Returns the properties of the display that shows `view`, with the view's own scale. visionOS has no screens, and its
// windows report SDR.
PrAppDisplay displayOf(PrUiKitHost* host, UIView* view) {
    PrAppDisplay display = prAppDisplayDefault();
#if TARGET_OS_VISION
    (void)host;
    UITraitCollection* traits = view.traitCollection;
#else
    UIScreen* screen = uiKitScreenOf(host, view);
    UITraitCollection* traits = view.window != nil ? view.traitCollection : screen.traitCollection;
#endif
    if (traits.displayScale > 0)
        display.pixelRatio = float(traits.displayScale);
    if (traits.displayGamut == UIDisplayGamutP3)
        display.gamut = PrColorSpace_DisplayP3;
#if !TARGET_OS_VISION
    if (screen != nil)
        takeHeadroom(display, screen);
#endif
    return display;
}

#if !TARGET_OS_TV

// The orientations of the interface, by PrAppOrientation. The natural orientation of a phone's display is portrait, and
// Landscape is the display turned a quarter counterclockwise, with the interface's top at the device's right. A
// television's display does not turn.
constexpr UIInterfaceOrientation kInterfaceOrientations[] = {
    UIInterfaceOrientationLandscapeLeft,
    UIInterfaceOrientationPortrait,
    UIInterfaceOrientationLandscapeRight,
    UIInterfaceOrientationPortraitUpsideDown,
};

#if TARGET_OS_IOS
// Only iOS reports the orientation of a display, which visionOS lacks.
PrAppOrientation orientationOf(UIInterfaceOrientation orientation) {
    for (uint8_t i = 0; i < countOf(kInterfaceOrientations); ++i) {
        if (kInterfaceOrientations[i] == orientation)
            return PrAppOrientation(i);
    }
    return PrAppOrientation_Portrait;
}
#endif

UIInterfaceOrientationMask maskOf(PrAppOrientationFlags flags) {
    UIInterfaceOrientationMask mask = 0;
    for (uint8_t i = 0; i < countOf(kInterfaceOrientations); ++i) {
        if ((flags & (1u << i)) != 0)
            mask |= UIInterfaceOrientationMask(1u << kInterfaceOrientations[i]);
    }
    return mask != 0 ? mask : UIInterfaceOrientationMaskAll;
}

#endif

// visionOS has no displays: its windows are in the user's space.
#if !TARGET_OS_VISION

// The name of the device's screen, which stays valid for the run.
constexpr PrStringView kScreenName = "Built-in Display";

bool screenInfo(PrUiKitHost* host, PrAppDisplayInfo* out) {
    UIWindowScene* scene = [host anyScene];
    UIScreen* screen = scene.screen;
    if (screen == nil)
        return false;
    PrUiKitWindow* first = host.windows.firstObject;
    CGRect bounds = screen.bounds;
    UIEdgeInsets safe = first.window != nil ? first.window.safeAreaInsets : UIEdgeInsetsZero;
    *out = PrAppDisplayInfo{
        .id = kUiKitScreenId,
        .name = kScreenName,
        .display = first != nil ? displayOf(host, first.view) : prAppDisplayDefault(),
        .extent = PrVec2{float(bounds.size.width), float(bounds.size.height)},
        .workArea = PrRect{
            float(safe.left), float(safe.top), float(bounds.size.width - safe.right),
            float(bounds.size.height - safe.bottom),
        },
        .refreshRate = float(screen.maximumFramesPerSecond),
#if TARGET_OS_TV
        .orientation = PrAppOrientation_Landscape,
#else
        .orientation = orientationOf(scene.effectiveGeometry.interfaceOrientation),
#endif
        .main = true,
    };
    if (first == nil) {
        out->display.pixelRatio = float(screen.scale);
        takeHeadroom(out->display, screen);
    }
    return true;
}

#endif

uint32_t displays(const PrApp* app, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity) {
#if TARGET_OS_VISION
    (void)app;
    (void)out;
    (void)capacity;
    return 0;
#else
    PrAppDisplayInfo info{};
    if (!screenInfo(uiKitHostOf(app), &info))
        return 0;
    if (capacity != 0)
        out[0] = info;
    return 1;
#endif
}

bool displayInfo(const PrApp* app, PrAppDisplayId id, PrAppDisplayInfo* out) {
#if TARGET_OS_VISION
    (void)app;
    (void)id;
    (void)out;
    return false;
#else
    return id == kUiKitScreenId && screenInfo(uiKitHostOf(app), out);
#endif
}

#if !TARGET_OS_TV

void setOrientations(PrApp* app, PrAppOrientationFlags orientations) {
    PrUiKitHost* host = uiKitHostOf(app);
    host.orientations = orientations;
    for (PrUiKitWindow* window in host.windows)
        [window.controller setNeedsUpdateOfSupportedInterfaceOrientations];
}

#endif

// ---------------------------------------------------------------------------------------------------------------------
// The application's functions

void destroy(PrApp* app) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    PrUiKitHost* host = (__bridge_transfer PrUiKitHost*)uiKit->host;
    host.app = nullptr;
    uiKitDeviceDestroy(app);
    appDataRequestsReset(uiKit->requests);
    appDataSourcesReset(uiKit->sources);
    prMemoryFree(uiKit);
}

void stop(PrApp* app) {
    [uiKitHostOf(app) stop];
}

void run(PrApp* app, PrAppHandler handler) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    uiKit->handler = handler;
    PrUiKitHost* host = uiKitHostOf(app);
    host.running = YES;
    if (!host.headless) {
        // UIKit runs the process from here, and the run ends with the process (stop).
        runningHost = host;
        char* arguments[] = {nullptr};
        UIApplicationMain(0, arguments, nil, NSStringFromClass(PrUiKitAppDelegate.class));
        return;
    }
    @autoreleasepool {
        [host startServices];
        [host beginCallback];
        if (!handler.start(handler.context, app))
            stop(app);
        [host endCallback];
        if (host.running)
            CFRunLoopRun();
        [host beginCallback];
        handler.shutdown(handler.context, app);
        [host endCallback];
        [host stopServices];
        // The windows that the program left open close with the run.
        while (host.windows.count != 0)
            prAppWindowDestroy(host.windows.lastObject.record);
    }
}

void wake(PrApp* app) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    if (uiKit->wakePending.exchange(true))
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        uiKit->wakePending.store(false);
        PrAppEvent event{};
        event.kind = PrAppEventKind_Wake;
        event.time = uiKitUptime();
        [(__bridge PrUiKitHost*)uiKit->host deliver:event to:nullptr];
    });
}

PrStatus openUrl(PrApp* app, PrStringView url) {
    NSURL* address = [NSURL URLWithString:uiKitStringOf(url)];
    if (address == nil)
        return PrStatus_Syntax;
    if (uiKitHostOf(app).headless)
        return PrStatus_NotSupported;
    [UIApplication.sharedApplication openURL:address options:@{} completionHandler:nil];
    return PrStatus_Ok;
}

PrAppLifecycle lifecycle(const PrApp* app) {
    return [uiKitHostOf(app) lifecycle];
}

void timerSet(PrApp* app, uint64_t tag, double delay, double interval) {
    [uiKitHostOf(app) setTimer:tag delay:delay interval:interval];
}

void timerCancel(PrApp* app, uint64_t tag) {
    [uiKitHostOf(app) cancelTimer:tag];
}

PrAppWindowModel windowModel(const PrApp* app) {
    return [uiKitHostOf(app) windowModel];
}

// ---------------------------------------------------------------------------------------------------------------------
// The windows' functions

void windowDestroy(PrAppWindow* window) {
    UiKitWindow* uiKit = static_cast<UiKitWindow*>(window);
    if (appWindowDeliveryDeferDestroy(uiKit->delivery))
        return;
    PrUiKitWindow* object = (__bridge_transfer PrUiKitWindow*)uiKit->object;
    [object.host forget:window];
    [object close];
    prRawStringReset(&uiKit->title);
    prMemoryFree(uiKit);
}

PrApp* windowApp(const PrAppWindow* window) {
    return static_cast<const UiKitWindow*>(window)->app;
}

PrAppFrame frameState(PrAppWindow* window) {
    return [uiKitObjectOf(window) frameAt:uiKitUptime()];
}

void requestFrame(PrAppWindow* window) {
    [uiKitObjectOf(window) requestFrame];
}

void setTitle(PrAppWindow* window, PrStringView title) {
    UiKitWindow* uiKit = static_cast<UiKitWindow*>(window);
    prRawStringReset(&uiKit->title);
    uiKit->title = prRawStringCopy(title);
    // The system shows a scene's title in its switcher, where the device has one.
    if (UIWindowScene* scene = uiKitObjectOf(window).window.windowScene)
        scene.title = uiKitStringOf(title);
    requestFrame(window);
}

PrStringView title(const PrAppWindow* window) {
    return prRawStringView(&static_cast<const UiKitWindow*>(window)->title);
}

PrAppWindowState state(const PrAppWindow*) {
    return PrAppWindowState_Fullscreen;
}

void setVisible(PrAppWindow* window, bool visible) {
    PrUiKitWindow* object = uiKitObjectOf(window);
    object.shown = visible;
    object.window.hidden = !visible;
    if (visible)
        [object requestFrame];
}

bool isVisible(const PrAppWindow* window) {
    return uiKitObjectOf(window).shown;
}

// A window of visionOS has a place in the user's space, and none on a display.
#if !TARGET_OS_VISION

// Returns the screen of a window whose place on it the system lets the user change, or nil for a window that fills the
// display of a phone, or that shows in no scene.
UIScreen* PR_NULLABLE placedScreenOf(const PrAppWindow* window) {
    PrUiKitWindow* object = uiKitObjectOf(window);
    if (object.window == nil || UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone)
        return nil;
    return object.scene.screen;
}

bool pointToDisplay(const PrAppWindow* window, PrVec2 point, PrAppDisplayPosition* out) {
    UIScreen* screen = placedScreenOf(window);
    if (screen == nil)
        return false;
    CGPoint on = [uiKitObjectOf(window).view convertPoint:CGPointMake(CGFloat(point.x), CGFloat(point.y))
        toCoordinateSpace:screen.coordinateSpace];
    *out = PrAppDisplayPosition{kUiKitScreenId, PrVec2{float(on.x), float(on.y)}};
    return true;
}

bool pointFromDisplay(const PrAppWindow* window, const PrAppDisplayPosition* position, PrVec2* out) {
    UIScreen* screen = placedScreenOf(window);
    if (screen == nil || position->display != kUiKitScreenId)
        return false;
    CGPoint in = [uiKitObjectOf(window).view convertPoint:CGPointMake(CGFloat(position->position.x),
        CGFloat(position->position.y)) fromCoordinateSpace:screen.coordinateSpace];
    *out = PrVec2{float(in.x), float(in.y)};
    return true;
}

bool position(const PrAppWindow* window, PrAppDisplayPosition* out) {
    return pointToDisplay(window, PrVec2{}, out);
}

#endif

bool isOccluded(const PrAppWindow* window) {
    UIWindowScene* scene = uiKitObjectOf(window).scene;
    return scene == nil || scene.activationState == UISceneActivationStateBackground;
}

bool isActive(const PrAppWindow* window) {
    PrUiKitWindow* object = uiKitObjectOf(window);
    return object.window.isKeyWindow && object.scene.activationState == UISceneActivationStateForegroundActive;
}

void focus(PrAppWindow* window) {
    PrUiKitWindow* object = uiKitObjectOf(window);
    object.shown = YES;
    [object.window makeKeyAndVisible];
    [object.view becomeFirstResponder];
}

void setHiddenSystemBars(PrAppWindow* window, PrAppSystemBarFlags hidden) {
    PrUiKitWindow* object = uiKitObjectOf(window);
    object.hiddenBars = hidden;
#if TARGET_OS_IOS
    [object.controller setNeedsStatusBarAppearanceUpdate];
    [object.controller setNeedsUpdateOfHomeIndicatorAutoHidden];
#endif
    [object requestFrame];
}

// UIKit's drags start from its own gestures, not from a press that the program holds, so a drag ends with None.
void dragStart(PrAppWindow* window, const PrAppDragDesc*) {
    PrAppEvent event{};
    event.kind = PrAppEventKind_DragEnd;
    event.time = uiKitUptime();
    event.drag.operation = PrAppDragOperation_None;
    [uiKitObjectOf(window).host deliver:event to:window];
}

PrStatus surface(const PrAppWindow* window, PrSurface* out) {
    out->kind = PrSurfaceKind_UiView;
    out->handle = (__bridge void*)uiKitObjectOf(window).view;
    return PrStatus_Ok;
}

// Calls the window's handler with an event. A window that the program destroyed during the call is freed.
void deliverEvent(PrAppWindow* window, const PrAppEvent& event) {
    if (!appWindowDeliverEvent(static_cast<UiKitWindow*>(window)->delivery, window, &event, frameState))
        windowDestroy(window);
}

// Calls the window's handler with a frame and applies the output. A window that the program destroyed during the call
// is freed.
void deliverFrame(PrAppWindow* window, const PrAppFrame& frame) {
    PrAppOutput output{};
    if (!appWindowDeliverFrame(static_cast<UiKitWindow*>(window)->delivery, window, &frame, &output)) {
        windowDestroy(window);
        return;
    }
    PrUiKitWindow* object = uiKitObjectOf(window);
    // Only text that the window edits itself applies, since the window's popups and dialogs are inside it.
    bool editing = output.hasTextInput && (output.textInputWindow == nullptr || output.textInputWindow == window);
    PrRect caret = output.textInput;
    object.view.caret = CGRectNull;
    if (editing) {
        object.view.caret = CGRectMake(CGFloat(caret.left), CGFloat(caret.top), CGFloat(caret.right - caret.left),
            CGFloat(caret.bottom - caret.top));
    }
    [object.view setEditing:editing kind:output.textInputKind];
    object.view.cursor = output.cursor;
    object.dropOperation = output.dropOperation;
    if (output.wantsFrame)
        [object requestFrame];
    [object requestFrameAt:output.frameTime];
}

PrStatus windowCreate(PrApp* app, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out) {
    PrUiKitHost* host = uiKitHostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    if ([host windowModel] == PrAppWindowModel_Single && host.windows.count != 0)
        return PrStatus_NotSupported;
    UiKitWindow* window = allocZeroed<UiKitWindow>();
    window->destroyFn = windowDestroy;
    window->appFn = windowApp;
    window->requestFrameFn = requestFrame;
    window->setTitleFn = setTitle;
    window->titleFn = title;
    window->stateFn = state;
    window->setVisibleFn = setVisible;
    window->isVisibleFn = isVisible;
    window->isOccludedFn = isOccluded;
    window->isActiveFn = isActive;
    window->focusFn = focus;
    window->setHiddenSystemBarsFn = setHiddenSystemBars;
    window->shareFn = uiKitShare;
#if !TARGET_OS_VISION
    window->positionFn = position;
    window->pointToDisplayFn = pointToDisplay;
    window->pointFromDisplayFn = pointFromDisplay;
#endif
    window->dragStartFn = dragStart;
    window->surfaceFn = surface;
    window->app = app;
    window->delivery.handler = desc->handler;
    window->title = prRawStringCopy(desc->title);
    PrUiKitWindow* object = [[PrUiKitWindow alloc] init];
    object.record = window;
    object.host = host;
    object.shown = !desc->hidden;
    object.keyboard = CGRectNull;
    object.prominent = desc->placement == PrAppWindowPlacement_Center;
    // The view has the description's extent until a window shows it, which fills the scene.
    object.view = [[PrUiKitView alloc] initWithFrame:CGRectMake(0, 0, desc->extent.x, desc->extent.y)];
    object.view.owner = object;
    object.controller = [[PrUiKitViewController alloc] init];
    object.controller.owner = object;
    object.controller.view = object.view;
    [host.windows addObject:object];
    window->object = (__bridge_retained void*)object;
    [host place:object];
    [object requestFrame];
    *out = window;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

@implementation PrUiKitViewController
#if !TARGET_OS_TV
- (BOOL)prefersStatusBarHidden {
    return (self.owner.hiddenBars & PrAppSystemBarFlag_Status) != 0;
}
- (BOOL)prefersHomeIndicatorAutoHidden {
    return (self.owner.hiddenBars & PrAppSystemBarFlag_HomeIndicator) != 0;
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    PrUiKitHost* host = self.owner.host;
    return maskOf(host != nil ? host.orientations : PrAppOrientationFlags(PrAppOrientationFlag_All));
}
#endif
- (void)viewWillTransitionToSize:(CGSize)size
        withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    __weak PrUiKitHost* weak = self.owner.host;
    [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext>) {
        [weak deliverKind:PrAppEventKind_DisplaysChanged];
    }];
}
@end

@implementation PrUiKitWindow
- (void)frame {
    PrUiKitHost* host = self.host;
    if (self.record == nullptr || !host.running || !self.shown)
        return;
    // A frame that comes during a callback waits for the next.
    if (host.inCallback) {
        [self requestFrame];
        return;
    }
    double now = uiKitUptime();
    PrAppFrame frame = [self frameAt:now];
    self.lastFrame = now;
    self.framedExtent = self.view.bounds.size;
    [host beginCallback];
    deliverFrame(self.record, frame);
    [host endCallback];
}
// The window as a frame at `now` finds it.
- (PrAppFrame)frameAt:(double)now {
    PrUiKitView* view = self.view;
    PrAppFrame frame{};
    frame.time = now;
    frame.delta = self.lastFrame == 0 ? 0 : now - self.lastFrame;
    frame.extent = PrVec2{float(view.bounds.size.width), float(view.bounds.size.height)};
    frame.display = displayOf(self.host, view);
    frame.backdrop = PrAppWindowBackdrop_Opaque;
    // The safe area keeps clear of the controls of a window that the user resizes, below them.
    UIViewLayoutRegion* region =
        [UIViewLayoutRegion safeAreaLayoutRegionWithCornerAdaptation:UIViewLayoutRegionAdaptivityAxisVertical];
    UIEdgeInsets safe = [view edgeInsetsForLayoutRegion:region];
    frame.safeArea = PrAppInsets{float(safe.top), float(safe.right), float(safe.bottom), float(safe.left)};
    CGRect keyboard = CGRectIntersection(self.keyboard, view.bounds);
    if (!CGRectIsNull(keyboard) && !CGRectIsEmpty(keyboard)) {
        frame.keyboard = PrRect{
            float(CGRectGetMinX(keyboard)), float(CGRectGetMinY(keyboard)),
            float(CGRectGetMaxX(keyboard)), float(CGRectGetMaxY(keyboard)),
        };
    }
    frame.preferences = prAppUserPreferencesDefault();
    PrAppUserPreferences& preferences = frame.preferences;
    UITraitCollection* traits = view.traitCollection;
    if (traits.userInterfaceStyle == UIUserInterfaceStyleDark)
        preferences.colorScheme = PrAppColorScheme_Dark;
    if (traits.accessibilityContrast == UIAccessibilityContrastHigh)
        preferences.contrast = PrAppContrast_More;
    preferences.reducedMotion = UIAccessibilityIsReduceMotionEnabled();
    preferences.reducedTransparency = UIAccessibilityIsReduceTransparencyEnabled();
    // The scale of the body text at the user's text size, which is 1 at the default size.
    preferences.textScale =
        float([UIFontMetrics.defaultMetrics scaledValueForValue:1 compatibleWithTraitCollection:traits]);
    return frame;
}
// A window frames at once when its extent changes, inside the layout's transaction, so that a swap chain that presents
// with the transaction shows its frame together with the new extent.
- (void)layoutChanged {
    if (self.window == nil)
        return;
    // A layout at the same extent may still move the window's controls, and so the safe area.
    if (CGSizeEqualToSize(self.view.bounds.size, self.framedExtent)) {
        [self requestFrame];
        return;
    }
    if (self.host.inCallback || !static_cast<UiKitWindow*>(self.record)->delivery.opened) {
        [self requestFrame];
        return;
    }
    self.frameRequested = NO;
    [self frame];
}
- (void)displayFrame:(CADisplayLink*)sender {
    // The window waits for its scene before its first frame.
    if (!self.frameRequested || self.window == nil)
        return;
    self.frameRequested = NO;
    sender.paused = YES;
    [self frame];
}
- (void)requestFrame {
    if (self.record == nullptr || !self.host.running)
        return;
    if (self.host.headless) {
        if (self.timer != nil)
            return;
        __weak PrUiKitWindow* weak = self;
        self.timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:NO block:^(NSTimer*) {
            PrUiKitWindow* window = weak;
            window.timer = nil;
            [window frame];
        }];
        [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
        return;
    }
    self.frameRequested = YES;
    if (self.displayLink == nil) {
        self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(displayFrame:)];
#if !TARGET_OS_VISION
        UIScreen* screen = uiKitScreenOf(self.host, self.view);
        float fastest = screen != nil ? float(screen.maximumFramesPerSecond) : 60.0f;
        self.displayLink.preferredFrameRateRange = CAFrameRateRangeMake(MIN(30.0f, fastest), fastest, fastest);
#endif
        [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    self.displayLink.paused = NO;
}
- (void)requestFrameAt:(double)time {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    if (self.record == nullptr || !self.host.running || time == 0)
        return;
    double delay = MAX(time - uiKitUptime(), 0.0);
    __weak PrUiKitWindow* weak = self;
    self.frameTimer = [NSTimer timerWithTimeInterval:delay repeats:NO block:^(NSTimer*) {
        PrUiKitWindow* window = weak;
        window.frameTimer = nil;
        [window requestFrame];
    }];
    [NSRunLoop.mainRunLoop addTimer:self.frameTimer forMode:NSRunLoopCommonModes];
}
- (void)stopFrames {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    [self.displayLink invalidate];
    self.displayLink = nil;
    [self.timer invalidate];
    self.timer = nil;
}
// Shows the window as the root of `scene`'s UIWindow.
- (void)attach:(UIWindowScene*)scene {
    if (self.window != nil || self.host.headless)
        return;
    self.scene = scene;
    self.waiting = NO;
    self.window = [[UIWindow alloc] initWithWindowScene:scene];
    self.window.rootViewController = self.controller;
    scene.title = uiKitStringOf(prRawStringView(&static_cast<UiKitWindow*>(self.record)->title));
    if (self.shown)
        [self.window makeKeyAndVisible];
    else
        self.window.hidden = YES;
    [self requestFrame];
}
// Takes the window out of its scene.
- (void)detach {
    self.window.hidden = YES;
    self.window.rootViewController = nil;
    self.window = nil;
    self.scene = nil;
}
// Closes the window, and hands its scene back to the host.
- (void)close {
    self.record = nullptr;
    [self stopFrames];
    [self.view resignFirstResponder];
    self.view.owner = nil;
    self.controller.owner = nil;
    UIWindowScene* scene = self.scene;
    [self detach];
    [self.host closed:scene];
}
- (void)deliver:(const PrAppEvent&)event {
    if (self.record != nullptr)
        [self.host deliver:event to:self.record];
}
@end

@implementation PrUiKitHost {
    // The events that arrived during the running callback, in order.
    RawArray<HeldEvent> _held;
    PrAppLifecycle _lifecycle;                         // the lifecycle that the last LifecycleChanged event reported
    NSMutableDictionary<NSNumber*, NSTimer*>* _timers; // the program's timers, by tag
    NSMutableArray<id>* _observers;                    // the tokens of the default center's observers
}
- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _windows = [NSMutableArray array];
        _presented = [NSMutableArray array];
        _orientations = PrAppOrientationFlag_All;
    }
    return self;
}
- (void)dealloc {
    for (uint32_t i = 0; i < _held.count; ++i)
        heldReset(_held.data[i], [self sources]);
    _held.reset();
}
- (AppDataSources*)sources {
    return self.app != nullptr ? &static_cast<UiKitApp*>(self.app)->sources : nullptr;
}
// Calls the program's start, once UIKit has launched the application.
- (void)started {
    [self startServices];
    _lifecycle = [self computedLifecycle];
    const PrAppHandler& handler = static_cast<UiKitApp*>(self.app)->handler;
    [self beginCallback];
    if (!handler.start(handler.context, self.app))
        [self stop];
    [self endCallback];
}
- (UIWindowScene* PR_NULLABLE)anyScene {
    if (self.headless)
        return nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:UIWindowScene.class])
            return (UIWindowScene*)scene;
    }
    return nil;
}
// Multiple, if the options asked for more than one window and the device shows several scenes, as an iPad does and an
// iPhone does not. A headless application has no scenes, and so no limit. A scene's place is the system's, so no
// application here is a Desktop one.
- (PrAppWindowModel)windowModel {
    if (self.requestedModel == PrAppWindowModel_Single)
        return PrAppWindowModel_Single;
    if (self.headless || UIApplication.sharedApplication.supportsMultipleScenes)
        return PrAppWindowModel_Multiple;
    return PrAppWindowModel_Single;
}
- (PrUiKitWindow* PR_NULLABLE)waitingWindow {
    for (PrUiKitWindow* window in self.windows) {
        if (window.waiting)
            return window;
    }
    return nil;
}
// Gives a new window a scene: the spare scene, or the scene of the launch, which the first window waits for, or a new
// scene that the system is asked for. Windows take the scenes that connect in the order that they were created.
- (void)place:(PrUiKitWindow*)window {
    if (self.headless)
        return;
    if (UIWindowScene* spare = self.spare) {
        self.spare = nil;
        [window attach:spare];
        return;
    }
    bool launching = !self.launched && [self waitingWindow] == nil;
    window.waiting = YES;
    if (launching)
        return;
    UISceneSessionActivationRequest* request =
        [UISceneSessionActivationRequest requestWithRole:UIWindowSceneSessionRoleApplication];
#if !TARGET_OS_TV
    UIWindowSceneActivationRequestOptions* options = [[UIWindowSceneActivationRequestOptions alloc] init];
    options.placement = [UIWindowSceneStandardPlacement standardPlacement];
    if (window.prominent)
        options.placement = [UIWindowSceneProminentPlacement prominentPlacement];
    request.options = options;
#endif
    [UIApplication.sharedApplication activateSceneSessionForRequest:request errorHandler:^(NSError* error) {
        NSLog(@"lib/app: the system made no scene for a window: %@", error);
    }];
}
// Takes back the scene of a window that closed. An application of several windows closes the scene, and one of a single
// window keeps it for the next window.
- (void)closed:(UIWindowScene* PR_NULLABLE)scene {
    if (scene == nil)
        return;
    if ([self windowModel] == PrAppWindowModel_Multiple)
        [UIApplication.sharedApplication requestSceneSessionDestruction:scene.session options:nil errorHandler:nil];
    else if (self.spare == nil)
        self.spare = scene;
}
// A scene connected: the launch's, one that a window asked for, or one that the system restored or the user opened. The
// first window that waits takes it. With no window waiting, the scene is kept for the next window if no window shows,
// and is closed otherwise, since the program did not ask for it.
- (void)connect:(UIWindowScene*)scene {
    self.launched = YES;
    PrUiKitWindow* waiting = [self waitingWindow];
    bool shown = false;
    for (PrUiKitWindow* window in self.windows)
        shown = shown || window.scene != nil;
    if (waiting != nil)
        [waiting attach:scene];
    else if (self.spare == nil && !shown)
        self.spare = scene;
    else
        [UIApplication.sharedApplication requestSceneSessionDestruction:scene.session options:nil errorHandler:nil];
    [self scenesChanged];
}
// The user closed a scene, which asks the program to close its window.
- (void)disconnect:(UIScene*)scene {
    if (scene == self.spare)
        self.spare = nil;
    PrAppEvent event{};
    event.kind = PrAppEventKind_CloseRequested;
    event.time = uiKitUptime();
    for (PrUiKitWindow* window in [self.windows copy]) {
        if (window.scene != scene)
            continue;
        [window detach];
        if (window.record != nullptr)
            [self deliver:event to:window.record];
    }
    [self scenesChanged];
}
// A scene's state changed, which may change the application's lifecycle and which window is active.
- (void)scenesChanged {
    for (PrUiKitWindow* window in self.windows)
        [window requestFrame];
    [self lifecycleMayChange];
}
// Delivers an Open event for URLs that the system asks the application to open. A file's URL opens as a path.
- (void)open:(NSSet<UIOpenURLContext*>*)contexts {
    if (!self.running || contexts.count == 0)
        return;
    AppDataStore* store = allocZeroed<AppDataStore>();
    for (UIOpenURLContext* context in contexts) {
        NSURL* url = context.URL;
        PrRawString text = uiKitTextOf(url.isFileURL ? url.path : url.absoluteString);
        if (url.isFileURL)
            appDataStoreAddPath(*store, prRawStringView(&text));
        else
            appDataStoreAddUrl(*store, prRawStringView(&text));
        prRawStringReset(&text);
    }
    PrAppEvent event{};
    event.kind = PrAppEventKind_Open;
    event.time = uiKitUptime();
    event.open.source = appDataSourceCreate(*[self sources], store, nullptr);
    [self deliver:event to:nullptr];
}
- (void)call:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window {
    self.inCallback = YES;
    if (window != nullptr) {
        deliverEvent(window, event);
    } else {
        const PrAppHandler& handler = static_cast<UiKitApp*>(self.app)->handler;
        handler.event(handler.context, self.app, &event);
    }
    self.inCallback = NO;
}
- (void)beginCallback {
    self.inCallback = YES;
}
// Ends a callback, and delivers the events that arrived during it, and those that arrive during their own delivery, in
// order. The events of a run that stopped are dropped, and so are those of a window that closed (forget:).
- (void)endCallback {
    self.inCallback = NO;
    for (uint32_t i = 0; i < _held.count && self.running; ++i) {
        if (_held.data[i].dropped)
            continue;
        if (UiKitAnswer* answer = _held.data[i].answer) {
            [self call:*answer];
            continue;
        }
        PrStringView text{_held.data[i].text.data != nullptr ? _held.data[i].text.data : "", _held.data[i].text.count};
        PrAppEvent event = _held.data[i].event;
        prAppEventSetText(&event, text);
        [self call:event to:_held.data[i].window];
    }
    for (uint32_t i = 0; i < _held.count; ++i)
        heldReset(_held.data[i], [self sources]);
    _held.clear();
}
// Calls the callback of an answer's request. No other callback may be running.
- (void)call:(UiKitAnswer&)answer {
    UiKitApp* app = static_cast<UiKitApp*>(self.app);
    self.inCallback = YES;
    switch (answer.kind) {
        case UiKitAnswerKind_Store:
            appDataRequestsAnswerFromStore(app->requests, answer.id, answer.status,
                answer.external != nullptr ? answer.external : &answer.store);
            break;
        case UiKitAnswerKind_Bytes:
            appDataRequestsAnswerBytes(app->requests, answer.id, answer.status,
                PrByteView{answer.bytes.data, answer.bytes.count});
            break;
        case UiKitAnswerKind_Status: appDataRequestsAnswerStatus(app->requests, answer.id, answer.status); break;
        default:
            appDataRequestsAnswerSource(app->requests, answer.id, answer.status, answer.source);
            appDataSourceRelease(app->sources, answer.source);
            answer.source = kPrAppDataSourceNone;
            break;
    }
    self.inCallback = NO;
}
- (void)answer:(void*)stored {
    UiKitAnswer* answer = static_cast<UiKitAnswer*>(stored);
    if (!self.running) {
        if (answer->source != kPrAppDataSourceNone)
            appDataSourceRelease(static_cast<UiKitApp*>(self.app)->sources, answer->source);
        uiKitAnswerDestroy(answer);
        return;
    }
    if (self.inCallback) {
        _held.push(HeldEvent{.answer = answer});
        return;
    }
    [self call:*answer];
    uiKitAnswerDestroy(answer);
    [self endCallback];
}
// Delivers an event now, or holds it if a callback is running. An event that hands over a source releases its reference
// once delivered.
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window {
    if (self.running && self.inCallback) {
        _held.push(heldOf(window, event));
        return;
    }
    if (self.running)
        [self call:event to:window];
    PrAppDataSource source = sourceOf(event);
    if (source != kPrAppDataSourceNone && [self sources] != nullptr)
        appDataSourceRelease(*[self sources], source);
    if (self.running)
        [self endCallback];
}
- (void)forget:(PrAppWindow*)window {
    for (uint32_t i = 0; i < _held.count; ++i) {
        if (_held.data[i].window == window)
            _held.data[i].dropped = true;
    }
    for (PrUiKitWindow* object in self.windows) {
        if (object.record == window) {
            [self.windows removeObject:object];
            break;
        }
    }
}
// Stops the run. A headless run returns from the run loop. UIKit's run never returns, so its shutdown runs once the
// callback that stopped it returns, and the process then exits.
- (void)stop {
    if (!self.running)
        return;
    self.running = NO;
    for (PrUiKitWindow* window in self.windows)
        [window stopFrames];
    if (self.headless) {
        CFRunLoopStop(CFRunLoopGetMain());
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        const PrAppHandler& handler = static_cast<UiKitApp*>(self.app)->handler;
        [self beginCallback];
        handler.shutdown(handler.context, self.app);
        [self endCallback];
        [self stopServices];
        while (self.windows.count != 0)
            prAppWindowDestroy(self.windows.lastObject.record);
        exit(0);
    });
}
- (void)deliverKind:(PrAppEventKind)kind {
    if (!self.running)
        return;
    PrAppEvent event{};
    event.kind = kind;
    event.time = uiKitUptime();
    [self deliver:event to:nullptr];
}
- (PrAppLifecycle)computedLifecycle {
    if (self.headless)
        return PrAppLifecycle_Background;
    // The application is as far forward as its foremost scene.
    PrAppLifecycle lifecycle = PrAppLifecycle_Background;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState == UISceneActivationStateForegroundActive)
            return PrAppLifecycle_Active;
        if (scene.activationState == UISceneActivationStateForegroundInactive)
            lifecycle = PrAppLifecycle_Inactive;
    }
    return lifecycle;
}
- (PrAppLifecycle)lifecycle {
    return self.running ? _lifecycle : [self computedLifecycle];
}
// Delivers LifecycleChanged if the scenes' states changed it, and requests a frame of each window, whose activity
// follows it.
- (void)lifecycleMayChange {
    PrAppLifecycle now = [self computedLifecycle];
    if (now == _lifecycle)
        return;
    _lifecycle = now;
    for (PrUiKitWindow* window in self.windows)
        [window requestFrame];
    [self deliverKind:PrAppEventKind_LifecycleChanged];
}
- (NSTimer*)timerFor:(uint64_t)tag after:(double)seconds repeats:(BOOL)repeats interval:(double)interval {
    __weak PrUiKitHost* weak = self;
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
    event.time = uiKitUptime();
    event.tag = tag;
    [self deliver:event to:nullptr];
}
// Runs `block` on the main queue for each notification `name`, while the application lives.
- (void)observe:(NSString*)name with:(void (^)(PrUiKitHost* host, NSNotification* note))block {
    __weak PrUiKitHost* weak = self;
    id token = [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification* note) {
            PrUiKitHost* host = weak;
            if (host != nil && host.app != nullptr)
                block(host, note);
        }];
    [_observers addObject:token];
}
- (void)observe:(NSString*)name kind:(PrAppEventKind)kind {
    [self observe:name with:^(PrUiKitHost* host, NSNotification*) {
        [host deliverKind:kind];
    }];
}
// Requests a frame of each window, whose parameters a notification `name` changes.
- (void)observeFrames:(NSString*)name {
    [self observe:name with:^(PrUiKitHost* host, NSNotification*) {
        for (PrUiKitWindow* window in host.windows)
            [window requestFrame];
    }];
}
- (void)startServices {
    uiKitDeviceStart(self.app);
    _observers = [NSMutableArray array];
    [self observe:UIApplicationDidReceiveMemoryWarningNotification kind:PrAppEventKind_LowMemory];
    [self observe:NSCurrentLocaleDidChangeNotification kind:PrAppEventKind_LocaleChanged];
#if !TARGET_OS_TV
    [self observe:UIPasteboardChangedNotification kind:PrAppEventKind_ClipboardChanged];
#endif
    [self observe:UITextInputCurrentInputModeDidChangeNotification kind:PrAppEventKind_KeyboardLayoutChanged];
    [self observe:UIApplicationProtectedDataWillBecomeUnavailable kind:PrAppEventKind_ScreenLocked];
    [self observe:UIApplicationProtectedDataDidBecomeAvailable kind:PrAppEventKind_ScreenUnlocked];
#if !TARGET_OS_TV
    [self observe:UIDeviceBatteryStateDidChangeNotification kind:PrAppEventKind_PowerChanged];
    [self observe:UIDeviceBatteryLevelDidChangeNotification kind:PrAppEventKind_PowerChanged];
#endif
    [self observe:NSProcessInfoPowerStateDidChangeNotification kind:PrAppEventKind_PowerChanged];
    [self observeFrames:UIAccessibilityReduceMotionStatusDidChangeNotification];
    [self observeFrames:UIAccessibilityReduceTransparencyStatusDidChangeNotification];
#if !TARGET_OS_VISION
    [self observeFrames:UIScreenReferenceDisplayModeStatusDidChangeNotification];
#endif
#if TARGET_OS_IOS
    // The keyboard's frame, in the screen's coordinates, which each window takes in its view's. A television's keyboard
    // covers the whole screen, and visionOS's floats apart from the window, and neither reports a frame in it.
    void (^keyboard)(PrUiKitHost*, NSNotification*) = ^(PrUiKitHost* host, NSNotification* note) {
        BOOL hiding = [note.name isEqualToString:UIKeyboardWillHideNotification];
        CGRect end = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
        for (PrUiKitWindow* window in host.windows) {
            UIScreen* screen = uiKitScreenOf(host, window.view);
            window.keyboard = CGRectNull;
            if (!hiding && screen != nil)
                window.keyboard = [window.view convertRect:end fromCoordinateSpace:screen.coordinateSpace];
            [window requestFrame];
        }
    };
    [self observe:UIKeyboardWillChangeFrameNotification with:keyboard];
    [self observe:UIKeyboardWillHideNotification with:keyboard];
#endif
}
- (void)stopServices {
    uiKitDeviceStop(self.app);
    for (id token in _observers)
        [NSNotificationCenter.defaultCenter removeObserver:token];
    _observers = nil;
    for (NSTimer* timer in _timers.allValues)
        [timer invalidate];
    _timers = nil;
}
@end

@implementation PrUiKitAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application;
    (void)options;
    [runningHost started];
    return YES;
}
- (UISceneConfiguration*)application:(UIApplication*)application
        configurationForConnectingSceneSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)application;
    (void)options;
    UISceneConfiguration* configuration = [UISceneConfiguration configurationWithName:@"Default"
        sessionRole:session.role];
    configuration.delegateClass = PrUiKitSceneDelegate.class;
    return configuration;
}
@end

@implementation PrUiKitSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)session;
    if ([scene isKindOfClass:UIWindowScene.class])
        [runningHost connect:(UIWindowScene*)scene];
    [runningHost open:options.URLContexts];
}
- (void)sceneDidDisconnect:(UIScene*)scene {
    [runningHost disconnect:scene];
}
- (void)sceneDidBecomeActive:(UIScene*)scene {
    (void)scene;
    [runningHost scenesChanged];
}
- (void)sceneWillResignActive:(UIScene*)scene {
    (void)scene;
    [runningHost scenesChanged];
}
- (void)sceneWillEnterForeground:(UIScene*)scene {
    (void)scene;
    [runningHost scenesChanged];
}
- (void)sceneDidEnterBackground:(UIScene*)scene {
    (void)scene;
    [runningHost scenesChanged];
}
- (void)scene:(UIScene*)scene openURLContexts:(NSSet<UIOpenURLContext*>*)contexts {
    (void)scene;
    [runningHost open:contexts];
}
@end

PrApp* prAppUiKitCreate(const PrAppOptions* options) {
    UiKitApp* app = allocZeroed<UiKitApp>();
    app->destroyFn = destroy;
    app->runFn = run;
    app->stopFn = stop;
    app->wakeFn = wake;
    app->cancelFn = uiKitDataCancel;
    app->windowModelFn = windowModel;
    app->lifecycleFn = lifecycle;
    app->timerSetFn = timerSet;
    app->timerCancelFn = timerCancel;
#if TARGET_OS_VISION
    app->immersiveOpenFn = uiKitImmersiveOpen;
    app->immersiveCloseFn = uiKitImmersiveClose;
#endif
    app->displaysFn = displays;
    app->displayInfoFn = displayInfo;
#if !TARGET_OS_TV
    app->setOrientationsFn = setOrientations;
#endif
    app->dataRetainFn = uiKitDataRetain;
    app->dataReleaseFn = uiKitDataRelease;
    app->dataReadFn = uiKitDataRead;
    app->dataWriteFn = uiKitDataWrite;
    app->dataWriteFileFn = uiKitDataWriteFile;
    app->fileDialogFn = uiKitFileDialog;
    app->openUrlFn = openUrl;
    app->powerFn = uiKitPower;
    app->setAwakeFn = uiKitSetAwake;
    app->hapticPlayFn = uiKitHapticPlay;
    app->sensorEnableFn = uiKitSensorEnable;
    app->notificationPostFn = uiKitNotificationPost;
    app->setBadgeFn = uiKitSetBadge;
    app->windowCreateFn = windowCreate;
    appDataSourcesInit(app->sources);
    appDataRequestsInit(app->requests, app->sources);
    PrUiKitHost* host = [[PrUiKitHost alloc] init];
    host.app = app;
    host.headless = options->headless;
    host.requestedModel = options->windowModel;
    app->host = (__bridge_retained void*)host;
    return app;
}
