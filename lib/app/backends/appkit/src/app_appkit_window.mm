// The AppKit backend's windows: their windows, views and panels, their frames, and their placement.

#include "app/backends/appkit/src/app_appkit_window.h"

#include "app/backends/appkit/src/app_appkit_data.h"
#include "app/backends/appkit/src/app_appkit_display.h"
#include "app/backends/appkit/src/app_appkit_input.h"
#include "app/backends/appkit/src/app_appkit_menu.h"
#include "app/backends/appkit/src/app_appkit_shell.h"

namespace pr_impl {

namespace {

PrApp* windowApp(const PrAppWindow* window) {
    return static_cast<const AppKitWindow*>(window)->app;
}

void setTextInputRect(PrAppWindow* window, PrRect rect) {
    appKitObjectOf(window).view.textInputRect = NSMakeRect(CGFloat(rect.left), CGFloat(rect.top),
        CGFloat(rect.right - rect.left), CGFloat(rect.bottom - rect.top));
}

// Returns the window whose latest output holds the text being edited in `window`: the window itself if its output has
// some there, or else a window that owns it whose output names it. Returns null if no output does.
const AppKitWindow* PR_NULLABLE textInputSource(PrAppWindow* window) {
    const AppKitWindow* own = static_cast<const AppKitWindow*>(window);
    if (own->textInputWindow == window)
        return own;
    for (PrAppKitWindow* other in appKitObjectOf(window).host.windows) {
        const AppKitWindow* appKit = static_cast<const AppKitWindow*>(other.record);
        if (appKit != nullptr && appKit->textInputWindow == window)
            return appKit;
    }
    return nullptr;
}

// Withdraws the text input that `window`, which is being destroyed, names in another window, and the text input that
// other windows name in it.
void forgetTextInput(PrAppWindow* window) {
    AppKitWindow* appKit = static_cast<AppKitWindow*>(window);
    PrAppWindow* named = appKit->textInputWindow;
    appKit->textInputWindow = nullptr;
    if (named != nullptr && named != window)
        appKitApplyTextInput(named);
    for (PrAppKitWindow* other in appKitObjectOf(window).host.windows) {
        AppKitWindow* owner = static_cast<AppKitWindow*>(other.record);
        if (owner != nullptr && owner->textInputWindow == window)
            owner->textInputWindow = nullptr;
    }
}

void setTitle(PrAppWindow* window, PrStringView title) {
    AppKitWindow* appKit = static_cast<AppKitWindow*>(window);
    prRawStringReset(&appKit->title);
    appKit->title = prRawStringCopy(title);
    appKitObjectOf(window).window.title = appKitStringOf(title);
    appKitRequestFrame(window);
}

PrStringView title(const PrAppWindow* window) {
    return prRawStringView(&static_cast<const AppKitWindow*>(window)->title);
}

// Returns `size` within the content of the screen that shows `window`, or the main screen's.
NSSize clampedToScreen(NSWindow* window, NSSize size, NSWindowStyleMask style) {
    NSScreen* screen = window.screen != nil ? window.screen : NSScreen.mainScreen;
    NSRect available = [NSWindow contentRectForFrameRect:screen.visibleFrame styleMask:style];
    return NSMakeSize(MIN(size.width, available.size.width), MIN(size.height, available.size.height));
}

void setExtent(PrAppWindow* window, PrUVec2 extent) {
    PrAppKitWindow* object = appKitObjectOf(window);
    // AppKit holds the user's resizes to the limits, but not the program's.
    NSSize min = object.window.contentMinSize;
    NSSize max = object.window.contentMaxSize;
    NSSize size = NSMakeSize(MIN(MAX(CGFloat(extent.x), min.width), max.width),
        MIN(MAX(CGFloat(extent.y), min.height), max.height));
    if (!object.host.headless)
        size = clampedToScreen(object.window, size, object.window.styleMask);
    [object.window setContentSize:size];
    // AppKit resizes a window about its bottom left corner. A popup is placed again, because its placement depends on
    // its size.
    [object place];
}

// The extent limits as AppKit holds them: a greatest extent of zero is unlimited.
NSSize maxSizeOf(PrUVec2 maxExtent) {
    return NSMakeSize(maxExtent.x != 0 ? CGFloat(maxExtent.x) : CGFLOAT_MAX,
        maxExtent.y != 0 ? CGFloat(maxExtent.y) : CGFLOAT_MAX);
}

void setExtentLimits(PrAppWindow* window, PrUVec2 minExtent, PrUVec2 maxExtent) {
    NSWindow* frame = appKitObjectOf(window).window;
    frame.contentMinSize = NSMakeSize(minExtent.x, minExtent.y);
    frame.contentMaxSize = maxSizeOf(maxExtent);
    // A window of one extent has no resize handles and no zoom. AppKit keeps the style of a fullscreen window. An
    // overlay has no border to resize by.
    bool fixed = maxExtent.x != 0 && maxExtent.y != 0 && minExtent.x == maxExtent.x && minExtent.y == maxExtent.y;
    if ((frame.styleMask & NSWindowStyleMaskFullScreen) == 0 && (frame.styleMask & NSWindowStyleMaskTitled) != 0) {
        if (fixed)
            frame.styleMask &= ~NSWindowStyleMaskResizable;
        else
            frame.styleMask |= NSWindowStyleMaskResizable;
    }
    NSSize size = frame.contentLayoutRect.size;
    NSSize max = frame.contentMaxSize;
    NSSize limited = NSMakeSize(MIN(MAX(size.width, CGFloat(minExtent.x)), max.width),
        MIN(MAX(size.height, CGFloat(minExtent.y)), max.height));
    if (!NSEqualSizes(limited, size))
        [frame setContentSize:limited];
}

PrAppWindowState state(const PrAppWindow* window) {
    PrAppKitWindow* object = appKitObjectOf(window);
    NSWindow* frame = object.window;
    if (object.host.headless)
        return PrAppWindowState_Normal;
    if ((frame.styleMask & NSWindowStyleMaskFullScreen) != 0)
        return PrAppWindowState_Fullscreen;
    if (frame.miniaturized)
        return PrAppWindowState_Minimized;
    return frame.zoomed ? PrAppWindowState_Maximized : PrAppWindowState_Normal;
}

bool position(const PrAppWindow* window, PrAppDisplayPosition* out) {
    PrAppKitWindow* object = appKitObjectOf(window);
    NSScreen* screen = object.window.screen;
    if (object.host.headless || screen == nil)
        return false;
    PrRect content = appKitRectOnScreen(screen, [object.window contentRectForFrameRect:object.window.frame]);
    *out = PrAppDisplayPosition{.display = appKitDisplayIdOf(screen), .position = PrVec2{content.left, content.top}};
    return true;
}

bool pointToDisplay(const PrAppWindow* window, PrVec2 point, PrAppDisplayPosition* out) {
    PrAppKitWindow* object = appKitObjectOf(window);
    if (object.host.headless)
        return false;
    NSRect content = [object.window contentRectForFrameRect:object.window.frame];
    return appKitDisplayPositionAt(NSMakePoint(NSMinX(content) + CGFloat(point.x), NSMaxY(content) - CGFloat(point.y)), out);
}

bool pointFromDisplay(const PrAppWindow* window, const PrAppDisplayPosition* position, PrVec2* out) {
    PrAppKitWindow* object = appKitObjectOf(window);
    NSScreen* screen = appKitScreenOf(position->display);
    if (object.host.headless || screen == nil)
        return false;
    NSRect frame = screen.frame;
    NSRect content = [object.window contentRectForFrameRect:object.window.frame];
    CGFloat x = NSMinX(frame) + CGFloat(position->position.x);
    CGFloat y = NSMaxY(frame) - CGFloat(position->position.y);
    *out = PrVec2{float(x - NSMinX(content)), float(NSMaxY(content) - y)};
    return true;
}

void setState(PrAppWindow* window, PrAppWindowState wanted);

void setPosition(PrAppWindow* window, const PrAppDisplayPosition* position) {
    PrAppKitWindow* object = appKitObjectOf(window);
    NSWindow* frame = object.window;
    if (object.host.headless || (frame.styleMask & NSWindowStyleMaskFullScreen) != 0)
        return;
    if (frame.zoomed)
        setState(window, PrAppWindowState_Normal);
    NSScreen* screen = appKitScreenOf(position->display);
    if (screen != nil) {
        appKitPlaceOnScreen(frame, screen, position->position);
        return;
    }
    // A display that is gone gives the main display, at the same position if the window fits there.
    screen = NSScreen.screens.firstObject;
    NSRect content = [frame contentRectForFrameRect:frame.frame];
    PrRect work = appKitRectOnScreen(screen, screen.visibleFrame);
    bool fits = position->position.x >= work.left && position->position.y >= work.top &&
        position->position.x + float(content.size.width) <= work.right &&
        position->position.y + float(content.size.height) <= work.bottom;
    if (fits)
        appKitPlaceOnScreen(frame, screen, position->position);
    else
        appKitCenterOnScreen(frame, screen);
}

void setState(PrAppWindow* window, PrAppWindowState wanted) {
    PrAppKitWindow* object = appKitObjectOf(window);
    NSWindow* frame = object.window;
    if (object.host.headless)
        return;
    PrAppWindowState current = state(window);
    if (current == wanted)
        return;
    // A fullscreen window can take no other state until AppKit has moved it out of fullscreen.
    if (current == PrAppWindowState_Fullscreen) {
        object.afterFullscreen = wanted;
        [frame toggleFullScreen:nil];
        return;
    }
    if (current == PrAppWindowState_Minimized)
        [frame deminiaturize:nil];
    switch (wanted) {
        case PrAppWindowState_Normal:
            if (frame.zoomed)
                [frame zoom:nil];
            break;
        case PrAppWindowState_Minimized:
            [frame miniaturize:nil];
            break;
        case PrAppWindowState_Maximized:
            if (!frame.zoomed)
                [frame zoom:nil];
            break;
        case PrAppWindowState_Fullscreen:
            [frame toggleFullScreen:nil];
            break;
        default:
            break;
    }
}

// An overlay is the key window without its application being active.
bool isActive(const PrAppWindow* window) {
    PrAppKitWindow* object = appKitObjectOf(window);
    return object.window.isKeyWindow && (NSApp.isActive || object.overlay);
}

void focus(PrAppWindow* window) {
    PrAppKitWindow* object = appKitObjectOf(window);
    object.shown = YES;
    object.showing = NO;
    if (object.host.headless)
        return;
    if (NSApp.isHidden && !object.overlay)
        [NSApp unhide:nil];
    if (object.window.miniaturized)
        [object.window deminiaturize:nil];
    [object.window makeKeyAndOrderFront:nil];
    if (!object.overlay)
        [NSApp activate];
}

void setVisible(PrAppWindow* window, bool visible) {
    PrAppKitWindow* object = appKitObjectOf(window);
    if (object.shown == visible)
        return;
    object.shown = visible;
    if (object.host.headless)
        return;
    if (visible) {
        [object showSoon];
        return;
    }
    object.showing = NO;
    [object freePointer];
    [object dismiss];
    [object.window orderOut:nil];
}

bool isVisible(const PrAppWindow* window) {
    return appKitObjectOf(window).shown;
}

void setBackdrop(PrAppWindow* window, PrAppWindowBackdrop backdrop) {
    [appKitObjectOf(window) setBackdropOf:backdrop];
}

void setPointerPassthrough(PrAppWindow* window, bool passthrough) {
    appKitObjectOf(window).window.ignoresMouseEvents = passthrough;
}

// The window locks the pointer for Locked and LockedRaw alike, as AppKit reports only the cursor's movement.
void setPointerMode(PrAppWindow* window, PrAppPointerMode mode) {
    PrAppKitWindow* object = appKitObjectOf(window);
    object.lockedMode = mode;
    [object setConfinement:mode == PrAppPointerMode_Confined];
    [object setLock:mode == PrAppPointerMode_Locked || mode == PrAppPointerMode_LockedRaw];
}

PrAppPointerMode pointerMode(const PrAppWindow* window) {
    PrAppKitWindow* object = appKitObjectOf(window);
    return object.locked || object.confined ? object.lockedMode : PrAppPointerMode(PrAppPointerMode_Free);
}

void setAspectRatio(PrAppWindow* window, float ratio) {
    NSWindow* frame = appKitObjectOf(window).window;
    // The aspect ratio and the resize increments exclude each other, so increments of one point clear the ratio.
    if (ratio > 0)
        frame.contentAspectRatio = NSMakeSize(CGFloat(ratio), 1);
    else
        frame.contentResizeIncrements = NSMakeSize(1, 1);
}

void setPopupPlacement(PrAppWindow* window, const PrAppPopupPlacement* placement) {
    PrAppKitWindow* object = appKitObjectOf(window);
    object.popup = *placement;
    [object place];
}

// A headless window is drawn off the screen, where nothing covers it.
bool isOccluded(const PrAppWindow* window) {
    PrAppKitWindow* object = appKitObjectOf(window);
    if (object.host.headless || !object.window.isVisible)
        return false;
    return (object.window.occlusionState & NSWindowOcclusionStateVisible) == 0;
}

void setOpacity(PrAppWindow* window, float opacity) {
    appKitObjectOf(window).window.alphaValue = CGFloat(MIN(MAX(opacity, 0.0f), 1.0f));
}

void setKeepAbove(PrAppWindow* window, bool above) {
    PrAppKitWindow* object = appKitObjectOf(window);
    if (object.role == PrAppWindowRole_Normal)
        object.window.level = above ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

// The Dock shows one progress for the application, so the window that set it last decides it.
void setProgress(PrAppWindow* window, PrAppProgressState state, float progress) {
    PrAppKitWindow* object = appKitObjectOf(window);
    if (!object.host.headless)
        [object.host setDockProgress:state value:progress];
}

void setDocument(PrAppWindow* window, PrStringView path, bool edited) {
    NSWindow* frame = appKitObjectOf(window).window;
    frame.representedURL = path.size != 0 ? [NSURL fileURLWithPath:appKitStringOf(path)] : nil;
    frame.documentEdited = edited;
}

void requestAttention(PrAppWindow*) {
    // An informational request bounces the Dock icon once, and does nothing while the application is active.
    [NSApp requestUserAttention:NSInformationalRequest];
}

PrStatus surface(const PrAppWindow* window, PrSurface* out) {
    out->kind = PrSurfaceKind_NsView;
    out->handle = (__bridge void*)appKitObjectOf(window).view;
    return PrStatus_Ok;
}

} // namespace

PrAppFrame appKitFrameState(PrAppWindow* window) {
    return [appKitObjectOf(window) frameAt:appKitUptime()];
}

void appKitWindowDestroy(PrAppWindow* window) {
    AppKitWindow* appKit = static_cast<AppKitWindow*>(window);
    if (appWindowDeliveryDeferDestroy(appKit->delivery))
        return;
    PrAppKitWindow* object = (__bridge_transfer PrAppKitWindow*)appKit->object;
    [object.host forget:window];
    // A file dialog on the window closes as the user's cancel.
    if (NSWindow* sheet = object.window.attachedSheet)
        [object.window endSheet:sheet returnCode:NSModalResponseCancel];
    [object dismiss];
    [object setSecureInput:NO];
    forgetTextInput(window);
    [object endPromises];
    object.menuBar = nil;
    [object close];
    [object.host showMenuBar];
    AppKitDrag& drag = static_cast<AppKitApp*>(appKit->app)->drag;
    if (drag.window == window)
        drag.window = nullptr;
    appTitleBarRegionsReset(appKit->titleBar);
    appDragPendingReset(appKit->drag);
    prRawStringReset(&appKit->title);
    prMemoryFree(appKit);
}

void appKitRequestFrame(PrAppWindow* window) {
    [appKitObjectOf(window) requestFrame];
}

void appKitRequestUnpacedFrame(PrAppWindow* window) {
    [appKitObjectOf(window) requestUnpacedFrame];
}

void appKitRequestFrameAt(PrAppWindow* window, double time) {
    [appKitObjectOf(window) requestFrameAt:time];
}

void appKitSetCursor(PrAppWindow* window, PrAppCursor cursor) {
    // A cursor is made once per shape, so that setting the same shape again finds the view's cursor unchanged. Only the
    // main thread reads the table.
    static NSCursor* made[PrAppCursor_Count];
    PrAppCursor shape = cursor < PrAppCursor_Count ? cursor : PrAppCursor_Arrow;
    if (made[shape] == nil)
        made[shape] = appKitCursorFor(shape);
    PrAppKitWindow* object = appKitObjectOf(window);
    NSCursor* selected = made[shape];
    if (object.view.cursor == selected)
        return;
    object.view.cursor = selected;
    [object.window invalidateCursorRectsForView:object.view];
}

void appKitApplyTextInput(PrAppWindow* window) {
    const AppKitWindow* source = textInputSource(window);
    if (source != nullptr)
        setTextInputRect(window, source->textInput);
    PrAppKitWindow* object = appKitObjectOf(window);
    bool password = source != nullptr && source->textInputKind == PrAppTextInputKind_Password;
    [object setSecureInput:password && object.window.isKeyWindow];
}

PrStatus appKitWindowCreate(PrApp* app, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out) {
    PrAppKitHost* host = appKitHostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    if (static_cast<AppKitApp*>(app)->windowModel == PrAppWindowModel_Single && host.windows.count != 0)
        return PrStatus_NotSupported;
    AppKitWindow* window = allocZeroed<AppKitWindow>();
    window->destroyFn = appKitWindowDestroy;
    window->appFn = windowApp;
    window->requestFrameFn = appKitRequestFrame;
    window->requestUnpacedFrameFn = appKitRequestUnpacedFrame;
    window->setTitleFn = setTitle;
    window->titleFn = title;
    window->setExtentFn = setExtent;
    window->setExtentLimitsFn = setExtentLimits;
    window->positionFn = position;
    window->setPositionFn = setPosition;
    window->pointToDisplayFn = pointToDisplay;
    window->pointFromDisplayFn = pointFromDisplay;
    window->setStateFn = setState;
    window->stateFn = state;
    window->isActiveFn = isActive;
    window->focusFn = focus;
    window->requestAttentionFn = requestAttention;
    window->setVisibleFn = setVisible;
    window->isVisibleFn = isVisible;
    window->setBackdropFn = setBackdrop;
    window->setPointerPassthroughFn = setPointerPassthrough;
    window->setPointerModeFn = setPointerMode;
    window->pointerModeFn = pointerMode;
    window->dragStartFn = appKitDragStart;
    window->shareFn = appKitShare;
    window->surfaceFn = surface;
    window->setAspectRatioFn = setAspectRatio;
    window->setPopupPlacementFn = setPopupPlacement;
    window->isOccludedFn = isOccluded;
    window->setOpacityFn = setOpacity;
    window->setKeepAboveFn = setKeepAbove;
    window->setProgressFn = setProgress;
    window->setDocumentFn = setDocument;
    window->setMenuBarFn = appKitWindowSetMenuBar;
    window->showMenuFn = appKitShowMenu;
    window->dragPromiseEndFn = appKitDragPromiseEnd;
    window->app = app;
    window->delivery.handler = desc->handler;
    PrAppKitWindow* object = [[PrAppKitWindow alloc] init];
    object.record = window;
    object.host = host;
    object.role = desc->role;
    object.overlay = desc->role == PrAppWindowRole_Overlay;
    object.shown = !desc->hidden;
    bool popup = desc->role == PrAppWindowRole_Popup;
    bool dialog = desc->role == PrAppWindowRole_Dialog;
    if (desc->parent != nullptr && (popup || dialog))
        object.parent = appKitObjectOf(desc->parent);
    object.modal = dialog && desc->modal && object.parent != nil;
    object.sheet = object.modal && desc->decoration != PrAppWindowDecoration_None;
    object.popup = desc->popup;
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
        NSWindowStyleMaskMiniaturizable;
    // A dialog follows its parent, which is minimized in its place.
    if (dialog)
        style &= ~NSWindowStyleMaskMiniaturizable;
    // An overlay and a popup are borderless panels, which never activate the application.
    bool paneled = object.overlay || popup;
    if (paneled)
        style = NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel;
    bool borderless = !paneled && desc->decoration == PrAppWindowDecoration_None;
    if (borderless)
        style = NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;
    // A custom title bar keeps the system's title bar and its buttons, transparent, over content that fills the window.
    bool custom = desc->decoration == PrAppWindowDecoration_Custom && !paneled;
    if (custom)
        style |= NSWindowStyleMaskFullSizeContentView;
    NSSize size = NSMakeSize(desc->extent.x, desc->extent.y);
    if (!host.headless) {
        NSRect available = [NSWindow contentRectForFrameRect:NSScreen.mainScreen.visibleFrame styleMask:style];
        size = NSMakeSize(MIN(size.width, available.size.width), MIN(size.height, available.size.height));
    }
    NSRect content = NSMakeRect(0, 0, size.width, size.height);
    if (object.overlay) {
        PrAppKitPanel* panel = [[PrAppKitPanel alloc] initWithContentRect:content styleMask:style
            backing:NSBackingStoreBuffered defer:NO];
        panel.floatingPanel = YES;
        panel.hidesOnDeactivate = NO;
        panel.becomesKeyOnlyIfNeeded = NO;
        // Above the menu bar's level, the panel shows over fullscreen windows, and on every space.
        panel.level = NSStatusWindowLevel;
        panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorStationary |
            NSWindowCollectionBehaviorIgnoresCycle;
        panel.hasShadow = NO;
        object.window = panel;
    } else if (popup) {
        PrAppKitPanel* panel = [[PrAppKitPanel alloc] initWithContentRect:content styleMask:style
            backing:NSBackingStoreBuffered defer:NO];
        panel.refusesKey = YES;
        panel.hidesOnDeactivate = NO;
        panel.level = NSPopUpMenuWindowLevel;
        panel.collectionBehavior = NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle;
        panel.hasShadow = YES;
        object.window = panel;
    } else if (borderless) {
        object.window = [[PrAppKitBorderlessWindow alloc] initWithContentRect:content styleMask:style
            backing:NSBackingStoreBuffered defer:NO];
        object.window.hasShadow = YES;
    } else {
        object.window = [[NSWindow alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered defer:NO];
    }
    object.window.title = appKitStringOf(desc->title);
    window->title = prRawStringCopy(desc->title);
    object.window.releasedWhenClosed = NO;
    object.window.ignoresMouseEvents = desc->pointerPassthrough;
    object.afterFullscreen = PrAppWindowState_Fullscreen;
    if (custom) {
        object.customTitleBar = YES;
        object.window.titlebarAppearsTransparent = YES;
        object.window.titleVisibility = NSWindowTitleHidden;
        // The program's drag regions move the window, and nothing else in the content does.
        object.window.movable = NO;
    }
    object.view = [[PrAppKitView alloc] initWithFrame:object.window.contentView.bounds];
    object.view.owner = object;
    object.view.host = host;
    object.view.cursor = NSCursor.arrowCursor;
    object.view.wantsLayer = YES;
    object.view.allowedTouchTypes = NSTouchTypeMaskIndirect;
    object.view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    object.view.layer.backgroundColor = NSColor.blackColor.CGColor;
    object.window.contentView = object.view;
    object.window.delegate = object;
    NSMutableArray<NSPasteboardType>* types = [NSMutableArray arrayWithArray:appKitImageTypes()];
    [types addObjectsFromArray:@[
        NSPasteboardTypeString, NSPasteboardTypeHTML, NSPasteboardTypeURL, NSPasteboardTypeFileURL, kPayloadType,]];
    [object.view registerForDraggedTypes:types];
    [object.window makeFirstResponder:object.view];
    object.transparent = desc->transparent || desc->backdrop != PrAppWindowBackdrop_Opaque;
    [object setBackdropOf:desc->backdrop];
    if (host.headless) {
        [object.window setFrameOrigin:NSMakePoint(-32000, -32000)];
    } else {
        // By default, a window cascades from the last, and the first is centered. A dialog is centered on its parent,
        // and a popup goes where its placement puts it.
        PrAppKitWindow* previous = host.windows.lastObject;
        NSScreen* chosen = desc->placement == PrAppWindowPlacement_Position ? appKitScreenOf(desc->position.display) : nil;
        NSWindow* parent = object.parent.window;
        if (popup && parent != nil) {
            [object place];
        } else if (chosen != nil) {
            appKitPlaceOnScreen(object.window, chosen, desc->position.position);
        } else if (parent != nil) {
            NSRect around = parent.frame;
            NSRect frame = object.window.frame;
            [object.window setFrameOrigin:NSMakePoint(NSMidX(around) - frame.size.width / 2,
                NSMidY(around) - frame.size.height / 2)];
        } else if (desc->placement == PrAppWindowPlacement_Center) {
            NSScreen* active = NSApp.keyWindow.screen;
            appKitCenterOnScreen(object.window, active != nil ? active : NSScreen.screens.firstObject);
        } else if (previous != nil) {
            [object.window cascadeTopLeftFromPoint:NSMakePoint(NSMinX(previous.window.frame),
                NSMaxY(previous.window.frame))];
        } else {
            [object.window center];
        }
        if (object.shown)
            [object present:YES];
    }
    [host.windows addObject:object];
    window->object = (__bridge_retained void*)object;
    setExtentLimits(window, desc->minExtent, desc->maxExtent);
    if (object.shown && desc->state != PrAppWindowState_Normal)
        setState(window, desc->state);
    [object requestFrame];
    *out = window;
    return PrStatus_Ok;
}

} // namespace pr_impl

using namespace pr_impl;

@implementation PrAppKitPanel
- (BOOL)canBecomeKeyWindow {
    return !self.refusesKey;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

@implementation PrAppKitBorderlessWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

// The Dock has no states of progress, so the bar takes the taskbar's colors: the accent color, yellow while paused and
// red for an error. An indeterminate bar is striped. The tile draws only when the Dock asks it to, so nothing animates.
@implementation PrAppKitDockProgress
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect bounds = self.bounds;
    // An executable outside a bundle has no icon of its own, and the Dock shows the executable's icon from the Finder.
    NSImage* icon = NSApp.applicationIconImage;
    if (!appKitIsBundled())
        icon = [NSWorkspace.sharedWorkspace iconForFile:NSBundle.mainBundle.executablePath];
    [icon drawInRect:bounds];
    CGFloat height = bounds.size.height * 0.12;
    NSRect track = NSMakeRect(bounds.size.width * 0.1, bounds.size.height * 0.1, bounds.size.width * 0.8, height);
    [[NSColor colorWithWhite:0 alpha:0.55] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:track xRadius:height / 2 yRadius:height / 2] fill];
    NSColor* fill = NSColor.controlAccentColor;
    if (self.state == PrAppProgressState_Paused)
        fill = NSColor.systemYellowColor;
    else if (self.state == PrAppProgressState_Error)
        fill = NSColor.systemRedColor;
    NSRect bar = NSInsetRect(track, 2, 2);
    CGFloat radius = bar.size.height / 2;
    if (self.state == PrAppProgressState_Indeterminate) {
        NSBezierPath* clip = [NSBezierPath bezierPathWithRoundedRect:bar xRadius:radius yRadius:radius];
        [NSGraphicsContext saveGraphicsState];
        [clip addClip];
        [[fill colorWithAlphaComponent:0.4] setFill];
        NSRectFill(bar);
        [fill setFill];
        CGFloat stripe = bar.size.height;
        for (CGFloat x = bar.origin.x - stripe; x < NSMaxX(bar); x += stripe * 2) {
            NSBezierPath* band = [NSBezierPath bezierPath];
            [band moveToPoint:NSMakePoint(x, bar.origin.y)];
            [band lineToPoint:NSMakePoint(x + stripe, bar.origin.y)];
            [band lineToPoint:NSMakePoint(x + stripe * 2, NSMaxY(bar))];
            [band lineToPoint:NSMakePoint(x + stripe, NSMaxY(bar))];
            [band closePath];
            [band fill];
        }
        [NSGraphicsContext restoreGraphicsState];
        return;
    }
    CGFloat filled = bar.size.width * CGFloat(MIN(MAX(self.value, 0.0f), 1.0f));
    if (filled <= 0)
        return;
    // A bar narrower than its height keeps its rounded ends.
    bar.size.width = MAX(filled, bar.size.height);
    [fill setFill];
    [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:radius yRadius:radius] fill];
}
@end

