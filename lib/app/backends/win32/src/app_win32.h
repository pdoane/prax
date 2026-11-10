// The Win32 application backend's state: the application, its windows, and what their parts share. Each part of the
// backend has a source and an internal header of its own.

#pragma once

#include "pr/alloc.h"
#include "pr/app_backend.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/count_of.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/random.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/vec.h"
#include "pr/windows_string.h"

#include "app/backends/shared/src/app_shared_data.h"
#include "app/backends/shared/src/app_shared_delivery.h"
#include "app/backends/shared/src/app_shared_displays.h"
#include "app/backends/shared/src/app_shared_drag.h"
#include "app/backends/shared/src/app_shared_events.h"
#include "app/backends/shared/src/app_shared_title_bar.h"

// windows.h without its min and max macros, and without the parts that the backend does not use.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <imm.h>
#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wtsapi32.h>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The messages that the backend posts to the application's message window.
constexpr UINT kMessageFrame = WM_APP + 0;      // the pacer saw the display refresh
constexpr UINT kMessageWake = WM_APP + 1;
constexpr UINT kMessageNotifyIcon = WM_APP + 2; // the user acted on the icon in the notification area

// The program started a request of a source's content or of a file. `lParam` is the request (Win32Request).
constexpr UINT kMessageRead = WM_APP + 3;

// The displays changed, as one of the windows heard. The message window delivers one DisplaysChanged event for the
// messages that every window receives.
constexpr UINT kMessageDisplaysChanged = WM_APP + 4;

// A window's drag waits for the window to start it, once no callback runs (prAppWindowDragStart).
constexpr UINT kMessageDragStart = WM_APP + 5;

// A file dialog waits to show, once no callback runs, as its own modal loop runs until the user answers. `lParam` is
// the dialog's request (Win32DialogRequest).
constexpr UINT kMessageDialog = WM_APP + 6;

// The program cancelled reads, which may include the read of the dialog that shows.
constexpr UINT kMessageDialogCancel = WM_APP + 7;

// A hidden window waits to show, once no callback runs, after a frame that draws what it shows.
constexpr UINT kMessageShow = WM_APP + 8;

// A context menu waits to show, once no callback runs (prAppWindowShowMenu).
constexpr UINT kMessageShowMenu = WM_APP + 9;

// The user acted on a status item's icon. The high word of lParam is the item's id, and the low word the action.
constexpr UINT kMessageStatusItem = WM_APP + 10;

// The identifier of the application's icon in the notification area. The status items' identifiers follow it.
constexpr UINT kNotifyIconId = 1;

// The message window's timers: the poll of the memory's state, and the program's timers, whose ids start past it.
constexpr UINT_PTR kTimerLowMemory = 1;
constexpr UINT kLowMemoryMilliseconds = 5000;
constexpr UINT_PTR kFirstProgramTimer = 16;

// The copy-data message of a second launch of a single instance, whose data is its command line in UTF-16.
constexpr ULONG_PTR kCopyDataOpen = 0x50724F70;

// The timer that times the frames of a window that is not composed, and its period.
constexpr UINT_PTR kTimerFrame = 1;
constexpr UINT_PTR kTimerFrameAt = 2;   // the time that an output names
constexpr UINT kHiddenFrameMilliseconds = 16;

// How often the display's headroom is read again, in seconds, while frames run. The headroom changes with the SDR
// brightness that the user sets, which Windows announces to no window.
constexpr double kDisplayRefreshSeconds = 1.0;

// The luminance of SDR white in nits that Windows assumes when it reports none.
constexpr float kDefaultSdrWhiteNits = 80.0f;

// ToUnicodeEx's flag that leaves the keyboard's dead-key state unchanged (Windows 10, version 1607).
constexpr UINT kToUnicodeKeepState = 1u << 2;

// The classes of the application's windows, of its message window, and of the dim over a modal dialog's parent.
constexpr wchar_t kClassName[] = L"PrAppWin32";
constexpr wchar_t kMessageClassName[] = L"PrAppWin32Message";
constexpr wchar_t kDimClassName[] = L"PrAppWin32Dim";

