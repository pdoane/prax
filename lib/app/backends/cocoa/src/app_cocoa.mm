// The Cocoa application backend: macOS windows and the AppKit event loop.

#include "pr/app_cocoa.h"

#include "pr/alloc.h"
#include "pr/app_backend.h"
#include "pr/atomic.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/random.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/vec.h"

#include "app/backends/shared/src/app_shared_data.h"
#include "app/backends/shared/src/app_shared_delivery.h"
#include "app/backends/shared/src/app_shared_drag.h"
#include "app/backends/shared/src/app_shared_title_bar.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <IOKit/ps/IOPSKeys.h>
#import <IOKit/ps/IOPowerSources.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UserNotifications/UserNotifications.h>

@class PrCocoaHost;
@class PrCocoaMenuTree;
@class PrCocoaWindow;

// The panel of an overlay or a popup. An overlay's panel becomes the key window when the program focuses it. A popup's
// never does, so that its parent keeps the keyboard.
@interface PrCocoaPanel : NSPanel
@property(nonatomic) BOOL refusesKey;
@end

// The window of a Normal or Dialog window without decoration, which AppKit would otherwise never make the key window.
@interface PrCocoaBorderlessWindow : NSWindow
@end

// The Dock tile's content while it shows progress: the application's icon, with a bar over its lower part.
@interface PrCocoaDockProgress : NSView
@property(nonatomic) PrAppProgressState state;
@property(nonatomic) float value;
@end

@interface PrCocoaView : NSView <CALayerDelegate, NSTextInputClient, NSDraggingSource>
@property(nonatomic, weak) PrCocoaWindow* owner;
@property(nonatomic, weak) PrCocoaHost* host;   // which outlives the owner, for a drag that ends after it closes
@property(nonatomic, strong) NSEvent* press;    // the latest event of the primary button while it is down, or nil
@property(nonatomic, strong) NSCursor* cursor;
@property(nonatomic) NSRect textInputRect;      // where the caret stands, from the top left of the view
@property(nonatomic, copy) NSString* marked;    // the text that an input method composes, or nil
@property(nonatomic) NSRange markedSelection;   // the selection within the marked text
@end

// A window: its NSWindow and view, its frames, and what is held down in it.
@interface PrCocoaWindow : NSObject <NSWindowDelegate>
@property(nonatomic) PrAppWindow* record;       // null once the window is destroyed
@property(nonatomic, weak) PrCocoaHost* host;
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) PrCocoaView* view;
@property(nonatomic, strong) CADisplayLink* displayLink;
@property(nonatomic, strong) NSTimer* timer;        // a headless window's frame
@property(nonatomic, strong) NSTimer* frameTimer;   // the frame at the time that the last frame asked for
@property(nonatomic) BOOL frameRequested;
@property(nonatomic) double lastFrame;
@property(nonatomic) BOOL eraser;   // the pen near the tablet points its eraser at it

// The state to take once the window leaves fullscreen, which AppKit does in steps, or Fullscreen for none.
@property(nonatomic) PrAppWindowState afterFullscreen;

// True if the program draws the title bar (PrAppWindowDecoration_Custom).
@property(nonatomic) BOOL customTitleBar;

// The window button of a custom title bar whose press waits for its release, or Client for none.
@property(nonatomic) PrAppTitleBarRegionKind pressedButton;

@property(nonatomic) BOOL overlay;                  // the window is an overlay (PrAppWindowRole_Overlay)
@property(nonatomic) PrAppWindowRole role;

// The window that a Dialog or a Popup belongs to, or nil. A modal Dialog with decoration shows as a sheet on its
// parent, and one without as a child window that blocks its parent.
@property(nonatomic, weak) PrCocoaWindow* parent;
@property(nonatomic) BOOL modal;
@property(nonatomic) BOOL sheet;

// The modal dialog without decoration that blocks the window's input, or nil, and the layer that dims the window's
// content while it does.
@property(nonatomic, weak) PrCocoaWindow* blocker;
@property(nonatomic, strong) CALayer* dim;
@property(nonatomic) PrAppPopupPlacement popup;

@property(nonatomic) BOOL secureInput;                     // the window holds secure event input for a password
@property(nonatomic, strong) PrCocoaMenuTree* menuBar;     // the window's own menu bar, or nil
@property(nonatomic, strong) PrCocoaMenuTree* contextMenu; // the context menu that shows, or nil
- (void)promise:(uint32_t)index to:(NSURL*)url done:(void (^)(NSError* PR_NULLABLE))done;
- (void)endPromise:(uint32_t)index status:(PrStatus)status;
- (void)endPromises;
@property(nonatomic) BOOL shown;                           // the program shows the window (prAppWindowSetVisible)
@property(nonatomic) BOOL showing;                         // the window shows once its next frame is drawn
@property(nonatomic) PrAppWindowBackdrop backdrop;
@property(nonatomic, strong) NSVisualEffectView* effect;   // the blur behind the view, or nil

// The pointer is over the view. `leaving` is true while it is outside with a button down, and leaves once the buttons
// are up.
@property(nonatomic) BOOL inside;
@property(nonatomic) BOOL leaving;

// The pointer is locked at `lockedAt` in the view, or confined to the view, in `lockedMode`. The monitor watches the
// application's mouse events while the pointer is confined.
@property(nonatomic) BOOL locked;
@property(nonatomic) BOOL confined;
@property(nonatomic) PrAppPointerMode lockedMode;
@property(nonatomic) PrVec2 lockedAt;
@property(nonatomic, strong) id PR_NULLABLE confineMonitor;
@property(nonatomic) BOOL acceptedMoves;   // the window's acceptsMouseMovedEvents before the pointer was confined
- (PrAppTitleBar)titleBar;
- (BOOL)titleBarPress:(NSEvent*)event;
- (BOOL)titleBarRelease:(NSEvent*)event;
- (void)setBackdropOf:(PrAppWindowBackdrop)backdrop;
- (void)setLock:(BOOL)locked;
- (void)setConfinement:(BOOL)confined;
- (void)freePointer;
- (void)showSoon;
- (void)present:(BOOL)activate;
- (void)dismiss;
- (void)blockBy:(PrCocoaWindow* PR_NULLABLE)blocker;
- (void)place;
- (void)setSecureInput:(BOOL)secure;
- (void)frame;
- (PrAppFrame)frameAt:(double)now;
- (void)requestFrame;
- (void)requestFrameAt:(double)time;
- (void)pointerAt:(PrVec2)position;
- (void)pointerDown:(uint8_t)button;
- (BOOL)pointerUp:(uint8_t)button;
- (BOOL)buttonsDown;
- (void)key:(PrAppKeyCode)code down:(BOOL)down key:(PrAppKey)key;
- (void)releaseHeld;
- (void)startDrag;
- (void)endDrag:(PrAppDragOperation)operation;
- (void)close;
@end

// A menu that AppKit shows for the program: its NSMenu, its tag, and where its events go. Each menu of a tree is its
// menu's delegate and its items' target. A menu of the application sends its events to the application, and a menu of a
// window to that window, or nowhere once the window is gone.
@interface PrCocoaMenu : NSObject <NSMenuDelegate>
@property(nonatomic) uint64_t tag;
@property(nonatomic, strong) NSMenu* menu;
@property(nonatomic, weak) PrCocoaHost* host;
@property(nonatomic, weak) PrCocoaWindow* owner;
@property(nonatomic) BOOL ofWindow;
- (void)chosen:(NSMenuItem*)item;
@end

// The menus that one description made, kept so that setting the description again updates them in place, as while one
// of them is open. `root` is the menu bar, or the menu itself.
@interface PrCocoaMenuTree : NSObject
@property(nonatomic, strong) NSMutableArray<PrCocoaMenu*>* menus;
@property(nonatomic, strong) NSMenu* root;
@end

// A status item in the menu bar, and the menu that a click opens, or the tag that a click sends if it has none.
@interface PrCocoaStatusItem : NSObject
@property(nonatomic, weak) PrCocoaHost* host;
@property(nonatomic, strong) NSStatusItem* item;
@property(nonatomic, strong) PrCocoaMenuTree* tree;
@property(nonatomic) uint64_t tag;
- (void)clicked:(id)sender;
@end

// A file that a drag of the window promised. AppKit asks for it once the drop's target knows where it goes, and the
// window then asks the program (PrAppEventKind_DragPromise).
@interface PrCocoaPromise : NSObject <NSFilePromiseProviderDelegate>
@property(nonatomic, weak) PrCocoaWindow* owner;
@property(nonatomic) uint32_t index;
@property(nonatomic, copy) NSString* name;
@end

// A share picker that waits for the user's choice, and the request that the choice answers. The host keeps it until
// then.
@interface PrCocoaShare : NSObject <NSSharingServicePickerDelegate>
@property(nonatomic) PrApp* app;
@property(nonatomic) uint32_t request;
@property(nonatomic, strong) NSSharingServicePicker* picker;
@end

// The application: its delegates, its windows, and the events that wait for a callback to return.
@interface PrCocoaHost : NSObject <NSApplicationDelegate, UNUserNotificationCenterDelegate>
@property(nonatomic) PrApp* app;
@property(nonatomic) BOOL headless;
@property(nonatomic) BOOL running;
@property(nonatomic) BOOL inCallback;    // a callback is running, and no other may start
@property(nonatomic) uint8_t permission; // notification permission, a CocoaPermission
@property(nonatomic, strong) NSMutableArray<PrCocoaWindow*>* windows;

// Notifications that wait for the user to answer the permission request, oldest first.
@property(nonatomic, strong) NSMutableArray<UNNotificationRequest*>* waiting;

// The file dialogs that are open, by the id of the read that answers each.
@property(nonatomic, strong) NSMutableDictionary<NSNumber*, NSSavePanel*>* dialogs;

// A request to quit waits for the program's answer (prAppQuitRespond).
@property(nonatomic) BOOL quitPending;
@property(nonatomic) BOOL quitWaiting;     // AppKit waits for the answer (NSTerminateLater)
- (void)quitRespond:(BOOL)quit;

@property(nonatomic) PrAppPresence presence;
@property(nonatomic, copy) NSString* singleInstance;    // the name of a single instance, or nil
- (void)setPresence:(PrAppPresence)presence;
- (void)deliverKind:(PrAppEventKind)kind;
- (PrAppLifecycle)lifecycle;
- (void)setTimer:(uint64_t)tag delay:(double)delay interval:(double)interval;
- (void)cancelTimer:(uint64_t)tag;
- (BOOL)startServices;
- (void)stopServices;

// The names of the displays, by id, which stay valid until the next DisplaysChanged event.
@property(nonatomic, strong) NSMutableDictionary<NSNumber*, NSData*>* displayNames;

@property(nonatomic, strong) NSMutableArray<PrCocoaShare*>* shares;     // the share pickers that wait for the user
@property(nonatomic, strong) PrCocoaMenuTree* menuBar;                  // the application's menu bar
- (void)showMenuBar;
- (void)closePopupsExcept:(NSWindow* PR_NULLABLE)window;
- (void)setDockProgress:(PrAppProgressState)state value:(float)value;

// The view that a drag of the application's started from, which the host keeps while the system carries the drag, as
// its window may close first.
@property(nonatomic, strong) PrCocoaView* dragSource;

- (void)deliverHotkey:(uint32_t)id;
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window;
- (uint32_t)holdEvents;
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow*)window ahead:(uint32_t)mark;
- (void)beginCallback;
- (void)endCallback;
- (void)forget:(PrAppWindow*)window;
- (void)stop;
- (PrStatus)postNotification:(const PrAppNotificationDesc*)desc;

// Delivers `answer`, a CocoaAnswer that the host owns from here on, now or once no callback runs.
- (void)answer:(void*)answer;
@end

