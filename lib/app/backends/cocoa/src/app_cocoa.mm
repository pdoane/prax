// The Cocoa application backend: a macOS window and the AppKit event loop.

#include "pr/app_cocoa.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "app/backends/cocoa/src/cocoa_browser.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UserNotifications/UserNotifications.h>

@class PrCocoaHost;

@interface PrCocoaView : NSView <CALayerDelegate, NSTextInputClient>
@property(nonatomic, weak) PrCocoaHost* host;
@property(nonatomic, strong) NSCursor* cursor;
@property(nonatomic) NSRect textInputRect;      // where the caret stands, from the top left of the view
@property(nonatomic, copy) NSString* marked;    // the text that an input method composes, or nil
@property(nonatomic) NSRange markedSelection;   // the selection within the marked text
@end

@interface PrCocoaHost : NSObject <NSApplicationDelegate, NSWindowDelegate, UNUserNotificationCenterDelegate>
@property(nonatomic) PrApp* app;
@property(nonatomic) PrAppHandler handler;
@property(nonatomic, copy) NSString* title;
@property(nonatomic) NSSize size;
@property(nonatomic) BOOL headless;
@property(nonatomic) BOOL running;
@property(nonatomic) BOOL inCallback;   // a callback is running, and no other may start
@property(nonatomic) BOOL frameRequested;
@property(nonatomic) double lastFrame;
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) PrCocoaView* view;
@property(nonatomic, strong) CADisplayLink* displayLink;
@property(nonatomic, strong) NSTimer* timer;
@property(nonatomic, strong) NSTimer* frameTimer;
@property(nonatomic) uint8_t permission;    // notification permission, a CocoaPermission

// Notifications that wait for the user to answer the permission request, oldest first.
@property(nonatomic, strong) NSMutableArray<UNNotificationRequest*>* waiting;
- (void)deliver:(const PrAppEvent&)event;
- (uint32_t)holdEvents;
- (void)deliver:(const PrAppEvent&)event ahead:(uint32_t)mark;
- (void)endCallback;
- (void)forgetHeld;
- (void)pointerAt:(PrVec2)position;
- (void)pointerDown:(uint8_t)button;
- (BOOL)pointerUp:(uint8_t)button;
- (void)key:(PrAppKeyCode)code down:(BOOL)down key:(PrAppKey)key;
- (void)frame;
- (PrAppFrame)frameAt:(double)now;
- (void)requestFrame;
- (void)requestFrameAt:(double)time;
- (void)stop;
- (PrStatus)postNotification:(const PrAppNotificationDesc*)desc;
@end