@implementation PrAppKitView {
    // The ids of the touches on the trackpad, by their identities (NSTouch::identity).
    NSMutableDictionary<id, NSNumber*>* _touchIds;
}
// The view backs itself with a Metal layer that asks the view to display as its bounds change, and AppKit asks the view
// to display at each step of a live resize. A frame drawn at that point presents within the resize's transaction, at
// the window's new size.
- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.delegate = self;
    layer.needsDisplayOnBoundsChange = YES;
    return layer;
}
- (void)displayLayer:(CALayer*)layer {
    (void)layer;
    [self.owner frame];
}
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)updateTrackingAreas {
    for (NSTrackingArea* area in self.trackingAreas)
        [self removeTrackingArea:area];
    // The windows of the active application track the pointer, as an overlay does whichever application is active. A
    // drag that leaves the view still reports the crossing, which waits for the release.
    NSTrackingAreaOptions options = NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingCursorUpdate |
        NSTrackingEnabledDuringMouseDrag | NSTrackingInVisibleRect;
    options |= self.owner.overlay ? NSTrackingActiveAlways : NSTrackingActiveInActiveApp;
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:options owner:self userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)resetCursorRects {
    [self addCursorRect:self.visibleRect cursor:self.cursor];
}
- (void)cursorUpdate:(NSEvent*)event {
    (void)event;
    [self.cursor set];
}
// Delivers an event to the view's window. A window that a modal dialog blocks takes no input.
- (void)deliver:(const PrAppEvent&)event {
    PrAppKitWindow* owner = self.owner;
    bool input = event.kind >= PrAppEventKind_PointerMove && event.kind <= PrAppEventKind_CompositionEnd;
    if (owner.record != nullptr && !(input && owner.blocker != nil))
        [owner.host deliver:event to:owner.record];
}
// Delivers the pointer's crossing into the view or out of it, at `position`, once for each crossing.
- (void)cross:(PrAppEventKind)kind at:(PrVec2)position time:(double)time modifiers:(NSEventModifierFlags)modifiers {
    PrAppKitWindow* owner = self.owner;
    if (owner == nil)
        return;
    if (kind == PrAppEventKind_PointerEnter) {
        // A pointer that comes back before its buttons are up never left.
        if (owner.leaving) {
            owner.leaving = NO;
            return;
        }
        if (owner.inside)
            return;
        owner.inside = YES;
    } else {
        if (!owner.inside)
            return;
        if ([owner buttonsDown]) {
            owner.leaving = YES;
            return;
        }
        owner.inside = NO;
        owner.leaving = NO;
    }
    PrAppEvent event{};
    event.kind = kind;
    event.time = time;
    event.modifiers = appKitModifiersOf(modifiers);
    event.pointer = PrAppPointerEvent{
        .position = position,
        .type = PrAppPointerType_Mouse,
        .primary = true,
        .id = kPrAppPointerMouse,
    };
    [self deliver:event];
}
- (void)mouseEntered:(NSEvent*)e {
    // A locked pointer stays where it was, and crosses nothing.
    if (self.owner.locked)
        return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    [self cross:PrAppEventKind_PointerEnter at:PrVec2{float(p.x), float(p.y)} time:e.timestamp
        modifiers:e.modifierFlags];
}
- (void)mouseExited:(NSEvent*)e {
    if (self.owner.locked)
        return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    [self cross:PrAppEventKind_PointerLeave at:PrVec2{float(p.x), float(p.y)} time:e.timestamp
        modifiers:e.modifierFlags];
}
- (void)pointer:(NSEvent*)source kind:(PrAppEventKind)kind {
    NSPoint p = [self convertPoint:source.locationInWindow fromView:nil];
    PrVec2 position{float(p.x), float(p.y)};
    PrAppKitWindow* owner = self.owner;
    // A locked pointer stays where the lock began, and reports only its movement.
    if (owner.locked)
        position = owner.lockedAt;
    PrAppEvent event{};
    event.kind = kind;
    event.time = source.timestamp;
    event.modifiers = appKitModifiersOf(source.modifierFlags);
    // The pointer is over the view before it moves or presses there, though the tracking area may not have seen it
    // come.
    if (kind != PrAppEventKind_Scroll && kind != PrAppEventKind_PointerUp && !owner.inside)
        [self cross:PrAppEventKind_PointerEnter at:position time:source.timestamp modifiers:source.modifierFlags];
    [owner pointerAt:position];
    if (kind == PrAppEventKind_Scroll) {
        // A mouse wheel's deltas are lines, and a trackpad's are pixels.
        event.scroll = PrAppScrollEvent{
            .position = position,
            .delta = {-float(source.scrollingDeltaX), -float(source.scrollingDeltaY)},
            .unit = PrAppScrollUnit(source.hasPreciseScrollingDeltas ? PrAppScrollUnit_Pixel : PrAppScrollUnit_Line),
        };
        [self deliver:event];
        return;
    }
    event.pointer = PrAppPointerEvent{
        .position = position,
        .button = uint8_t(source.buttonNumber),
        .type = PrAppPointerType_Mouse,
        .primary = true,
        .id = kPrAppPointerMouse,
    };
    // AppKit's deltas are in points, with y down, as the view's coordinates are.
    if (kind == PrAppEventKind_PointerMove)
        event.pointer.movement = PrVec2{float(source.deltaX), float(source.deltaY)};
    if (source.type == NSEventTypeLeftMouseDown || source.type == NSEventTypeLeftMouseDragged)
        self.press = source;
    else if (source.type == NSEventTypeLeftMouseUp)
        self.press = nil;
    // A pen on a tablet reports as the mouse, with the tablet's measures in the event. Only mouse events have subtypes.
    bool pen = source.subtype == NSEventSubtypeTabletPoint;
    if (pen)
        appKitDescribePen(source, self.owner.eraser, &event);
    // A button that the window released as it stopped being the key window reports no release of its own.
    if (kind == PrAppEventKind_PointerDown) {
        event.pointer.clicks = uint32_t(source.clickCount);
        [self.owner pointerDown:event.pointer.button];
    }
    else if (kind == PrAppEventKind_PointerUp && ![self.owner pointerUp:event.pointer.button])
        return;
    if (!pen)
        event.pointer.pressure = [self.owner buttonsDown] ? 0.5f : 0.0f;
    [self deliver:event];
    // A pointer that left during the press leaves once its last button is up.
    if (kind == PrAppEventKind_PointerUp && owner.leaving && ![owner buttonsDown]) {
        owner.leaving = NO;
        [self cross:PrAppEventKind_PointerLeave at:position time:source.timestamp modifiers:source.modifierFlags];
    }
}
// Records which end of the pen points at the tablet.
- (void)tabletProximity:(NSEvent*)e {
    self.owner.eraser = e.isEnteringProximity && e.pointingDeviceType == NSPointingDeviceTypeEraser;
}
- (void)mouseMoved:(NSEvent*)e {
    [self pointer:e kind:PrAppEventKind_PointerMove];
}
- (void)mouseDragged:(NSEvent*)e {
    [self mouseMoved:e];
}
- (void)rightMouseDragged:(NSEvent*)e {
    [self mouseMoved:e];
}
- (void)otherMouseDragged:(NSEvent*)e {
    [self mouseMoved:e];
}
// A press on a window that is not the key window activates it and reaches the program too.
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
- (void)mouseDown:(NSEvent*)e {
    // A press on a window that a modal dialog blocks gives the dialog the keyboard.
    if (PrAppKitWindow* blocker = self.owner.blocker) {
        [blocker.window makeKeyWindow];
        return;
    }
    if ([self.owner titleBarPress:e])
        return;
    [self pointer:e kind:PrAppEventKind_PointerDown];
}
- (void)rightMouseDown:(NSEvent*)e {
    [self mouseDown:e];
}
- (void)otherMouseDown:(NSEvent*)e {
    [self mouseDown:e];
}
- (void)mouseUp:(NSEvent*)e {
    if ([self.owner titleBarRelease:e])
        return;
    [self pointer:e kind:PrAppEventKind_PointerUp];
}
- (void)rightMouseUp:(NSEvent*)e {
    [self mouseUp:e];
}
- (void)otherMouseUp:(NSEvent*)e {
    [self mouseUp:e];
}
- (void)scrollWheel:(NSEvent*)e {
    [self pointer:e kind:PrAppEventKind_Scroll];
}
// Returns the gesture event of `source`. A gesture that AppKit reports without phases, as a smart magnify or a swipe,
// is recognized at once and has only Begin.
- (PrAppEvent)gesture:(NSEvent*)source kind:(PrAppGestureKind)kind {
    NSPoint p = [self convertPoint:source.locationInWindow fromView:nil];
    PrAppEvent event{};
    event.kind = PrAppEventKind_Gesture;
    event.time = source.timestamp;
    event.modifiers = appKitModifiersOf(source.modifierFlags);
    event.gesture.kind = kind;
    event.gesture.position = PrVec2{float(p.x), float(p.y)};
    event.gesture.phase = PrAppGesturePhase_Begin;
    if ((source.phase & NSEventPhaseChanged) != 0)
        event.gesture.phase = PrAppGesturePhase_Change;
    else if ((source.phase & NSEventPhaseEnded) != 0)
        event.gesture.phase = PrAppGesturePhase_End;
    else if ((source.phase & NSEventPhaseCancelled) != 0)
        event.gesture.phase = PrAppGesturePhase_Cancel;
    return event;
}
- (void)magnifyWithEvent:(NSEvent*)e {
    PrAppEvent event = [self gesture:e kind:PrAppGestureKind_Magnify];
    event.gesture.magnification = float(e.magnification);
    [self deliver:event];
}
// AppKit's rotation is counterclockwise, in degrees.
- (void)rotateWithEvent:(NSEvent*)e {
    PrAppEvent event = [self gesture:e kind:PrAppGestureKind_Rotate];
    event.gesture.rotation = -e.rotation;
    [self deliver:event];
}
- (void)smartMagnifyWithEvent:(NSEvent*)e {
    [self deliver:[self gesture:e kind:PrAppGestureKind_SmartMagnify]];
}
// AppKit's deltas point opposite to the fingers, and its y grows upward. The content moves with the fingers.
- (void)swipeWithEvent:(NSEvent*)e {
    PrAppEvent event = [self gesture:e kind:PrAppGestureKind_Swipe];
    event.gesture.swipe = PrVec2{float(-e.deltaX), float(e.deltaY)};
    [self deliver:event];
}
// Delivers the touches that are on the trackpad. A touch keeps its id while it stays on the trackpad. The ids of the
// touches that left are free for new touches.
- (void)touches:(NSEvent*)e {
    if (_touchIds == nil)
        _touchIds = [NSMutableDictionary dictionary];
    NSSet<NSTouch*>* touching = [e touchesMatchingPhase:NSTouchPhaseTouching inView:self];
    NSMutableDictionary<id, NSNumber*>* kept = [NSMutableDictionary dictionary];
    RawArray<PrAppTrackpadTouch> touches;
    for (NSTouch* touch in touching) {
        NSNumber* id = _touchIds[touch.identity];
        if (id == nil) {
            uint32_t free = 1;
            while ([kept.allValues containsObject:@(free)] || [_touchIds.allValues containsObject:@(free)])
                ++free;
            id = @(free);
        }
        kept[touch.identity] = id;
        NSPoint at = touch.normalizedPosition;
        touches.push(PrAppTrackpadTouch{.id = id.unsignedIntValue, .position = PrVec2{float(at.x), float(1 - at.y)}});
    }
    _touchIds = kept;
    PrAppEvent event{};
    event.kind = PrAppEventKind_TrackpadTouches;
    event.time = e.timestamp;
    event.modifiers = appKitModifiersOf(e.modifierFlags);
    event.trackpad.touches = touches.data;
    event.trackpad.count = touches.count;
    [self deliver:event];
    touches.reset();
}
- (void)touchesBeganWithEvent:(NSEvent*)e {
    [self touches:e];
}
- (void)touchesMovedWithEvent:(NSEvent*)e {
    [self touches:e];
}
- (void)touchesEndedWithEvent:(NSEvent*)e {
    [self touches:e];
}
- (void)touchesCancelledWithEvent:(NSEvent*)e {
    [self touches:e];
}
// Delivers the key event of `source`. If `mark` is not kNoMark, the host holds the events since `mark`, and the key
// goes ahead of them.
- (void)key:(NSEvent*)source kind:(PrAppEventKind)kind composing:(BOOL)composing mark:(uint32_t)mark {
    AppKitMacKey mac = appKitMacKeyOf(source);
    PrAppEvent event{};
    event.kind = kind;
    event.time = source.timestamp;
    event.keyboard.code = mac.code;
    event.modifiers = appKitModifiersOf(source.modifierFlags);
    event.keyboard.repeat = kind == PrAppEventKind_KeyDown && source.isARepeat;
    event.keyboard.composing = composing;
    PrRawString text = prRawStringNull();
    if (mac.key != PrAppKey_Unknown) {
        event.keyboard.key = mac.key;
    } else {
        if (NSString* character = appKitCharacterOf(source)) {
            text = prAppleNSStringToRawString(character);
            event.keyboard.text = prRawStringView(&text);
        }
        // A key that is dead with Option held, as Option-E is in the US layout, means its key in a shortcut.
        NSEventModifierFlags chord =
            NSEventModifierFlagOption | NSEventModifierFlagCommand | NSEventModifierFlagControl;
        bool dead = (source.modifierFlags & chord) == 0 && appKitIsDead(source);
        event.keyboard.key = dead ? PrAppKey_Dead : appKitKeyOfCharacter(source);
    }
    PrAppKitWindow* owner = self.owner;
    [owner key:event.keyboard.code down:kind == PrAppEventKind_KeyDown key:event.keyboard.key];
    if (mark == kNoMark)
        [self deliver:event];
    else if (owner.record != nullptr)
        [owner.host deliver:event to:owner.record ahead:mark];
    prRawStringReset(&text);
}
- (void)key:(NSEvent*)source kind:(PrAppEventKind)kind {
    [self key:source kind:kind composing:NO mark:kNoMark];
}
- (void)keyDown:(NSEvent*)e {
    // A Command chord types nothing.
    if ((e.modifierFlags & NSEventModifierFlagCommand) != 0) {
        [self key:e kind:PrAppEventKind_KeyDown];
        return;
    }
    // The input method types the key's text, if any, through insertText:, or composes it through setMarkedText:. Only
    // then is it known whether the input method took the key for a composition: one was open before the key, or the key
    // opened one. The events of the input method wait, so that the key still comes first.
    BOOL composing = self.marked != nil;
    uint32_t mark = [self.owner.host holdEvents];
    [self interpretKeyEvents:@[ e ]];
    composing = composing || self.marked != nil;
    [self key:e kind:PrAppEventKind_KeyDown composing:composing mark:mark];
}
- (void)keyUp:(NSEvent*)e {
    [self key:e kind:PrAppEventKind_KeyUp];
}
// A modifier key reports a change of the flags, which says whether the key went down or up.
- (void)flagsChanged:(NSEvent*)e {
    AppKitMacKey mac = appKitMacKeyOf(e);
    if (mac.key == PrAppKey_Unknown)
        return;
    [self key:e kind:appKitModifierDown(e, mac.code) ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp];
}
- (void)deliverText:(NSString*)text kind:(PrAppEventKind)kind selection:(NSRange)selection {
    PrRawString utf8 = prAppleNSStringToRawString(text);
    PrAppEvent event{};
    event.kind = kind;
    event.time = appKitUptime();
    event.input.text = prRawStringView(&utf8);
    if (selection.location != NSNotFound)
        event.input.selection = PrAppTextRange{
            appKitWtf8Length(text, selection.location),
            appKitWtf8Length(text, selection.location + selection.length),
        };
    [self deliver:event];
    prRawStringReset(&utf8);
}
// Ends the composition, if there is one, committing `text`, which follows as a Text event.
- (void)commit:(NSString*)text {
    if (self.marked != nil) {
        self.marked = nil;
        [self deliverText:text kind:PrAppEventKind_CompositionEnd selection:NSMakeRange(NSNotFound, 0)];
    }
    if (text.length != 0)
        [self deliverText:text kind:PrAppEventKind_Text selection:NSMakeRange(NSNotFound, 0)];
}

