// The Cocoa application backend: a macOS window and the AppKit event loop.

#include "pr/app_cocoa.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/vec.h"

#include "app/backends/cocoa/src/cocoa_browser.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

@class PrCocoaHost;

@interface PrCocoaView : NSView <CALayerDelegate, NSTextInputClient>
@property(nonatomic, weak) PrCocoaHost* host;
@property(nonatomic, strong) NSCursor* cursor;
@property(nonatomic) NSRect textInputRect;      // where the caret stands, from the top left of the view
@property(nonatomic, copy) NSString* marked;    // the text that an input method composes, or nil
@end

@interface PrCocoaHost : NSObject <NSApplicationDelegate, NSWindowDelegate>
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
- (void)deliver:(const PrAppEvent&)event;
- (void)endCallback;
- (void)forgetHeld;
- (void)pointerAt:(PrVec2)position;
- (void)pointerDown:(uint8_t)button;
- (BOOL)pointerUp:(uint8_t)button;
- (void)key:(PrAppKeyCode)code down:(BOOL)down key:(PrAppKey)key;
- (void)frame;
- (PrAppFrame)frameAt:(double)now;
- (void)requestFrame;
- (void)stop;
@end

namespace pr_impl {

namespace {

// An event that arrived while a callback ran, which waits for the callback to return, with its own copy of its text.
struct HeldEvent {
    PrAppEvent event;
    Array<char> text;
};

// What a key that is down reports, so that the window can release it when it stops being the key window.
struct HeldKey {
    bool down;
    PrAppKey key;
};

// The pointer buttons that the view can report down, by PrAppEvent::button.
constexpr uint8_t kPointerButtons = 32;

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
        event.text = viewOf(bytes);
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
    event.position.x = float(p.x);
    event.position.y = float(p.y);
    event.button = uint8_t(source.buttonNumber);
    event.modifiers = modifiersOf(source.modifierFlags);
    if (kind == PrAppEventKind_Scroll) {
        // A mouse wheel's deltas are lines, and a trackpad's are pixels.
        event.scrollUnit = source.hasPreciseScrollingDeltas ? PrAppScrollUnit_Pixel : PrAppScrollUnit_Line;
        event.delta.x = -float(source.scrollingDeltaX);
        event.delta.y = -float(source.scrollingDeltaY);
    }
    [self.host pointerAt:event.position];
    // A button that the window released as it stopped being the key window reports no release of its own.
    if (kind == PrAppEventKind_PointerDown)
        [self.host pointerDown:event.button];
    else if (kind == PrAppEventKind_PointerUp && ![self.host pointerUp:event.button])
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
- (void)key:(NSEvent*)source kind:(PrAppEventKind)kind {
    MacKey mac = macKeyOf(source);
    PrAppEvent event{};
    event.kind = kind;
    event.time = source.timestamp;
    event.code = mac.code;
    event.modifiers = modifiersOf(source.modifierFlags);
    event.repeat = kind == PrAppEventKind_KeyDown && source.isARepeat;
    NSData* bytes = nil;
    if (mac.key != PrAppKey_Unknown) {
        event.key = mac.key;
    } else if (NSString* character = characterOf(source)) {
        bytes = [character dataUsingEncoding:NSUTF8StringEncoding];
        event.text = viewOf(bytes);
        unichar first = [character.uppercaseString characterAtIndex:0];
        event.key = first < 128 ? PrAppKey(first) : PrAppKey_Unknown;
    } else if (isDead(source)) {
        event.key = PrAppKey_Dead;
    }
    [self.host key:event.code down:kind == PrAppEventKind_KeyDown key:event.key];
    [self.host deliver:event];
}
- (void)keyDown:(NSEvent*)e {
    [self key:e kind:PrAppEventKind_KeyDown];
    // The input method types the key's text, if any, through insertText: or composes it through setMarkedText:. A
    // Command chord types nothing.
    if ((e.modifierFlags & NSEventModifierFlagCommand) == 0)
        [self interpretKeyEvents:@[ e ]];
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
    event.text = viewOf(bytes);
    if (selection.location != NSNotFound)
        event.selection = PrAppTextRange{
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
    [self deliverText:text kind:PrAppEventKind_CompositionUpdate selection:selectedRange];
}
- (void)unmarkText {
    if (self.marked != nil)
        [self commit:self.marked];
}
- (NSRange)selectedRange {
    return NSMakeRange(0, 0);
}
- (NSRange)markedRange {
    return self.marked != nil ? NSMakeRange(0, self.marked.length) : NSMakeRange(NSNotFound, 0);
}
- (BOOL)hasMarkedText {
    return self.marked != nil;
}
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    (void)actualRange;
    return nil;
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    (void)actualRange;
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
    Array<HeldEvent> _held;

    // The pointer buttons that are down, by bit of PrAppEvent::button, where the pointer last was, and the keys that
    // are down, by their position. The window releases them when it stops being the key window, as the releases then go
    // to another window.
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
        event.text = PrStringView{
            _held.data[i].text.data != nullptr ? _held.data[i].text.data : "",
            _held.data[i].text.count,
        };
        [self callEvent:event];
    }
    for (uint32_t i = 0; i < _held.count; ++i)
        _held.data[i].text.reset();
    _held.clear();
}
// Delivers an event, or holds it until the running callback returns.
- (void)deliver:(const PrAppEvent&)event {
    if (!self.running)
        return;
    if (self.inCallback) {
        HeldEvent held{.event = event};
        if (event.text.size != 0)
            held.text.append(event.text.data, uint32_t(event.text.size));
        _held.push(held);
        return;
    }
    [self callEvent:event];
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
        event.button = button;
        event.position = _pointer;
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
        event.key = _keys[code].key;
        event.code = PrAppKeyCode(code);
        event.text = "";
        [self deliver:event];
    }
}
// Forgets the buttons and the keys that are down, at the end of a run.
- (void)forgetHeld {
    _buttons = 0;
    memZero(_keys, sizeof(_keys));
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
- (void)stop {
    if (!self.running)
        return;
    self.running = NO;
    [self.displayLink invalidate];
    self.displayLink = nil;
    [self.timer invalidate];
    self.timer = nil;
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
}
- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    [self releaseHeld];
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
    app->setCursor = setCursor;
    app->setTextInputRect = setTextInputRect;
    app->setTitle = setTitle;
    app->surface = surface;
    app->openUrl = openUrl;
    app->requestClipboardText = requestClipboardText;
    app->setClipboardText = setClipboardText;
    app->browserCreate = [](PrApp* app, const PrAppBrowserDesc* desc, PrAppBrowser** out) {
        return cocoaBrowserCreate(app, (__bridge void*)hostOf(app).view, desc, out);
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