namespace pr_impl {

namespace {

// An event that arrived while a callback ran, which waits for the callback to return, with its own copy of its text or
// pixels.
struct HeldEvent {
    PrAppEvent event;
    RawArray<char> text;
};

// Returns a held copy of `event`, with its own copy of its text or pixels.
HeldEvent heldOf(const PrAppEvent& event) {
    HeldEvent held{.event = event};
    PrStringView text = prAppEventText(&event);
    if (text.size != 0)
        held.text.append(text.data, uint32_t(text.size));
    if (event.kind == PrAppEventKind_ClipboardImage && event.image.pixels.extent.y != 0) {
        const PrPixelView& pixels = event.image.pixels;
        held.text.append(reinterpret_cast<const char*>(pixels.data), uint32_t(pixels.stride * pixels.extent.y));
    }
    return held;
}

// The mark of a key event that the host delivers at once, rather than ahead of held events.
constexpr uint32_t kNoMark = UINT32_MAX;

// What a key that is down reports, so that the window can release it when it stops being the key window.
struct HeldKey {
    bool down;
    PrAppKey key;
};

// The pointer buttons that the view can report down, by PrAppPointerEvent::button.
constexpr uint8_t kPointerButtons = 32;

// The state of the user's permission for notifications. The application asks at its first post.
enum {
    CocoaPermission_Unasked,
    CocoaPermission_Asking,
    CocoaPermission_Granted,
    CocoaPermission_Refused,
};
typedef uint8_t CocoaPermission;

// The most notifications that wait for the user's answer. If more are posted, the oldest is dropped.
constexpr NSUInteger kWaitingNotificationsMax = 16;

// The key in a notification's user info that holds its tag.
NSString* const kNotificationTagKey = @"tag";

struct CocoaApp : PrApp {
    void* host;                 // the PrCocoaHost, retained
    Atomic<bool> wakePending;
};

PrCocoaHost* hostOf(PrApp* app) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    return (__bridge PrCocoaHost*)cocoa->host;
}

PrAppModifierFlags modifiersOf(NSEventModifierFlags flags) {
    PrAppModifierFlags modifiers = 0;
    if ((flags & NSEventModifierFlagCapsLock) != 0)
        modifiers |= PrAppModifierFlag_CapsLock;
    if ((flags & NSEventModifierFlagShift) != 0)
        modifiers |= PrAppModifierFlag_Shift;
    if ((flags & NSEventModifierFlagControl) != 0)
        modifiers |= PrAppModifierFlag_Control;
    if ((flags & NSEventModifierFlagOption) != 0)
        modifiers |= PrAppModifierFlag_Alt;
    if ((flags & NSEventModifierFlagCommand) != 0)
        modifiers |= PrAppModifierFlag_Super;
    return modifiers;
}

// A key of a Mac's keyboard: its virtual key code (kVK_ in HIToolbox's Events.h), its position, and for a key that
// types no character, its name. A key that types a character takes its value from the character.
struct MacKey {
    uint16_t virtualKey;
    PrAppKeyCode code;
    PrAppKey key = PrAppKey_Unknown;
};

constexpr MacKey kMacKeys[] = {
    {0, PrAppKeyCode_KeyA}, {1, PrAppKeyCode_KeyS}, {2, PrAppKeyCode_KeyD}, {3, PrAppKeyCode_KeyF},
    {4, PrAppKeyCode_KeyH}, {5, PrAppKeyCode_KeyG}, {6, PrAppKeyCode_KeyZ}, {7, PrAppKeyCode_KeyX},
    {8, PrAppKeyCode_KeyC}, {9, PrAppKeyCode_KeyV}, {10, PrAppKeyCode_IntlBackslash}, {11, PrAppKeyCode_KeyB},
    {12, PrAppKeyCode_KeyQ}, {13, PrAppKeyCode_KeyW}, {14, PrAppKeyCode_KeyE}, {15, PrAppKeyCode_KeyR},
    {16, PrAppKeyCode_KeyY}, {17, PrAppKeyCode_KeyT}, {18, PrAppKeyCode_Digit1}, {19, PrAppKeyCode_Digit2},
    {20, PrAppKeyCode_Digit3}, {21, PrAppKeyCode_Digit4}, {22, PrAppKeyCode_Digit6}, {23, PrAppKeyCode_Digit5},
    {24, PrAppKeyCode_Equal}, {25, PrAppKeyCode_Digit9}, {26, PrAppKeyCode_Digit7}, {27, PrAppKeyCode_Minus},
    {28, PrAppKeyCode_Digit8}, {29, PrAppKeyCode_Digit0}, {30, PrAppKeyCode_BracketRight}, {31, PrAppKeyCode_KeyO},
    {32, PrAppKeyCode_KeyU}, {33, PrAppKeyCode_BracketLeft}, {34, PrAppKeyCode_KeyI}, {35, PrAppKeyCode_KeyP},
    {36, PrAppKeyCode_Enter, PrAppKey_Enter}, {37, PrAppKeyCode_KeyL}, {38, PrAppKeyCode_KeyJ},
    {39, PrAppKeyCode_Quote}, {40, PrAppKeyCode_KeyK}, {41, PrAppKeyCode_Semicolon}, {42, PrAppKeyCode_Backslash},
    {43, PrAppKeyCode_Comma}, {44, PrAppKeyCode_Slash}, {45, PrAppKeyCode_KeyN}, {46, PrAppKeyCode_KeyM},
    {47, PrAppKeyCode_Period}, {48, PrAppKeyCode_Tab, PrAppKey_Tab}, {49, PrAppKeyCode_Space},
    {50, PrAppKeyCode_Backquote}, {51, PrAppKeyCode_Backspace, PrAppKey_Backspace},
    {53, PrAppKeyCode_Escape, PrAppKey_Escape}, {54, PrAppKeyCode_MetaRight, PrAppKey_Meta},
    {55, PrAppKeyCode_MetaLeft, PrAppKey_Meta}, {56, PrAppKeyCode_ShiftLeft, PrAppKey_Shift},
    {57, PrAppKeyCode_CapsLock, PrAppKey_CapsLock}, {58, PrAppKeyCode_AltLeft, PrAppKey_Alt},
    {59, PrAppKeyCode_ControlLeft, PrAppKey_Control}, {60, PrAppKeyCode_ShiftRight, PrAppKey_Shift},
    {61, PrAppKeyCode_AltRight, PrAppKey_Alt}, {62, PrAppKeyCode_ControlRight, PrAppKey_Control},
    {63, PrAppKeyCode_Fn, PrAppKey_Fn}, {64, PrAppKeyCode_F17, PrAppKey_F17}, {65, PrAppKeyCode_NumpadDecimal},
    {67, PrAppKeyCode_NumpadMultiply}, {69, PrAppKeyCode_NumpadAdd}, {71, PrAppKeyCode_NumLock, PrAppKey_Clear},
    {75, PrAppKeyCode_NumpadDivide}, {76, PrAppKeyCode_NumpadEnter, PrAppKey_Enter}, {78, PrAppKeyCode_NumpadSubtract},
    {79, PrAppKeyCode_F18, PrAppKey_F18}, {80, PrAppKeyCode_F19, PrAppKey_F19}, {81, PrAppKeyCode_NumpadEqual},
    {82, PrAppKeyCode_Numpad0}, {83, PrAppKeyCode_Numpad1}, {84, PrAppKeyCode_Numpad2}, {85, PrAppKeyCode_Numpad3},
    {86, PrAppKeyCode_Numpad4}, {87, PrAppKeyCode_Numpad5}, {88, PrAppKeyCode_Numpad6}, {89, PrAppKeyCode_Numpad7},
    {90, PrAppKeyCode_F20, PrAppKey_F20}, {91, PrAppKeyCode_Numpad8}, {92, PrAppKeyCode_Numpad9},
    {93, PrAppKeyCode_IntlYen}, {94, PrAppKeyCode_IntlRo}, {95, PrAppKeyCode_NumpadComma},
    {96, PrAppKeyCode_F5, PrAppKey_F5}, {97, PrAppKeyCode_F6, PrAppKey_F6}, {98, PrAppKeyCode_F7, PrAppKey_F7},
    {99, PrAppKeyCode_F3, PrAppKey_F3}, {100, PrAppKeyCode_F8, PrAppKey_F8}, {101, PrAppKeyCode_F9, PrAppKey_F9},
    {102, PrAppKeyCode_Lang2}, {103, PrAppKeyCode_F11, PrAppKey_F11}, {104, PrAppKeyCode_Lang1},
    {105, PrAppKeyCode_F13, PrAppKey_F13}, {106, PrAppKeyCode_F16, PrAppKey_F16}, {107, PrAppKeyCode_F14, PrAppKey_F14},
    {109, PrAppKeyCode_F10, PrAppKey_F10}, {110, PrAppKeyCode_ContextMenu, PrAppKey_ContextMenu},
    {111, PrAppKeyCode_F12, PrAppKey_F12}, {113, PrAppKeyCode_F15, PrAppKey_F15},
    {114, PrAppKeyCode_Insert, PrAppKey_Insert}, {115, PrAppKeyCode_Home, PrAppKey_Home},
    {116, PrAppKeyCode_PageUp, PrAppKey_PageUp}, {117, PrAppKeyCode_Delete, PrAppKey_Delete},
    {118, PrAppKeyCode_F4, PrAppKey_F4}, {119, PrAppKeyCode_End, PrAppKey_End}, {120, PrAppKeyCode_F2, PrAppKey_F2},
    {121, PrAppKeyCode_PageDown, PrAppKey_PageDown}, {122, PrAppKeyCode_F1, PrAppKey_F1},
    {123, PrAppKeyCode_ArrowLeft, PrAppKey_Left}, {124, PrAppKeyCode_ArrowRight, PrAppKey_Right},
    {125, PrAppKeyCode_ArrowDown, PrAppKey_Down}, {126, PrAppKeyCode_ArrowUp, PrAppKey_Up},
};

MacKey macKeyOf(NSEvent* event) {
    for (const MacKey& key : kMacKeys) {
        if (key.virtualKey == event.keyCode)
            return key;
    }
    return MacKey{event.keyCode, PrAppKeyCode_Unidentified, PrAppKey_Unknown};
}

// Returns the character that a key types with Shift and Option applied, and Control and Command not, or nil for a key
// that types none. A dead key types none until the key that it combines with.
NSString* PR_NULLABLE characterOf(NSEvent* event) {
    NSEventModifierFlags commands = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    // charactersIgnoringModifiers keeps Shift and drops Option, Control and Command.
    NSString* characters = (event.modifierFlags & commands) != 0 ? event.charactersIgnoringModifiers : event.characters;
    unichar first = characters.length != 0 ? [characters characterAtIndex:0] : 0;
    // Excludes control characters, delete, and the private-use range that AppKit assigns to function keys.
    bool printable = first >= 32 && first != 127 && (first < 0xF700 || first > 0xF8FF);
    return printable ? characters : nil;
}

// Returns true if a key is a dead key: it types nothing itself, though it types a character without its modifiers.
bool isDead(NSEvent* event) {
    if (event.characters.length != 0)
        return false;
    NSString* plain = event.charactersIgnoringModifiers;
    return plain.length != 0 && [plain characterAtIndex:0] >= 32;
}

// The device-dependent flags (NX_DEVICE*KEYMASK in IOKit) that tell a modifier's left key from its right.
constexpr NSEventModifierFlags kLeftControl = 0x0001;
constexpr NSEventModifierFlags kLeftShift = 0x0002;
constexpr NSEventModifierFlags kRightShift = 0x0004;
constexpr NSEventModifierFlags kLeftCommand = 0x0008;
constexpr NSEventModifierFlags kRightCommand = 0x0010;
constexpr NSEventModifierFlags kLeftOption = 0x0020;
constexpr NSEventModifierFlags kRightOption = 0x0040;
constexpr NSEventModifierFlags kRightControl = 0x2000;

// Returns true if the modifier or lock key whose flags changed in `event` is now down, or on.
bool modifierDown(NSEvent* event, PrAppKeyCode code) {
    NSEventModifierFlags flags = event.modifierFlags;
    switch (code) {
        case PrAppKeyCode_ShiftLeft: return (flags & kLeftShift) != 0;
        case PrAppKeyCode_ShiftRight: return (flags & kRightShift) != 0;
        case PrAppKeyCode_ControlLeft: return (flags & kLeftControl) != 0;
        case PrAppKeyCode_ControlRight: return (flags & kRightControl) != 0;
        case PrAppKeyCode_AltLeft: return (flags & kLeftOption) != 0;
        case PrAppKeyCode_AltRight: return (flags & kRightOption) != 0;
        case PrAppKeyCode_MetaLeft: return (flags & kLeftCommand) != 0;
        case PrAppKeyCode_MetaRight: return (flags & kRightCommand) != 0;
        case PrAppKeyCode_CapsLock: return (flags & NSEventModifierFlagCapsLock) != 0;
        case PrAppKeyCode_Fn: return (flags & NSEventModifierFlagFunction) != 0;
        default: return false;
    }
}

// Returns the length in UTF-8 of the first `units` UTF-16 units of `string`.
uint32_t utf8Length(NSString* string, NSUInteger units) {
    units = MIN(units, string.length);
    return uint32_t([[string substringToIndex:units] lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
}

PrStringView viewOf(NSData* bytes) {
    return PrStringView{static_cast<const char*>(bytes.bytes), bytes.length};
}

void destroy(PrApp* app) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrCocoaHost* host = (__bridge_transfer PrCocoaHost*)cocoa->host;
    host.app = nullptr;
    // Only a process that has posted has touched the notification center. Any other process must not touch it, because
    // the center raises an exception outside an application bundle.
    UNUserNotificationCenter* center = nil;
    if (host.permission != CocoaPermission_Unasked)
        center = UNUserNotificationCenter.currentNotificationCenter;
    if (center != nil && center.delegate == host)
        center.delegate = nil;
    prMemoryFree(cocoa);
}

void run(PrApp* app, const PrAppHandler* handler) {
    @autoreleasepool {
        PrCocoaHost* host = hostOf(app);
        host.handler = *handler;
        NSApplication* application = NSApplication.sharedApplication;
        if (!host.headless)
            [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;
        NSRect content = NSMakeRect(0, 0, host.size.width, host.size.height);
        if (!host.headless) {
            NSRect available = [NSWindow contentRectForFrameRect:NSScreen.mainScreen.visibleFrame styleMask:style];
            content.size.width = MIN(content.size.width, available.size.width);
            content.size.height = MIN(content.size.height, available.size.height);
        }
        host.window = [[NSWindow alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered defer:NO];
        host.window.title = host.title;
        host.window.releasedWhenClosed = NO;
        host.view = [[PrCocoaView alloc] initWithFrame:host.window.contentView.bounds];
        host.view.host = host;
        host.view.cursor = NSCursor.arrowCursor;
        host.view.wantsLayer = YES;
        host.view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        host.view.layer.backgroundColor = NSColor.blackColor.CGColor;
        host.window.contentView = host.view;
        host.window.delegate = host;
        application.delegate = host;
        if (host.headless) {
            [host.window setFrameOrigin:NSMakePoint(-32000, -32000)];
        } else {
            NSMenu* menu = [[NSMenu alloc] init];
            NSMenuItem* item = [[NSMenuItem alloc] init];
            [menu addItem:item];
            item.submenu = [[NSMenu alloc] init];
            [item.submenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
            application.mainMenu = menu;
            [host.window center];
            [host.window makeKeyAndOrderFront:nil];
            [application activateIgnoringOtherApps:YES];
        }
        [host.window makeFirstResponder:host.view];
        host.running = YES;
        host.lastFrame = 0;
        host.inCallback = YES;
        PrAppFrame window = [host frameAt:NSProcessInfo.processInfo.systemUptime];
        host.handler.start(host.handler.context, app, &window);
        [host endCallback];
        [host requestFrame];
        if (host.running)
            [application run];
        host.inCallback = YES;
        host.handler.shutdown(host.handler.context, app);
        [host endCallback];
        host.window.delegate = nil;
        if (application.delegate == host)
            application.delegate = nil;
        [host.window orderOut:nil];
        [host.window close];
        host.window = nil;
        host.view = nil;
        [host forgetHeld];
    }
}

void stop(PrApp* app) {
    [hostOf(app) stop];
}

void wake(PrApp* app) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    if (cocoa->wakePending.exchange(true))
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        cocoa->wakePending.store(false);
        PrAppEvent event{};
        event.kind = PrAppEventKind_Wake;
        event.time = NSProcessInfo.processInfo.systemUptime;
        [(__bridge PrCocoaHost*)cocoa->host deliver:event];
    });
}

void requestFrame(PrApp* app) {
    [hostOf(app) requestFrame];
}

void requestFrameAt(PrApp* app, double time) {
    [hostOf(app) requestFrameAt:time];
}

// Returns a cursor that shows nothing, for PrAppCursor_None. A hidden cursor would need [NSCursor hide], whose calls
// must balance, so a transparent image stands in for it.
NSCursor* emptyCursor() {
    static NSCursor* empty = [[NSCursor alloc] initWithImage:[[NSImage alloc] initWithSize:NSMakeSize(1, 1)]
        hotSpot:NSZeroPoint];
    return empty;
}

// Returns the system's cursor that resizes a frame from `position`, outward only or in both directions.
NSCursor* frameResize(NSCursorFrameResizePosition position, NSCursorFrameResizeDirections directions) {
    return [NSCursor frameResizeCursorFromPosition:position inDirections:directions];
}

// Returns the system's nearest cursor to `cursor`. AppKit has no public cursors for help, busy, cell, move or
// all-scroll, which show the arrow, or the crosshair for a cell.
NSCursor* cursorFor(PrAppCursor cursor) {
    constexpr NSCursorFrameResizeDirections kOutward = NSCursorFrameResizeDirectionsOutward;
    constexpr NSCursorFrameResizeDirections kBoth = NSCursorFrameResizeDirectionsAll;
    switch (cursor) {
        case PrAppCursor_None: return emptyCursor();
        case PrAppCursor_ContextMenu: return NSCursor.contextualMenuCursor;
        case PrAppCursor_Hand: return NSCursor.pointingHandCursor;
        case PrAppCursor_Cell: return NSCursor.crosshairCursor;
        case PrAppCursor_Crosshair: return NSCursor.crosshairCursor;
        case PrAppCursor_Text: return NSCursor.IBeamCursor;
        case PrAppCursor_VerticalText: return NSCursor.IBeamCursorForVerticalLayout;
        case PrAppCursor_Alias: return NSCursor.dragLinkCursor;
        case PrAppCursor_Copy: return NSCursor.dragCopyCursor;
        case PrAppCursor_NoDrop: return NSCursor.operationNotAllowedCursor;
        case PrAppCursor_NotAllowed: return NSCursor.operationNotAllowedCursor;
        case PrAppCursor_Grab: return NSCursor.openHandCursor;
        case PrAppCursor_Grabbing: return NSCursor.closedHandCursor;
        case PrAppCursor_ResizeN: return frameResize(NSCursorFrameResizePositionTop, kOutward);
        case PrAppCursor_ResizeE: return frameResize(NSCursorFrameResizePositionRight, kOutward);
        case PrAppCursor_ResizeS: return frameResize(NSCursorFrameResizePositionBottom, kOutward);
        case PrAppCursor_ResizeW: return frameResize(NSCursorFrameResizePositionLeft, kOutward);
        case PrAppCursor_ResizeNE: return frameResize(NSCursorFrameResizePositionTopRight, kOutward);
        case PrAppCursor_ResizeNW: return frameResize(NSCursorFrameResizePositionTopLeft, kOutward);
        case PrAppCursor_ResizeSE: return frameResize(NSCursorFrameResizePositionBottomRight, kOutward);
        case PrAppCursor_ResizeSW: return frameResize(NSCursorFrameResizePositionBottomLeft, kOutward);
        case PrAppCursor_ResizeX: return frameResize(NSCursorFrameResizePositionRight, kBoth);
        case PrAppCursor_ResizeY: return frameResize(NSCursorFrameResizePositionBottom, kBoth);
        case PrAppCursor_ResizeNESW: return frameResize(NSCursorFrameResizePositionTopRight, kBoth);
        case PrAppCursor_ResizeNWSE: return frameResize(NSCursorFrameResizePositionTopLeft, kBoth);
        case PrAppCursor_ResizeColumn: return NSCursor.columnResizeCursor;
        case PrAppCursor_ResizeRow: return NSCursor.rowResizeCursor;
        case PrAppCursor_ZoomIn: return NSCursor.zoomInCursor;
        case PrAppCursor_ZoomOut: return NSCursor.zoomOutCursor;
        default: return NSCursor.arrowCursor;
    }
}

void setCursor(PrApp* app, PrAppCursor cursor) {
    // A cursor is made once per shape, so that setting the same shape again finds the view's cursor unchanged. Only the
    // main thread reads the table.
    static NSCursor* made[PrAppCursor_Count];
    PrAppCursor shape = cursor < PrAppCursor_Count ? cursor : PrAppCursor_Arrow;
    if (made[shape] == nil)
        made[shape] = cursorFor(shape);
    PrCocoaHost* host = hostOf(app);
    NSCursor* selected = made[shape];
    if (host.view == nil || host.view.cursor == selected)
        return;
    host.view.cursor = selected;
    [host.window invalidateCursorRectsForView:host.view];
}

void setTitle(PrApp* app, PrStringView title) {
    PrCocoaHost* host = hostOf(app);
    NSString* string = [[NSString alloc] initWithBytes:title.data length:title.size encoding:NSUTF8StringEncoding];
    host.title = string != nil ? string : @"";
    if (host.window != nil)
        host.window.title = host.title;
}

void setExtent(PrApp* app, PrUVec2 extent) {
    PrCocoaHost* host = hostOf(app);
    NSSize size = NSMakeSize(extent.x, extent.y);
    if (host.window == nil) {
        host.size = size;
        return;
    }
    if (!host.headless) {
        NSScreen* screen = host.window.screen != nil ? host.window.screen : NSScreen.mainScreen;
        NSRect available = [NSWindow contentRectForFrameRect:screen.visibleFrame styleMask:host.window.styleMask];
        size.width = MIN(size.width, available.size.width);
        size.height = MIN(size.height, available.size.height);
    }
    [host.window setContentSize:size];
}

void requestAttention(PrApp*) {
    // An informational request bounces the Dock icon once, and does nothing while the application is active.
    [NSApp requestUserAttention:NSInformationalRequest];
}

bool isActive(PrApp* app) {
    PrCocoaHost* host = hostOf(app);
    return NSApp.isActive && host.window != nil && host.window.isKeyWindow;
}

// Returns true if the process runs from an application bundle. In any other process, the notification center raises an
// exception, even if the executable embeds an Info.plist with a bundle identifier. The older NSUserNotificationCenter
// has no default center in such a process.
bool isBundled() {
    NSBundle* bundle = NSBundle.mainBundle;
    return bundle.bundleIdentifier != nil && [bundle.bundleURL.pathExtension isEqualToString:@"app"];
}

PrStatus notificationPost(PrApp* app, const PrAppNotificationDesc* desc) {
    PrCocoaHost* host = hostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    // A headless window posts nothing, because the permission request would go to a user who never sees the window.
    if (host.headless || !isBundled())
        return PrStatus_NotSupported;
    return [host postNotification:desc];
}

PrStatus surface(PrApp* app, PrAppSurface* out) {
    PrCocoaHost* host = hostOf(app);
    if (host.view == nil)
        return PrStatus_InvalidState;
    out->kind = PrAppSurfaceKind_NsView;
    out->handle = (__bridge void*)host.view;
    return PrStatus_Ok;
}

PrStatus openUrl(PrApp*, PrStringView url) {
    NSString* string = [[NSString alloc] initWithBytes:url.data length:url.size encoding:NSUTF8StringEncoding];
    NSURL* address = string != nil ? [NSURL URLWithString:string] : nil;
    if (address == nil)
        return PrStatus_Syntax;
    return [NSWorkspace.sharedWorkspace openURL:address] ? PrStatus_Ok : PrStatus_Io;
}

void requestClipboardText(PrApp* app) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSString* string = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
        NSData* bytes = [string != nil ? string : @"" dataUsingEncoding:NSUTF8StringEncoding];
        PrAppEvent event{};
        event.kind = PrAppEventKind_Clipboard;
        event.time = NSProcessInfo.processInfo.systemUptime;
        event.input.text = viewOf(bytes);
        [(__bridge PrCocoaHost*)cocoa->host deliver:event];
    });
}