// NSTextInputClient. The view holds no text of its own: the program does. The input method's window opens at the caret
// that the program last gave.
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    [self commit:[string isKindOfClass:NSAttributedString.class] ? [(NSAttributedString*)string string] : string];
}
- (void)doCommandBySelector:(SEL)selector {
    // The program has the KeyDown, and decides what a command key does.
    (void)selector;
}
// An editing item of a menu that the program's view handles, which sends the item's tag to the window. A system text
// field that has the keyboard takes these in the view's place.
- (void)editingItem:(id)sender {
    if (![sender isKindOfClass:NSMenuItem.class])
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_MenuItem;
    event.time = appKitUptime();
    event.tag = uint64_t([(NSMenuItem*)sender tag]);
    [self deliver:event];
}
- (void)cut:(id)sender {
    [self editingItem:sender];
}
- (void)copy:(id)sender {
    [self editingItem:sender];
}
- (void)selectAll:(id)sender {
    [self editingItem:sender];
}
- (void)undo:(id)sender {
    [self editingItem:sender];
}
- (void)redo:(id)sender {
    [self editingItem:sender];
}
- (void)paste:(id)sender {
    [self editingItem:sender];
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString* text = [string isKindOfClass:NSAttributedString.class] ? [(NSAttributedString*)string string] : string;
    if (text.length == 0) {
        if (self.marked != nil) {
            self.marked = nil;
            [self deliverText:@"" kind:PrAppEventKind_CompositionEnd selection:NSMakeRange(NSNotFound, 0)];
        }
        return;
    }
    self.marked = text;
    self.markedSelection = selectedRange;
    [self deliverText:text kind:PrAppEventKind_CompositionUpdate selection:selectedRange];
}
- (void)unmarkText {
    if (self.marked != nil)
        [self commit:self.marked];
}
- (NSRange)selectedRange {
    return self.marked != nil ? self.markedSelection : NSMakeRange(NSNotFound, 0);
}
- (NSRange)markedRange {
    return self.marked != nil ? NSMakeRange(0, self.marked.length) : NSMakeRange(NSNotFound, 0);
}
- (BOOL)hasMarkedText {
    return self.marked != nil;
}
// Of the program's text, the view knows only the marked text.
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (self.marked == nil)
        return nil;
    NSRange known = NSIntersectionRange(range, NSMakeRange(0, self.marked.length));
    if (known.length == 0)
        return nil;
    if (actualRange != nullptr)
        *actualRange = known;
    return [[NSAttributedString alloc] initWithString:[self.marked substringWithRange:known]];
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[ NSUnderlineStyleAttributeName, NSUnderlineColorAttributeName, NSMarkedClauseSegmentAttributeName ];
}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (actualRange != nullptr)
        *actualRange = range;
    NSRect caret = self.textInputRect;
    if (!self.isFlipped)
        caret.origin.y = self.bounds.size.height - caret.origin.y - caret.size.height;
    return [self.window convertRectToScreen:[self convertRect:caret toView:nil]];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return NSNotFound;
}
// NSDraggingDestination. The window answers a drag with the drop operation of its latest output. Each of the drag's
// events requests a frame, so that the program answers the drag's latest position.
- (PrAppDragEvent)dragOf:(id<NSDraggingInfo>)sender {
    PrAppKitWindow* owner = self.owner;
    NSPasteboard* board = sender.draggingPasteboard;
    NSPoint p = [self convertPoint:sender.draggingLocation fromView:nil];
    PrAppDragEvent drag{
        .position = PrVec2{float(p.x), float(p.y)},
        .forms = appKitFormsOf(board),
        .operations = appKitOperationsOf(sender.draggingSourceOperationMask),
    };
    const AppKitDrag& carried = static_cast<AppKitApp*>(owner.host.app)->drag;
    if (appKitCarriesDrag(board, carried)) {
        drag.fromThisWindow = carried.window == owner.record;
        if (carried.hasPayload) {
            drag.forms |= PrAppDataFormFlag_Payload;
            drag.payload = carried.payload;
        }
    }
    return drag;
}
- (void)deliverDrag:(PrAppEventKind)kind drag:(const PrAppDragEvent&)drag {
    PrAppEvent event{.kind = kind, .modifiers = appKitModifiersOf(NSEvent.modifierFlags), .time = appKitUptime(), .drag = drag};
    [self deliver:event];
    [self.owner requestFrame];
}
// Returns the operation that the window's latest output answers a drag with, within what the source allows.
- (PrAppDragOperation)answer:(id<NSDraggingInfo>)sender {
    PrAppWindow* record = self.owner.record;
    if (record == nullptr)
        return PrAppDragOperation_None;
    PrAppDragOperation wanted = static_cast<AppKitWindow*>(record)->dropOperation;
    return appDragAnswer(wanted, appKitOperationsOf(sender.draggingSourceOperationMask));
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    [self deliverDrag:PrAppEventKind_DragEnter drag:[self dragOf:sender]];
    return appKitAnswerOf([self answer:sender], sender.draggingSourceOperationMask);
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    [self deliverDrag:PrAppEventKind_DragOver drag:[self dragOf:sender]];
    return appKitAnswerOf([self answer:sender], sender.draggingSourceOperationMask);
}
- (void)draggingExited:(id<NSDraggingInfo>)sender {
    if (sender != nil)
        [self deliverDrag:PrAppEventKind_DragLeave drag:[self dragOf:sender]];
}
- (BOOL)wantsPeriodicDraggingUpdates {
    return YES;
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    return [self answer:sender] != PrAppDragOperation_None;
}
// Reads the drag's pasteboard into a source, and delivers the drop with it. The pasteboard is the system's only until
// this returns.
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    PrAppDragOperation operation = [self answer:sender];
    PrAppWindow* record = self.owner.record;
    if (operation == PrAppDragOperation_None || record == nullptr)
        return NO;
    PrAppDragEvent drag = [self dragOf:sender];
    drag.operation = operation;
    AppDataStore* store = allocZeroed<AppDataStore>();
    appKitReadDrop(sender.draggingPasteboard, *store);
    drag.drop = appDataSourceCreate(static_cast<AppKitApp*>(self.owner.host.app)->sources, store, nullptr);
    [self deliverDrag:PrAppEventKind_Drop drag:drag];
    return YES;
}
// NSDraggingSource. The window's drag allows the operations of its description, within the application and outside it.
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
        sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    (void)context;
    PrAppWindow* record = self.owner.record;
    return record != nullptr ? appKitMaskOf(static_cast<AppKitWindow*>(record)->drag.operations) : NSDragOperationNone;
}
- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)screenPoint
        operation:(NSDragOperation)operation {
    (void)session;
    (void)screenPoint;
    PrAppKitHost* host = self.host;
    if (PrAppKitWindow* owner = self.owner) {
        [owner endDrag:appKitOperationOf(operation)];
    } else if (host.app != nullptr) {
        // The window closed during the drag, and receives no DragEnd. The drag is over all the same.
        AppKitDrag& carried = static_cast<AppKitApp*>(host.app)->drag;
        if (carried.window == nullptr)
            carried = AppKitDrag{};
    }
    if (host.dragSource == self)
        host.dragSource = nil;
}
@end

