// The Cocoa application backend's state: its classes, the application and its windows, and what the backend's parts
// share. Each part has a source and an internal header of its own.

#pragma once

#include "pr/alloc.h"
#include "pr/app_backend.h"
#include "pr/app_cocoa.h"
#include "pr/apple_string.h"
#include "pr/atomic.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/random.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/vec.h"

#include "app/backends/shared/src/app_shared_data.h"
#include "app/backends/shared/src/app_shared_delivery.h"
#include "app/backends/shared/src/app_shared_displays.h"
#include "app/backends/shared/src/app_shared_drag.h"
#include "app/backends/shared/src/app_shared_events.h"
#include "app/backends/shared/src/app_shared_title_bar.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <IOKit/ps/IOPSKeys.h>
#import <IOKit/ps/IOPowerSources.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UserNotifications/UserNotifications.h>

PR_ASSUME_NONNULL_BEGIN

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

// A window's content: its layer, which the program draws to, and the input, the text editing and the drags in it.
@interface PrCocoaView : NSView <CALayerDelegate, NSTextInputClient, NSDraggingSource>
@property(nonatomic, weak) PrCocoaWindow* PR_NULLABLE owner;

// The host, which outlives the owner, for a drag that ends after the window closes.
@property(nonatomic, weak) PrCocoaHost* PR_NULLABLE host;

// The latest event of the primary button while it is down, or nil.
@property(nonatomic, strong) NSEvent* PR_NULLABLE press;
@property(nonatomic, strong) NSCursor* cursor;
@property(nonatomic) NSRect textInputRect;               // where the caret stands, from the top left of the view
@property(nonatomic, copy) NSString* PR_NULLABLE marked; // the text that an input method composes, or nil
@property(nonatomic) NSRange markedSelection;            // the selection within the marked text
- (void)deliver:(const PrAppEvent&)event;
- (void)pointer:(NSEvent*)source kind:(PrAppEventKind)kind;
- (void)key:(NSEvent*)source kind:(PrAppEventKind)kind composing:(BOOL)composing mark:(uint32_t)mark;
- (void)key:(NSEvent*)source kind:(PrAppEventKind)kind;
@end

// A window: its NSWindow and view, its frames, and what is held down in it.
@interface PrCocoaWindow : NSObject <NSWindowDelegate>
@property(nonatomic) PrAppWindow* PR_NULLABLE record;      // null once the window is destroyed
@property(nonatomic, weak) PrCocoaHost* PR_NULLABLE host;
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) PrCocoaView* view;
@property(nonatomic, strong) CADisplayLink* PR_NULLABLE displayLink;
@property(nonatomic, strong) NSTimer* PR_NULLABLE timer;        // a headless window's frame
@property(nonatomic, strong) NSTimer* PR_NULLABLE frameTimer;   // the frame at the time that the last frame asked for
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
@property(nonatomic, weak) PrCocoaWindow* PR_NULLABLE parent;
@property(nonatomic) BOOL modal;
@property(nonatomic) BOOL sheet;

// The modal dialog without decoration that blocks the window's input, or nil, and the layer that dims the window's
// content while it does.
@property(nonatomic, weak) PrCocoaWindow* PR_NULLABLE blocker;
@property(nonatomic, strong) CALayer* PR_NULLABLE dim;
@property(nonatomic) PrAppPopupPlacement popup;

@property(nonatomic) BOOL secureInput;                                 // the window holds secure event input for a
                                                                       // password
@property(nonatomic, strong) PrCocoaMenuTree* PR_NULLABLE menuBar;     // the window's own menu bar, or nil
@property(nonatomic, strong) PrCocoaMenuTree* PR_NULLABLE contextMenu; // the context menu that shows, or nil
- (void)promise:(uint32_t)index to:(NSURL*)url done:(void (^)(NSError* PR_NULLABLE))done;
- (void)endPromise:(uint32_t)index status:(PrStatus)status;
- (void)endPromises;
@property(nonatomic) BOOL shown;                           // the program shows the window (prAppWindowSetVisible)
@property(nonatomic) BOOL showing;                         // the window shows once its next frame is drawn
@property(nonatomic) PrAppWindowBackdrop backdrop;
@property(nonatomic) BOOL transparent;                               // the window can show a backdrop
                                                                     // (PrAppWindowDesc::transparent)
@property(nonatomic, strong) NSVisualEffectView* PR_NULLABLE effect; // the blur behind the view, or nil

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
- (void)stopFrames;
@end

// A menu that AppKit shows for the program: its NSMenu, its tag, and where its events go. Each menu of a tree is its
// menu's delegate and its items' target. A menu of the application sends its events to the application, and a menu of a
// window to that window, or nowhere once the window is gone.
@interface PrCocoaMenu : NSObject <NSMenuDelegate>
@property(nonatomic) uint64_t tag;
@property(nonatomic, strong) NSMenu* PR_NULLABLE menu;
@property(nonatomic, weak) PrCocoaHost* PR_NULLABLE host;
@property(nonatomic, weak) PrCocoaWindow* PR_NULLABLE owner;
@property(nonatomic) BOOL ofWindow;
- (void)chosen:(NSMenuItem*)item;
- (void)deliverKind:(PrAppEventKind)kind tag:(uint64_t)tag;
@end

// The menus that one description made, kept so that setting the description again updates them in place, as while one
// of them is open. `root` is the menu bar, or the menu itself.
@interface PrCocoaMenuTree : NSObject
@property(nonatomic, strong) NSMutableArray<PrCocoaMenu*>* menus;
@property(nonatomic, strong) NSMenu* root;
@end