void setTextInputRect(PrApp* app, PrRect rect) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrCocoaHost* host = (__bridge PrCocoaHost*)cocoa->host;
    if (host != nil) {
        host.view.textInputRect = NSMakeRect(CGFloat(rect.left), CGFloat(rect.top), CGFloat(rect.right - rect.left),
            CGFloat(rect.bottom - rect.top));
    }
}

PrStatus setClipboardText(PrApp*, PrStringView text) {
    NSString* string = [[NSString alloc] initWithBytes:text.data length:text.size encoding:NSUTF8StringEncoding];
    NSPasteboard* board = NSPasteboard.generalPasteboard;
    [board clearContents];
    return [board setString:string forType:NSPasteboardTypeString] ? PrStatus_Ok : PrStatus_Io;
}

// Returns the image that the pasteboard holds or names, as an image file copied in the Finder does, as straight-alpha
// sRGB Rgba8 rows in `pixels`, and its extent, or a zero extent if it holds none.
PrUVec2 pasteboardPixels(NSPasteboard* board, NSMutableData* pixels) {
    if (![NSImage canInitWithPasteboard:board])
        return PrUVec2{0, 0};
    NSImage* image = [[NSImage alloc] initWithPasteboard:board];
    CGImageRef source = image != nil ? [image CGImageForProposedRect:nil context:nil hints:nil] : nullptr;
    if (source == nullptr)
        return PrUVec2{0, 0};
    size_t width = CGImageGetWidth(source);
    size_t height = CGImageGetHeight(source);
    pixels.length = width * height * 4;
    uint8_t* rows = static_cast<uint8_t*>(pixels.mutableBytes);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(rows, width, height, 8, width * 4, space,
        kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    if (context == nullptr)
        return PrUVec2{0, 0};
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, CGFloat(width), CGFloat(height)), source);
    CGContextRelease(context);
    // Core Graphics draws only into premultiplied pixels.
    for (size_t i = 0; i < width * height * 4; i += 4) {
        uint32_t alpha = rows[i + 3];
        if (alpha == 0 || alpha == 255)
            continue;
        for (size_t channel = 0; channel < 3; ++channel) {
            uint32_t straight = (rows[i + channel] * 255u + alpha / 2) / alpha;
            rows[i + channel] = uint8_t(straight < 255 ? straight : 255);
        }
    }
    return PrUVec2{uint32_t(width), uint32_t(height)};
}