namespace pr_impl {

namespace {

// What answers a request: content from a store, a file's bytes, a write's status, or a dialog's source, which the
// answer holds a reference to.
enum {
    CocoaAnswerKind_Store,
    CocoaAnswerKind_Bytes,
    CocoaAnswerKind_Status,
    CocoaAnswerKind_Source,
};
typedef uint8_t CocoaAnswerKind;

// The answer to the request `id`, which owns what it answers with.
struct CocoaAnswer {
    CocoaAnswerKind kind;
    uint32_t id;
    PrStatus status;
    AppDataStore store;
    AppDataStore* PR_NULLABLE external;     // a source's store, which a Store answer answers from in place of its own
    RawArray<uint8_t> bytes;
    PrAppDataSource source;
};

// An event or an answer that arrived during a callback. The host delivers it after the callback returns. It owns a copy
// of its text, because the sender's text does not outlive the call.
struct HeldEvent {
    PrAppWindow* PR_NULLABLE window;    // the target window, or null for the application
    PrAppEvent event;
    RawArray<char> text;                  // the event's text
    AppStrings arguments;                 // the arguments of an Open event
    RawArray<PrAppTrackpadTouch> touches; // the touches of a TrackpadTouches event
    bool dropped;                         // true if the target window closed before delivery
    CocoaAnswer* PR_NULLABLE answer;      // an answer in place of an event
};

// Copies `event` and its text for delivery after the running callback.
HeldEvent heldOf(PrAppWindow* PR_NULLABLE window, const PrAppEvent& event) {
    HeldEvent held{.window = window, .event = event};
    PrStringView text = prAppEventText(&event);
    if (text.size != 0)
        held.text.append(text.data, uint32_t(text.size));
    if (event.kind == PrAppEventKind_Open) {
        for (uint32_t i = 0; i < event.open.argumentCount; ++i)
            appStringsAdd(held.arguments, event.open.arguments[i]);
    }
    if (event.kind == PrAppEventKind_TrackpadTouches && event.trackpad.count != 0)
        held.touches.append(event.trackpad.touches, event.trackpad.count);
    return held;
}

// Makes an answer of `kind` to the request `id`.
CocoaAnswer* answerOf(CocoaAnswerKind kind, uint32_t id, PrStatus status) {
    CocoaAnswer* answer = allocZeroed<CocoaAnswer>();
    answer->kind = kind;
    answer->id = id;
    answer->status = status;
    return answer;
}

// Frees an answer.
void answerDestroy(CocoaAnswer* PR_NULLABLE answer) {
    if (answer == nullptr)
        return;
    appDataStoreReset(answer->store);
    answer->bytes.reset();
    prMemoryFree(answer);
}

// Returns the source that `event` hands over, whose reference the event holds until its delivery ends, or None.
PrAppDataSource sourceOf(const PrAppEvent& event) {
    if (event.kind == PrAppEventKind_Drop)
        return event.drag.drop;
    if (event.kind == PrAppEventKind_Open)
        return event.open.source;
    return kPrAppDataSourceNone;
}

// Frees what a held event owns, and releases the source that it handed over, of `sources` if it is not null.
void heldReset(HeldEvent& held, AppDataSources* PR_NULLABLE sources) {
    held.text.reset();
    appStringsReset(held.arguments);
    held.touches.reset();
    answerDestroy(held.answer);
    held.answer = nullptr;
    PrAppDataSource source = sourceOf(held.event);
    if (sources != nullptr && source != kPrAppDataSourceNone)
        appDataSourceRelease(*sources, source);
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

// The pointer id of a pen is this plus its tablet's device id, so that it never equals kPrAppPointerMouse.
constexpr uint32_t kPenPointerBase = 2;

// Describes the pen of a tablet event in `event`: its pressure, tilt and twist, and its eraser as
// kPrAppPointerButtonEraser. AppKit reports tilt in [-1, 1] on each axis with +y toward the top of the tablet, and the
// barrel's rotation in degrees counterclockwise.
void describePen(NSEvent* source, bool eraser, PrAppEvent* event) {
    event->pointer.type = PrAppPointerType_Pen;
    event->pointer.id = kPenPointerBase + uint32_t(source.deviceID);
    event->pointer.pressure = float(source.pressure);
    NSPoint tilt = source.tilt;
    event->pointer.tilt = PrVec2{float(tilt.x) * 90.0f, float(-tilt.y) * 90.0f};
    float twist = 360.0f - float(fmod(double(source.rotation), 360.0));
    event->pointer.twist = twist >= 360.0f ? twist - 360.0f : twist;
    if (eraser && event->kind != PrAppEventKind_PointerMove)
        event->pointer.button = kPrAppPointerButtonEraser;
}

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

// The private pasteboard type of a drag that a window of the application started. Its data is the drag's token. A
// window of the same process compares the token with the drag in flight, and takes that drag's payload if they match.
NSPasteboardType const kPayloadType = @"org.prax.app.drag-payload";

// The drag that a window of the application started, while the system carries it.
struct CocoaDrag {
    bool active;
    uint64_t token;             // a random value that names the drag on its pasteboard
    PrAppWindow* PR_NULLABLE window;
    PrAppDataPayload payload;
    bool hasPayload;
};

// A global hotkey that the application registered: the program's tag, and Carbon's id and registration of it.
struct CocoaHotkey {
    uint64_t tag;
    uint32_t id;
    EventHotKeyRef ref;
};

struct CocoaApp : PrApp {
    void* host;                 // the PrCocoaHost, retained
    PrAppHandler handler;       // the handler of the run
    PrAppWindowModel windowModel;
    Atomic<bool> wakePending;
    AppDataSources sources;
    AppDataRequests requests;
    CocoaDrag drag;

    // The global hotkeys, and Carbon's handler of their presses, which is installed with the first.
    RawArray<CocoaHotkey> hotkeys;
    uint32_t lastHotkeyId;
    EventHandlerRef PR_NULLABLE hotkeyHandler;

    // The power assertions that keep the display or the system awake (prAppSetAwake), or kIOPMNullAssertionID.
    IOPMAssertionID displayAwake;
    IOPMAssertionID systemAwake;

    // The label of each key code in the current layout, computed on first use (prAppKeyCodeLabel), which a query of the
    // application fills. An empty label that is computed means the key's name.
    mutable PrRawString keyLabels[PrAppKeyCode_Count];
    mutable bool keyLabeled[PrAppKeyCode_Count];
};

struct CocoaWindow : PrAppWindow {
    void* object;               // the PrCocoaWindow, retained
    PrApp* app;
    AppWindowDelivery delivery;
    AppTitleBarRegions titleBar;
    PrRawString title;

    PrAppDragOperation dropOperation;   // the answer to a drag over the window, from the latest output
    AppDragPending drag;                // the drag that prAppWindowDragStart asked for, kept while it is in flight

    // The text being edited, as the window's latest output reported it: the caret, in logical px of `textInputWindow`,
    // which is the window itself or a window that it owns, or null if the output has none.
    PrAppWindow* PR_NULLABLE textInputWindow;
    PrAppTextInputKind textInputKind;
    PrRect textInput;
};

PrCocoaHost* hostOf(const PrApp* app) {
    return (__bridge PrCocoaHost*)static_cast<const CocoaApp*>(app)->host;
}

PrCocoaWindow* objectOf(const PrAppWindow* window) {
    return (__bridge PrCocoaWindow*)static_cast<const CocoaWindow*>(window)->object;
}

double uptime() {
    return NSProcessInfo.processInfo.systemUptime;
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

// Returns the key that a key that types a character means: the character that it types in the layout with Shift applied
// and no other modifier, uppercase for a letter. Option-1 is the 1 key with Alt held, though it types "¡", as Alt-1 is
// on other systems. Returns PrAppKey_Unknown for a character outside ASCII, or a key that types none.
PrAppKey keyOfCharacter(NSEvent* event) {
    // charactersIgnoringModifiers keeps Shift and drops Option, Control and Command.
    NSString* plain = event.charactersIgnoringModifiers;
    unichar first = plain.length != 0 ? [plain.uppercaseString characterAtIndex:0] : 0;
    return first >= ' ' && first < 127 ? PrAppKey(first) : PrAppKey_Unknown;
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

NSString* stringOf(PrStringView text) {
    NSString* string = [[NSString alloc] initWithBytes:text.data length:text.size encoding:NSUTF8StringEncoding];
    return string != nil ? string : @"";
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

// Returns the cursor of AppKit's undeclared class method `name`, or the arrow if AppKit has none. Only the help and
// busy cursors come from here: some undeclared methods, such as `_moveCursor`, answer the selector and then throw.
NSCursor* undeclaredCursor(NSString* name) {
    SEL selector = NSSelectorFromString(name);
    if (![NSCursor respondsToSelector:selector])
        return NSCursor.arrowCursor;
    using CursorFn = NSCursor* (*)(id, SEL);
    NSCursor* cursor = reinterpret_cast<CursorFn>([NSCursor methodForSelector:selector])(NSCursor.class, selector);
    return cursor != nil ? cursor : NSCursor.arrowCursor;
}

// Returns a cursor of arrows in four directions, for Move and AllScroll, which AppKit lacks. It is drawn in black with
// a white outline, as the system's cursors are.
NSCursor* moveCursor() {
    static NSCursor* move = [] {
        constexpr CGFloat kSize = 24;
        NSImage* image = [NSImage imageWithSize:NSMakeSize(kSize, kSize) flipped:NO drawingHandler:^BOOL(NSRect bounds) {
            (void)bounds;
            // One arrow, pointing up from the center, turned to each direction.
            NSBezierPath* arrows = [NSBezierPath bezierPath];
            for (int turn = 0; turn < 4; ++turn) {
                NSAffineTransform* transform = [NSAffineTransform transform];
                [transform translateXBy:kSize / 2 yBy:kSize / 2];
                [transform rotateByDegrees:CGFloat(turn * 90)];
                NSBezierPath* arrow = [NSBezierPath bezierPath];
                [arrow moveToPoint:NSMakePoint(-1.5, 0)];
                [arrow lineToPoint:NSMakePoint(-1.5, 6)];
                [arrow lineToPoint:NSMakePoint(-4.5, 6)];
                [arrow lineToPoint:NSMakePoint(0, 10.5)];
                [arrow lineToPoint:NSMakePoint(4.5, 6)];
                [arrow lineToPoint:NSMakePoint(1.5, 6)];
                [arrow lineToPoint:NSMakePoint(1.5, 0)];
                [arrow closePath];
                [arrow transformUsingAffineTransform:transform];
                [arrows appendBezierPath:arrow];
            }
            arrows.lineJoinStyle = NSLineJoinStyleRound;
            arrows.lineWidth = 2;
            [NSColor.whiteColor setStroke];
            [arrows stroke];
            [NSColor.blackColor setFill];
            [arrows fill];
            return YES;
        }];
        return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(kSize / 2, kSize / 2)];
    }();
    return move;
}

// Returns the system's nearest cursor to `cursor`. A cell shows the crosshair. macOS has no wait cursor of a program's
// own, since the system shows its own while a program does not respond, so a wait shows the busy arrow of progress.
NSCursor* cursorFor(PrAppCursor cursor) {
    constexpr NSCursorFrameResizeDirections kOutward = NSCursorFrameResizeDirectionsOutward;
    constexpr NSCursorFrameResizeDirections kBoth = NSCursorFrameResizeDirectionsAll;
    switch (cursor) {
        case PrAppCursor_None: return emptyCursor();
        case PrAppCursor_ContextMenu: return NSCursor.contextualMenuCursor;
        case PrAppCursor_Help: return undeclaredCursor(@"_helpCursor");
        case PrAppCursor_Hand: return NSCursor.pointingHandCursor;
        case PrAppCursor_Progress: return undeclaredCursor(@"busyButClickableCursor");
        case PrAppCursor_Wait: return undeclaredCursor(@"busyButClickableCursor");
        case PrAppCursor_Cell: return NSCursor.crosshairCursor;
        case PrAppCursor_Crosshair: return NSCursor.crosshairCursor;
        case PrAppCursor_Text: return NSCursor.IBeamCursor;
        case PrAppCursor_VerticalText: return NSCursor.IBeamCursorForVerticalLayout;
        case PrAppCursor_Alias: return NSCursor.dragLinkCursor;
        case PrAppCursor_Copy: return NSCursor.dragCopyCursor;
        case PrAppCursor_Move: return moveCursor();
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
        case PrAppCursor_AllScroll: return moveCursor();
        case PrAppCursor_ZoomIn: return NSCursor.zoomInCursor;
        case PrAppCursor_ZoomOut: return NSCursor.zoomOutCursor;
        default: return NSCursor.arrowCursor;
    }
}

// Returns true if the process runs from an application bundle. In any other process, the notification center raises an
// exception, even if the executable embeds an Info.plist with a bundle identifier. The older NSUserNotificationCenter
// has no default center in such a process.
bool isBundled() {
    NSBundle* bundle = NSBundle.mainBundle;
    return bundle.bundleIdentifier != nil && [bundle.bundleURL.pathExtension isEqualToString:@"app"];
}

PrUVec2 imagePixels(NSImage* PR_NULLABLE image, NSMutableData* pixels);

// Returns the image that the pasteboard holds or names, as an image file copied in the Finder does, as straight-alpha
// sRGB Rgba8 rows in `pixels`, and its extent, or a zero extent if it holds none.
PrUVec2 pasteboardPixels(NSPasteboard* board, NSMutableData* pixels) {
    if (![NSImage canInitWithPasteboard:board])
        return PrUVec2{0, 0};
    return imagePixels([[NSImage alloc] initWithPasteboard:board], pixels);
}

// Returns `image` as straight-alpha sRGB Rgba8 rows in `pixels`, and its extent, or a zero extent if there is none.
PrUVec2 imagePixels(NSImage* PR_NULLABLE image, NSMutableData* pixels) {
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

// Returns straight-alpha sRGB Rgba8 `pixels` as an image representation, or nil if AppKit cannot make one.
NSBitmapImageRep* PR_NULLABLE imageRepOf(PrPixelView pixels) {
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
        pixelsWide:NSInteger(pixels.extent.x) pixelsHigh:NSInteger(pixels.extent.y) bitsPerSample:8 samplesPerPixel:4
        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
        bytesPerRow:NSInteger(pixels.extent.x) * 4 bitsPerPixel:32];
    if (rep == nil)
        return nil;
    for (uint32_t y = 0; y < pixels.extent.y; ++y)
        prMemCopy(rep.bitmapData + size_t(y) * size_t(rep.bytesPerRow), pixels.data + y * pixels.stride,
            size_t(pixels.extent.x) * 4);
    return [rep bitmapImageRepByRetaggingWithColorSpace:NSColorSpace.sRGBColorSpace];
}

// ---------------------------------------------------------------------------------------------------------------------
// Drags

// Returns the drag operations of an AppKit mask. The generic operation lets the destination choose. A destination moves
// within a volume, so the generic operation counts as a move.
PrAppDragOperationFlags operationsOf(NSDragOperation mask) {
    PrAppDragOperationFlags operations = 0;
    if ((mask & NSDragOperationCopy) != 0)
        operations |= PrAppDragOperationFlag_Copy;
    if ((mask & (NSDragOperationMove | NSDragOperationGeneric)) != 0)
        operations |= PrAppDragOperationFlag_Move;
    if ((mask & NSDragOperationLink) != 0)
        operations |= PrAppDragOperationFlag_Link;
    return operations;
}

// Returns the AppKit mask of a set of drag operations.
NSDragOperation maskOf(PrAppDragOperationFlags operations) {
    NSDragOperation mask = NSDragOperationNone;
    if ((operations & PrAppDragOperationFlag_Copy) != 0)
        mask |= NSDragOperationCopy;
    if ((operations & PrAppDragOperationFlag_Move) != 0)
        mask |= NSDragOperationMove | NSDragOperationGeneric;
    if ((operations & PrAppDragOperationFlag_Link) != 0)
        mask |= NSDragOperationLink;
    return mask;
}

// Returns the operation that a destination answers with, within the source's `mask`.
NSDragOperation answerOf(PrAppDragOperation operation, NSDragOperation mask) {
    switch (operation) {
        case PrAppDragOperation_Copy: return NSDragOperationCopy;
        case PrAppDragOperation_Move:
            return (mask & NSDragOperationMove) != 0 ? NSDragOperationMove : NSDragOperationGeneric;
        case PrAppDragOperation_Link: return NSDragOperationLink;
        default: return NSDragOperationNone;
    }
}

// Returns the operation that a drag's session reports as it ends.
PrAppDragOperation operationOf(NSDragOperation operation) {
    if ((operation & (NSDragOperationMove | NSDragOperationGeneric)) != 0)
        return PrAppDragOperation_Move;
    if ((operation & NSDragOperationCopy) != 0)
        return PrAppDragOperation_Copy;
    if ((operation & NSDragOperationLink) != 0)
        return PrAppDragOperation_Link;
    return PrAppDragOperation_None;
}

// The types of a pasteboard that hold an image's own data, rather than its file.
NSArray<NSPasteboardType>* imageTypes() {
    return @[ NSPasteboardTypePNG, NSPasteboardTypeTIFF ];
}

// The options that read only file URLs from a pasteboard, or every URL.
NSDictionary<NSPasteboardReadingOptionKey, id>* fileOptions() {
    return @{NSPasteboardURLReadingFileURLsOnlyKey: @YES};
}

// Returns true if `type` is a pasteboard type of the system's, which no custom content of a program's has.
bool isSystemType(NSPasteboardType type) {
    for (NSString* prefix in @[ @"public.", @"com.apple.", @"dyn.", @"NS", @"Apple", @"CorePasteboardFlavorType" ]) {
        if ([type hasPrefix:prefix])
            return true;
    }
    return [type isEqualToString:kPayloadType];
}

// Returns the URLs of the pasteboard that do not name files.
NSArray<NSURL*>* webUrlsOf(NSPasteboard* board) {
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    for (NSURL* url in [board readObjectsForClasses:@[ NSURL.class ] options:@{}]) {
        if (!url.isFileURL)
            [urls addObject:url];
    }
    return urls;
}

// Returns the forms of content that the pasteboard holds, other than a payload.
PrAppDataFormFlags formsOf(NSPasteboard* board) {
    PrAppDataFormFlags forms = 0;
    if ([board availableTypeFromArray:@[ NSPasteboardTypeString ]] != nil)
        forms |= PrAppDataFormFlag_Text;
    if ([board availableTypeFromArray:@[ NSPasteboardTypeHTML ]] != nil)
        forms |= PrAppDataFormFlag_Html;
    if (webUrlsOf(board).count != 0)
        forms |= PrAppDataFormFlag_Urls;
    if ([board canReadObjectForClasses:@[ NSURL.class ] options:fileOptions()])
        forms |= PrAppDataFormFlag_Files;
    if ([board availableTypeFromArray:imageTypes()] != nil)
        forms |= PrAppDataFormFlag_Image;
    for (NSPasteboardType type in board.types) {
        if (!isSystemType(type))
            forms |= PrAppDataFormFlag_Custom;
    }
    return forms;
}

// Adds the file at `url` to `out`, with its name, its path, its size and whether it is a directory.
void addFile(NSURL* url, AppDataStore& out) {
    NSNumber* size = nil;
    NSNumber* directory = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    [url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:nil];
    NSData* name = [url.lastPathComponent dataUsingEncoding:NSUTF8StringEncoding];
    appDataStoreAddFile(out, PrAppDataFile{
        .name = viewOf(name),
        .path = url.fileSystemRepresentation,
        .size = size.unsignedLongLongValue,
        .directory = directory.boolValue,
    });
}

// Reads the content of the pasteboard that `answer` asks for into `out`. A read of forms reads them all but the image
// and the custom content, whose forms the store then names. Text reads a link that holds no text as its URL.
void readPasteboard(NSPasteboard* board, AppDataAnswer answer, PrStringView type, AppDataStore& out) {
    switch (answer) {
        case PrAppDataReadKind_Forms:
            out.forms = formsOf(board);
            break;
        case PrAppDataReadKind_Text: {
            NSString* text = [board stringForType:NSPasteboardTypeString];
            if (text == nil)
                text = [NSURL URLFromPasteboard:board].absoluteString;
            appDataStoreSetText(out, viewOf([text != nil ? text : @"" dataUsingEncoding:NSUTF8StringEncoding]));
            break;
        }
        case PrAppDataReadKind_Html: {
            NSString* html = [board stringForType:NSPasteboardTypeHTML];
            appDataStoreSetHtml(out, viewOf([html != nil ? html : @"" dataUsingEncoding:NSUTF8StringEncoding]));
            break;
        }
        case PrAppDataReadKind_Urls:
            for (NSURL* url in webUrlsOf(board))
                appDataStoreAddUrl(out, viewOf([url.absoluteString dataUsingEncoding:NSUTF8StringEncoding]));
            break;
        case PrAppDataReadKind_Files:
            for (NSURL* url in [board readObjectsForClasses:@[ NSURL.class ] options:fileOptions()])
                addFile(url, out);
            break;
        case PrAppDataReadKind_Image: {
            NSMutableData* pixels = [NSMutableData data];
            PrUVec2 extent = pasteboardPixels(board, pixels);
            if (extent.x != 0 && extent.y != 0)
                appDataStoreSetImage(out, PrPixelView{
                    static_cast<const uint8_t*>(pixels.bytes), size_t(extent.x) * 4,
                    extent,
                });
            break;
        }
        case PrAppDataReadKind_Custom: {
            NSData* bytes = [board dataForType:stringOf(type)];
            if (bytes != nil)
                appDataStoreAddCustom(out, type, PrByteView{static_cast<const uint8_t*>(bytes.bytes), bytes.length});
            break;
        }
        default:
            break;
    }
}

// Reads every form of a drop's pasteboard, which is the system's only until the drop returns, into `out`. The image
// keeps its encoded bytes as custom content of its pasteboard type, which a read of the image decodes, so that a
// program that reads the text decodes no image.
void readDrop(NSPasteboard* board, AppDataStore& out) {
    PrAppDataFormFlags forms = formsOf(board);
    // The forms that the drop reads now, each with the answer that reads it.
    struct Read {
        PrAppDataFormFlags form;
        AppDataAnswer answer;
    };
    constexpr Read kReads[] = {
        {PrAppDataFormFlag_Text, PrAppDataReadKind_Text},
        {PrAppDataFormFlag_Html, PrAppDataReadKind_Html},
        {PrAppDataFormFlag_Urls, PrAppDataReadKind_Urls},
        {PrAppDataFormFlag_Files, PrAppDataReadKind_Files},
    };
    for (const Read& read : kReads) {
        if ((forms & read.form) != 0)
            readPasteboard(board, read.answer, {}, out);
    }
    for (NSPasteboardType type in board.types) {
        bool image = [imageTypes() containsObject:type];
        if (!image && isSystemType(type))
            continue;
        NSData* bytes = [board dataForType:type];
        if (bytes != nil)
            appDataStoreAddCustom(out, viewOf([type dataUsingEncoding:NSUTF8StringEncoding]),
                PrByteView{static_cast<const uint8_t*>(bytes.bytes), bytes.length});
    }
    // The image shows as a form of its own, and not as custom content.
    out.forms = forms;
}

// Decodes the image of a drop's store, kept as the custom content of its pasteboard type, into `out`.
void readDropImage(AppDataStore& store, AppDataStore& out) {
    for (NSPasteboardType type in imageTypes()) {
        PrByteView bytes = appDataStoreCustom(store, viewOf([type dataUsingEncoding:NSUTF8StringEncoding]));
        if (bytes.size == 0)
            continue;
        NSData* data = [NSData dataWithBytesNoCopy:const_cast<uint8_t*>(bytes.data) length:bytes.size freeWhenDone:NO];
        NSMutableData* pixels = [NSMutableData data];
        PrUVec2 extent = imagePixels([[NSImage alloc] initWithData:data], pixels);
        if (extent.x != 0 && extent.y != 0)
            appDataStoreSetImage(out, PrPixelView{
                static_cast<const uint8_t*>(pixels.bytes), size_t(extent.x) * 4,
                extent,
            });
        return;
    }
}

// Writes `content` to `first`, the first item of a pasteboard, and its further files to items of their own, which it
// adds to `writers` after `first`. `token`, if not zero, names the application's drag of the content's payload.
void writeContent(const PrAppDataContent& content, uint64_t token, NSPasteboardItem* first,
        NSMutableArray<id<NSPasteboardWriting>>* writers) {
    if ((content.forms & PrAppDataFormFlag_Text) != 0)
        [first setString:stringOf(content.text) forType:NSPasteboardTypeString];
    if ((content.forms & PrAppDataFormFlag_Html) != 0)
        [first setString:stringOf(content.html) forType:NSPasteboardTypeHTML];
    if ((content.forms & PrAppDataFormFlag_Urls) != 0 && content.urlCount != 0)
        [first setString:stringOf(content.urls[0]) forType:NSPasteboardTypeURL];
    if ((content.forms & PrAppDataFormFlag_Image) != 0 && content.image.data != nullptr) {
        NSBitmapImageRep* rep = imageRepOf(content.image);
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (png != nil)
            [first setData:png forType:NSPasteboardTypePNG];
        if (rep.TIFFRepresentation != nil)
            [first setData:rep.TIFFRepresentation forType:NSPasteboardTypeTIFF];
    }
    for (uint32_t i = 0; (content.forms & PrAppDataFormFlag_Custom) != 0 && i < content.customCount; ++i) {
        const PrAppDataCustom& custom = content.custom[i];
        [first setData:[NSData dataWithBytes:custom.bytes.data length:custom.bytes.size] forType:stringOf(custom.type)];
    }
    if ((content.forms & PrAppDataFormFlag_Payload) != 0 && token != 0)
        [first setData:[NSData dataWithBytes:&token length:sizeof(token)] forType:kPayloadType];
    for (uint32_t i = 0; (content.forms & PrAppDataFormFlag_Files) != 0 && i < content.pathCount; ++i) {
        NSString* url = [NSURL fileURLWithPath:stringOf(content.paths[i])].absoluteString;
        NSPasteboardItem* item = i == 0 ? first : [[NSPasteboardItem alloc] init];
        [item setString:url forType:NSPasteboardTypeFileURL];
        if (i != 0)
            [writers addObject:item];
    }
}

// Returns true if the pasteboard carries the token of the drag that the application has in flight.
bool carriesDrag(NSPasteboard* board, const CocoaDrag& drag) {
    NSData* data = [board dataForType:kPayloadType];
    uint64_t token = 0;
    if (!drag.active || data.length != sizeof(token))
        return false;
    [data getBytes:&token length:sizeof(token)];
    return token == drag.token;
}

// ---------------------------------------------------------------------------------------------------------------------
// Displays

// Returns the id of a screen: its UUID folded to 64 bits, which stays the same across connections, or its display
// number if it has none. Neither is ever zero.
PrAppDisplayId displayIdOf(NSScreen* screen) {
    CGDirectDisplayID number = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(number);
    if (uuid == nullptr)
        return PrAppDisplayId(number) | (PrAppDisplayId(1) << 63);
    CFUUIDBytes bytes = CFUUIDGetUUIDBytes(uuid);
    CFRelease(uuid);
    uint64_t halves[2];
    static_assert(sizeof(halves) == sizeof(bytes), "a UUID is 16 bytes");
    prMemCopy(halves, &bytes, sizeof(bytes));
    PrAppDisplayId id = halves[0] ^ halves[1];
    return id != kPrAppDisplayNone ? id : 1;
}

// Returns the screen with `id`, or nil if none is connected.
NSScreen* PR_NULLABLE screenOf(PrAppDisplayId id) {
    for (NSScreen* screen in NSScreen.screens) {
        if (displayIdOf(screen) == id)
            return screen;
    }
    return nil;
}

// Returns what `screen` shows at `pixelRatio`.
PrAppDisplay displayOf(NSScreen* PR_NULLABLE screen, CGFloat pixelRatio) {
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = float(pixelRatio);
    if (screen != nil) {
        display.headroom = float(screen.maximumExtendedDynamicRangeColorComponentValue);
        display.potentialHeadroom = float(screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
        if ([screen canRepresentDisplayGamut:NSDisplayGamutP3])
            display.gamut = PrColorSpace_DisplayP3;
    }
    return display;
}

// Returns `rect`, in AppKit's global points, from the top left of `screen`, in the screen's logical pixels.
PrRect rectOnScreen(NSScreen* screen, NSRect rect) {
    NSRect frame = screen.frame;
    float left = float(NSMinX(rect) - NSMinX(frame));
    float top = float(NSMaxY(frame) - NSMaxY(rect));
    return PrRect{left, top, left + float(rect.size.width), top + float(rect.size.height)};
}

// Returns how `screen` is turned. A screen whose rotation leaves it taller than wide is a portrait.
PrAppOrientation orientationOf(NSScreen* screen) {
    CGDirectDisplayID number = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    double rotation = CGDisplayRotation(number);
    if (rotation == 90)
        return PrAppOrientation_Portrait;
    if (rotation == 180)
        return PrAppOrientation_LandscapeFlipped;
    if (rotation == 270)
        return PrAppOrientation_PortraitFlipped;
    return screen.frame.size.height > screen.frame.size.width ? PrAppOrientation_Portrait : PrAppOrientation_Landscape;
}

// Returns the connected screen. `name` is the screen's name, which the caller keeps for as long as the view must last.
PrAppDisplayInfo infoOf(NSScreen* screen, bool main, NSData* PR_NULLABLE name) {
    return PrAppDisplayInfo{
        .id = displayIdOf(screen),
        .name = name != nil ? viewOf(name) : PrStringView{},
        .display = displayOf(screen, screen.backingScaleFactor),
        .extent = PrVec2{float(screen.frame.size.width), float(screen.frame.size.height)},
        .workArea = rectOnScreen(screen, screen.visibleFrame),
        .refreshRate = float(screen.maximumFramesPerSecond),
        .orientation = orientationOf(screen),
        .main = main,
    };
}

// Moves `window` so that the top left of its content is at `position` on `screen`, within the screen's work area.
void placeOnScreen(NSWindow* window, NSScreen* screen, PrVec2 position) {
    NSRect content = [window contentRectForFrameRect:window.frame];
    NSRect frame = screen.frame;
    NSRect work = screen.visibleFrame;
    CGFloat left = NSMinX(frame) + CGFloat(position.x);
    CGFloat top = NSMaxY(frame) - CGFloat(position.y);
    left = MAX(MIN(left, NSMaxX(work) - content.size.width), NSMinX(work));
    top = MIN(MAX(top, NSMinY(work) + content.size.height), NSMaxY(work));
    content.origin = NSMakePoint(left, top - content.size.height);
    [window setFrameOrigin:[window frameRectForContentRect:content].origin];
}

// Centers `window` on the work area of `screen`.
void centerOnScreen(NSWindow* window, NSScreen* screen) {
    NSRect content = [window contentRectForFrameRect:window.frame];
    NSRect work = screen.visibleFrame;
    float x = float((work.size.width - content.size.width) / 2 + NSMinX(work) - NSMinX(screen.frame));
    float y = float(NSMaxY(screen.frame) - NSMaxY(work) + (work.size.height - content.size.height) / 2);
    placeOnScreen(window, screen, PrVec2{x, y});
}

// Returns the frame of a popup's content of `size`, in AppKit's global points, as `placement` puts it against the
// content of `parent`, within the work area of the parent's screen.
NSRect popupFrame(NSWindow* parent, NSSize size, const PrAppPopupPlacement& placement) {
    // AppKit's global y grows upward from the bottom of the first screen, and the placement's grows downward.
    CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    auto downward = [top](NSRect rect) {
        return PrRect{float(NSMinX(rect)), float(top - NSMaxY(rect)), float(NSMaxX(rect)), float(top - NSMinY(rect))};
    };
    PrRect content = downward([parent contentRectForFrameRect:parent.frame]);
    const PrRect& a = placement.anchor;
    PrRect anchor{content.left + a.left, content.top + a.top, content.left + a.right, content.top + a.bottom};
    NSScreen* screen = parent.screen != nil ? parent.screen : NSScreen.mainScreen;
    bool rightToLeft = NSApp.userInterfaceLayoutDirection == NSUserInterfaceLayoutDirectionRightToLeft;
    PrAppPopupPlacement onScreen = placement;
    onScreen.anchor = anchor;
    PrRect placed = prAppPopupRect(&onScreen, PrVec2{float(size.width), float(size.height)},
        downward(screen.visibleFrame),
        PrAppLayoutDirection(rightToLeft ? PrAppLayoutDirection_RightToLeft : PrAppLayoutDirection_LeftToRight));
    return NSMakeRect(CGFloat(placed.left), top - CGFloat(placed.bottom), CGFloat(placed.right - placed.left),
        CGFloat(placed.bottom - placed.top));
}

// ---------------------------------------------------------------------------------------------------------------------
// The application's functions

void hotkeysReset(CocoaApp* app);
void setAwake(PrApp* app, PrAppAwakeFlags flags);
void menuBarOf(const PrAppMenu* PR_NULLABLE description, PrCocoaMenuTree* tree, PrCocoaHost* host,
    PrCocoaWindow* PR_NULLABLE owner);
void keyLabelsReset(CocoaApp* app);

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
    hotkeysReset(cocoa);
    setAwake(app, 0);
    keyLabelsReset(cocoa);
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
    [hostOf(app) stop];
}

void run(PrApp* app, PrAppHandler handler) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    cocoa->handler = handler;
    @autoreleasepool {
        PrCocoaHost* host = hostOf(app);
        NSApplication* application = NSApplication.sharedApplication;
        // A second instance hands its request to the first, and its run ends without a callback.
        if (![host startServices])
            return;
        if (!host.headless) {
            [application setActivationPolicy:activationPolicyOf(host.presence)];
            if (host.menuBar == nil) {
                host.menuBar = [[PrCocoaMenuTree alloc] init];
                menuBarOf(nullptr, host.menuBar, host, nil);
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
        hotkeysReset(cocoa);
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
        event.time = uptime();
        [(__bridge PrCocoaHost*)cocoa->host deliver:event to:nullptr];
    });
}

PrStatus notificationPost(PrApp* app, const PrAppNotificationDesc* desc) {
    PrCocoaHost* host = hostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    // A headless application posts nothing, because the permission request would go to a user who never sees it.
    if (host.headless || !isBundled())
        return PrStatus_NotSupported;
    return [host postNotification:desc];
}

PrStatus openUrl(PrApp*, PrStringView url) {
    NSString* string = [[NSString alloc] initWithBytes:url.data length:url.size encoding:NSUTF8StringEncoding];
    NSURL* address = string != nil ? [NSURL URLWithString:string] : nil;
    if (address == nil)
        return PrStatus_Syntax;
    return [NSWorkspace.sharedWorkspace openURL:address] ? PrStatus_Ok : PrStatus_Io;
}

// Hands `answer` to the host from the main queue, which delivers it once no callback runs.
void answerSoon(PrApp* app, CocoaAnswer* answer) {
    __weak PrCocoaHost* weak = hostOf(app);
    dispatch_async(dispatch_get_main_queue(), ^{
        PrCocoaHost* host = weak;
        if (host == nil || host.app == nullptr) {
            answerDestroy(answer);
            return;
        }
        [host answer:answer];
    });
}

// Answers the request `id` of content from the main queue: from the general pasteboard, or from a source's store. A
// source's store does not change, and the request's reference keeps it, so its answer copies only a decoded image.
void readContent(PrApp* app, uint32_t id, PrAppDataSource source, AppDataAnswer kind, PrStringView type) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrRawString custom = prRawStringCopy(type);
    __weak PrCocoaHost* weak = hostOf(app);
    dispatch_async(dispatch_get_main_queue(), ^{
        PrRawString owned = custom;
        PrCocoaHost* host = weak;
        if (host == nil || host.app == nullptr) {
            prRawStringReset(&owned);
            return;
        }
        CocoaAnswer* answer = answerOf(CocoaAnswerKind_Store, id, PrStatus_Ok);
        AppDataStore* store = appDataSourceStore(cocoa->sources, source);
        if (source == kPrAppDataSourceClipboard)
            readPasteboard(NSPasteboard.generalPasteboard, kind, prRawStringView(&owned), answer->store);
        else if (source == kPrAppDataSourceSelection || source == kPrAppDataSourceNone)
            ;
        else if (store == nullptr)
            answer->status = PrStatus_NotFound;
        else if (kind == PrAppDataReadKind_Image)
            readDropImage(*store, answer->store);
        else
            answer->external = store;
        prRawStringReset(&owned);
        [host answer:answer];
    });
}

void quitRespond(PrApp* app, bool quit) {
    [hostOf(app) quitRespond:quit];
}

void dataRetain(PrApp* app, PrAppDataSource source) {
    appDataSourceRetain(static_cast<CocoaApp*>(app)->sources, source);
}

void dataRelease(PrApp* app, PrAppDataSource source) {
    appDataSourceRelease(static_cast<CocoaApp*>(app)->sources, source);
}

// Returns the path of the file at `index` of a source's files, or nil if it has none.
NSString* PR_NULLABLE filePathOf(CocoaApp* app, PrAppDataSource source, uint32_t index) {
    AppDataStore* store = appDataSourceStore(app->sources, source);
    uint32_t count = 0;
    const PrAppDataFile* files = store != nullptr ? appDataStoreFiles(*store, &count) : nullptr;
    if (index >= count || files[index].path.size == 0 || files[index].directory)
        return nil;
    return stringOf(files[index].path);
}

// Reads a file's bytes on a queue of its own, and answers from the main queue.
void readFile(PrApp* app, uint32_t id, PrAppDataSource source, uint32_t index, uint64_t offset, uint64_t size) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    NSString* path = filePathOf(cocoa, source, index);
    CocoaAnswer* answer = answerOf(CocoaAnswerKind_Bytes, id, path != nil ? PrStatus_Ok : PrStatus_NotFound);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSFileHandle* file = path != nil ? [NSFileHandle fileHandleForReadingAtPath:path] : nil;
        if (path != nil && file == nil)
            answer->status = PrStatus_Io;
        NSData* bytes = nil;
        if (file != nil && [file seekToOffset:offset error:nil])
            bytes = [file readDataUpToLength:NSUInteger(size) error:nil];
        if (bytes != nil)
            answer->bytes.append(static_cast<const uint8_t*>(bytes.bytes), uint32_t(bytes.length));
        [file closeAndReturnError:nil];
        answerSoon(app, answer);
    });
}

void dataRead(PrApp* app, const PrAppDataRead* read) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    AppDataAnswer kind = read->kind;
    uint32_t id = appDataRequestsAdd(cocoa->requests, read->source, kind, read->done, read->context, read->type);
    if (read->kind == PrAppDataReadKind_File)
        readFile(app, id, read->source, read->index, read->offset, read->size);
    else
        readContent(app, id, read->source, kind, read->type);
}