// How dark the dim over a modal dialog's parent is, as the alpha of its black, and the radius of a window's corners in
// logical pixels, which the dim matches.
constexpr BYTE kDimAlpha = 0x60;
constexpr int kCornerRadius = 8;
constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

// What a key types, which the message loop reads before TranslateMessage records a dead key that the key begins. Read
// after that, a dead key would combine with itself.
struct Win32KeyText {
    UINT message;
    WPARAM virtualKey;
    LPARAM flags;
    bool dead;                  // the key is a dead key, which types nothing until the key it combines with
    int count;                  // UTF-16 units
    wchar_t units[8];
};

struct Win32Window;
struct Win32App;

// The drag that a window of the application started, while the system carries it. The data object offers its content
// and its token. A window of this process that finds the token knows that the drag is this one.
struct Win32Drag {
    bool active;
    uint64_t token;
    Win32Window* PR_NULLABLE window;
    AppDataStore content;
    PrAppDragOperationFlags operations;
};

// A copy of a menu's description, which the application keeps to show its menu bar on the windows that open later.
// `strings` holds the menu's title and then its items' titles, which `menu` and `items` view.
struct Win32MenuCopy {
    PrAppMenu menu;
    RawArray<PrAppMenuItem> items;
    RawArray<Win32MenuCopy*> submenus;
    AppStrings strings;
};

// A menu that a description made, and its tag, which WM_INITMENUPOPUP reports as MenuOpening.
struct Win32Menu {
    HMENU handle;
    uint64_t tag;
};

// A command of a menu: its id in WM_COMMAND, and its item's tag and role.
struct Win32Command {
    UINT id;
    uint64_t tag;
    PrAppMenuRole role;
};

// The menus and the commands that one description made. Setting the description again fills the menus of the same tags
// in place, so that a menu that is open shows the new items. `accelerators` are the items' keys.
struct Win32MenuTree {
    HMENU PR_NULLABLE root;
    bool bar;
    RawArray<Win32Menu> menus;
    RawArray<Win32Command> commands;
    HACCEL PR_NULLABLE accelerators;
};

// A status item: its identifier in the notification area, its tag, its icon, and the menu that a click opens, if any.
struct Win32StatusItem : PrAppStatusItem {
    Win32App* app;
    UINT id;
    uint64_t tag;
    HICON PR_NULLABLE icon;
    bool hasMenu;
    Win32MenuTree menu;
};

// A timer of the program: its tag, the id of the message window's timer, and the interval of its repeats, or zero.
// `repeating` is true once the first firing has set the timer to its interval.
struct Win32Timer {
    uint64_t tag;
    UINT_PTR id;
    double interval;
    bool repeating;
};

// A global hotkey that the application registered: the program's tag, and the id that RegisterHotKey took.
struct Win32Hotkey {
    uint64_t tag;
    int id;
};

// A file dialog that waits to show: the read that its answer completes, and copies of its description. `strings` holds
// the title, the directory, the name, and then each file type's name and its pattern, each ended by a NUL, from the
// offsets of `starts`.
struct Win32DialogRequest {
    uint32_t id;
    PrAppFileDialogKind kind;
    PrAppFileDialogFlags flags;
    HWND PR_NULLABLE parent;
    RawArray<wchar_t> strings;
    RawArray<uint32_t> starts;
};

// What a key that is down reports, so that the window can release it when it loses the keyboard.
struct Win32HeldKey {
    bool down;
    PrAppKey key;
};

// The pointer buttons that a window can report down, by PrAppPointerEvent::button.
constexpr uint8_t kPointerButtons = 5;

// The application's state, after its function table (pr/app_backend.h).
struct Win32App : PrApp {
    PrAppHandler handler;       // the handler of the run
    AppDataSources sources;
    AppDataRequests requests;

    bool headless;
    bool running;
    bool inCallback;            // a callback is running, and no other may start
    bool frameDue;              // the pacer's refresh came, and the requested frames run once no other message waits