void requestClipboardImage(PrApp* app) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSMutableData* pixels = [NSMutableData data];
        PrUVec2 extent = pasteboardPixels(NSPasteboard.generalPasteboard, pixels);
        PrAppEvent event{};
        event.kind = PrAppEventKind_ClipboardImage;
        event.time = NSProcessInfo.processInfo.systemUptime;
        event.image.pixels = PrPixelView{static_cast<const uint8_t*>(pixels.bytes), size_t(extent.x) * 4, extent};
        [(__bridge PrCocoaHost*)cocoa->host deliver:event];
    });
}

PrStatus setClipboardImage(PrApp*, PrPixelView pixels) {
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
        pixelsWide:NSInteger(pixels.extent.x) pixelsHigh:NSInteger(pixels.extent.y) bitsPerSample:8 samplesPerPixel:4
        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
        bytesPerRow:NSInteger(pixels.extent.x) * 4 bitsPerPixel:32];
    if (rep == nil)
        return PrStatus_Io;
    for (uint32_t y = 0; y < pixels.extent.y; ++y)
        prMemCopy(rep.bitmapData + size_t(y) * size_t(rep.bytesPerRow), pixels.data + y * pixels.stride,
            size_t(pixels.extent.x) * 4);
    rep = [rep bitmapImageRepByRetaggingWithColorSpace:NSColorSpace.sRGBColorSpace];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSData* tiff = rep.TIFFRepresentation;
    if (png == nil || tiff == nil)
        return PrStatus_Io;
    // Programs that read only one of the two find the type they know.
    NSPasteboard* board = NSPasteboard.generalPasteboard;
    [board clearContents];
    BOOL written = [board setData:png forType:NSPasteboardTypePNG];
    written = [board setData:tiff forType:NSPasteboardTypeTIFF] && written;
    return written ? PrStatus_Ok : PrStatus_Io;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