PrStatus dataWrite(PrApp* app, PrAppDataSource source, const PrAppDataContent* content) {
    (void)app;
    if (source == kPrAppDataSourceSelection)
        return PrStatus_NotSupported;
    NSPasteboardItem* first = [[NSPasteboardItem alloc] init];
    NSMutableArray<id<NSPasteboardWriting>>* writers = [NSMutableArray arrayWithObject:first];
    writeContent(*content, 0, first, writers);
    NSPasteboard* board = NSPasteboard.generalPasteboard;
    [board clearContents];
    return [board writeObjects:writers] ? PrStatus_Ok : PrStatus_Io;
}

// Writes a file's bytes on a queue of its own, and answers from the main queue. A write at offset zero empties the file
// first.
void dataWriteFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
        PrAppDataStatusFn done, void* PR_NULLABLE context) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    uint32_t id = appDataRequestsAdd(cocoa->requests, source, AppDataAnswer_Status, (void (*)(void))done, context);
    NSString* path = filePathOf(cocoa, source, index);
    NSData* data = [NSData dataWithBytes:bytes.data length:bytes.size];
    CocoaAnswer* answer = answerOf(CocoaAnswerKind_Status, id, path != nil ? PrStatus_Ok : PrStatus_NotFound);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (path != nil && offset == 0)
            [NSFileManager.defaultManager createFileAtPath:path contents:nil attributes:nil];
        NSFileHandle* file = path != nil ? [NSFileHandle fileHandleForWritingAtPath:path] : nil;
        bool written = file != nil && [file seekToOffset:offset error:nil] && [file writeData:data error:nil];
        if (path != nil && !written)
            answer->status = PrStatus_Io;
        [file closeAndReturnError:nil];
        answerSoon(app, answer);
    });
}