    // The hidden window that receives the application's own messages: its wakes, the pacer's refreshes, the
    // notification area's actions and the system's broadcasts. It owns the clipboard's data and the icons in the
    // notification area.
    HWND messageWindow;
    bool displaysChanged;       // a kMessageDisplaysChanged is posted and not yet delivered

    // The displays that queries reported, with their names, which stay valid until the next DisplaysChanged event.
    mutable AppDisplayList displays;

    // The windows, in the order they opened, and the one that was last active, which a click on the icon in the
    // notification area brings forward.
    RawArray<Win32Window*> windows;
    Win32Window* PR_NULLABLE lastActive;

    // What the key that the message loop is dispatching types (Win32KeyText).
    Win32KeyText keyText;

    // The thread that waits for the display's refresh after a frame is requested, and the event that it waits on.
    PrThread pacer;
    HANDLE pacerRequest;
    Atomic<bool> pacerQuit;

    // The window that prAppWake posts to, read from any thread, or null outside a run.
    Atomic<HWND> wakeWindow;
    Atomic<bool> wakePending;

    // The events that arrived during the running callback, in order. A system call that a callback makes may dispatch
    // messages, and the events that they deliver wait here, so that no callback nests in another.
    AppEventQueue held;

    // Scratch storage for conversions, which queries use too.
    mutable RawArray<char> utf8;
    RawArray<wchar_t> utf16;

    // Drags. `ole` is true once OLE runs on the thread, which drags need. `payloadFormat` is the registered clipboard
    // format of a drag's token. `dropHelper` shows the image of a drag over the windows, or is null.
    bool ole;
    UINT payloadFormat;
    UINT htmlFormat;                    // the clipboard's HTML, as CF_HTML ("HTML Format") holds it
    Win32Drag drag;
    IDropTargetHelper* PR_NULLABLE dropHelper;

    // Notifications. `notifyIcon` is true once the application's icon is in the notification area. `notificationTag` is
    // the tag of the last notification, whose balloon replaced any earlier one. `taskbarCreated` is the shell's message
    // that the taskbar was recreated, as when Explorer restarts, which means the icon is gone.
    bool notifyIcon;
    uint64_t notificationTag;
    UINT taskbarCreated;

    // The global hotkeys, which the message window receives.
    RawArray<Win32Hotkey> hotkeys;
    int lastHotkeyId;

    // The file dialog that shows, the id of its read, and the window that it belongs to, or null.
    IFileDialog* PR_NULLABLE dialog;
    uint32_t dialogRead;
    HWND PR_NULLABLE dialogParent;

    bool hidden;                // the application hid its windows (prAppSetHidden)

    uint64_t lastSerial;        // the serial of the window made last (Win32Window::serial)

    PrAppPresence presence;
    PrAppWindowModel windowModel;
    PrAppLifecycle lifecycle;   // the lifecycle that the last LifecycleChanged event reported

    RawArray<Win32Timer> timers;
    UINT_PTR lastTimerId;

    // A QuitRequested event is being delivered, and the program cancelled it (prAppQuitCancel). `quitAccepted` is true
    // if the program let the last QuitRequested event stop the run.
    bool quitDelivering;
    bool quitCancelled;
    bool quitAccepted;

    // The label of each key code in the current layout, computed on first use (prAppKeyCodeLabel), which a query of the
    // application fills. An empty label that is computed means the key's name.
    mutable PrRawString keyLabels[PrAppKeyCode_Count];
    mutable bool keyLabeled[PrAppKeyCode_Count];

    // The memory's state, which a timer polls, since Windows sends no message as memory runs low.
    HANDLE PR_NULLABLE lowMemory;
    bool memoryLow;

    // The taskbar, which shows each window's progress and the application's badge, once a window's button exists.
    ITaskbarList3* PR_NULLABLE taskbar;
    UINT taskbarButtonCreated;
    HICON PR_NULLABLE badge;
    RawArray<wchar_t> badgeText;        // the badge's text, NUL-terminated, for the overlay's description

    // Menus: the description of the application's menu bar, which each Normal window without its own shows, and the
    // last id of a command, which WM_COMMAND carries in 16 bits.
    Win32MenuCopy* PR_NULLABLE menuBar;
    UINT lastCommandId;
    RawArray<Win32StatusItem*> statusItems;
    UINT lastStatusItemId;