// A status item in the menu bar, and the menu that a click opens, or the tag that a click sends if it has none.
@interface PrCocoaStatusItem : NSObject
@property(nonatomic, weak) PrCocoaHost* PR_NULLABLE host;
@property(nonatomic, strong) NSStatusItem* item;
@property(nonatomic, strong) PrCocoaMenuTree* PR_NULLABLE tree;
@property(nonatomic) uint64_t tag;
- (void)clicked:(id)sender;
@end

// A file that a drag of the window promised. AppKit asks for it once the drop's target knows where it goes, and the
// window then asks the program (PrAppEventKind_DragPromise).
@interface PrCocoaPromise : NSObject <NSFilePromiseProviderDelegate>
@property(nonatomic, weak) PrCocoaWindow* PR_NULLABLE owner;
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
@property(nonatomic) PrApp* PR_NULLABLE app;
@property(nonatomic) BOOL headless;
@property(nonatomic) BOOL running;
@property(nonatomic) BOOL inCallback;    // a callback is running, and no other may start
@property(nonatomic) uint8_t permission; // notification permission, a CocoaPermission
@property(nonatomic, strong) NSMutableArray<PrCocoaWindow*>* windows;

// Notifications that wait for the user to answer the permission request, oldest first.
@property(nonatomic, strong) NSMutableArray<UNNotificationRequest*>* waiting;

// The file dialogs that are open, by the id of the read that answers each.
@property(nonatomic, strong) NSMutableDictionary<NSNumber*, NSSavePanel*>* dialogs;

// A QuitRequested event is being delivered, and the program cancelled it (prAppQuitCancel).
@property(nonatomic) BOOL quitDelivering;
@property(nonatomic) BOOL quitCancelled;

@property(nonatomic) PrAppPresence presence;
@property(nonatomic, copy) NSString* singleInstance;    // the name of a single instance, or nil
- (void)setPresence:(PrAppPresence)presence;
- (void)deliverKind:(PrAppEventKind)kind;
- (PrAppLifecycle)lifecycle;
- (void)setTimer:(uint64_t)tag delay:(double)delay interval:(double)interval;
- (void)cancelTimer:(uint64_t)tag;
- (BOOL)startServices;
- (void)stopServices;

@property(nonatomic, strong) NSMutableArray<PrCocoaShare*>* shares;     // the share pickers that wait for the user
@property(nonatomic, strong) PrCocoaMenuTree* PR_NULLABLE menuBar;      // the application's menu bar
- (void)showMenuBar;
- (void)closePopupsExcept:(NSWindow* PR_NULLABLE)window;
- (void)setDockProgress:(PrAppProgressState)state value:(float)value;

// The view that a drag of the application's started from, which the host keeps while the system carries the drag, as
// its window may close first.
@property(nonatomic, strong) PrCocoaView* PR_NULLABLE dragSource;

- (void)deliverHotkey:(uint32_t)id;
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window;
- (uint32_t)holdEvents;
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow*)window ahead:(uint32_t)mark;
- (void)beginCallback;
- (void)endCallback;
- (void)forget:(PrAppWindow*)window;
- (void)stop;
- (PrStatus)postNotification:(const PrAppNotificationDesc*)desc;

// Delivers `answer`, an AppAnswer that the host owns from here on, now or once no callback runs.
- (void)answer:(void*)answer;
@end

namespace pr_impl {

// The mark of a key event that the host delivers at once, rather than ahead of held events.
constexpr uint32_t kNoMark = UINT32_MAX;

// What a key that is down reports, so that the window can release it when it stops being the key window.
struct CocoaHeldKey {
    bool down;
    PrAppKey key;
};

// The pointer buttons that the view can report down, by PrAppPointerEvent::button.
constexpr uint8_t kPointerButtons = 32;

// The pointer id of a pen is this plus its tablet's device id, so that it never equals kPrAppPointerMouse.
constexpr uint32_t kPenPointerBase = 2;

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

// The application's state, after its function table (pr/app_backend.h).
struct CocoaApp : PrApp {
    void* host;                 // the PrCocoaHost, retained
    PrAppHandler handler;       // the handler of the run
    PrAppWindowModel windowModel;
    Atomic<bool> wakePending;
    AppDataSources sources;
    AppDataRequests requests;
    CocoaDrag drag;

    // The displays that queries reported, with their names, which stay valid until the next DisplaysChanged event.
    mutable AppDisplayList displays;

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

// A window's state, after its function table (pr/app_backend.h).
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

// A key of a Mac's keyboard: its virtual key code (kVK_ in HIToolbox's Events.h), its position, and for a key that
// types no character, its name. A key that types a character takes its value from the character.
struct CocoaMacKey {
    uint16_t virtualKey;
    PrAppKeyCode code;
    PrAppKey key = PrAppKey_Unknown;
};

// Returns the host of the application, and the object of a window.
PrCocoaHost* cocoaHostOf(const PrApp* app);
PrCocoaWindow* cocoaObjectOf(const PrAppWindow* window);

// Returns the seconds since the system started, the clock of AppKit's events.
double cocoaUptime();

// Returns true if the process runs from an application bundle. In any other process, the notification center raises an
// exception, even if the executable embeds an Info.plist with a bundle identifier. The older NSUserNotificationCenter
// has no default center in such a process.
bool cocoaIsBundled();

// Calls the window's handler with a frame and applies the output. A window that the program destroyed during the call
// is freed.
void cocoaDeliverFrame(PrAppWindow* window, const PrAppFrame& frame);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