@implementation PrCocoaView
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
    [self.host frame];
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
    NSTrackingAreaOptions options = NSTrackingMouseMoved | NSTrackingCursorUpdate |
        NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;
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
- (void)pointer:(NSEvent*)source kind:(PrAppEventKind)kind {
    NSPoint p = [self convertPoint:source.locationInWindow fromView:nil];
    PrAppEvent event{};
    event.kind = kind;
    event.time = source.timestamp;
    event.modifiers = modifiersOf(source.modifierFlags);
    PrVec2 position{float(p.x), float(p.y)};
    if (kind == PrAppEventKind_Scroll) {
        // A mouse wheel's deltas are lines, and a trackpad's are pixels.
        event.scroll.position = position;
        event.scroll.unit = source.hasPreciseScrollingDeltas ? PrAppScrollUnit_Pixel : PrAppScrollUnit_Line;
        event.scroll.delta.x = -float(source.scrollingDeltaX);
        event.scroll.delta.y = -float(source.scrollingDeltaY);
    } else {
        event.pointer.position = position;
        event.pointer.button = uint8_t(source.buttonNumber);
    }
    [self.host pointerAt:position];
    // A button that the window released as it stopped being the key window reports no release of its own.
    if (kind == PrAppEventKind_PointerDown) {
        event.pointer.clicks = uint32_t(source.clickCount);
        [self.host pointerDown:event.pointer.button];
    }
    else if (kind == PrAppEventKind_PointerUp && ![self.host pointerUp:event.pointer.button])
        return;
    [self.host deliver:event];
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
- (void)mouseDown:(NSEvent*)e {
    [self pointer:e kind:PrAppEventKind_PointerDown];
}
- (void)rightMouseDown:(NSEvent*)e {
    [self mouseDown:e];
}
- (void)otherMouseDown:(NSEvent*)e {
    [self mouseDown:e];
}
- (void)mouseUp:(NSEvent*)e {
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
// Delivers the key event of `source`. If `mark` is not kNoMark, the host holds the events since `mark`, and the key
// goes ahead of them.
- (void)key:(NSEvent*)source kind:(PrAppEventKind)kind composing:(BOOL)composing mark:(uint32_t)mark {
    MacKey mac = macKeyOf(source);
    PrAppEvent event{};
    event.kind = kind;
    event.time = source.timestamp;
    event.keyboard.code = mac.code;
    event.modifiers = modifiersOf(source.modifierFlags);
    event.keyboard.repeat = kind == PrAppEventKind_KeyDown && source.isARepeat;
    event.keyboard.composing = composing;
    NSData* bytes = nil;
    if (mac.key != PrAppKey_Unknown) {
        event.keyboard.key = mac.key;
    } else if (NSString* character = characterOf(source)) {
        bytes = [character dataUsingEncoding:NSUTF8StringEncoding];
        event.keyboard.text = viewOf(bytes);
        unichar first = [character.uppercaseString characterAtIndex:0];
        event.keyboard.key = first < 128 ? PrAppKey(first) : PrAppKey_Unknown;
    } else if (isDead(source)) {
        event.keyboard.key = PrAppKey_Dead;
    }
    [self.host key:event.keyboard.code down:kind == PrAppEventKind_KeyDown key:event.keyboard.key];
    if (mark == kNoMark)
        [self.host deliver:event];
    else
        [self.host deliver:event ahead:mark];
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
    uint32_t mark = [self.host holdEvents];
    [self interpretKeyEvents:@[ e ]];
    composing = composing || self.marked != nil;
    [self key:e kind:PrAppEventKind_KeyDown composing:composing mark:mark];
}
- (void)keyUp:(NSEvent*)e {
    [self key:e kind:PrAppEventKind_KeyUp];
}
// A modifier key reports a change of the flags, which says whether the key went down or up.
- (void)flagsChanged:(NSEvent*)e {
    MacKey mac = macKeyOf(e);
    if (mac.key == PrAppKey_Unknown)
        return;
    [self key:e kind:modifierDown(e, mac.code) ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp];
}
- (void)deliverText:(NSString*)text kind:(PrAppEventKind)kind selection:(NSRange)selection {
    NSData* bytes = [text dataUsingEncoding:NSUTF8StringEncoding];
    PrAppEvent event{};
    event.kind = kind;
    event.time = NSProcessInfo.processInfo.systemUptime;
    event.input.text = viewOf(bytes);
    if (selection.location != NSNotFound)
        event.input.selection = PrAppTextRange{
            utf8Length(text, selection.location),
            utf8Length(text, selection.location + selection.length),
        };
    [self.host deliver:event];
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
@end

@implementation PrCocoaHost {
    // The events that arrived during the running callback, in order. A callback that runs a run loop of its own, as a
    // modal panel does, has events delivered within it, and they wait here, so that no callback nests in another.
    RawArray<HeldEvent> _held;
    BOOL _holding;              // events wait in `_held` while a key's input method runs (holdEvents)

    // The pointer buttons that are down, by bit of PrAppPointerEvent::button, where the pointer last was, and the keys
    // that are down, by their position. The window releases them when it stops being the key window, as the releases
    // then go to another window.
    uint32_t _buttons;
    PrVec2 _pointer;
    HeldKey _keys[PrAppKeyCode_Count];
}
- (void)dealloc {
    _held.reset();
}
// Calls the program's event callback, as no other callback runs.
- (void)callEvent:(const PrAppEvent&)event {
    self.inCallback = YES;
    self.handler.event(self.handler.context, self.app, &event);
    self.inCallback = NO;
}
// Ends a callback, and delivers the events that arrived during it, and those that arrive during their own delivery, in
// order. The events of a run that stopped are dropped.
- (void)endCallback {
    self.inCallback = NO;
    for (uint32_t i = 0; i < _held.count && self.running; ++i) {
        // An event that its delivery holds goes to the end of the array, which may move the array but not the text.
        PrAppEvent event = _held.data[i].event;
        PrStringView text{_held.data[i].text.data != nullptr ? _held.data[i].text.data : "", _held.data[i].text.count};
        prAppEventSetText(&event, text);
        if (event.kind == PrAppEventKind_ClipboardImage)
            event.image.pixels.data = reinterpret_cast<const uint8_t*>(_held.data[i].text.data);
        [self callEvent:event];
    }
    for (uint32_t i = 0; i < _held.count; ++i)
        _held.data[i].text.reset();
    _held.clear();
}
// Delivers an event, or holds it until the running callback returns, or while events are held (holdEvents).
- (void)deliver:(const PrAppEvent&)event {
    if (!self.running)
        return;
    if (self.inCallback || _holding) {
        _held.push(heldOf(event));
        return;
    }
    [self callEvent:event];
    [self endCallback];
}
// Holds the events that arrive from now on, and returns the mark of the first of them, for deliver:ahead:.
- (uint32_t)holdEvents {
    PR_ASSERT(!_holding);
    _holding = YES;
    return _held.count;
}
// Ends holdEvents, and delivers `event` ahead of the events that arrived since `mark`, and then those events.
- (void)deliver:(const PrAppEvent&)event ahead:(uint32_t)mark {
    _holding = NO;
    if (!self.running) {
        for (uint32_t i = mark; i < _held.count; ++i)
            _held.data[i].text.reset();
        _held.resize(mark);
        return;
    }
    _held.insert(mark, heldOf(event));
    if (!self.inCallback)
        [self endCallback];
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
// Records a key's press or release. A lock key reports its state, not its movement, and is not held.
- (void)key:(PrAppKeyCode)code down:(BOOL)down key:(PrAppKey)key {
    if (code != PrAppKeyCode_Unidentified && code != PrAppKeyCode_CapsLock)
        _keys[code] = HeldKey{.down = bool(down), .key = key};
}
// Releases the buttons and the keys that are down, the buttons where the pointer last was, for a window that stopped
// being the key window.
- (void)releaseHeld {
    PrAppModifierFlags modifiers = modifiersOf(NSEvent.modifierFlags);
    double now = NSProcessInfo.processInfo.systemUptime;
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
        [self deliver:event];
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
        [self deliver:event];
    }
}
// Forgets the buttons and the keys that are down, at the end of a run.
- (void)forgetHeld {
    _buttons = 0;
    prMemZero(_keys, sizeof(_keys));
}
- (void)frame {
    if (!self.running)
        return;
    // A frame that comes during a callback, from a run loop that the callback runs, waits for the next.
    if (self.inCallback) {
        [self requestFrame];
        return;
    }
    double now = NSProcessInfo.processInfo.systemUptime;
    PrAppFrame frame = [self frameAt:now];
    self.lastFrame = now;
    self.inCallback = YES;
    PrAppHandler handler = self.handler;
    prAppDeliverFrame(self.app, &handler, &frame);
    [self endCallback];
}
// The window as a frame at `now` finds it.
- (PrAppFrame)frameAt:(double)now {
    PrAppFrame frame{};
    frame.time = now;
    frame.delta = self.lastFrame == 0 ? 0 : now - self.lastFrame;
    frame.extent.x = float(self.view.bounds.size.width);
    frame.extent.y = float(self.view.bounds.size.height);
    PrAppDisplay& display = frame.display;
    display = prAppDisplayDefault();
    display.pixelRatio = float(self.window.backingScaleFactor);
    NSScreen* screen = self.window.screen;
    if (screen != nil) {
        display.headroom = float(screen.maximumExtendedDynamicRangeColorComponentValue);
        display.potentialHeadroom = float(screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
        if ([screen canRepresentDisplayGamut:NSDisplayGamutP3])
            display.gamut = PrColorSpace_DisplayP3;
    }
    PrAppUserPreferences& preferences = frame.preferences;
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
- (void)displayFrame:(CADisplayLink*)sender {
    if (!self.frameRequested)
        return;
    self.frameRequested = NO;
    sender.paused = YES;
    [self frame];
}
- (void)requestFrame {
    if (!self.running)
        return;
    if (self.headless) {
        if (self.timer != nil)
            return;
        __weak PrCocoaHost* weak = self;
        self.timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:NO block:^(NSTimer*) {
            PrCocoaHost* host = weak;
            host.timer = nil;
            [host frame];
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
- (void)requestFrameAt:(double)time {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    if (!self.running || time == 0)
        return;
    double delay = MAX(time - NSProcessInfo.processInfo.systemUptime, 0.0);
    __weak PrCocoaHost* weak = self;
    self.frameTimer = [NSTimer timerWithTimeInterval:delay repeats:NO block:^(NSTimer*) {
        PrCocoaHost* host = weak;
        host.frameTimer = nil;
        [host requestFrame];
    }];
    [NSRunLoop.mainRunLoop addTimer:self.frameTimer forMode:NSRunLoopCommonModes];
}
- (void)stop {
    if (!self.running)
        return;
    self.running = NO;
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    [self.displayLink invalidate];
    self.displayLink = nil;
    [self.timer invalidate];
    self.timer = nil;
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
}
// Posts a notification once the user allows notifications. The first post asks for permission.
- (PrStatus)postNotification:(const PrAppNotificationDesc*)desc {
    if (self.permission == CocoaPermission_Refused)
        return PrStatus_NotAllowed;
    NSString* title = [[NSString alloc] initWithBytes:desc->title.data length:desc->title.size
        encoding:NSUTF8StringEncoding];
    NSString* body = [[NSString alloc] initWithBytes:desc->body.data length:desc->body.size
        encoding:NSUTF8StringEncoding];
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = title != nil ? title : @"";
    content.body = body != nil ? body : @"";
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
// Brings the window forward for a click on a notification, and delivers the click.
- (void)notificationClicked:(uint64_t)tag {
    if (!self.running)
        return;
    [NSApp activate];
    if (self.window.miniaturized)
        [self.window deminiaturize:nil];
    [self.window makeKeyAndOrderFront:nil];
    PrAppEvent event{};
    event.kind = PrAppEventKind_Notification;
    event.notification.tag = tag;
    event.time = NSProcessInfo.processInfo.systemUptime;
    [self deliver:event];
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
- (void)windowDidBecomeKey:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    [self releaseHeld];
    [self requestFrame];
}
- (void)applicationDidBecomeActive:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
- (void)applicationDidResignActive:(NSNotification*)note {
    (void)note;
    [self requestFrame];
}
- (void)windowWillClose:(NSNotification*)note {
    (void)note;
    [self stop];
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
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)application {
    (void)application;
    [self stop];
    return NSTerminateCancel;
}
@end

PrApp* prAppCocoaCreate(const PrAppDesc* desc) {
    CocoaApp* app = allocZeroed<CocoaApp>();
    app->destroy = destroy;
    app->run = run;
    app->stop = stop;
    app->wake = wake;
    app->requestFrame = requestFrame;
    app->requestFrameAt = requestFrameAt;
    app->setCursor = setCursor;
    app->setTextInputRect = setTextInputRect;
    app->setTitle = setTitle;
    app->setExtent = setExtent;
    app->requestAttention = requestAttention;
    app->isActive = isActive;
    app->notificationPost = notificationPost;
    app->surface = surface;
    app->openUrl = openUrl;
    app->requestClipboardText = requestClipboardText;
    app->setClipboardText = setClipboardText;
    app->requestClipboardImage = requestClipboardImage;
    app->setClipboardImage = setClipboardImage;
    app->browserCreate = [](PrApp* base, const PrAppBrowserDesc* desc, PrAppBrowser** out) {
        return cocoaBrowserCreate(base, (__bridge void*)hostOf(base).view, desc, out);
    };
    app->browserDestroy = cocoaBrowserDestroy;
    app->browserResize = cocoaBrowserResize;
    app->browserLoad = cocoaBrowserLoad;
    app->browserEvaluate = cocoaBrowserEvaluate;
    app->browserCapture = cocoaBrowserCapture;
    app->browserShow = cocoaBrowserShow;
    app->browserHide = cocoaBrowserHide;
    app->browserPrint = cocoaBrowserPrint;
    app->browserProcesses = cocoaBrowserProcesses;
    PrCocoaHost* host = [[PrCocoaHost alloc] init];
    host.app = app;
    host.title = [[NSString alloc] initWithBytes:desc->title.data length:desc->title.size
        encoding:NSUTF8StringEncoding];
    host.size = NSMakeSize(desc->extent.x, desc->extent.y);
    host.headless = desc->headless;
    app->host = (__bridge_retained void*)host;
    return app;
}