    // A single instance: its name, which titles the message window so that a second launch finds it, and the mutex that
    // the first launch holds.
    RawArray<wchar_t> instanceName;
    HANDLE PR_NULLABLE instanceMutex;
};

// A window's state, after its function table (pr/app_backend.h).
struct Win32Window : PrAppWindow {
    Win32App* app;

    // The window's number among those that the application made, which no other window ever has. A callback may free
    // the window and another may take its memory, so code that holds a window across a callback checks its serial.
    uint64_t serial;
    AppWindowDelivery delivery;
    HWND window;
    PrAppWindowRole role;
    wchar_t* title;             // NUL-terminated
    PrRawString titleText;      // the same title, in UTF-8
    PrUVec2 extent;             // the extent that the window opens at, in logical pixels

    // The least and the greatest extent of the content, in logical pixels, or zero for no limit.
    PrUVec2 minExtent;
    PrUVec2 maxExtent;

    // True while the window covers its display, and where it stood before.
    bool fullscreen;
    WINDOWPLACEMENT restore;

    // A custom title bar (PrAppWindowDecoration_Custom): the regions of its title bar, as the last frame gave them, and
    // the button whose press waits for its release, as HTMINBUTTON, HTMAXBUTTON or HTCLOSE, or HTNOWHERE for none.
    bool customTitleBar;
    AppTitleBarRegions titleBar;
    LRESULT pressedButton;

    // The text being edited, as the window's latest output reported it: the caret, in logical px of `textInputWindow`,
    // which is the window itself or a window that it owns, or null if the output has none. `textInputRect` is where the
    // caret of the text being edited in this window stands, from this window's output or its owner's.
    PrAppWindow* PR_NULLABLE textInputWindow;
    PrAppTextInputKind textInputKind;
    PrRect textInput;
    PrRect textInputRect;
    bool frameRequested;
    bool framed;                // a frame ran
    bool inSizeMove;            // the user moves or resizes the window, in the system's modal loop
    bool composing;             // an input method composes text
    double lastFrame;

    // The last press, which the next continues as a multi-click by the system's settings.
    double clickTime;
    POINT clickPoint;
    uint8_t clickButton;
    uint32_t clicks;

    PrAppProgressState progressState;   // the progress that the taskbar button shows
    float progress;

    // The window that a Dialog or a Popup belongs to, or null, and whether a Dialog blocks the parent's input.
    Win32Window* PR_NULLABLE parent;
    bool modal;
    bool blocking;                      // the modal dialog shows, and its parent is disabled
    bool sheet;                         // a modal dialog with decoration, which has no title bar
    HWND PR_NULLABLE dim;               // the window that dims this one while one of its modal dialogs shows
    PrAppPopupPlacement popup;

    DWORD style;                        // the window's style, as its role and decoration choose it

    // The window's menu bar: its own, if `ownMenuBar`, or else the application's. The context menu that waits to show,
    // and where.
    Win32MenuTree menuBar;
    bool ownMenuBar;
    Win32MenuTree contextMenu;
    bool contextPending;
    PrVec2 contextAt;

    bool rawInput;                      // the window receives the mouse's raw input, for LockedRaw
    float aspectRatio;                  // the content's width over its height while the user resizes, or zero
    HICON PR_NULLABLE icon;             // the program's icon, or null for the application's
    HIMC PR_NULLABLE inputContext;      // the input method's context, set aside while a password is edited

    PrAppCursor shape;          // the pointer's shape over the client area
    HCURSOR PR_NULLABLE cursor; // the system's cursor for `shape`, or null to hide the pointer
    UINT dpi;
    wchar_t highSurrogate;      // the first half of a character that WM_CHAR splits, or zero
    PrAppUserPreferences preferences;

    // The display that shows the window, as it was last read, and the DXGI objects that it was read from.
    PrAppDisplay display;
    HMONITOR monitor;
    double displayRead;
    IDXGIFactory1* factory;
    IDXGIOutput6* output;