void closeCancelledDialogs(PrCocoaHost* host);

void dataCancel(PrApp* app, void* PR_NULLABLE context) {
    appDataRequestsCancel(static_cast<CocoaApp*>(app)->requests, context);
    // A dialog closes on the main thread, which alone holds the dialogs.
    PrCocoaHost* host = hostOf(app);
    if (NSThread.isMainThread) {
        closeCancelledDialogs(host);
        return;
    }
    __weak PrCocoaHost* weak = host;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (PrCocoaHost* main = weak)
            closeCancelledDialogs(main);
    });
}

// Returns the position of `at`, in AppKit's global coordinates, on the screen that holds it, or on the nearest screen
// if none does. Returns false if there are no screens.
bool displayPositionAt(NSPoint at, PrAppDisplayPosition* out) {
    NSScreen* nearest = nil;
    CGFloat nearestDistance = 0;
    for (NSScreen* screen in NSScreen.screens) {
        NSRect frame = screen.frame;
        CGFloat dx = MAX(MAX(NSMinX(frame) - at.x, at.x - NSMaxX(frame)), 0);
        CGFloat dy = MAX(MAX(NSMinY(frame) - at.y, at.y - NSMaxY(frame)), 0);
        CGFloat distance = dx * dx + dy * dy;
        if (nearest == nil || distance < nearestDistance) {
            nearest = screen;
            nearestDistance = distance;
        }
    }
    if (nearest == nil)
        return false;
    NSRect frame = nearest.frame;
    *out = PrAppDisplayPosition{
        .display = displayIdOf(nearest),
        .position = PrVec2{float(at.x - NSMinX(frame)), float(NSMaxY(frame) - at.y)},
    };
    return true;
}

bool pointerPosition(const PrApp* app, PrAppDisplayPosition* out) {
    return !hostOf(app).headless && displayPositionAt(NSEvent.mouseLocation, out);
}

void setHidden(PrApp* app, bool hidden) {
    if (hostOf(app).headless)
        return;
    if (hidden)
        [NSApp hide:nil];
    else
        [NSApp unhide:nil];
}

bool isHidden(const PrApp*) {
    return NSApp.isHidden;
}

PrStatus revealPath(PrApp* app, PrStringView path) {
    NSString* string = stringOf(path);
    if (![NSFileManager.defaultManager fileExistsAtPath:string])
        return PrStatus_NotFound;
    if (hostOf(app).headless)
        return PrStatus_NotSupported;
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:string] ]];
    return PrStatus_Ok;
}

PrStatus trashPath(PrApp*, PrStringView path) {
    NSString* string = stringOf(path);
    if (![NSFileManager.defaultManager fileExistsAtPath:string])
        return PrStatus_NotFound;
    NSURL* url = [NSURL fileURLWithPath:string];
    return [NSFileManager.defaultManager trashItemAtURL:url resultingItemURL:nil error:nil] ? PrStatus_Ok : PrStatus_Io;
}

void recentDocumentAdd(PrApp*, PrStringView path) {
    [NSDocumentController.sharedDocumentController noteNewRecentDocumentURL:[NSURL fileURLWithPath:stringOf(path)]];
}

void recentDocumentsClear(PrApp*) {
    [NSDocumentController.sharedDocumentController clearRecentDocuments:nil];
}

void setBadge(PrApp* app, PrStringView badge) {
    if (hostOf(app).headless)
        return;
    NSApp.dockTile.badgeLabel = badge.size != 0 ? stringOf(badge) : nil;
}

void setPresence(PrApp* app, PrAppPresence presence) {
    [hostOf(app) setPresence:presence];
}

PrAppLifecycle lifecycle(const PrApp* app) {
    return [hostOf(app) lifecycle];
}

void timerSet(PrApp* app, uint64_t tag, double delay, double interval) {
    [hostOf(app) setTimer:tag delay:delay interval:interval];
}

void timerCancel(PrApp* app, uint64_t tag) {
    [hostOf(app) cancelTimer:tag];
}

// ---------------------------------------------------------------------------------------------------------------------
// The device

PrAppPower power(const PrApp*) {
    PrAppPower result{.source = PrAppPowerSource_Unknown, .level = -1};
    result.lowPower = NSProcessInfo.processInfo.isLowPowerModeEnabled;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == nullptr)
        return result;
    CFStringRef providing = IOPSGetProvidingPowerSourceType(info);
    if (providing != nullptr && CFEqual(providing, CFSTR(kIOPMACPowerKey)))
        result.source = PrAppPowerSource_External;
    else if (providing != nullptr && CFEqual(providing, CFSTR(kIOPMBatteryPowerKey)))
        result.source = PrAppPowerSource_Battery;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; list != nullptr && i < CFArrayGetCount(list); ++i) {
        NSDictionary* description =
            (__bridge NSDictionary*)IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        if (![description[@kIOPSTypeKey] isEqual:@kIOPSInternalBatteryType])
            continue;
        double current = [description[@kIOPSCurrentCapacityKey] doubleValue];
        double maximum = [description[@kIOPSMaxCapacityKey] doubleValue];
        if (maximum > 0)
            result.level = float(current / maximum);
        result.charging = [description[@kIOPSIsChargingKey] boolValue];
        break;
    }
    if (list != nullptr)
        CFRelease(list);
    CFRelease(info);
    return result;
}

// Holds or releases the power assertion `type` in `held`.
void holdAssertion(IOPMAssertionID& held, CFStringRef type, bool hold) {
    if (hold && held == kIOPMNullAssertionID)
        IOPMAssertionCreateWithName(type, kIOPMAssertionLevelOn, CFSTR("Prax application"), &held);
    if (!hold && held != kIOPMNullAssertionID) {
        IOPMAssertionRelease(held);
        held = kIOPMNullAssertionID;
    }
}

void setAwake(PrApp* app, PrAppAwakeFlags flags) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    holdAssertion(cocoa->displayAwake, kIOPMAssertionTypePreventUserIdleDisplaySleep, (flags & PrAppAwakeFlag_Display) != 0);
    holdAssertion(cocoa->systemAwake, kIOPMAssertionTypePreventUserIdleSystemSleep, (flags & PrAppAwakeFlag_System) != 0);
}

// A Mac plays haptics on its Force Touch trackpad, which has three patterns. The impacts map to them by strength, and
// the outcomes to the generic pattern.
void hapticPlay(PrApp*, PrAppHaptic haptic) {
    NSHapticFeedbackPattern pattern = NSHapticFeedbackPatternGeneric;
    if (haptic == PrAppHaptic_Selection || haptic == PrAppHaptic_Light)
        pattern = NSHapticFeedbackPatternAlignment;
    else if (haptic == PrAppHaptic_Heavy)
        pattern = NSHapticFeedbackPatternLevelChange;
    [NSHapticFeedbackManager.defaultPerformer performFeedbackPattern:pattern
        performanceTime:NSHapticFeedbackPerformanceTimeNow];
}

// ---------------------------------------------------------------------------------------------------------------------
// Global hotkeys, which Carbon's hot keys implement: AppKit has none, and Carbon's need no permission of the user.

// The signature of the application's hot keys, which tells them from those of other code in the process.
constexpr OSType kHotkeySignature = 'PrAx';

OSStatus hotkeyPressed(EventHandlerCallRef, EventRef event, void* context) {
    EventHotKeyID id{};
    OSStatus status = GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(id), nullptr, &id);
    if (status != noErr || id.signature != kHotkeySignature)
        return eventNotHandledErr;
    [(__bridge PrCocoaHost*)context deliverHotkey:id.id];
    return noErr;
}

UInt32 carbonModifiersOf(PrAppModifierFlags modifiers) {
    UInt32 carbon = 0;
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        carbon |= shiftKey;
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        carbon |= controlKey;
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        carbon |= optionKey;
    if ((modifiers & PrAppModifierFlag_Super) != 0)
        carbon |= cmdKey;
    return carbon;
}

// Writes the virtual key code of the key at `code` to `out`, and returns false for a key that a Mac's keyboard lacks.
bool virtualKeyOf(PrAppKeyCode code, uint16_t* out) {
    for (const MacKey& key : kMacKeys) {
        if (key.code == code) {
            *out = key.virtualKey;
            return true;
        }
    }
    return false;
}

void hotkeyUnregister(PrApp* app, uint64_t tag) {
    RawArray<CocoaHotkey>& hotkeys = static_cast<CocoaApp*>(app)->hotkeys;
    for (uint32_t i = 0; i < hotkeys.count; ++i) {
        if (hotkeys.data[i].tag == tag) {
            UnregisterEventHotKey(hotkeys.data[i].ref);
            hotkeys.erase(i);
            return;
        }
    }
}

PrStatus hotkeyRegister(PrApp* app, PrAppHotkey hotkey, uint64_t tag) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrCocoaHost* host = hostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    uint16_t key = 0;
    if (host.headless || !virtualKeyOf(hotkey.code, &key))
        return PrStatus_NotSupported;
    if (cocoa->hotkeyHandler == nullptr) {
        EventTypeSpec pressed{kEventClassKeyboard, kEventHotKeyPressed};
        EventHandlerRef handler = nullptr;
        if (InstallEventHandler(GetApplicationEventTarget(), hotkeyPressed, 1, &pressed, (__bridge void*)host, &handler) != noErr)
            return PrStatus_NotSupported;
        cocoa->hotkeyHandler = handler;
    }
    hotkeyUnregister(app, tag);
    uint32_t id = ++cocoa->lastHotkeyId;
    EventHotKeyRef ref = nullptr;
    if (RegisterEventHotKey(key, carbonModifiersOf(hotkey.modifiers), EventHotKeyID{kHotkeySignature, id},
            GetApplicationEventTarget(), 0, &ref) != noErr)
        return PrStatus_NotAllowed;
    cocoa->hotkeys.push(CocoaHotkey{.tag = tag, .id = id, .ref = ref});
    return PrStatus_Ok;
}