@implementation PrAppKitWindow {
    // The promises of the window's last drag, which hold their providers' delegates, since a provider's delegate is a
    // weak reference. The promises that wait for the program, by index, each a pair of its destination and AppKit's
    // completion.
    NSMutableArray<PrAppKitPromise*>* _promises;
    NSMutableDictionary<NSNumber*, NSArray*>* _promised;
    // The pointer buttons that are down, by bit of PrAppPointerEvent::button, where the pointer last was, and the keys
    // that are down, by their position. The window releases them when it stops being the key window, as the releases
    // then go to another window.
    uint32_t _buttons;
    PrVec2 _pointer;
    AppKitHeldKey _keys[PrAppKeyCode_Count];
}
- (void)frame {
    PrAppKitHost* host = self.host;
    // A hidden window has no frames, but for the one that it shows with.
    if (self.record == nullptr || !host.running || (!self.shown && !self.showing))
        return;
    // A frame that comes during a callback, from a run loop that the callback runs, waits for the next.
    if (host.inCallback) {
        [self requestFrame];
        return;
    }
    double now = appKitUptime();
    PrAppFrame frame = [self frameAt:now];
    self.lastFrame = now;
    [host beginCallback];
    appKitDeliverFrame(self.record, frame);
    [host endCallback];
    [self startDrag];
}
// The window as a frame at `now` finds it.
- (PrAppFrame)frameAt:(double)now {
    PrAppFrame frame{};
    frame.time = now;
    frame.delta = self.lastFrame == 0 ? 0 : now - self.lastFrame;
    frame.extent.x = float(self.view.bounds.size.width);
    frame.extent.y = float(self.view.bounds.size.height);
    frame.display = appKitDisplayOf(self.window.screen, self.window.backingScaleFactor);
    frame.titleBar = [self titleBar];
    frame.backdrop = self.backdrop;
    frame.preferences = prAppUserPreferencesDefault();
    PrAppUserPreferences& preferences = frame.preferences;
    // The accent color is the system's, in sRGB, unless the user chose the multicolor accent, which leaves it to the
    // program.
    NSColor* accent = [NSColor.controlAccentColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (accent != nil && [NSUserDefaults.standardUserDefaults objectForKey:@"AppleAccentColor"] != nil) {
        preferences.accentColor = PrColor{
            float(accent.redComponent), float(accent.greenComponent), float(accent.blueComponent), 1,
        };
    }
    NSArray<NSAppearanceName>* names = @[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ];
    NSAppearanceName appearance = [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:names];
    if ([appearance isEqualToString:NSAppearanceNameDarkAqua])
        preferences.colorScheme = PrAppColorScheme_Dark;
    NSWorkspace* workspace = NSWorkspace.sharedWorkspace;
    if (workspace.accessibilityDisplayShouldIncreaseContrast)
        preferences.contrast = PrAppContrast_More;
    preferences.reducedMotion = workspace.accessibilityDisplayShouldReduceMotion;
    preferences.reducedTransparency = workspace.accessibilityDisplayShouldReduceTransparency;
    return frame;
}
// A custom title bar: the band that the system's transparent title bar covers, and the room of the system's buttons, at
// the leading edge in a left-to-right layout and the trailing edge otherwise. A fullscreen window hides its title bar.
- (PrAppTitleBar)titleBar {
    NSWindow* window = self.window;
    if (!self.customTitleBar || (window.styleMask & NSWindowStyleMaskFullScreen) != 0)
        return PrAppTitleBar{};
    PrAppTitleBar titleBar{};
    titleBar.height = float(window.frame.size.height - window.contentLayoutRect.size.height);
    NSButton* close = [window standardWindowButton:NSWindowCloseButton];
    NSButton* zoom = [window standardWindowButton:NSWindowZoomButton];
    if (close == nil || zoom == nil)
        return titleBar;
    // The space after the buttons matches the space before the first, as the system's own title bars leave it.
    CGFloat left = MIN(NSMinX(close.frame), NSMinX(zoom.frame));
    CGFloat right = MAX(NSMaxX(close.frame), NSMaxX(zoom.frame));
    CGFloat width = self.view.bounds.size.width;
    if (left < width / 2)
        titleBar.leading = float(right + left);
    else
        titleBar.trailing = float(width - left + (width - right));
    return titleBar;
}
// Takes a press of the primary button in the program's title bar, and returns true if the press was the system's.
- (BOOL)titleBarPress:(NSEvent*)event {
    if (!self.customTitleBar || self.record == nullptr || event.buttonNumber != 0)
        return NO;
    NSPoint p = [self.view convertPoint:event.locationInWindow fromView:nil];
    const AppTitleBarRegions& regions = static_cast<AppKitWindow*>(self.record)->titleBar;
    switch (appTitleBarRegionAt(regions, PrVec2{float(p.x), float(p.y)})) {
        case PrAppTitleBarRegionKind_Drag:
            if (event.clickCount == 2) {
                // The user chooses what a double click on a title bar does, in the Desktop & Dock settings.
                NSString* action = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
                if ([action isEqualToString:@"Minimize"])
                    [self.window miniaturize:nil];
                else if (![action isEqualToString:@"None"])
                    [self.window zoom:nil];
            } else {
                [self.window performWindowDragWithEvent:event];
            }
            return YES;
        // A window button acts on its release over it, as the system's own buttons do.
        case PrAppTitleBarRegionKind_Minimize:
        case PrAppTitleBarRegionKind_Maximize:
        case PrAppTitleBarRegionKind_Close:
            self.pressedButton = appTitleBarRegionAt(regions, PrVec2{float(p.x), float(p.y)});
            return YES;
        default:
            return NO;
    }
}
// Takes the release of a window button's press, and returns true if there was one. The button acts if the release is
// over it.
- (BOOL)titleBarRelease:(NSEvent*)event {
    PrAppTitleBarRegionKind pressed = self.pressedButton;
    if (pressed == PrAppTitleBarRegionKind_Client || event.buttonNumber != 0)
        return NO;
    self.pressedButton = PrAppTitleBarRegionKind_Client;
    if (self.record == nullptr)
        return YES;
    NSPoint p = [self.view convertPoint:event.locationInWindow fromView:nil];
    const AppTitleBarRegions& regions = static_cast<AppKitWindow*>(self.record)->titleBar;
    if (appTitleBarRegionAt(regions, PrVec2{float(p.x), float(p.y)}) != pressed)
        return YES;
    switch (pressed) {
        case PrAppTitleBarRegionKind_Minimize: [self.window miniaturize:nil]; break;
        case PrAppTitleBarRegionKind_Maximize: [self.window zoom:nil]; break;
        default: [self.window performClose:nil]; break;
    }
    return YES;
}
// Shows the blur, the transparency, or neither, behind the view. A blur is a visual effect view that holds the view, as
// the window's content.
- (void)setBackdropOf:(PrAppWindowBackdrop)backdrop {
    if (!self.transparent)
        backdrop = PrAppWindowBackdrop_Opaque;
    self.backdrop = backdrop;
    NSWindow* window = self.window;
    bool opaque = backdrop == PrAppWindowBackdrop_Opaque;
    window.opaque = opaque;
    window.backgroundColor = opaque ? NSColor.windowBackgroundColor : NSColor.clearColor;
    self.view.layer.backgroundColor = opaque ? NSColor.blackColor.CGColor : NSColor.clearColor.CGColor;
    bool blur = backdrop == PrAppWindowBackdrop_Blur;
    bool focused = window.firstResponder == self.view;
    if (blur && self.effect == nil) {
        // A HUD's blur stays whether or not the window is active.
        NSVisualEffectView* effect = [[NSVisualEffectView alloc] initWithFrame:self.view.frame];
        effect.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        effect.material = NSVisualEffectMaterialHUDWindow;
        effect.state = NSVisualEffectStateActive;
        self.effect = effect;
        window.contentView = effect;
        self.view.frame = effect.bounds;
        self.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [effect addSubview:self.view];
    } else if (!blur && self.effect != nil) {
        [self.view removeFromSuperview];
        self.effect = nil;
        window.contentView = self.view;
    }
    if (focused && window.firstResponder != self.view)
        [window makeFirstResponder:self.view];
    [self requestFrame];
}
// Locks the pointer to the view where it is, or releases it. Only the key window locks it.
- (void)setLock:(BOOL)locked {
    if (self.locked == locked)
        return;
    if (locked && (self.host.headless || !self.window.isKeyWindow || (!NSApp.isActive && !self.overlay)))
        return;
    self.locked = locked;
    if (locked) {
        self.lockedAt = _pointer;
        CGAssociateMouseAndMouseCursorPosition(false);
        [NSCursor hide];
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
    }
    [self requestFrame];
}
// Confines the pointer to the view, or frees it. Only the key window confines it. AppKit has no confinement, so the
// window moves the cursor back inside whenever it leaves, and drops the mouse's events from outside.
- (void)setConfinement:(BOOL)confined {
    if (self.confined == confined)
        return;
    if (confined && (self.host.headless || !self.window.isKeyWindow || (!NSApp.isActive && !self.overlay)))
        return;
    self.confined = confined;
    if (!confined) {
        [NSEvent removeMonitor:self.confineMonitor];
        self.confineMonitor = nil;
        self.window.acceptsMouseMovedEvents = self.acceptedMoves;
        return;
    }
    // The key window then receives the mouse's moves outside its frame, which bring the cursor back.
    self.acceptedMoves = self.window.acceptsMouseMovedEvents;
    self.window.acceptsMouseMovedEvents = YES;
    __weak PrAppKitWindow* weak = self;
    NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
        NSEventMaskOtherMouseDragged | NSEventMaskMouseExited;
    self.confineMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent* PR_NULLABLE(NSEvent* e) {
        return [weak keepPointerInside] ? nil : e;
    }];
    [self keepPointerInside];
}
// Moves the cursor to the nearest point inside the view while the pointer is confined. Returns true if it was outside.
- (BOOL)keepPointerInside {
    if (!self.confined)
        return NO;
    NSPoint at = NSEvent.mouseLocation;
    NSRect frame = [self.window convertRectToScreen:[self.view convertRect:self.view.bounds toView:nil]];
    NSPoint inside = NSMakePoint(MIN(MAX(at.x, NSMinX(frame) + 1), NSMaxX(frame) - 1),
        MIN(MAX(at.y, NSMinY(frame) + 1), NSMaxY(frame) - 1));
    if (NSEqualPoints(at, inside))
        return NO;
    // Quartz's global coordinates have their origin at the top left of the main display, with y down.
    CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    CGWarpMouseCursorPosition(CGPointMake(inside.x, top - inside.y));
    // A warp holds back the mouse's events for a moment, which associating the mouse with the cursor again ends.
    CGAssociateMouseAndMouseCursorPosition(true);
    return YES;
}
// Releases a pointer that the window locks or confines.
- (void)freePointer {
    [self setLock:NO];
    [self setConfinement:NO];
}
// Shows the window once no callback runs, after a frame that draws what it shows.
- (void)showSoon {
    self.showing = YES;
    __weak PrAppKitWindow* weak = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        PrAppKitWindow* window = weak;
        if (window == nil || window.record == nullptr || !window.showing)
            return;
        if (window.host.inCallback) {
            [window showSoon];
            return;
        }
        [window frame];
        if (window.record == nullptr || !window.showing)
            return;
        window.showing = NO;
        [window present:NO];
    });
}
// Shows the window as its role does: an overlay without taking the keyboard, a popup and a dialog over their parent, a
// modal dialog with decoration as a sheet on it, and any other window in front, made the key window and active if
// `activate`. A modal dialog without decoration blocks its parent.
- (void)present:(BOOL)activate {
    NSWindow* parent = self.parent.window;
    if (self.overlay) {
        [self.window orderFrontRegardless];
    } else if (self.sheet && parent != nil) {
        if (self.window.sheetParent == nil)
            [parent beginSheet:self.window completionHandler:nil];
    } else if (parent != nil) {
        [self.window orderFront:nil];
        [parent addChildWindow:self.window ordered:NSWindowAbove];
        if (self.modal)
            [self.parent blockBy:self];
        if (self.role == PrAppWindowRole_Dialog)
            [self.window makeKeyWindow];
    } else if (activate) {
        [self.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    } else {
        [self.window orderFront:nil];
    }
}
// Detaches the window from its parent, as it hides or closes. A sheet ends, and a modal dialog stops blocking.
- (void)dismiss {
    if (self.parent.blocker == self)
        [self.parent blockBy:nil];
    if (NSWindow* sheetParent = self.window.sheetParent)
        [sheetParent endSheet:self.window];
    if (NSWindow* parent = self.window.parentWindow)
        [parent removeChildWindow:self.window];
}
// Blocks the window's input while `blocker`, a modal dialog without decoration, shows, and dims its content, or ends
// the block for nil.
- (void)blockBy:(PrAppKitWindow* PR_NULLABLE)blocker {
    self.blocker = blocker;
    if (blocker == nil) {
        [self.dim removeFromSuperlayer];
        self.dim = nil;
        return;
    }
    [self freePointer];
    [self releaseHeld];
    if (self.dim != nil)
        return;
    CALayer* dim = [CALayer layer];
    dim.backgroundColor = [NSColor colorWithWhite:0 alpha:0.2].CGColor;
    dim.frame = self.view.layer.bounds;
    dim.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    dim.zPosition = 1;
    [self.view.layer addSublayer:dim];
    self.dim = dim;
}
// Moves a popup to where its placement puts it against its parent.
- (void)place {
    NSWindow* parent = self.parent.window;
    if (self.role != PrAppWindowRole_Popup || parent == nil || self.host.headless)
        return;
    NSRect content = [self.window contentRectForFrameRect:self.window.frame];
    NSRect placed = appKitPopupFrame(parent, content.size, self.popup);
    [self.window setFrame:[self.window frameRectForContentRect:placed] display:YES];
}
// Holds or releases secure event input, which keeps other processes from reading the keys of a password. The system
// counts the holds, so the window holds at most one.
- (void)setSecureInput:(BOOL)secure {
    if (secure == _secureInput)
        return;
    _secureInput = secure;
    if (secure)
        EnableSecureEventInput();
    else
        DisableSecureEventInput();
}
- (void)displayFrame:(CADisplayLink*)sender {
    if (!self.frameRequested)
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
        __weak PrAppKitWindow* weak = self;
        self.timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:NO block:^(NSTimer*) {
            PrAppKitWindow* window = weak;
            window.timer = nil;
            [window frame];
        }];
        [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
        return;
    }
    self.frameRequested = YES;
    if (self.displayLink == nil) {
        self.displayLink = [self.view displayLinkWithTarget:self selector:@selector(displayFrame:)];
        [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    self.displayLink.paused = NO;
}
// A timer of no delay delivers the frame, so the display link, which stops while the display sleeps, does not pace it.
- (void)requestUnpacedFrame {
    if (self.record == nullptr || !self.host.running || self.timer != nil)
        return;
    __weak PrAppKitWindow* weak = self;
    self.timer = [NSTimer timerWithTimeInterval:0 repeats:NO block:^(NSTimer*) {
        PrAppKitWindow* window = weak;
        window.timer = nil;
        [window frame];
    }];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (void)requestFrameAt:(double)time {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    if (self.record == nullptr || !self.host.running || time == 0)
        return;
    double delay = MAX(time - appKitUptime(), 0.0);
    __weak PrAppKitWindow* weak = self;
    self.frameTimer = [NSTimer timerWithTimeInterval:delay repeats:NO block:^(NSTimer*) {
        PrAppKitWindow* window = weak;
        window.frameTimer = nil;
        [window requestFrame];
    }];
    [NSRunLoop.mainRunLoop addTimer:self.frameTimer forMode:NSRunLoopCommonModes];
}
// Stops the window's frames, which the run's end does for every window.
- (void)stopFrames {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    [self.displayLink invalidate];
    self.displayLink = nil;
    [self.timer invalidate];
    self.timer = nil;
}
- (void)close {
    [self freePointer];
    self.record = nullptr;
    [self stopFrames];
    self.window.delegate = nil;
    [self.window orderOut:nil];
    [self.window close];
    self.view.owner = nil;
}
- (void)pointerAt:(PrVec2)position {
    _pointer = position;
}
- (void)pointerDown:(uint8_t)button {
    if (button < kPointerButtons)
        _buttons |= 1u << button;
}
// Records a button's release, and returns false for a button that is not down.
- (BOOL)pointerUp:(uint8_t)button {
    if (button >= kPointerButtons || (_buttons & (1u << button)) == 0)
        return NO;
    _buttons &= ~(1u << button);
    return YES;
}
// Returns true if a pointer button is down.
- (BOOL)buttonsDown {
    return _buttons != 0;
}
// Records a key's press or release. A lock key reports its state, not its movement, and is not held.
- (void)key:(PrAppKeyCode)code down:(BOOL)down key:(PrAppKey)key {
    if (code != PrAppKeyCode_Unidentified && code != PrAppKeyCode_CapsLock)
        _keys[code] = AppKitHeldKey{.down = bool(down), .key = key};
}
// Releases the buttons and the keys that are down, the buttons where the pointer last was, for a window that stopped
// being the key window.
- (void)releaseHeld {
    PrAppModifierFlags modifiers = appKitModifiersOf(NSEvent.modifierFlags);
    double now = appKitUptime();
    for (uint8_t button = 0; button < kPointerButtons; ++button) {
        if ((_buttons & (1u << button)) == 0)
            continue;
        _buttons &= ~(1u << button);
        PrAppEvent event{};
        event.kind = PrAppEventKind_PointerUp;
        event.time = now;
        event.modifiers = modifiers;
        event.pointer.button = button;
        event.pointer.position = _pointer;
        event.pointer.primary = true;
        event.pointer.id = kPrAppPointerMouse;
        event.pointer.pressure = _buttons != 0 ? 0.5f : 0.0f;
        [self.view deliver:event];
    }
    for (uint32_t code = 0; code < countOf(_keys); ++code) {
        if (!_keys[code].down)
            continue;
        _keys[code].down = false;
        PrAppEvent event{};
        event.kind = PrAppEventKind_KeyUp;
        event.time = now;
        event.modifiers = modifiers;
        event.keyboard.key = _keys[code].key;
        event.keyboard.code = PrAppKeyCode(code);
        event.keyboard.text = "";
        [self.view deliver:event];
    }
}
// Returns the items that carry `content` for a drag from the pointer, with `token` under the payload's type. The first
// item carries every form of the content and the preview, and each further item carries one more file.
- (NSArray<NSDraggingItem*>*)dragItems:(const PrAppDataContent&)content token:(uint64_t)token {
    NSPasteboardItem* first = [[NSPasteboardItem alloc] init];
    NSMutableArray<id<NSPasteboardWriting>>* writers = [NSMutableArray arrayWithObject:first];
    appKitWriteContent(content, token, first, writers);
    if (first.types.count == 0)
        [writers removeObject:first];
    // Each promised file is a provider of its own, whose type the file's extension gives. A drag replaces the last
    // drag's promises, whose indices it reuses.
    AppDragPending& pending = static_cast<AppKitWindow*>(self.record)->drag;
    if (_promises == nil)
        _promises = [NSMutableArray array];
    [_promises removeAllObjects];
    const PrStringView* names = appStringsViews(pending.promises);
    for (uint32_t i = 0; i < pending.promises.ends.count; ++i) {
        NSString* name = appKitStringOf(names[i]);
        UTType* type = [UTType typeWithFilenameExtension:name.pathExtension];
        NSString* identifier = type != nil ? type.identifier : UTTypeData.identifier;
        PrAppKitPromise* promise = [[PrAppKitPromise alloc] init];
        promise.owner = self;
        promise.index = i;
        promise.name = name;
        [_promises addObject:promise];
        [writers addObject:[[NSFilePromiseProvider alloc] initWithFileType:identifier delegate:promise]];
    }
    if (writers.count == 0)
        return @[];
    // The preview is in target pixels, and the view measures in logical px.
    NSImage* preview = nil;
    NSRect frame = NSMakeRect(CGFloat(_pointer.x), CGFloat(_pointer.y), 1, 1);
    NSBitmapImageRep* rep = nil;
    if (pending.previewExtent.x != 0 && pending.previewExtent.y != 0)
        rep = appKitImageRepOf(PrPixelView{
            pending.preview.data, size_t(pending.previewExtent.x) * 4,
            pending.previewExtent,
        });
    if (rep != nil) {
        CGFloat scale = self.window.backingScaleFactor;
        NSSize size = NSMakeSize(CGFloat(pending.previewExtent.x) / scale, CGFloat(pending.previewExtent.y) / scale);
        preview = [[NSImage alloc] initWithSize:size];
        [preview addRepresentation:rep];
        frame = NSMakeRect(CGFloat(_pointer.x - pending.previewOffset.x), CGFloat(_pointer.y - pending.previewOffset.y),
            size.width, size.height);
    }
    NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
    for (id<NSPasteboardWriting> writer in writers) {
        NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
        [item setDraggingFrame:frame contents:items.count == 0 ? preview : nil];
        [items addObject:item];
    }
    return items;
}
// Starts the drag that prAppWindowDragStart asked for, once no callback runs. A drag starts only while the primary
// button's press is still down, and the system then takes the pointer.
- (void)startDrag {
    AppKitWindow* appKit = static_cast<AppKitWindow*>(self.record);
    if (appKit == nullptr || !appKit->drag.pending)
        return;
    appKit->drag.pending = false;
    PrAppKitHost* host = self.host;
    NSEvent* press = self.view.press;
    PrAppDataContent content = appDataStoreContent(appKit->drag.content);
    uint64_t token = prRandomSeed();
    bool down = (_buttons & 1u) != 0 && press != nil && !host.headless;
    NSArray<NSDraggingItem*>* items = down ? [self dragItems:content token:token] : @[];
    if (items.count == 0) {
        [self endDrag:PrAppDragOperation_None];
        return;
    }
    static_cast<AppKitApp*>(host.app)->drag = AppKitDrag{
        .active = true,
        .token = token,
        .window = appKit,
        .payload = content.payload,
        .hasPayload = (content.forms & PrAppDataFormFlag_Payload) != 0,
    };
    [self.view beginDraggingSessionWithItems:items event:press source:self.view];
    host.dragSource = self.view;
    _buttons &= ~1u;
    self.view.press = nil;
    PrAppEvent cancel{
        .kind = PrAppEventKind_PointerCancel,
        .modifiers = appKitModifiersOf(NSEvent.modifierFlags),
        .time = appKitUptime(),
        .pointer = {.position = _pointer, .type = PrAppPointerType_Mouse, .primary = true, .id = kPrAppPointerMouse},
    };
    [self.view deliver:cancel];
}
// Ends the window's drag with the operation that the target performed.
- (void)endDrag:(PrAppDragOperation)operation {
    AppKitWindow* appKit = static_cast<AppKitWindow*>(self.record);
    if (appKit == nullptr)
        return;
    AppKitDrag& carried = static_cast<AppKitApp*>(appKit->app)->drag;
    if (carried.window == appKit)
        carried = AppKitDrag{};
    PrAppEvent event{
        .kind = PrAppEventKind_DragEnd,
        .modifiers = appKitModifiersOf(NSEvent.modifierFlags),
        .time = appKitUptime(),
        .drag = {
            .forms = appKit->drag.content.forms,
            .operations = appKit->drag.operations,
            .operation = operation,
            .fromThisWindow = true,
            .payload = appKit->drag.content.payload,
        },
    };
    [self.view deliver:event];
}
// Asks the program for the promise at `index`, whose file goes to `url`. The destination is a source of that one file,
// which the window holds until the promise ends.
- (void)promise:(uint32_t)index to:(NSURL*)url done:(void (^)(NSError* PR_NULLABLE))done {
    AppKitWindow* appKit = static_cast<AppKitWindow*>(self.record);
    if (appKit == nullptr || !self.host.running) {
        done([NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
        return;
    }
    [self endPromise:index status:PrStatus_Io];
    AppKitApp* app = static_cast<AppKitApp*>(appKit->app);
    AppDataStore* store = allocZeroed<AppDataStore>();
    PrRawString name = prAppleNSStringToRawString(url.lastPathComponent);
    appDataStoreAddFile(*store, PrAppDataFile{.name = prRawStringView(&name), .path = url.fileSystemRepresentation});
    prRawStringReset(&name);
    PrAppDataSource destination = appDataSourceCreate(app->sources, store, nullptr);
    if (_promised == nil)
        _promised = [NSMutableDictionary dictionary];
    _promised[@(index)] = @[ @(destination), [done copy] ];
    PrAppEvent event{};
    event.kind = PrAppEventKind_DragPromise;
    event.time = appKitUptime();
    event.promise = PrAppDragPromiseEvent{.index = index, .destination = destination};
    [self.view deliver:event];
}
// Ends the promise at `index`, which tells AppKit whether its file was written, and releases its destination.
- (void)endPromise:(uint32_t)index status:(PrStatus)status {
    NSArray* entry = _promised[@(index)];
    if (entry == nil)
        return;
    [_promised removeObjectForKey:@(index)];
    void (^done)(NSError* PR_NULLABLE) = entry[1];
    NSError* error = nil;
    if (status != PrStatus_Ok)
        error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil];
    done(error);
    AppKitWindow* appKit = static_cast<AppKitWindow*>(self.record);
    if (appKit != nullptr)
        appDataSourceRelease(static_cast<AppKitApp*>(appKit->app)->sources,
            PrAppDataSource([entry[0] unsignedLongLongValue]));
}
// Ends every promise that waits, as the window closes.
- (void)endPromises {
    for (NSNumber* index in _promised.allKeys)
        [self endPromise:index.unsignedIntValue status:PrStatus_Io];
    [_promises removeAllObjects];
}
- (void)windowDidBecomeKey:(NSNotification*)note {
    (void)note;
    // The modal dialog that blocks the window keeps the keyboard.
    if (PrAppKitWindow* blocker = self.blocker) {
        [blocker.window makeKeyWindow];
        return;
    }
    [self.host showMenuBar];
    [self requestFrame];
}
- (void)windowDidChangeOcclusionState:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    [self setSecureInput:NO];
    [self freePointer];
    [self releaseHeld];
    [self requestFrame];
}
// The close button asks the program, which closes the window if it chooses to.
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    PrAppEvent event{};
    event.kind = PrAppEventKind_CloseRequested;
    event.time = appKitUptime();
    [self.view deliver:event];
    return NO;
}
- (void)windowDidResize:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
- (void)windowDidChangeBackingProperties:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
- (void)windowDidChangeScreen:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
// A change of state requests a frame, so that the next frame sees the new state.
- (void)windowDidMiniaturize:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
- (void)windowDidDeminiaturize:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
- (void)windowDidEnterFullScreen:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
// Takes the state that was asked for while the window was fullscreen.
- (void)windowDidExitFullScreen:(NSNotification*)note {
    (void)note;
    PrAppWindowState after = self.afterFullscreen;
    self.afterFullscreen = PrAppWindowState_Fullscreen;
    if (after != PrAppWindowState_Fullscreen && self.record != nullptr)
        setState(self.record, after);
    [self requestFrame];
}
@end