    // The pointer buttons that are down, by bit of PrAppPointerEvent::button, where the pointer last was, and the keys
    // that are down, by their position. The window releases them when it loses the pointer's capture or the keyboard,
    // which then reports no release of its own.
    uint8_t buttons;
    PrVec2 pointer;
    Win32HeldKey keys[PrAppKeyCode_Count];

    PrAppDragOperation dropOperation;   // the answer to a drag over the window, from the latest output
    AppDragPending drag;                // the drag that prAppWindowDragStart asked for

    bool overlay;               // the window is an overlay (PrAppWindowRole_Overlay)

    // The window's opacity, and whether the pointer passes through it. Either makes the window layered.
    float opacity;
    bool passthrough;

    // The backdrop that the program set, and the one that the window shows, which is Opaque where Windows refuses it or
    // the window is not transparent (PrAppWindowDesc::transparent).
    PrAppWindowBackdrop backdrop;
    PrAppWindowBackdrop shownBackdrop;
    bool transparent;
    bool shown;                 // the program shows the window (prAppWindowSetVisible)
    bool showing;               // the window shows once its next frame is drawn
    bool hiddenByApp;           // the application hid the window, and shows it again (prAppSetHidden)

    // The pointer over the window: whether it is over the content, which a custom title bar is part of, and whether the
    // system tracks its leaving of the client area and of the frame. `leaving` is true while it is outside with a
    // button down, and leaves once the buttons are up.
    bool inside;
    bool leaving;
    bool trackingClient;
    bool trackingFrame;

    // The pointer is locked at `lockedAt`, in logical px, which is `lockedScreen` on the screen, where the window keeps
    // the cursor.
    bool locked;
    PrVec2 lockedAt;
    POINT lockedScreen;
    PrAppPointerMode pointerMode;       // the mode that the program set, which holds while the window is active
};

// A monitor as the backend places windows on it: its bounds and its work area in physical pixels on the virtual screen,
// and its scale.
struct Win32Monitor {
    HMONITOR handle;
    RECT bounds;
    RECT work;
    UINT dpi;
    bool primary;
    PrAppDisplayId id;
    wchar_t device[CCHDEVICENAME];      // the GDI name of the display device, such as \\.\DISPLAY1
};

// A request posts to the message window, whose procedure answers it on the application's thread: a read of content with
// a copy of its custom type, or a read or a write of a file. Without a run, the message window and the answer never
// come.
struct Win32Request {
    uint32_t id;
    PrRawString type;
    bool file;
    bool write;
    uint32_t index;
    uint64_t offset;
    uint64_t size;
    RawArray<uint8_t> bytes;
};

// Returns the application that a window belongs to.
Win32App* appOf(const Win32Window* window);

// Returns the time of the monotonic clock that events and frames report, in seconds.
double win32SecondsNow();

// Ends a callback, and delivers the events that arrived during it, and those that arrive during their own delivery, in
// order. The events of a run that stopped are dropped, and so are those of a window that closed (forgetWindow).
void win32EndCallback(Win32App* app);

// Delivers an event to a window, or to the application if `window` is null, or holds it until the running callback
// returns. An event that hands over a source releases its reference once delivered.
void win32Deliver(Win32App* app, Win32Window* PR_NULLABLE window, const PrAppEvent& event);

// Delivers an answer now, or holds it until the running callback returns, as win32Deliver does with an event. The
// application owns `answer` from here on.
void win32AnswerDeliver(Win32App* app, AppAnswer* answer);

// Delivers an event to `window`, as the application's overload does.
void win32Deliver(Win32Window* window, const PrAppEvent& event);

// Drops the held events of a window that is closing, and removes the window from the application.
void win32ForgetWindow(Win32App* app, Win32Window* window);

// Delivers an application event of `kind` that carries no details.
void win32DeliverKind(Win32App* app, PrAppEventKind kind);

// Delivers LifecycleChanged if the application's lifecycle changed since the last one.
void win32LifecycleMayChange(Win32App* app);

// Windows has no hidden application, so the application hides its windows that show, and shows them again.
void win32SetHidden(PrApp* base, bool hidden);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