// Unregisters every hotkey, and removes their handler, as the run ends.
void hotkeysReset(CocoaApp* app) {
    for (uint32_t i = 0; i < app->hotkeys.count; ++i)
        UnregisterEventHotKey(app->hotkeys.data[i].ref);
    app->hotkeys.reset();
    if (app->hotkeyHandler != nullptr)
        RemoveEventHandler(app->hotkeyHandler);
    app->hotkeyHandler = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Key labels, which the current keyboard layout's Unicode table gives

// Returns the character that the key `virtualKey` types without modifiers in the current layout, uppercased, or nil if
// it types none that a label can show.
NSString* PR_NULLABLE layoutCharacterOf(uint16_t virtualKey) {
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (source == nullptr)
        return nil;
    CFDataRef data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    NSString* result = nil;
    if (data != nullptr) {
        const UCKeyboardLayout* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        UInt32 dead = 0;
        UniChar characters[4];
        UniCharCount length = 0;
        OSStatus status = UCKeyTranslate(layout, virtualKey, kUCKeyActionDisplay, 0, LMGetKbdType(),
            kUCKeyTranslateNoDeadKeysBit, &dead, countOf(characters), &length, characters);
        // A control character or a space is a key that a label names rather than shows.
        if (status == noErr && length != 0 && characters[0] > ' ' && characters[0] != 0x7f)
            result = [NSString stringWithCharacters:characters length:length].uppercaseString;
    }
    CFRelease(source);
    return result;
}

PrStringView keyCodeLabel(const PrApp* app, PrAppKeyCode code) {
    const CocoaApp* cocoa = static_cast<const CocoaApp*>(app);
    if (code >= PrAppKeyCode_Count)
        return prAppKeyCodeName(PrAppKeyCode_Unidentified);
    if (!cocoa->keyLabeled[code]) {
        cocoa->keyLabeled[code] = true;
        uint16_t virtualKey = 0;
        NSString* character = virtualKeyOf(code, &virtualKey) ? layoutCharacterOf(virtualKey) : nil;
        prRawStringReset(&cocoa->keyLabels[code]);
        if (character != nil)
            cocoa->keyLabels[code] = prRawStringCopy(character.UTF8String);
    }
    if (!prRawStringIsEmpty(&cocoa->keyLabels[code]))
        return prRawStringView(&cocoa->keyLabels[code]);
    return prAppKeyCodeName(code);
}

// Forgets the labels, for a change of the keyboard layout or the end of the application.
void keyLabelsReset(CocoaApp* app) {
    for (uint32_t i = 0; i < PrAppKeyCode_Count; ++i) {
        prRawStringReset(&app->keyLabels[i]);
        app->keyLabeled[i] = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// File dialogs, which AppKit's panels show. A dialog's answer is a read of files that no source names.

// Returns the content types of the dialog's file types, of each extension that the system knows.
NSArray<UTType*>* contentTypesOf(const PrAppFileDialogDesc* desc) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    NSCharacterSet* space = NSCharacterSet.whitespaceCharacterSet;
    for (uint32_t i = 0; i < desc->typeCount; ++i) {
        for (NSString* extension in [stringOf(desc->types[i].extensions) componentsSeparatedByString:@","]) {
            NSString* trimmed = [extension stringByTrimmingCharactersInSet:space];
            UTType* type = trimmed.length != 0 ? [UTType typeWithFilenameExtension:trimmed] : nil;
            if (type != nil)
                [types addObject:type];
        }
    }
    return types;
}

void fileDialog(PrApp* app, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrCocoaHost* host = hostOf(app);
    uint32_t id = appDataRequestsAdd(cocoa->requests, kPrAppDataSourceNone, AppDataAnswer_Source, (void (*)(void))done, context);
    if (host.headless || !host.running) {
        answerSoon(app, answerOf(CocoaAnswerKind_Source, id, PrStatus_NotSupported));
        return;
    }
    bool directories = desc->kind == PrAppFileDialogKind_Open && (desc->flags & PrAppFileDialogFlag_Directories) != 0;
    NSSavePanel* panel = nil;
    if (desc->kind == PrAppFileDialogKind_Open) {
        NSOpenPanel* open = [NSOpenPanel openPanel];
        open.allowsMultipleSelection = (desc->flags & PrAppFileDialogFlag_Multiple) != 0;
        open.canChooseDirectories = directories;
        open.canChooseFiles = !directories;
        panel = open;
    } else {
        panel = [NSSavePanel savePanel];
        panel.canCreateDirectories = YES;
        if (desc->name.size != 0)
            panel.nameFieldStringValue = stringOf(desc->name);
    }
    // A sheet shows its message, and a panel of its own shows its title.
    if (desc->title.size != 0) {
        panel.title = stringOf(desc->title);
        panel.message = stringOf(desc->title);
    }
    if (desc->directory.size != 0)
        panel.directoryURL = [NSURL fileURLWithPath:stringOf(desc->directory) isDirectory:YES];
    NSArray<UTType*>* types = contentTypesOf(desc);
    if (types.count != 0 && !directories)
        panel.allowedContentTypes = types;
    if (host.dialogs == nil)
        host.dialogs = [NSMutableDictionary dictionary];
    host.dialogs[@(id)] = panel;
    __weak PrCocoaHost* weak = host;
    __weak NSSavePanel* weakPanel = panel;
    void (^finished)(NSModalResponse) = ^(NSModalResponse response) {
        PrCocoaHost* answering = weak;
        NSSavePanel* chosen = weakPanel;
        if (answering == nil || answering.app == nullptr)
            return;
        [answering.dialogs removeObjectForKey:@(id)];
        // The user's choice is a source of its files, which the answer holds a reference to. A cancel answers None.
        CocoaAnswer* answer = answerOf(CocoaAnswerKind_Source, id, PrStatus_Ok);
        if (response == NSModalResponseOK && chosen != nil) {
            NSArray<NSURL*>* urls = @[ chosen.URL ];
            if ([chosen isKindOfClass:NSOpenPanel.class])
                urls = ((NSOpenPanel*)chosen).URLs;
            AppDataStore* store = allocZeroed<AppDataStore>();
            for (NSURL* url in urls)
                addFile(url, *store);
            answer->source = appDataSourceCreate(static_cast<CocoaApp*>(answering.app)->sources, store, nullptr);
        }
        [answering answer:answer];
    };
    NSWindow* parent = desc->parent != nullptr ? objectOf(desc->parent).window : nil;
    if (parent != nil && parent.isVisible && parent.attachedSheet == nil) {
        [panel beginSheetModalForWindow:parent completionHandler:finished];
    } else {
        [NSApp activate];
        [panel beginWithCompletionHandler:finished];
    }
}

// Closes the dialogs whose reads were cancelled, whose answers then call nothing.
void closeCancelledDialogs(PrCocoaHost* host) {
    if (host.app == nullptr)
        return;
    AppDataRequests& requests = static_cast<CocoaApp*>(host.app)->requests;
    for (NSNumber* id in host.dialogs.allKeys) {
        PrAppDataSource source = kPrAppDataSourceNone;
        AppDataAnswer answer = AppDataAnswer_Source;
        if (appDataRequestsFind(requests, id.unsignedIntValue, &source, &answer))
            continue;
        NSSavePanel* panel = host.dialogs[id];
        [host.dialogs removeObjectForKey:id];
        if (panel.sheetParent != nil)
            [panel.sheetParent endSheet:panel returnCode:NSModalResponseCancel];
        else
            [panel cancel:nil];
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The windows' functions

void forgetTextInput(PrAppWindow* window);

void windowDestroy(PrAppWindow* window) {
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(window);
    if (appWindowDeliveryDeferDestroy(cocoa->delivery))
        return;
    PrCocoaWindow* object = (__bridge_transfer PrCocoaWindow*)cocoa->object;
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
    CocoaDrag& drag = static_cast<CocoaApp*>(cocoa->app)->drag;
    if (drag.window == window)
        drag.window = nullptr;
    appTitleBarRegionsReset(cocoa->titleBar);
    appDragPendingReset(cocoa->drag);
    prRawStringReset(&cocoa->title);
    prMemoryFree(cocoa);
}

PrApp* windowApp(const PrAppWindow* window) {
    return static_cast<const CocoaWindow*>(window)->app;
}

PrAppFrame frameState(PrAppWindow* window) {
    return [objectOf(window) frameAt:uptime()];
}

void requestFrame(PrAppWindow* window) {
    [objectOf(window) requestFrame];
}

void requestFrameAt(PrAppWindow* window, double time) {
    [objectOf(window) requestFrameAt:time];
}

void setCursor(PrAppWindow* window, PrAppCursor cursor) {
    // A cursor is made once per shape, so that setting the same shape again finds the view's cursor unchanged. Only the
    // main thread reads the table.
    static NSCursor* made[PrAppCursor_Count];
    PrAppCursor shape = cursor < PrAppCursor_Count ? cursor : PrAppCursor_Arrow;
    if (made[shape] == nil)
        made[shape] = cursorFor(shape);
    PrCocoaWindow* object = objectOf(window);
    NSCursor* selected = made[shape];
    if (object.view.cursor == selected)
        return;
    object.view.cursor = selected;
    [object.window invalidateCursorRectsForView:object.view];
}

void setTextInputRect(PrAppWindow* window, PrRect rect) {
    objectOf(window).view.textInputRect = NSMakeRect(CGFloat(rect.left), CGFloat(rect.top),
        CGFloat(rect.right - rect.left), CGFloat(rect.bottom - rect.top));
}

// Returns the window whose latest output holds the text being edited in `window`: the window itself if its output has
// some there, or else a window that owns it whose output names it. Returns null if no output does.
const CocoaWindow* PR_NULLABLE textInputSource(PrAppWindow* window) {
    const CocoaWindow* own = static_cast<const CocoaWindow*>(window);
    if (own->textInputWindow == window)
        return own;
    for (PrCocoaWindow* other in objectOf(window).host.windows) {
        const CocoaWindow* cocoa = static_cast<const CocoaWindow*>(other.record);
        if (cocoa != nullptr && cocoa->textInputWindow == window)
            return cocoa;
    }
    return nullptr;
}

// Applies the text being edited in the window: where the input method opens its windows, and whether the window holds
// secure input for a password.
void applyTextInput(PrAppWindow* window) {
    const CocoaWindow* source = textInputSource(window);
    if (source != nullptr)
        setTextInputRect(window, source->textInput);
    PrCocoaWindow* object = objectOf(window);
    bool password = source != nullptr && source->textInputKind == PrAppTextInputKind_Password;
    [object setSecureInput:password && object.window.isKeyWindow];
}

// Withdraws the text input that `window`, which is being destroyed, names in another window, and the text input that
// other windows name in it.
void forgetTextInput(PrAppWindow* window) {
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(window);
    PrAppWindow* named = cocoa->textInputWindow;
    cocoa->textInputWindow = nullptr;
    if (named != nullptr && named != window)
        applyTextInput(named);
    for (PrCocoaWindow* other in objectOf(window).host.windows) {
        CocoaWindow* owner = static_cast<CocoaWindow*>(other.record);
        if (owner != nullptr && owner->textInputWindow == window)
            owner->textInputWindow = nullptr;
    }
}

void setTitle(PrAppWindow* window, PrStringView title) {
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(window);
    prRawStringReset(&cocoa->title);
    cocoa->title = prRawStringCopy(title);
    objectOf(window).window.title = stringOf(title);
    requestFrame(window);
}

PrStringView title(const PrAppWindow* window) {
    return prRawStringView(&static_cast<const CocoaWindow*>(window)->title);
}

// Returns `size` within the content of the screen that shows `window`, or the main screen's.
NSSize clampedToScreen(NSWindow* window, NSSize size, NSWindowStyleMask style) {
    NSScreen* screen = window.screen != nil ? window.screen : NSScreen.mainScreen;
    NSRect available = [NSWindow contentRectForFrameRect:screen.visibleFrame styleMask:style];
    return NSMakeSize(MIN(size.width, available.size.width), MIN(size.height, available.size.height));
}

void setExtent(PrAppWindow* window, PrUVec2 extent) {
    PrCocoaWindow* object = objectOf(window);
    // AppKit holds the user's resizes to the limits, but not the program's.
    NSSize min = object.window.contentMinSize;
    NSSize max = object.window.contentMaxSize;
    NSSize size = NSMakeSize(MIN(MAX(CGFloat(extent.x), min.width), max.width),
        MIN(MAX(CGFloat(extent.y), min.height), max.height));
    if (!object.host.headless)
        size = clampedToScreen(object.window, size, object.window.styleMask);
    [object.window setContentSize:size];
}

// The extent limits as AppKit holds them: a greatest extent of zero is unlimited.
NSSize maxSizeOf(PrUVec2 maxExtent) {
    return NSMakeSize(maxExtent.x != 0 ? CGFloat(maxExtent.x) : CGFLOAT_MAX,
        maxExtent.y != 0 ? CGFloat(maxExtent.y) : CGFLOAT_MAX);
}

void setExtentLimits(PrAppWindow* window, PrUVec2 minExtent, PrUVec2 maxExtent) {
    NSWindow* frame = objectOf(window).window;
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
    PrCocoaWindow* object = objectOf(window);
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
    PrCocoaWindow* object = objectOf(window);
    NSScreen* screen = object.window.screen;
    if (object.host.headless || screen == nil)
        return false;
    PrRect content = rectOnScreen(screen, [object.window contentRectForFrameRect:object.window.frame]);
    *out = PrAppDisplayPosition{.display = displayIdOf(screen), .position = PrVec2{content.left, content.top}};
    return true;
}

bool pointToDisplay(const PrAppWindow* window, PrVec2 point, PrAppDisplayPosition* out) {
    PrCocoaWindow* object = objectOf(window);
    if (object.host.headless)
        return false;
    NSRect content = [object.window contentRectForFrameRect:object.window.frame];
    return displayPositionAt(NSMakePoint(NSMinX(content) + CGFloat(point.x), NSMaxY(content) - CGFloat(point.y)), out);
}

bool pointFromDisplay(const PrAppWindow* window, const PrAppDisplayPosition* position, PrVec2* out) {
    PrCocoaWindow* object = objectOf(window);
    NSScreen* screen = screenOf(position->display);
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
    PrCocoaWindow* object = objectOf(window);
    NSWindow* frame = object.window;
    if (object.host.headless || (frame.styleMask & NSWindowStyleMaskFullScreen) != 0)
        return;
    if (frame.zoomed)
        setState(window, PrAppWindowState_Normal);
    NSScreen* screen = screenOf(position->display);
    if (screen != nil) {
        placeOnScreen(frame, screen, position->position);
        return;
    }
    // A display that is gone gives the main display, at the same position if the window fits there.
    screen = NSScreen.screens.firstObject;
    NSRect content = [frame contentRectForFrameRect:frame.frame];
    PrRect work = rectOnScreen(screen, screen.visibleFrame);
    bool fits = position->position.x >= work.left && position->position.y >= work.top &&
        position->position.x + float(content.size.width) <= work.right &&
        position->position.y + float(content.size.height) <= work.bottom;
    if (fits)
        placeOnScreen(frame, screen, position->position);
    else
        centerOnScreen(frame, screen);
}

void setState(PrAppWindow* window, PrAppWindowState wanted) {
    PrCocoaWindow* object = objectOf(window);
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
    PrCocoaWindow* object = objectOf(window);
    return object.window.isKeyWindow && (NSApp.isActive || object.overlay);
}

void focus(PrAppWindow* window) {
    PrCocoaWindow* object = objectOf(window);
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
    PrCocoaWindow* object = objectOf(window);
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
    return objectOf(window).shown;
}

void setBackdrop(PrAppWindow* window, PrAppWindowBackdrop backdrop) {
    [objectOf(window) setBackdropOf:backdrop];
}

void setPointerPassthrough(PrAppWindow* window, bool passthrough) {
    objectOf(window).window.ignoresMouseEvents = passthrough;
}

// The window locks the pointer for Locked and LockedRaw alike, as AppKit reports only the cursor's movement.
void setPointerMode(PrAppWindow* window, PrAppPointerMode mode) {
    PrCocoaWindow* object = objectOf(window);
    object.lockedMode = mode;
    [object setConfinement:mode == PrAppPointerMode_Confined];
    [object setLock:mode == PrAppPointerMode_Locked || mode == PrAppPointerMode_LockedRaw];
}

PrAppPointerMode pointerMode(const PrAppWindow* window) {
    PrCocoaWindow* object = objectOf(window);
    return object.locked || object.confined ? object.lockedMode : PrAppPointerMode(PrAppPointerMode_Free);
}

void setAspectRatio(PrAppWindow* window, float ratio) {
    NSWindow* frame = objectOf(window).window;
    // The aspect ratio and the resize increments exclude each other, so increments of one point clear the ratio.
    if (ratio > 0)
        frame.contentAspectRatio = NSMakeSize(CGFloat(ratio), 1);
    else
        frame.contentResizeIncrements = NSMakeSize(1, 1);
}

void setPopupPlacement(PrAppWindow* window, const PrAppPopupPlacement* placement) {
    PrCocoaWindow* object = objectOf(window);
    object.popup = *placement;
    [object place];
}

// A headless window is drawn off the screen, where nothing covers it.
bool isOccluded(const PrAppWindow* window) {
    PrCocoaWindow* object = objectOf(window);
    if (object.host.headless || !object.window.isVisible)
        return false;
    return (object.window.occlusionState & NSWindowOcclusionStateVisible) == 0;
}

void setOpacity(PrAppWindow* window, float opacity) {
    objectOf(window).window.alphaValue = CGFloat(MIN(MAX(opacity, 0.0f), 1.0f));
}

void setKeepAbove(PrAppWindow* window, bool above) {
    PrCocoaWindow* object = objectOf(window);
    if (object.role == PrAppWindowRole_Normal)
        object.window.level = above ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

// The Dock shows one progress for the application, so the window that set it last decides it.
void setProgress(PrAppWindow* window, PrAppProgressState state, float progress) {
    PrCocoaWindow* object = objectOf(window);
    if (!object.host.headless)
        [object.host setDockProgress:state value:progress];
}

void dragPromiseEnd(PrAppWindow* window, uint32_t index, PrStatus status) {
    [objectOf(window) endPromise:index status:status];
}

void setDocument(PrAppWindow* window, PrStringView path, bool edited) {
    NSWindow* frame = objectOf(window).window;
    frame.representedURL = path.size != 0 ? [NSURL fileURLWithPath:stringOf(path)] : nil;
    frame.documentEdited = edited;
}

void requestAttention(PrAppWindow*) {
    // An informational request bounces the Dock icon once, and does nothing while the application is active.
    [NSApp requestUserAttention:NSInformationalRequest];
}

// Starts the drag from the main queue once no callback runs, as a modal run loop within a callback may run the queue.
void startDragSoon(PrCocoaWindow* object) {
    __weak PrCocoaWindow* weak = object;
    dispatch_async(dispatch_get_main_queue(), ^{
        PrCocoaWindow* window = weak;
        if (window == nil || window.record == nullptr)
            return;
        if (window.host.inCallback)
            startDragSoon(window);
        else
            [window startDrag];
    });
}

void dragStart(PrAppWindow* window, const PrAppDragDesc* desc) {
    appDragPendingSet(static_cast<CocoaWindow*>(window)->drag, *desc);
    startDragSoon(objectOf(window));
}

// ---------------------------------------------------------------------------------------------------------------------
// Menus

// Returns AppKit's key equivalent for `key`, or the empty string for none.
NSString* keyEquivalentOf(PrAppKey key) {
    if (key > ' ' && key < 127) {
        unichar character = unichar(key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key);
        return [NSString stringWithCharacters:&character length:1];
    }
    unichar character = 0;
    switch (key) {
        case PrAppKey_Enter:
            character = '\r';
            break;
        case PrAppKey_Escape:
            character = 0x1b;
            break;
        case PrAppKey_Backspace:
            character = NSBackspaceCharacter;
            break;
        case PrAppKey_Tab:
            character = '\t';
            break;
        case PrAppKey_Delete:
            character = NSDeleteFunctionKey;
            break;
        case PrAppKey_Left:
            character = NSLeftArrowFunctionKey;
            break;
        case PrAppKey_Right:
            character = NSRightArrowFunctionKey;
            break;
        case PrAppKey_Up:
            character = NSUpArrowFunctionKey;
            break;
        case PrAppKey_Down:
            character = NSDownArrowFunctionKey;
            break;
        case PrAppKey_Home:
            character = NSHomeFunctionKey;
            break;
        case PrAppKey_End:
            character = NSEndFunctionKey;
            break;
        case PrAppKey_PageUp:
            character = NSPageUpFunctionKey;
            break;
        case PrAppKey_PageDown:
            character = NSPageDownFunctionKey;
            break;
        default:
            if (key >= PrAppKey_F1 && key <= PrAppKey_F20)
                character = unichar(NSF1FunctionKey + (key - PrAppKey_F1));
            break;
    }
    return character != 0 ? [NSString stringWithCharacters:&character length:1] : @"";
}

NSEventModifierFlags keyModifiersOf(PrAppModifierFlags modifiers) {
    NSEventModifierFlags flags = 0;
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        flags |= NSEventModifierFlagShift;
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        flags |= NSEventModifierFlagControl;
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        flags |= NSEventModifierFlagOption;
    if ((modifiers & PrAppModifierFlag_Super) != 0)
        flags |= NSEventModifierFlagCommand;
    return flags;
}

// What AppKit does for an item of a role: the action that the responder chain takes, and the title and the key
// equivalent of an item that the program leaves without them. A title of nil is the application's name after `title`.
struct CocoaMenuRole {
    PrAppMenuRole role;
    SEL action;
    NSString* title;
    PrAppKey key;
    PrAppModifierFlags modifiers;
};

const CocoaMenuRole* PR_NULLABLE menuRoleOf(PrAppMenuRole role) {
    static const CocoaMenuRole kRoles[] = {
        {PrAppMenuRole_About, @selector(orderFrontStandardAboutPanel:), @"About ", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Services, nullptr, @"Services", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Hide, @selector(hide:), @"Hide ", PrAppKey('H'), PrAppModifierFlag_Super},
        {
            PrAppMenuRole_HideOthers, @selector(hideOtherApplications:), @"Hide Others", PrAppKey('H'),
            PrAppModifierFlag_Super | PrAppModifierFlag_Alt,
        },
        {PrAppMenuRole_ShowAll, @selector(unhideAllApplications:), @"Show All", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Quit, @selector(terminate:), @"Quit ", PrAppKey('Q'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Undo, @selector(undo:), @"Undo", PrAppKey('Z'), PrAppModifierFlag_Super},
        {
            PrAppMenuRole_Redo, @selector(redo:), @"Redo", PrAppKey('Z'),
            PrAppModifierFlag_Super | PrAppModifierFlag_Shift,
        },
        {PrAppMenuRole_Cut, @selector(cut:), @"Cut", PrAppKey('X'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Copy, @selector(copy:), @"Copy", PrAppKey('C'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Paste, @selector(paste:), @"Paste", PrAppKey('V'), PrAppModifierFlag_Super},
        {PrAppMenuRole_SelectAll, @selector(selectAll:), @"Select All", PrAppKey('A'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Minimize, @selector(performMiniaturize:), @"Minimize", PrAppKey('M'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Zoom, @selector(performZoom:), @"Zoom", PrAppKey_Unknown, 0},
        {
            PrAppMenuRole_Fullscreen, @selector(toggleFullScreen:), @"Enter Full Screen", PrAppKey('F'),
            PrAppModifierFlag_Super | PrAppModifierFlag_Control,
        },
        {PrAppMenuRole_BringAllToFront, @selector(arrangeInFront:), @"Bring All to Front", PrAppKey_Unknown, 0},
        {PrAppMenuRole_WindowList, nullptr, @"Window", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Help, nullptr, @"Help", PrAppKey_Unknown, 0},
    };
    for (const CocoaMenuRole& entry : kRoles) {
        if (entry.role == role)
            return &entry;
    }
    return nullptr;
}

// Returns the title of a role's item: the role's title, which ends in a space where the application's name follows.
NSString* roleTitleOf(const CocoaMenuRole& role) {
    if (![role.title hasSuffix:@" "])
        return role.title;
    return [role.title stringByAppendingString:NSProcessInfo.processInfo.processName];
}

NSMenu* menuOf(const PrAppMenu& description, PrCocoaMenuTree* tree, NSMutableArray<PrCocoaMenu*>* previous,
    PrCocoaHost* host, PrCocoaWindow* PR_NULLABLE owner);

// Returns the item that `description` describes in a menu of `tree`.
NSMenuItem* menuItemOf(const PrAppMenuItem& description, PrCocoaMenu* parent, PrCocoaMenuTree* tree,
        NSMutableArray<PrCocoaMenu*>* previous) {
    if (description.role == PrAppMenuRole_Separator)
        return NSMenuItem.separatorItem;
    const CocoaMenuRole* role = menuRoleOf(description.role);
    NSString* title = description.title.size != 0 ? stringOf(description.title) : @"";
    if (description.title.size == 0 && role != nullptr)
        title = roleTitleOf(*role);
    if (description.title.size == 0 && description.submenu != nullptr)
        title = stringOf(description.submenu->title);
    PrAppKey key = description.key;
    PrAppModifierFlags modifiers = description.modifiers;
    if (key == PrAppKey_Unknown && role != nullptr) {
        key = role->key;
        modifiers = role->modifiers;
    }
    // A role's action goes through the responder chain, to a text field, a window or the application.
    SEL action = role != nullptr ? role->action : @selector(chosen:);
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:keyEquivalentOf(key)];
    item.keyEquivalentModifierMask = keyModifiersOf(modifiers);
    item.target = role != nullptr ? nil : parent;
    item.tag = NSInteger(description.tag);
    item.enabled = (description.flags & PrAppMenuItemFlag_Disabled) == 0;
    item.state = (description.flags & PrAppMenuItemFlag_Checked) != 0 ? NSControlStateValueOn : NSControlStateValueOff;
    if (description.submenu != nullptr) {
        item.action = nil;
        item.submenu = menuOf(*description.submenu, tree, previous, parent.host, parent.owner);
        item.submenu.title = title;
        if (description.role == PrAppMenuRole_Services)
            NSApp.servicesMenu = item.submenu;
        else if (description.role == PrAppMenuRole_WindowList)
            NSApp.windowsMenu = item.submenu;
        else if (description.role == PrAppMenuRole_Help)
            NSApp.helpMenu = item.submenu;
    }
    return item;
}

// Returns the menu that `description` describes, made a menu of `tree`. A menu of `previous` with the same tag is
// filled again in place, so that a menu that is open shows the new items.
NSMenu* menuOf(const PrAppMenu& description, PrCocoaMenuTree* tree, NSMutableArray<PrCocoaMenu*>* previous,
        PrCocoaHost* host, PrCocoaWindow* PR_NULLABLE owner) {
    PrCocoaMenu* node = nil;
    for (PrCocoaMenu* candidate in previous) {
        if (candidate.tag == description.tag) {
            node = candidate;
            break;
        }
    }
    if (node != nil) {
        [previous removeObject:node];
        [node.menu removeAllItems];
    } else {
        node = [[PrCocoaMenu alloc] init];
        node.menu = [[NSMenu alloc] init];
        node.tag = description.tag;
    }
    node.host = host;
    node.owner = owner;
    node.ofWindow = owner != nil;
    node.menu.title = stringOf(description.title);
    node.menu.delegate = node;
    node.menu.autoenablesItems = NO;
    [tree.menus addObject:node];
    for (uint32_t i = 0; i < description.count; ++i)
        [node.menu addItem:menuItemOf(description.items[i], node, tree, previous)];
    return node.menu;
}

// Returns the application's own menu, which macOS shows first in the menu bar.
NSMenuItem* applicationMenuItem() {
    PrAppMenuItem items[] = {
        {.role = PrAppMenuRole_About},
        {.role = PrAppMenuRole_Separator},
        {.role = PrAppMenuRole_Hide},
        {.role = PrAppMenuRole_HideOthers},
        {.role = PrAppMenuRole_ShowAll},
        {.role = PrAppMenuRole_Separator},
        {.role = PrAppMenuRole_Quit},
    };
    NSMenu* menu = [[NSMenu alloc] init];
    for (const PrAppMenuItem& description : items) {
        if (description.role == PrAppMenuRole_Separator) {
            [menu addItem:NSMenuItem.separatorItem];
            continue;
        }
        const CocoaMenuRole* role = menuRoleOf(description.role);
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:roleTitleOf(*role) action:role->action
            keyEquivalent:keyEquivalentOf(role->key)];
        item.keyEquivalentModifierMask = keyModifiersOf(role->modifiers);
        [menu addItem:item];
    }
    NSMenuItem* services = [[NSMenuItem alloc] initWithTitle:@"Services" action:nil keyEquivalent:@""];
    services.submenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSApp.servicesMenu = services.submenu;
    [menu insertItem:services atIndex:2];
    [menu insertItem:NSMenuItem.separatorItem atIndex:3];
    NSMenuItem* item = [[NSMenuItem alloc] init];
    item.submenu = menu;
    return item;
}

// Makes `tree` the menu bar that `description` describes, after the application's own menu, updating its menus in
// place. A null description leaves the application's menu alone.
void menuBarOf(const PrAppMenu* PR_NULLABLE description, PrCocoaMenuTree* tree, PrCocoaHost* host,
        PrCocoaWindow* PR_NULLABLE owner) {
    NSMutableArray<PrCocoaMenu*>* previous = tree.menus != nil ? tree.menus : [NSMutableArray array];
    tree.menus = [NSMutableArray array];
    if (tree.root == nil)
        tree.root = [[NSMenu alloc] init];
    [tree.root removeAllItems];
    [tree.root addItem:applicationMenuItem()];
    for (uint32_t i = 0; description != nullptr && i < description->count; ++i) {
        const PrAppMenuItem& top = description->items[i];
        if (top.submenu == nullptr)
            continue;
        NSMenuItem* item = [[NSMenuItem alloc] init];
        item.submenu = menuOf(*top.submenu, tree, previous, host, owner);
        item.title = top.title.size != 0 ? stringOf(top.title) : stringOf(top.submenu->title);
        item.submenu.title = item.title;
        if (top.role == PrAppMenuRole_WindowList)
            NSApp.windowsMenu = item.submenu;
        else if (top.role == PrAppMenuRole_Help)
            NSApp.helpMenu = item.submenu;
        [tree.root addItem:item];
    }
}

void setMenuBar(PrApp* app, const PrAppMenu* PR_NULLABLE menu) {
    PrCocoaHost* host = hostOf(app);
    if (host.menuBar == nil)
        host.menuBar = [[PrCocoaMenuTree alloc] init];
    menuBarOf(menu, host.menuBar, host, nil);
    [host showMenuBar];
}

void windowSetMenuBar(PrAppWindow* window, const PrAppMenu* PR_NULLABLE menu) {
    PrCocoaWindow* object = objectOf(window);
    if (menu == nullptr) {
        object.menuBar = nil;
    } else {
        if (object.menuBar == nil)
            object.menuBar = [[PrCocoaMenuTree alloc] init];
        menuBarOf(menu, object.menuBar, object.host, object);
    }
    [object.host showMenuBar];
}

// Shows a context menu from the main queue once no callback runs, so that its MenuOpening event reaches the program
// before the menu opens.
void showMenuSoon(PrCocoaWindow* object, PrVec2 position) {
    __weak PrCocoaWindow* weak = object;
    dispatch_async(dispatch_get_main_queue(), ^{
        PrCocoaWindow* window = weak;
        if (window == nil || window.record == nullptr || window.contextMenu == nil)
            return;
        if (window.host.inCallback) {
            showMenuSoon(window, position);
            return;
        }
        NSMenu* menu = window.contextMenu.root;
        [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(CGFloat(position.x), CGFloat(position.y))
            inView:window.view];
        window.contextMenu = nil;
    });
}

void showMenu(PrAppWindow* window, const PrAppMenu* menu, PrVec2 position) {
    PrCocoaWindow* object = objectOf(window);
    if (object.host.headless)
        return;
    PrCocoaMenuTree* tree = [[PrCocoaMenuTree alloc] init];
    tree.menus = [NSMutableArray array];
    tree.root = menuOf(*menu, tree, [NSMutableArray array], object.host, object);
    object.contextMenu = tree;
    showMenuSoon(object, position);
}

// ---------------------------------------------------------------------------------------------------------------------
// Status items

struct CocoaStatusItem : PrAppStatusItem {
    void* object;               // the PrCocoaStatusItem, retained
};

// Returns the icon of a status item at the menu bar's height, from straight-alpha pixels, or nil for none.
NSImage* PR_NULLABLE statusIconOf(PrPixelView pixels, bool templated) {
    NSBitmapImageRep* rep = imageRepOf(pixels);
    if (rep == nil)
        return nil;
    CGFloat height = NSStatusBar.systemStatusBar.thickness - 4;
    CGFloat width = height * CGFloat(pixels.extent.x) / CGFloat(MAX(pixels.extent.y, 1u));
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:rep];
    [image setTemplate:templated];
    return image;
}

void statusItemApply(PrCocoaStatusItem* object, const PrAppStatusItemDesc& desc) {
    NSStatusBarButton* button = object.item.button;
    object.tag = desc.tag;
    button.image = statusIconOf(desc.icon, desc.templateIcon);
    button.title = stringOf(desc.title);
    button.imagePosition = desc.title.size != 0 ? NSImageLeft : NSImageOnly;
    button.toolTip = desc.tooltip.size != 0 ? stringOf(desc.tooltip) : nil;
    if (desc.menu != nullptr) {
        NSMutableArray<PrCocoaMenu*>* previous = object.tree.menus != nil ? object.tree.menus : [NSMutableArray array];
        if (object.tree == nil)
            object.tree = [[PrCocoaMenuTree alloc] init];
        object.tree.menus = [NSMutableArray array];
        object.tree.root = menuOf(*desc.menu, object.tree, previous, object.host, nil);
        object.item.menu = object.tree.root;
    } else {
        object.tree = nil;
        object.item.menu = nil;
        button.target = object;
        button.action = @selector(clicked:);
    }
}

void statusItemDestroy(PrAppStatusItem* item) {
    CocoaStatusItem* cocoa = static_cast<CocoaStatusItem*>(item);
    PrCocoaStatusItem* object = (__bridge_transfer PrCocoaStatusItem*)cocoa->object;
    [NSStatusBar.systemStatusBar removeStatusItem:object.item];
    prMemoryFree(cocoa);
}

void statusItemSet(PrAppStatusItem* item, const PrAppStatusItemDesc* desc) {
    statusItemApply((__bridge PrCocoaStatusItem*)static_cast<CocoaStatusItem*>(item)->object, *desc);
}

PrStatus statusItemCreate(PrApp* app, const PrAppStatusItemDesc* desc, PrAppStatusItem* PR_NULLABLE* PR_NONNULL out) {
    PrCocoaHost* host = hostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    if (host.headless)
        return PrStatus_NotSupported;
    PrCocoaStatusItem* object = [[PrCocoaStatusItem alloc] init];
    object.host = host;
    object.item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    statusItemApply(object, *desc);
    CocoaStatusItem* item = allocZeroed<CocoaStatusItem>();
    item->destroyFn = statusItemDestroy;
    item->setFn = statusItemSet;
    item->object = (__bridge_retained void*)object;
    *out = item;
    return PrStatus_Ok;
}

// Returns the items of `content` that a share picker offers: its text, its URLs, its files and its image.
NSArray* shareItemsOf(const PrAppDataContent& content) {
    NSMutableArray* items = [NSMutableArray array];
    if ((content.forms & PrAppDataFormFlag_Text) != 0 && content.text.size != 0)
        [items addObject:stringOf(content.text)];
    for (uint32_t i = 0; (content.forms & PrAppDataFormFlag_Urls) != 0 && i < content.urlCount; ++i) {
        if (NSURL* url = [NSURL URLWithString:stringOf(content.urls[i])])
            [items addObject:url];
    }
    for (uint32_t i = 0; (content.forms & PrAppDataFormFlag_Files) != 0 && i < content.pathCount; ++i)
        [items addObject:[NSURL fileURLWithPath:stringOf(content.paths[i])]];
    if ((content.forms & PrAppDataFormFlag_Image) != 0) {
        if (NSBitmapImageRep* rep = imageRepOf(content.image)) {
            NSImage* image = [[NSImage alloc] initWithSize:rep.size];
            [image addRepresentation:rep];
            [items addObject:image];
        }
    }
    return items;
}

void share(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done, void* PR_NULLABLE context) {
    PrApp* app = static_cast<CocoaWindow*>(window)->app;
    uint32_t id = appDataRequestsAdd(static_cast<CocoaApp*>(app)->requests, kPrAppDataSourceNone, AppDataAnswer_Status,
        (void (*)(void))done, context);
    PrCocoaWindow* object = objectOf(window);
    NSArray* items = shareItemsOf(*content);
    if (object.host.headless || items.count == 0) {
        PrStatus status = object.host.headless ? PrStatus_NotSupported : PrStatus_InvalidData;
        answerSoon(app, answerOf(CocoaAnswerKind_Status, id, status));
        return;
    }
    PrCocoaShare* picker = [[PrCocoaShare alloc] init];
    picker.app = app;
    picker.request = id;
    picker.picker = [[NSSharingServicePicker alloc] initWithItems:items];
    picker.picker.delegate = picker;
    [object.host.shares addObject:picker];
    NSRect rect = NSMakeRect(CGFloat(anchor.left), CGFloat(anchor.top), CGFloat(anchor.right - anchor.left),
        CGFloat(anchor.bottom - anchor.top));
    [picker.picker showRelativeToRect:rect ofView:object.view preferredEdge:NSRectEdgeMaxY];
}

PrStatus surface(const PrAppWindow* window, PrAppSurface* out) {
    out->kind = PrAppSurfaceKind_NsView;
    out->handle = (__bridge void*)objectOf(window).view;
    return PrStatus_Ok;
}

// Calls the window's handler with an event. A window that the program destroyed during the call is freed.
void deliverEvent(PrAppWindow* window, const PrAppEvent& event) {
    if (!appWindowDeliverEvent(static_cast<CocoaWindow*>(window)->delivery, window, &event, frameState))
        windowDestroy(window);
}

// Calls the window's handler with a frame and applies the output. A window that the program destroyed during the call
// is freed.
void deliverFrame(PrAppWindow* window, const PrAppFrame& frame) {
    PrAppOutput output{};
    if (!appWindowDeliverFrame(static_cast<CocoaWindow*>(window)->delivery, window, &frame, &output)) {
        windowDestroy(window);
        return;
    }
    setCursor(window, output.cursor);
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(window);
    // The text input applies to the window that the output names, which may be another than the one before.
    PrAppWindow* before = cocoa->textInputWindow;
    PrAppWindow* named = output.textInputWindow != nullptr ? output.textInputWindow : window;
    cocoa->textInputWindow = output.hasTextInput ? named : nullptr;
    cocoa->textInputKind = output.textInputKind;
    cocoa->textInput = output.textInput;
    applyTextInput(window);
    if (before != nullptr && before != window)
        applyTextInput(before);
    if (cocoa->textInputWindow != nullptr && cocoa->textInputWindow != window && cocoa->textInputWindow != before)
        applyTextInput(cocoa->textInputWindow);
    appTitleBarRegionsSet(cocoa->titleBar, output);
    cocoa->dropOperation = output.dropOperation;
    if (output.wantsFrame)
        requestFrame(window);
    requestFrameAt(window, output.frameTime);
}

// Returns what the application reports of `screen`, whose name it keeps until the displays change. The first screen
// holds the menu bar.
PrAppDisplayInfo displayInfoOf(const PrApp* app, NSScreen* screen) {
    PrCocoaHost* host = hostOf(app);
    if (host.displayNames == nil)
        host.displayNames = [NSMutableDictionary dictionary];
    NSNumber* key = @(displayIdOf(screen));
    NSData* name = host.displayNames[key];
    if (name == nil) {
        name = [screen.localizedName dataUsingEncoding:NSUTF8StringEncoding];
        if (name != nil)
            host.displayNames[key] = name;
    }
    return infoOf(screen, screen == NSScreen.screens.firstObject, name);
}

uint32_t displays(const PrApp* app, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity) {
    NSArray<NSScreen*>* screens = NSScreen.screens;
    for (NSUInteger i = 0; i < screens.count && i < capacity; ++i)
        out[i] = displayInfoOf(app, screens[i]);
    return uint32_t(screens.count);
}

bool displayInfo(const PrApp* app, PrAppDisplayId id, PrAppDisplayInfo* out) {
    NSScreen* screen = screenOf(id);
    if (screen == nil)
        return false;
    *out = displayInfoOf(app, screen);
    return true;
}

PrStatus windowCreate(PrApp* app, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out) {
    PrCocoaHost* host = hostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    if (static_cast<CocoaApp*>(app)->windowModel == PrAppWindowModel_Single && host.windows.count != 0)
        return PrStatus_NotSupported;
    CocoaWindow* window = allocZeroed<CocoaWindow>();
    window->destroyFn = windowDestroy;
    window->appFn = windowApp;
    window->requestFrameFn = requestFrame;
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
    window->dragStartFn = dragStart;
    window->shareFn = share;
    window->surfaceFn = surface;
    window->setAspectRatioFn = setAspectRatio;
    window->setPopupPlacementFn = setPopupPlacement;
    window->isOccludedFn = isOccluded;
    window->setOpacityFn = setOpacity;
    window->setKeepAboveFn = setKeepAbove;
    window->setProgressFn = setProgress;
    window->setDocumentFn = setDocument;
    window->setMenuBarFn = windowSetMenuBar;
    window->showMenuFn = showMenu;
    window->dragPromiseEndFn = dragPromiseEnd;
    window->app = app;
    window->delivery.handler = desc->handler;
    PrCocoaWindow* object = [[PrCocoaWindow alloc] init];
    object.record = window;
    object.host = host;
    object.role = desc->role;
    object.overlay = desc->role == PrAppWindowRole_Overlay;
    object.shown = !desc->hidden;
    bool popup = desc->role == PrAppWindowRole_Popup;
    bool dialog = desc->role == PrAppWindowRole_Dialog;
    if (desc->parent != nullptr && (popup || dialog))
        object.parent = objectOf(desc->parent);
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
        PrCocoaPanel* panel = [[PrCocoaPanel alloc] initWithContentRect:content styleMask:style
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
        PrCocoaPanel* panel = [[PrCocoaPanel alloc] initWithContentRect:content styleMask:style
            backing:NSBackingStoreBuffered defer:NO];
        panel.refusesKey = YES;
        panel.hidesOnDeactivate = NO;
        panel.level = NSPopUpMenuWindowLevel;
        panel.collectionBehavior = NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle;
        panel.hasShadow = YES;
        object.window = panel;
    } else if (borderless) {
        object.window = [[PrCocoaBorderlessWindow alloc] initWithContentRect:content styleMask:style
            backing:NSBackingStoreBuffered defer:NO];
        object.window.hasShadow = YES;
    } else {
        object.window = [[NSWindow alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered defer:NO];
    }
    object.window.title = stringOf(desc->title);
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
    object.view = [[PrCocoaView alloc] initWithFrame:object.window.contentView.bounds];
    object.view.owner = object;
    object.view.host = host;
    object.view.cursor = NSCursor.arrowCursor;
    object.view.wantsLayer = YES;
    object.view.allowedTouchTypes = NSTouchTypeMaskIndirect;
    object.view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    object.view.layer.backgroundColor = NSColor.blackColor.CGColor;
    object.window.contentView = object.view;
    object.window.delegate = object;
    NSMutableArray<NSPasteboardType>* types = [NSMutableArray arrayWithArray:imageTypes()];
    [types addObjectsFromArray:@[
        NSPasteboardTypeString, NSPasteboardTypeHTML, NSPasteboardTypeURL, NSPasteboardTypeFileURL, kPayloadType,]];
    [object.view registerForDraggedTypes:types];
    [object.window makeFirstResponder:object.view];
    [object setBackdropOf:desc->backdrop];
    if (host.headless) {
        [object.window setFrameOrigin:NSMakePoint(-32000, -32000)];
    } else {
        // By default, a window cascades from the last, and the first is centered. A dialog is centered on its parent,
        // and a popup goes where its placement puts it.
        PrCocoaWindow* previous = host.windows.lastObject;
        NSScreen* chosen = desc->placement == PrAppWindowPlacement_Position ? screenOf(desc->position.display) : nil;
        NSWindow* parent = object.parent.window;
        if (popup && parent != nil) {
            [object place];
        } else if (chosen != nil) {
            placeOnScreen(object.window, chosen, desc->position.position);
        } else if (parent != nil) {
            NSRect around = parent.frame;
            NSRect frame = object.window.frame;
            [object.window setFrameOrigin:NSMakePoint(NSMidX(around) - frame.size.width / 2,
                NSMidY(around) - frame.size.height / 2)];
        } else if (desc->placement == PrAppWindowPlacement_Center) {
            NSScreen* active = NSApp.keyWindow.screen;
            centerOnScreen(object.window, active != nil ? active : NSScreen.screens.firstObject);
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

PrAppWindowModel windowModel(const PrApp* app) {
    return static_cast<const CocoaApp*>(app)->windowModel;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

@implementation PrCocoaPanel
- (BOOL)canBecomeKeyWindow {
    return !self.refusesKey;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

@implementation PrCocoaMenu
// Delivers the item's tag to the menu's window or to the application.
- (void)deliverKind:(PrAppEventKind)kind tag:(uint64_t)tag {
    PrCocoaHost* host = self.host;
    if (host == nil || !host.running)
        return;
    PrAppWindow* target = nullptr;
    if (self.ofWindow) {
        target = self.owner.record;
        if (target == nullptr)
            return;
    }
    PrAppEvent event{};
    event.kind = kind;
    event.time = uptime();
    event.tag = tag;
    event.modifiers = modifiersOf(NSEvent.modifierFlags);
    [host deliver:event to:target];
}
- (void)chosen:(NSMenuItem*)item {
    [self deliverKind:PrAppEventKind_MenuItem tag:uint64_t(item.tag)];
}
// AppKit asks before the menu shows, which lets the program set the menu again with its current state.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    (void)menu;
    [self deliverKind:PrAppEventKind_MenuOpening tag:self.tag];
}
@end

@implementation PrCocoaMenuTree
@end

@implementation PrCocoaStatusItem
- (void)clicked:(id)sender {
    (void)sender;
    PrCocoaHost* host = self.host;
    if (host == nil || !host.running)
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_StatusItem;
    event.time = uptime();
    event.tag = self.tag;
    [host deliver:event to:nullptr];
}
@end

@implementation PrCocoaPromise
- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)filePromiseProvider fileNameForType:(NSString*)fileType {
    (void)filePromiseProvider;
    (void)fileType;
    return self.name;
}
// AppKit asks on the main queue, the provider's default.
- (void)filePromiseProvider:(NSFilePromiseProvider*)filePromiseProvider writePromiseToURL:(NSURL*)url
        completionHandler:(void (^)(NSError* PR_NULLABLE))completionHandler {
    (void)filePromiseProvider;
    PrCocoaWindow* owner = self.owner;
    if (owner == nil) {
        completionHandler([NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
        return;
    }
    [owner promise:self.index to:url done:completionHandler];
}
@end

@implementation PrCocoaShare
// The user chose a service, or none, which ends the share either way.
- (void)sharingServicePicker:(NSSharingServicePicker*)sharingServicePicker
        didChooseSharingService:(NSSharingService*)service {
    (void)sharingServicePicker;
    (void)service;
    PrApp* app = self.app;
    PrCocoaHost* host = hostOf(app);
    answerSoon(app, answerOf(CocoaAnswerKind_Status, self.request, PrStatus_Ok));
    [host.shares removeObject:self];
}
@end

@implementation PrCocoaBorderlessWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

// The Dock has no states of progress, so the bar takes the taskbar's colors: the accent color, yellow while paused and
// red for an error. An indeterminate bar is striped. The tile draws only when the Dock asks it to, so nothing animates.
@implementation PrCocoaDockProgress
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect bounds = self.bounds;
    // An executable outside a bundle has no icon of its own, and the Dock shows the executable's icon from the Finder.
    NSImage* icon = NSApp.applicationIconImage;
    if (!isBundled())
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

@implementation PrCocoaView {
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
    PrCocoaWindow* owner = self.owner;
    bool input = event.kind >= PrAppEventKind_PointerMove && event.kind <= PrAppEventKind_CompositionEnd;
    if (owner.record != nullptr && !(input && owner.blocker != nil))
        [owner.host deliver:event to:owner.record];
}
// Delivers the pointer's crossing into the view or out of it, at `position`, once for each crossing.
- (void)cross:(PrAppEventKind)kind at:(PrVec2)position time:(double)time modifiers:(NSEventModifierFlags)modifiers {
    PrCocoaWindow* owner = self.owner;
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
    event.modifiers = modifiersOf(modifiers);
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
    PrCocoaWindow* owner = self.owner;
    // A locked pointer stays where the lock began, and reports only its movement.
    if (owner.locked)
        position = owner.lockedAt;
    PrAppEvent event{};
    event.kind = kind;
    event.time = source.timestamp;
    event.modifiers = modifiersOf(source.modifierFlags);
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
        describePen(source, self.owner.eraser, &event);
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
    if (PrCocoaWindow* blocker = self.owner.blocker) {
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
    event.modifiers = modifiersOf(source.modifierFlags);
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
    event.modifiers = modifiersOf(e.modifierFlags);
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
    } else {
        if (NSString* character = characterOf(source)) {
            bytes = [character dataUsingEncoding:NSUTF8StringEncoding];
            event.keyboard.text = viewOf(bytes);
        }
        // A key that is dead with Option held, as Option-E is in the US layout, means its key in a shortcut.
        NSEventModifierFlags chord =
            NSEventModifierFlagOption | NSEventModifierFlagCommand | NSEventModifierFlagControl;
        bool dead = (source.modifierFlags & chord) == 0 && isDead(source);
        event.keyboard.key = dead ? PrAppKey_Dead : keyOfCharacter(source);
    }
    PrCocoaWindow* owner = self.owner;
    [owner key:event.keyboard.code down:kind == PrAppEventKind_KeyDown key:event.keyboard.key];
    if (mark == kNoMark)
        [self deliver:event];
    else if (owner.record != nullptr)
        [owner.host deliver:event to:owner.record ahead:mark];
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
    MacKey mac = macKeyOf(e);
    if (mac.key == PrAppKey_Unknown)
        return;
    [self key:e kind:modifierDown(e, mac.code) ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp];
}
- (void)deliverText:(NSString*)text kind:(PrAppEventKind)kind selection:(NSRange)selection {
    NSData* bytes = [text dataUsingEncoding:NSUTF8StringEncoding];
    PrAppEvent event{};
    event.kind = kind;
    event.time = uptime();
    event.input.text = viewOf(bytes);
    if (selection.location != NSNotFound)
        event.input.selection = PrAppTextRange{
            utf8Length(text, selection.location),
            utf8Length(text, selection.location + selection.length),
        };
    [self deliver:event];
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
    event.time = uptime();
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
// A paste that the user chose from a menu. The program receives the pasteboard's text in a Paste event.
- (void)paste:(id)sender {
    (void)sender;
    NSString* string = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
    [self deliverText:string != nil ? string : @"" kind:PrAppEventKind_Paste selection:NSMakeRange(NSNotFound, 0)];
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
    PrCocoaWindow* owner = self.owner;
    NSPasteboard* board = sender.draggingPasteboard;
    NSPoint p = [self convertPoint:sender.draggingLocation fromView:nil];
    PrAppDragEvent drag{
        .position = PrVec2{float(p.x), float(p.y)},
        .forms = formsOf(board),
        .operations = operationsOf(sender.draggingSourceOperationMask),
    };
    const CocoaDrag& carried = static_cast<CocoaApp*>(owner.host.app)->drag;
    if (carriesDrag(board, carried)) {
        drag.fromThisWindow = carried.window == owner.record;
        if (carried.hasPayload) {
            drag.forms |= PrAppDataFormFlag_Payload;
            drag.payload = carried.payload;
        }
    }
    return drag;
}
- (void)deliverDrag:(PrAppEventKind)kind drag:(const PrAppDragEvent&)drag {
    PrAppEvent event{.kind = kind, .modifiers = modifiersOf(NSEvent.modifierFlags), .time = uptime(), .drag = drag};
    [self deliver:event];
    [self.owner requestFrame];
}
// Returns the operation that the window's latest output answers a drag with, within what the source allows.
- (PrAppDragOperation)answer:(id<NSDraggingInfo>)sender {
    PrAppWindow* record = self.owner.record;
    if (record == nullptr)
        return PrAppDragOperation_None;
    PrAppDragOperation wanted = static_cast<CocoaWindow*>(record)->dropOperation;
    return appDragAnswer(wanted, operationsOf(sender.draggingSourceOperationMask));
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    [self deliverDrag:PrAppEventKind_DragEnter drag:[self dragOf:sender]];
    return answerOf([self answer:sender], sender.draggingSourceOperationMask);
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    [self deliverDrag:PrAppEventKind_DragOver drag:[self dragOf:sender]];
    return answerOf([self answer:sender], sender.draggingSourceOperationMask);
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
    readDrop(sender.draggingPasteboard, *store);
    drag.drop = appDataSourceCreate(static_cast<CocoaApp*>(self.owner.host.app)->sources, store, nullptr);
    [self deliverDrag:PrAppEventKind_Drop drag:drag];
    return YES;
}
// NSDraggingSource. The window's drag allows the operations of its description, within the application and outside it.
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
        sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    (void)context;
    PrAppWindow* record = self.owner.record;
    return record != nullptr ? maskOf(static_cast<CocoaWindow*>(record)->drag.operations) : NSDragOperationNone;
}
- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)screenPoint
        operation:(NSDragOperation)operation {
    (void)session;
    (void)screenPoint;
    PrCocoaHost* host = self.host;
    if (PrCocoaWindow* owner = self.owner) {
        [owner endDrag:operationOf(operation)];
    } else if (host.app != nullptr) {
        // The window closed during the drag, and receives no DragEnd. The drag is over all the same.
        CocoaDrag& carried = static_cast<CocoaApp*>(host.app)->drag;
        if (carried.window == nullptr)
            carried = CocoaDrag{};
    }
    if (host.dragSource == self)
        host.dragSource = nil;
}
@end

@implementation PrCocoaWindow {
    // The promises of the window's last drag, which hold their providers' delegates, since a provider's delegate is a
    // weak reference. The promises that wait for the program, by index, each a pair of its destination and AppKit's
    // completion.
    NSMutableArray<PrCocoaPromise*>* _promises;
    NSMutableDictionary<NSNumber*, NSArray*>* _promised;
    // The pointer buttons that are down, by bit of PrAppPointerEvent::button, where the pointer last was, and the keys
    // that are down, by their position. The window releases them when it stops being the key window, as the releases
    // then go to another window.
    uint32_t _buttons;
    PrVec2 _pointer;
    HeldKey _keys[PrAppKeyCode_Count];
}
- (void)frame {
    PrCocoaHost* host = self.host;
    // A hidden window has no frames, but for the one that it shows with.
    if (self.record == nullptr || !host.running || (!self.shown && !self.showing))
        return;
    // A frame that comes during a callback, from a run loop that the callback runs, waits for the next.
    if (host.inCallback) {
        [self requestFrame];
        return;
    }
    double now = uptime();
    PrAppFrame frame = [self frameAt:now];
    self.lastFrame = now;
    [host beginCallback];
    deliverFrame(self.record, frame);
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
    frame.display = displayOf(self.window.screen, self.window.backingScaleFactor);
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
    const AppTitleBarRegions& regions = static_cast<CocoaWindow*>(self.record)->titleBar;
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
    const AppTitleBarRegions& regions = static_cast<CocoaWindow*>(self.record)->titleBar;
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
    __weak PrCocoaWindow* weak = self;
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
    __weak PrCocoaWindow* weak = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        PrCocoaWindow* window = weak;
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
- (void)blockBy:(PrCocoaWindow* PR_NULLABLE)blocker {
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
    NSRect placed = popupFrame(parent, content.size, self.popup);
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
        __weak PrCocoaWindow* weak = self;
        self.timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:NO block:^(NSTimer*) {
            PrCocoaWindow* window = weak;
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
- (void)requestFrameAt:(double)time {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    if (self.record == nullptr || !self.host.running || time == 0)
        return;
    double delay = MAX(time - uptime(), 0.0);
    __weak PrCocoaWindow* weak = self;
    self.frameTimer = [NSTimer timerWithTimeInterval:delay repeats:NO block:^(NSTimer*) {
        PrCocoaWindow* window = weak;
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
        _keys[code] = HeldKey{.down = bool(down), .key = key};
}
// Releases the buttons and the keys that are down, the buttons where the pointer last was, for a window that stopped
// being the key window.
- (void)releaseHeld {
    PrAppModifierFlags modifiers = modifiersOf(NSEvent.modifierFlags);
    double now = uptime();
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
    writeContent(content, token, first, writers);
    if (first.types.count == 0)
        [writers removeObject:first];
    // Each promised file is a provider of its own, whose type the file's extension gives. A drag replaces the last
    // drag's promises, whose indices it reuses.
    AppDragPending& pending = static_cast<CocoaWindow*>(self.record)->drag;
    if (_promises == nil)
        _promises = [NSMutableArray array];
    [_promises removeAllObjects];
    const PrStringView* names = appStringsViews(pending.promises);
    for (uint32_t i = 0; i < pending.promises.ends.count; ++i) {
        NSString* name = stringOf(names[i]);
        UTType* type = [UTType typeWithFilenameExtension:name.pathExtension];
        NSString* identifier = type != nil ? type.identifier : UTTypeData.identifier;
        PrCocoaPromise* promise = [[PrCocoaPromise alloc] init];
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
        rep = imageRepOf(PrPixelView{pending.preview.data, size_t(pending.previewExtent.x) * 4, pending.previewExtent});
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
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(self.record);
    if (cocoa == nullptr || !cocoa->drag.pending)
        return;
    cocoa->drag.pending = false;
    PrCocoaHost* host = self.host;
    NSEvent* press = self.view.press;
    PrAppDataContent content = appDataStoreContent(cocoa->drag.content);
    uint64_t token = prRandomSeed();
    bool down = (_buttons & 1u) != 0 && press != nil && !host.headless;
    NSArray<NSDraggingItem*>* items = down ? [self dragItems:content token:token] : @[];
    if (items.count == 0) {
        [self endDrag:PrAppDragOperation_None];
        return;
    }
    static_cast<CocoaApp*>(host.app)->drag = CocoaDrag{
        .active = true,
        .token = token,
        .window = cocoa,
        .payload = content.payload,
        .hasPayload = (content.forms & PrAppDataFormFlag_Payload) != 0,
    };
    [self.view beginDraggingSessionWithItems:items event:press source:self.view];
    host.dragSource = self.view;
    _buttons &= ~1u;
    self.view.press = nil;
    PrAppEvent cancel{
        .kind = PrAppEventKind_PointerCancel,
        .modifiers = modifiersOf(NSEvent.modifierFlags),
        .time = uptime(),
        .pointer = {.position = _pointer, .type = PrAppPointerType_Mouse, .primary = true, .id = kPrAppPointerMouse},
    };
    [self.view deliver:cancel];
}
// Ends the window's drag with the operation that the target performed.
- (void)endDrag:(PrAppDragOperation)operation {
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(self.record);
    if (cocoa == nullptr)
        return;
    CocoaDrag& carried = static_cast<CocoaApp*>(cocoa->app)->drag;
    if (carried.window == cocoa)
        carried = CocoaDrag{};
    PrAppEvent event{
        .kind = PrAppEventKind_DragEnd,
        .modifiers = modifiersOf(NSEvent.modifierFlags),
        .time = uptime(),
        .drag = {
            .forms = cocoa->drag.content.forms,
            .operations = cocoa->drag.operations,
            .operation = operation,
            .fromThisWindow = true,
            .payload = cocoa->drag.content.payload,
        },
    };
    [self.view deliver:event];
}
// Asks the program for the promise at `index`, whose file goes to `url`. The destination is a source of that one file,
// which the window holds until the promise ends.
- (void)promise:(uint32_t)index to:(NSURL*)url done:(void (^)(NSError* PR_NULLABLE))done {
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(self.record);
    if (cocoa == nullptr || !self.host.running) {
        done([NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
        return;
    }
    [self endPromise:index status:PrStatus_Io];
    CocoaApp* app = static_cast<CocoaApp*>(cocoa->app);
    AppDataStore* store = allocZeroed<AppDataStore>();
    NSData* name = [url.lastPathComponent dataUsingEncoding:NSUTF8StringEncoding];
    appDataStoreAddFile(*store, PrAppDataFile{.name = viewOf(name), .path = url.fileSystemRepresentation});
    PrAppDataSource destination = appDataSourceCreate(app->sources, store, nullptr);
    if (_promised == nil)
        _promised = [NSMutableDictionary dictionary];
    _promised[@(index)] = @[ @(destination), [done copy] ];
    PrAppEvent event{};
    event.kind = PrAppEventKind_DragPromise;
    event.time = uptime();
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
    CocoaWindow* cocoa = static_cast<CocoaWindow*>(self.record);
    if (cocoa != nullptr)
        appDataSourceRelease(static_cast<CocoaApp*>(cocoa->app)->sources,
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
    if (PrCocoaWindow* blocker = self.blocker) {
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
    event.time = uptime();
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

@implementation PrCocoaHost {
    // The events that arrived during the running callback, in order. A callback that runs a run loop of its own, as a
    // modal panel does, has events delivered within it, and they wait here, so that no callback nests in another.
    RawArray<HeldEvent> _held;
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
    for (uint32_t i = 0; i < _held.count; ++i)
        heldReset(_held.data[i], [self sources]);
    _held.reset();
}
// The application's sources, or null once the application is gone.
- (AppDataSources*)sources {
    return self.app != nullptr ? &static_cast<CocoaApp*>(self.app)->sources : nullptr;
}
// Calls the program with an event, as no other callback runs.
- (void)call:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window {
    self.inCallback = YES;
    if (window != nullptr) {
        deliverEvent(window, event);
    } else {
        const PrAppHandler& handler = static_cast<CocoaApp*>(self.app)->handler;
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
        // An event that its delivery holds goes to the end of the array, which may move the array but not the text.
        if (_held.data[i].dropped)
            continue;
        PrStringView text{_held.data[i].text.data != nullptr ? _held.data[i].text.data : "", _held.data[i].text.count};
        if (CocoaAnswer* answer = _held.data[i].answer) {
            // The array may move as the callback runs, and the answer that it owns stays in place.
            [self call:*answer];
            continue;
        }
        PrAppEvent event = _held.data[i].event;
        prAppEventSetText(&event, text);
        if (event.kind == PrAppEventKind_Open)
            event.open.arguments = appStringsViews(_held.data[i].arguments);
        if (event.kind == PrAppEventKind_TrackpadTouches)
            event.trackpad.touches = _held.data[i].touches.data;
        [self call:event to:_held.data[i].window];
    }
    for (uint32_t i = 0; i < _held.count; ++i)
        heldReset(_held.data[i], [self sources]);
    _held.clear();
}
// Calls the callback of an answer's request. No other callback may be running. A dialog's source loses the answer's
// reference once the callback returns, so that the program keeps it only by retaining it.
- (void)call:(CocoaAnswer&)answer {
    CocoaApp* app = static_cast<CocoaApp*>(self.app);
    self.inCallback = YES;
    switch (answer.kind) {
        case CocoaAnswerKind_Store:
            appDataRequestsAnswerFromStore(app->requests, answer.id, answer.status,
                answer.external != nullptr ? answer.external : &answer.store);
            break;
        case CocoaAnswerKind_Bytes:
            appDataRequestsAnswerBytes(app->requests, answer.id, answer.status,
                PrByteView{answer.bytes.data, answer.bytes.count});
            break;
        case CocoaAnswerKind_Status:
            appDataRequestsAnswerStatus(app->requests, answer.id, answer.status);
            break;
        default:
            appDataRequestsAnswerSource(app->requests, answer.id, answer.status, answer.source);
            appDataSourceRelease(app->sources, answer.source);
            answer.source = kPrAppDataSourceNone;
            break;
    }
    self.inCallback = NO;
}
- (void)answer:(void*)stored {
    CocoaAnswer* answer = static_cast<CocoaAnswer*>(stored);
    if (!self.running) {
        if (answer->source != kPrAppDataSourceNone)
            appDataSourceRelease(static_cast<CocoaApp*>(self.app)->sources, answer->source);
        answerDestroy(answer);
        return;
    }
    if (self.inCallback || _holding) {
        _held.push(HeldEvent{.answer = answer});
        return;
    }
    [self call:*answer];
    answerDestroy(answer);
    [self endCallback];
}
// Delivers an event now, or holds it if a callback is running or holdEvents is in effect. Held events are delivered in
// order when the callback returns or the hold ends. An event that hands over a source releases its reference once
// delivered.
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window {
    if (self.running && (self.inCallback || _holding)) {
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
// Holds the events that arrive from now on, and returns the mark of the first of them, for deliver:to:ahead:.
- (uint32_t)holdEvents {
    PR_ASSERT(!_holding);
    _holding = YES;
    return _held.count;
}
// Ends holdEvents, and delivers `event` ahead of the events that arrived since `mark`, and then those events.
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow*)window ahead:(uint32_t)mark {
    _holding = NO;
    if (!self.running) {
        for (uint32_t i = mark; i < _held.count; ++i)
            heldReset(_held.data[i], [self sources]);
        _held.resize(mark);
        return;
    }
    _held.insert(mark, heldOf(window, event));
    if (!self.inCallback)
        [self endCallback];
}
// Drops the held events of a window that is closing, and the window.
- (void)forget:(PrAppWindow*)window {
    for (uint32_t i = 0; i < _held.count; ++i) {
        // The event stays in place, since the delivery that is under way counts through the array.
        if (_held.data[i].window == window)
            _held.data[i].dropped = true;
    }
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
    content.title = stringOf(desc->title);
    content.body = stringOf(desc->body);
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
    event.time = uptime();
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
    [self.displayNames removeAllObjects];
    PrAppEvent event{};
    event.kind = PrAppEventKind_DisplaysChanged;
    event.time = uptime();
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
        event.time = uptime();
        event.modifiers = modifiersOf(NSEvent.modifierFlags);
        event.tag = hotkeys.data[i].tag;
        [self deliver:event to:nullptr];
        return;
    }
}
// A request to quit asks the program, which answers with prAppQuitRespond. The run stops itself on a quit, so AppKit's
// request is canceled either way.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)application {
    (void)application;
    if (self.quitPending)
        return NSTerminateLater;
    self.quitPending = YES;
    PrAppEvent event{};
    event.kind = PrAppEventKind_QuitRequested;
    event.time = uptime();
    NSAppleEventDescriptor* request = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
    OSType reason = [request attributeDescriptorForKeyword:kAEQuitReason].enumCodeValue;
    event.quit.sessionEnding = reason == kAELogOut || reason == kAEReallyLogOut || reason == kAEShowRestartDialog ||
        reason == kAEShowShutdownDialog || reason == kAERestart || reason == kAEShutDown;
    [self deliver:event to:nullptr];
    // A program that answered during the event has its answer taken already. Otherwise AppKit waits for it.
    self.quitWaiting = self.quitPending;
    return self.quitPending ? NSTerminateLater : NSTerminateCancel;
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
        event.time = uptime();
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
    event.time = uptime();
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
    event.time = uptime();
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
            keyLabelsReset(static_cast<CocoaApp*>(host.app));
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
    NSMutableArray<NSData*>* bytes = [NSMutableArray array];
    RawArray<PrStringView> views;
    for (id argument in (NSArray*)arguments) {
        if (![argument isKindOfClass:NSString.class])
            continue;
        NSData* data = [(NSString*)argument dataUsingEncoding:NSUTF8StringEncoding];
        [bytes addObject:data];
        views.push(viewOf(data));
    }
    if (!self.headless)
        [NSApp activate];
    PrAppEvent event{};
    event.kind = PrAppEventKind_Open;
    event.time = uptime();
    event.open.arguments = views.data;
    event.open.argumentCount = views.count;
    [self deliver:event to:nullptr];
    views.reset();
}
- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    (void)application;
    if (self.app == nullptr)
        return;
    AppDataStore* store = allocZeroed<AppDataStore>();
    for (NSURL* url in urls) {
        if (url.isFileURL)
            addFile(url, *store);
        else
            appDataStoreAddUrl(*store, url.absoluteString.UTF8String);
    }
    PrAppEvent event{};
    event.kind = PrAppEventKind_Open;
    event.time = uptime();
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
- (void)quitRespond:(BOOL)quit {
    if (!self.quitPending)
        return;
    self.quitPending = NO;
    if (self.quitWaiting)
        [NSApp replyToApplicationShouldTerminate:NO];
    self.quitWaiting = NO;
    if (quit)
        [self stop];
}
@end

PrApp* prAppCocoaCreate(const PrAppOptions* options) {
    CocoaApp* app = allocZeroed<CocoaApp>();
    app->destroyFn = destroy;
    app->runFn = run;
    app->stopFn = stop;
    app->wakeFn = wake;
    app->notificationPostFn = notificationPost;
    app->openUrlFn = openUrl;
    app->cancelFn = dataCancel;
    app->quitRespondFn = quitRespond;
    app->dataRetainFn = dataRetain;
    app->dataReleaseFn = dataRelease;
    app->dataReadFn = dataRead;
    app->dataWriteFn = dataWrite;
    app->dataWriteFileFn = dataWriteFile;
    app->windowCreateFn = windowCreate;
    app->displaysFn = displays;
    app->displayInfoFn = displayInfo;
    app->pointerPositionFn = pointerPosition;
    app->setHiddenFn = setHidden;
    app->isHiddenFn = isHidden;
    app->revealPathFn = revealPath;
    app->hotkeyRegisterFn = hotkeyRegister;
    app->hotkeyUnregisterFn = hotkeyUnregister;
    app->fileDialogFn = fileDialog;
    app->setPresenceFn = setPresence;
    app->windowModelFn = windowModel;
    app->lifecycleFn = lifecycle;
    app->timerSetFn = timerSet;
    app->timerCancelFn = timerCancel;
    app->keyCodeLabelFn = keyCodeLabel;
    app->trashPathFn = trashPath;
    app->recentDocumentAddFn = recentDocumentAdd;
    app->recentDocumentsClearFn = recentDocumentsClear;
    app->setBadgeFn = setBadge;
    app->powerFn = power;
    app->setAwakeFn = setAwake;
    app->hapticPlayFn = hapticPlay;
    app->setMenuBarFn = setMenuBar;
    app->statusItemCreateFn = statusItemCreate;
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
        host.singleInstance = stringOf(options->singleInstance);
    app->host = (__bridge_retained void*)host;
    return app;
}
