// The Win32 application backend: Windows windows and the Win32 message loop.

#include "pr/app_win32.h"

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

#include "app/backends/shared/src/app_shared_data.h"
#include "app/backends/shared/src/app_shared_delivery.h"
#include "app/backends/shared/src/app_shared_drag.h"
#include "app/backends/shared/src/app_shared_title_bar.h"
#include "app/backends/win32/src/win32_text.h"

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

namespace pr_impl {

namespace {

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

constexpr wchar_t kClassName[] = L"PrAppWin32";
constexpr wchar_t kMessageClassName[] = L"PrAppWin32Message";
constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

// What a key types, which the message loop reads before TranslateMessage records a dead key that the key begins. Read
// after that, a dead key would combine with itself.
struct KeyText {
    UINT message;
    WPARAM virtualKey;
    LPARAM flags;
    bool dead;                  // the key is a dead key, which types nothing until the key it combines with
    int count;                  // UTF-16 units
    wchar_t units[8];
};

struct Win32Window;
struct Win32App;

// What answers a request: content from a store, a file's bytes, a write's status, or a dialog's source, which the
// answer holds a reference to.
enum {
    Win32AnswerKind_Store,
    Win32AnswerKind_Bytes,
    Win32AnswerKind_Status,
    Win32AnswerKind_Source,
};
typedef uint8_t Win32AnswerKind;

// The answer to the request `id`, which owns what it answers with. A Store answer answers from `external`, a source's
// store that the request's reference keeps, in place of its own store if it is set.
struct Win32Answer {
    Win32AnswerKind kind;
    uint32_t id;
    PrStatus status;
    AppDataStore store;
    AppDataStore* PR_NULLABLE external;
    RawArray<uint8_t> bytes;
    PrAppDataSource source;
};

Win32Answer* answerOf(Win32AnswerKind kind, uint32_t id, PrStatus status) {
    Win32Answer* answer = allocZeroed<Win32Answer>();
    answer->kind = kind;
    answer->id = id;
    answer->status = status;
    return answer;
}

void answerDestroy(Win32Answer* PR_NULLABLE answer) {
    if (answer == nullptr)
        return;
    appDataStoreReset(answer->store);
    answer->bytes.reset();
    prMemoryFree(answer);
}

// An event or an answer that arrived while a callback ran, which waits for the callback to return. It owns a copy of
// its text, because the sender's text does not outlive the call. An event of a window that closed is dropped.
struct HeldEvent {
    Win32Window* PR_NULLABLE window;    // the target window, or null for the application
    PrAppEvent event;
    RawArray<char> text;                // the event's text
    bool dropped;
    Win32Answer* PR_NULLABLE answer;    // an answer in place of an event
};

// Copies `event` and its text, for delivery to `window` after the running callback.
HeldEvent heldOf(Win32Window* PR_NULLABLE window, const PrAppEvent& event) {
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

// Frees what a held event owns, and releases the source that it handed over.
void heldReset(HeldEvent& held, AppDataSources& sources) {
    held.text.reset();
    bool answer = held.answer != nullptr;
    answerDestroy(held.answer);
    held.answer = nullptr;
    if (!answer)
        appDataSourceRelease(sources, sourceOf(held.event));
}

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

// A command of a menu: its id in WM_COMMAND, its item's tag and role, and the window that it acts on, or null for the
// application.
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
struct HeldKey {
    bool down;
    PrAppKey key;
};

// The pointer buttons that a window can report down, by PrAppPointerEvent::button.
constexpr uint8_t kPointerButtons = 5;

struct Win32App : PrApp {
    PrAppHandler handler;       // the handler of the run
    AppDataSources sources;
    AppDataRequests requests;

    // The latest drop's image, as the bytes of its CF_DIBV5, which a read of the drop's image converts. The drop's
    // other forms are read as it lands, and the image's bytes are kept, because the source may stop providing them once
    // the drag ends.
    bool headless;
    bool running;
    bool inCallback;            // a callback is running, and no other may start
    bool frameDue;              // the pacer's refresh came, and the requested frames run once no other message waits

    // The message-only window that receives the application's own messages: its wakes, the pacer's refreshes and the
    // notification area's actions. It owns the clipboard's data and the icon in the notification area.
    HWND messageWindow;

    // The names of the displays, by id, which a query of the displays fills and which stay valid until the next
    // DisplaysChanged event.
    mutable RawArray<PrAppDisplayId> displayNameIds;
    mutable RawArray<char*> displayNames;   // each NUL-terminated, in memory of its own

    // The windows, in the order they opened, and the one that was last active, which a click on the icon in the
    // notification area brings forward.
    RawArray<Win32Window*> windows;
    Win32Window* PR_NULLABLE lastActive;

    // What the key that the message loop is dispatching types (KeyText).
    KeyText keyText;

    // The thread that waits for the display's refresh after a frame is requested, and the event that it waits on.
    PrThread pacer;
    HANDLE pacerRequest;
    Atomic<bool> pacerQuit;

    // The window that prAppWake posts to, read from any thread, or null outside a run.
    Atomic<HWND> wakeWindow;
    Atomic<bool> wakePending;

    // The events that arrived during the running callback, in order. A system call that a callback makes may dispatch
    // messages, and the events that they deliver wait here, so that no callback nests in another.
    RawArray<HeldEvent> held;

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
    bool displaysChanged;       // a kMessageDisplaysChanged is posted and not yet delivered
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

    // The session asks to end (WM_QUERYENDSESSION), and the program has not answered. The answer that came during the
    // QuitRequested event, if any.
    bool quitPending;
    bool quitAnswered;
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
    HeldKey keys[PrAppKeyCode_Count];

    PrAppDragOperation dropOperation;   // the answer to a drag over the window, from the latest output
    AppDragPending drag;                // the drag that prAppWindowDragStart asked for

    bool overlay;               // the window is an overlay (PrAppWindowRole_Overlay)
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

Win32App* appOf(const Win32Window* window) {
    return window->app;
}

// The window's style. The title bar's minimize and maximize buttons set a least width that no window message lowers, so
// a window that is never shown has none, and opens at any extent.
DWORD styleOf(const Win32App* app) {
    return app->headless ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME : WS_OVERLAPPEDWINDOW;
}

// Returns true if the window's limits allow one extent, which leaves it no resize border and no maximize button.
bool isFixed(const Win32Window* window) {
    return window->maxExtent.x != 0 && window->maxExtent.y != 0 && window->minExtent.x == window->maxExtent.x &&
        window->minExtent.y == window->maxExtent.y;
}

// The style of one window: the application's, without resizing if its extent is fixed, or a popup while it is
// fullscreen or if it is an overlay.
DWORD windowStyle(const Win32Window* window) {
    if (window->fullscreen)
        return WS_POPUP | WS_VISIBLE;
    if (window->overlay)
        return WS_POPUP | DWORD(GetWindowLongPtrW(window->window, GWL_STYLE) & WS_VISIBLE);
    DWORD style = window->style;
    if (isFixed(window))
        style &= ~DWORD(WS_THICKFRAME | WS_MAXIMIZEBOX);
    return style | (GetWindowLongPtrW(window->window, GWL_STYLE) & WS_VISIBLE);
}

// Returns `extent` within the window's limits.
PrUVec2 limitedExtent(const Win32Window* window, PrUVec2 extent) {
    PrUVec2 limited = extent;
    limited.x = limited.x > window->minExtent.x ? limited.x : window->minExtent.x;
    limited.y = limited.y > window->minExtent.y ? limited.y : window->minExtent.y;
    if (window->maxExtent.x != 0 && limited.x > window->maxExtent.x)
        limited.x = window->maxExtent.x;
    if (window->maxExtent.y != 0 && limited.y > window->maxExtent.y)
        limited.y = window->maxExtent.y;
    return limited;
}

// Returns the outer size of a window whose content is `extent` logical pixels.
POINT frameSizeOf(const Win32Window* window, PrUVec2 extent, UINT dpi) {
    RECT frame{0, 0, MulDiv(int(extent.x), int(dpi), 96), MulDiv(int(extent.y), int(dpi), 96)};
    AdjustWindowRectExForDpi(&frame, windowStyle(window), FALSE, 0, dpi);
    return POINT{frame.right - frame.left, frame.bottom - frame.top};
}

double secondsNow() {
    return prTimeMonotonicMilliseconds() / 1e3;
}

bool isDown(int virtualKey) {
    return (GetKeyState(virtualKey) & 0x8000) != 0;
}

PrAppModifierFlags modifiersNow() {
    PrAppModifierFlags modifiers = 0;
    if ((GetKeyState(VK_CAPITAL) & 1) != 0)
        modifiers |= PrAppModifierFlag_CapsLock;
    if (isDown(VK_SHIFT))
        modifiers |= PrAppModifierFlag_Shift;
    if (isDown(VK_CONTROL))
        modifiers |= PrAppModifierFlag_Control;
    if (isDown(VK_MENU))
        modifiers |= PrAppModifierFlag_Alt;
    if (isDown(VK_LWIN) || isDown(VK_RWIN))
        modifiers |= PrAppModifierFlag_Super;
    return modifiers;
}

// A key's position by its scan code in set 1, with 0xE000 added for a key that Windows reports as extended.
struct ScanKey {
    uint16_t scan;
    PrAppKeyCode code;
};

constexpr ScanKey kScanKeys[] = {
    {0x01, PrAppKeyCode_Escape}, {0x02, PrAppKeyCode_Digit1}, {0x03, PrAppKeyCode_Digit2},
    {0x04, PrAppKeyCode_Digit3}, {0x05, PrAppKeyCode_Digit4}, {0x06, PrAppKeyCode_Digit5},
    {0x07, PrAppKeyCode_Digit6}, {0x08, PrAppKeyCode_Digit7}, {0x09, PrAppKeyCode_Digit8},
    {0x0A, PrAppKeyCode_Digit9}, {0x0B, PrAppKeyCode_Digit0}, {0x0C, PrAppKeyCode_Minus}, {0x0D, PrAppKeyCode_Equal},
    {0x0E, PrAppKeyCode_Backspace}, {0x0F, PrAppKeyCode_Tab}, {0x10, PrAppKeyCode_KeyQ}, {0x11, PrAppKeyCode_KeyW},
    {0x12, PrAppKeyCode_KeyE}, {0x13, PrAppKeyCode_KeyR}, {0x14, PrAppKeyCode_KeyT}, {0x15, PrAppKeyCode_KeyY},
    {0x16, PrAppKeyCode_KeyU}, {0x17, PrAppKeyCode_KeyI}, {0x18, PrAppKeyCode_KeyO}, {0x19, PrAppKeyCode_KeyP},
    {0x1A, PrAppKeyCode_BracketLeft}, {0x1B, PrAppKeyCode_BracketRight}, {0x1C, PrAppKeyCode_Enter},
    {0x1D, PrAppKeyCode_ControlLeft}, {0x1E, PrAppKeyCode_KeyA}, {0x1F, PrAppKeyCode_KeyS},
    {0x20, PrAppKeyCode_KeyD}, {0x21, PrAppKeyCode_KeyF}, {0x22, PrAppKeyCode_KeyG}, {0x23, PrAppKeyCode_KeyH},
    {0x24, PrAppKeyCode_KeyJ}, {0x25, PrAppKeyCode_KeyK}, {0x26, PrAppKeyCode_KeyL}, {0x27, PrAppKeyCode_Semicolon},
    {0x28, PrAppKeyCode_Quote}, {0x29, PrAppKeyCode_Backquote}, {0x2A, PrAppKeyCode_ShiftLeft},
    {0x2B, PrAppKeyCode_Backslash}, {0x2C, PrAppKeyCode_KeyZ}, {0x2D, PrAppKeyCode_KeyX}, {0x2E, PrAppKeyCode_KeyC},
    {0x2F, PrAppKeyCode_KeyV}, {0x30, PrAppKeyCode_KeyB}, {0x31, PrAppKeyCode_KeyN}, {0x32, PrAppKeyCode_KeyM},
    {0x33, PrAppKeyCode_Comma}, {0x34, PrAppKeyCode_Period}, {0x35, PrAppKeyCode_Slash},
    {0x36, PrAppKeyCode_ShiftRight}, {0x37, PrAppKeyCode_NumpadMultiply}, {0x38, PrAppKeyCode_AltLeft},
    {0x39, PrAppKeyCode_Space}, {0x3A, PrAppKeyCode_CapsLock}, {0x3B, PrAppKeyCode_F1}, {0x3C, PrAppKeyCode_F2},
    {0x3D, PrAppKeyCode_F3}, {0x3E, PrAppKeyCode_F4}, {0x3F, PrAppKeyCode_F5}, {0x40, PrAppKeyCode_F6},
    {0x41, PrAppKeyCode_F7}, {0x42, PrAppKeyCode_F8}, {0x43, PrAppKeyCode_F9}, {0x44, PrAppKeyCode_F10},
    {0x45, PrAppKeyCode_Pause}, {0x46, PrAppKeyCode_ScrollLock}, {0x47, PrAppKeyCode_Numpad7},
    {0x48, PrAppKeyCode_Numpad8}, {0x49, PrAppKeyCode_Numpad9}, {0x4A, PrAppKeyCode_NumpadSubtract},
    {0x4B, PrAppKeyCode_Numpad4}, {0x4C, PrAppKeyCode_Numpad5}, {0x4D, PrAppKeyCode_Numpad6},
    {0x4E, PrAppKeyCode_NumpadAdd}, {0x4F, PrAppKeyCode_Numpad1}, {0x50, PrAppKeyCode_Numpad2},
    {0x51, PrAppKeyCode_Numpad3}, {0x52, PrAppKeyCode_Numpad0}, {0x53, PrAppKeyCode_NumpadDecimal},
    {0x54, PrAppKeyCode_PrintScreen}, {0x56, PrAppKeyCode_IntlBackslash}, {0x57, PrAppKeyCode_F11},
    {0x58, PrAppKeyCode_F12}, {0x59, PrAppKeyCode_NumpadEqual}, {0x64, PrAppKeyCode_F13}, {0x65, PrAppKeyCode_F14},
    {0x66, PrAppKeyCode_F15}, {0x67, PrAppKeyCode_F16}, {0x68, PrAppKeyCode_F17}, {0x69, PrAppKeyCode_F18},
    {0x6A, PrAppKeyCode_F19}, {0x6B, PrAppKeyCode_F20}, {0x71, PrAppKeyCode_Lang2}, {0x72, PrAppKeyCode_Lang1},
    {0x73, PrAppKeyCode_IntlRo}, {0x7D, PrAppKeyCode_IntlYen}, {0x7E, PrAppKeyCode_NumpadComma},
    {0xE01C, PrAppKeyCode_NumpadEnter}, {0xE01D, PrAppKeyCode_ControlRight}, {0xE035, PrAppKeyCode_NumpadDivide},
    {0xE037, PrAppKeyCode_PrintScreen}, {0xE038, PrAppKeyCode_AltRight}, {0xE045, PrAppKeyCode_NumLock},
    {0xE046, PrAppKeyCode_Pause}, {0xE047, PrAppKeyCode_Home}, {0xE048, PrAppKeyCode_ArrowUp},
    {0xE049, PrAppKeyCode_PageUp}, {0xE04B, PrAppKeyCode_ArrowLeft}, {0xE04D, PrAppKeyCode_ArrowRight},
    {0xE04F, PrAppKeyCode_End}, {0xE050, PrAppKeyCode_ArrowDown}, {0xE051, PrAppKeyCode_PageDown},
    {0xE052, PrAppKeyCode_Insert}, {0xE053, PrAppKeyCode_Delete}, {0xE05B, PrAppKeyCode_MetaLeft},
    {0xE05C, PrAppKeyCode_MetaRight}, {0xE05D, PrAppKeyCode_ContextMenu},
};

PrAppKeyCode codeOf(uint16_t scan) {
    for (const ScanKey& key : kScanKeys) {
        if (key.scan == scan)
            return key.code;
    }
    return PrAppKeyCode_Unidentified;
}

// The scan code of a key message, as kScanKeys numbers it. A message that carries none, as one that a program
// synthesizes may, takes the scan code that the layout gives its virtual key.
uint16_t scanOf(WPARAM virtualKey, LPARAM flags) {
    uint16_t scan = uint16_t((flags >> 16) & 0xFF);
    if (scan == 0)
        return uint16_t(MapVirtualKeyW(UINT(virtualKey), MAPVK_VK_TO_VSC_EX));
    return (flags & (LPARAM(1) << 24)) != 0 ? uint16_t(0xE000 | scan) : scan;
}

// A key that types no character, by its virtual key.
struct NamedKey {
    uint8_t virtualKey;
    PrAppKey key;
};

constexpr NamedKey kNamedKeys[] = {
    {VK_LEFT, PrAppKey_Left}, {VK_RIGHT, PrAppKey_Right}, {VK_UP, PrAppKey_Up}, {VK_DOWN, PrAppKey_Down},
    {VK_HOME, PrAppKey_Home}, {VK_END, PrAppKey_End}, {VK_PRIOR, PrAppKey_PageUp}, {VK_NEXT, PrAppKey_PageDown},
    {VK_RETURN, PrAppKey_Enter}, {VK_ESCAPE, PrAppKey_Escape}, {VK_BACK, PrAppKey_Backspace},
    {VK_DELETE, PrAppKey_Delete}, {VK_TAB, PrAppKey_Tab}, {VK_INSERT, PrAppKey_Insert}, {VK_SHIFT, PrAppKey_Shift},
    {VK_LSHIFT, PrAppKey_Shift}, {VK_RSHIFT, PrAppKey_Shift}, {VK_CONTROL, PrAppKey_Control},
    {VK_LCONTROL, PrAppKey_Control}, {VK_RCONTROL, PrAppKey_Control}, {VK_MENU, PrAppKey_Alt},
    {VK_LMENU, PrAppKey_Alt}, {VK_RMENU, PrAppKey_Alt}, {VK_LWIN, PrAppKey_Meta}, {VK_RWIN, PrAppKey_Meta},
    {VK_CAPITAL, PrAppKey_CapsLock}, {VK_NUMLOCK, PrAppKey_NumLock}, {VK_APPS, PrAppKey_ContextMenu},
    {VK_HELP, PrAppKey_Help}, {VK_CLEAR, PrAppKey_Clear},
};

PrAppKey namedKeyOf(WPARAM virtualKey) {
    if (virtualKey >= VK_F1 && virtualKey <= VK_F20)
        return PrAppKey(PrAppKey_F1 + (virtualKey - VK_F1));
    for (const NamedKey& named : kNamedKeys) {
        if (named.virtualKey == virtualKey)
            return named.key;
    }
    return PrAppKey_Unknown;
}

// Returns true if `count` UTF-16 units begin with a character that prints: not a control character or delete.
bool isPrintable(const wchar_t* units, int count) {
    return count > 0 && units[0] >= 32 && units[0] != 127 && (units[0] < 0x80 || units[0] > 0x9F);
}

// Returns what a key types with Shift and AltGr applied, and Control and Alt not. Windows reports AltGr as Control with
// the right Alt key. A layout that has no AltGr types nothing with that pair, and then the key types what it types
// without them. The keyboard's dead-key state is left as it was.
KeyText translateKey(UINT message, WPARAM virtualKey, LPARAM flags) {
    KeyText text{};
    text.message = message;
    text.virtualKey = virtualKey;
    text.flags = flags;
    BYTE state[256];
    if (!GetKeyboardState(state))
        return text;
    UINT scan = UINT((flags >> 16) & 0xFF);
    HKL layout = GetKeyboardLayout(0);
    if ((state[VK_RMENU] & 0x80) != 0) {
        text.count = ToUnicodeEx(UINT(virtualKey), scan, state, text.units, int(countOf(text.units)),
            kToUnicodeKeepState, layout);
        if (text.count < 0 || isPrintable(text.units, text.count)) {
            text.dead = text.count < 0;
            text.count = text.dead ? 0 : text.count;
            return text;
        }
    }
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
    state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;
    text.count = ToUnicodeEx(UINT(virtualKey), scan, state, text.units, int(countOf(text.units)), kToUnicodeKeepState, layout);
    text.dead = text.count < 0;
    if (text.dead || !isPrintable(text.units, text.count))
        text.count = 0;
    return text;
}

// The DXGI output that shows `monitor`, or null. The caller releases it.
IDXGIOutput6* PR_NULLABLE outputOf(IDXGIFactory1* factory, HMONITOR monitor) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; SUCCEEDED(factory->EnumAdapters1(a, &adapter)); ++a) {
        IDXGIOutput* output = nullptr;
        IDXGIOutput6* found = nullptr;
        for (UINT o = 0; found == nullptr && SUCCEEDED(adapter->EnumOutputs(o, &output)); ++o) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&found));
            output->Release();
        }
        adapter->Release();
        if (found != nullptr)
            return found;
    }
    return nullptr;
}

// The luminance of SDR white in nits on `monitor`, which the user sets for a display that shows HDR.
float sdrWhiteNits(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (!GetMonitorInfoW(monitor, &info) ||
            GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return kDefaultSdrWhiteNits;
    RawArray<DISPLAYCONFIG_PATH_INFO> paths;
    RawArray<DISPLAYCONFIG_MODE_INFO> modes;
    paths.resize(pathCount);
    modes.resize(modeCount);
    float nits = kDefaultSdrWhiteNits;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data, &modeCount, modes.data, nullptr) ==
            ERROR_SUCCESS) {
        for (uint32_t i = 0; i < pathCount; ++i) {
            const DISPLAYCONFIG_PATH_INFO& path = paths.data[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                    CompareStringOrdinal(source.viewGdiDeviceName, -1, info.szDevice, -1, TRUE) != CSTR_EQUAL)
                continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size = sizeof(white);
            white.header.adapterId = path.targetInfo.adapterId;
            white.header.id = path.targetInfo.id;
            // The level is in thousandths of 80 nits.
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel != 0)
                nits = float(white.SDRWhiteLevel) / 1000.0f * 80.0f;
            break;
        }
    }
    paths.reset();
    modes.reset();
    return nits;
}

// The chromaticities of an RGB space's primaries, red, green then blue, as CIE xy.
struct Primaries {
    PrColorSpace space;
    float xy[6];
};

constexpr Primaries kGamuts[] = {
    {PrColorSpace_Srgb, {0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f}},
    {PrColorSpace_DisplayP3, {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f}},
    {PrColorSpace_Rec2020, {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f}},
};

// The RGB space whose primaries are nearest the display's.
PrColorSpace gamutOf(const DXGI_OUTPUT_DESC1& desc) {
    const float display[6] = {
        desc.RedPrimary[0],   desc.RedPrimary[1],  desc.GreenPrimary[0],
        desc.GreenPrimary[1], desc.BluePrimary[0], desc.BluePrimary[1],
    };
    PrColorSpace nearest = PrColorSpace_Srgb;
    float nearestDistance = 0;
    for (uint32_t g = 0; g < countOf(kGamuts); ++g) {
        float distance = 0;
        for (uint32_t i = 0; i < 6; ++i)
            distance += (display[i] - kGamuts[g].xy[i]) * (display[i] - kGamuts[g].xy[i]);
        if (g == 0 || distance < nearestDistance) {
            nearest = kGamuts[g].space;
            nearestDistance = distance;
        }
    }
    return nearest;
}

DWORD personalizeValue(const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) !=
            ERROR_SUCCESS)
        return fallback;
    return value;
}

// Waits until the display's next refresh. The compositor reports the time of a refresh and its period, and a waitable
// timer sleeps to the next one. This holds the pace whether or not anything presents, unlike DwmFlush, which returns at
// once when the compositor has nothing to compose, and it needs no DXGI output of the window's display, as
// IDXGIOutput::WaitForVBlank does. The compositor runs at the refresh of the primary display.
void waitForRefresh(HANDLE PR_NULLABLE timer) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    int64_t period = frequency.QuadPart / 60;
    int64_t vblank = now.QuadPart;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod != 0) {
        period = int64_t(timing.qpcRefreshPeriod);
        vblank = int64_t(timing.qpcVBlank);
    }
    // The compositor reports a refresh that may be past or more than one period ahead, and the next refresh is the
    // first on its grid after now.
    int64_t ahead = (vblank - now.QuadPart) % period;
    if (ahead <= 0)
        ahead += period;
    // A due time below zero is relative, in units of 100 ns.
    int64_t units = ahead * 10000000 / frequency.QuadPart;
    LARGE_INTEGER due;
    due.QuadPart = -(units > 1 ? units : 1);
    if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep(DWORD(units / 10000));
}

// Opens the clipboard. Another process may hold it for a moment, so the open is tried a few times.
bool openClipboard(HWND window) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(window))
            return true;
        Sleep(1);
    }
    return false;
}

// Reads a device-independent bitmap of `size` bytes as straight-alpha Rgba8 rows into `out`. Returns its extent, or
// zero for a form that it does not read. It reads 24 bits, and 32 bits with no masks or with the masks of BGRA.
PrUVec2 dibPixels(const uint8_t* bytes, size_t size, RawArray<uint8_t>& out) {
    if (size < sizeof(BITMAPINFOHEADER))
        return PrUVec2{0, 0};
    BITMAPINFOHEADER header;
    prMemCopy(&header, bytes, sizeof(header));
    bool bitfields = header.biCompression == BI_BITFIELDS;
    bool readable = header.biPlanes == 1 && (header.biCompression == BI_RGB || bitfields) &&
        (header.biBitCount == 32 || (header.biBitCount == 24 && !bitfields));
    if (!readable || header.biWidth <= 0 || header.biHeight == 0 || header.biHeight == INT32_MIN)
        return PrUVec2{0, 0};
    size_t width = size_t(header.biWidth);
    size_t height = size_t(header.biHeight < 0 ? -header.biHeight : header.biHeight);
    size_t offset = header.biSize + header.biClrUsed * sizeof(RGBQUAD);
    // A header of the first version keeps its masks after it. The later versions hold them within.
    if (bitfields && header.biSize == sizeof(BITMAPINFOHEADER))
        offset += 3 * sizeof(DWORD);
    size_t stride = ((width * header.biBitCount + 31) / 32) * 4;
    if (offset > size || stride * height > size - offset)
        return PrUVec2{0, 0};
    out.resize(uint32_t(width * height * 4));
    // A 32-bit bitmap with no masks leaves its alpha undefined, and most programs write zero there.
    bool hasAlpha = false;
    if (header.biBitCount == 32) {
        for (size_t i = offset + 3; i < offset + stride * height && !hasAlpha; i += 4)
            hasAlpha = bytes[i] != 0;
    }
    for (size_t y = 0; y < height; ++y) {
        size_t sourceRow = header.biHeight < 0 ? y : height - 1 - y;
        const uint8_t* from = bytes + offset + sourceRow * stride;
        uint8_t* to = out.data + y * width * 4;
        for (size_t x = 0; x < width; ++x) {
            to[x * 4] = from[2];
            to[x * 4 + 1] = from[1];
            to[x * 4 + 2] = from[0];
            to[x * 4 + 3] = hasAlpha ? from[3] : 255;
            from += header.biBitCount / 8;
        }
    }
    return PrUVec2{uint32_t(width), uint32_t(height)};
}

// One of the system's cursors, by its IDC_ name, which is an integer whatever character type the name is spelled in.
template <typename Name>
HCURSOR systemCursor(Name name) {
    return LoadCursorW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(name)));
}

// Returns the system's nearest cursor to `cursor`, or null for none, which hides the pointer over the window. Windows
// has no system cursors for a context menu, a cell, an alias, a copy, a grab or a zoom. A cell shows the crosshair, a
// grab shows the hand, and the others show the arrow.
HCURSOR PR_NULLABLE cursorFor(PrAppCursor cursor) {
    switch (cursor) {
        case PrAppCursor_None: return nullptr;
        case PrAppCursor_Help: return systemCursor(IDC_HELP);
        case PrAppCursor_Hand: return systemCursor(IDC_HAND);
        case PrAppCursor_Progress: return systemCursor(IDC_APPSTARTING);
        case PrAppCursor_Wait: return systemCursor(IDC_WAIT);
        case PrAppCursor_Cell: return systemCursor(IDC_CROSS);
        case PrAppCursor_Crosshair: return systemCursor(IDC_CROSS);
        case PrAppCursor_Text: return systemCursor(IDC_IBEAM);
        case PrAppCursor_VerticalText: return systemCursor(IDC_IBEAM);
        case PrAppCursor_Move: return systemCursor(IDC_SIZEALL);
        case PrAppCursor_NoDrop: return systemCursor(IDC_NO);
        case PrAppCursor_NotAllowed: return systemCursor(IDC_NO);
        case PrAppCursor_Grab: return systemCursor(IDC_HAND);
        case PrAppCursor_Grabbing: return systemCursor(IDC_HAND);
        case PrAppCursor_ResizeN: return systemCursor(IDC_SIZENS);
        case PrAppCursor_ResizeS: return systemCursor(IDC_SIZENS);
        case PrAppCursor_ResizeY: return systemCursor(IDC_SIZENS);
        case PrAppCursor_ResizeRow: return systemCursor(IDC_SIZENS);
        case PrAppCursor_ResizeE: return systemCursor(IDC_SIZEWE);
        case PrAppCursor_ResizeW: return systemCursor(IDC_SIZEWE);
        case PrAppCursor_ResizeX: return systemCursor(IDC_SIZEWE);
        case PrAppCursor_ResizeColumn: return systemCursor(IDC_SIZEWE);
        case PrAppCursor_ResizeNE: return systemCursor(IDC_SIZENESW);
        case PrAppCursor_ResizeSW: return systemCursor(IDC_SIZENESW);
        case PrAppCursor_ResizeNESW: return systemCursor(IDC_SIZENESW);
        case PrAppCursor_ResizeNW: return systemCursor(IDC_SIZENWSE);
        case PrAppCursor_ResizeSE: return systemCursor(IDC_SIZENWSE);
        case PrAppCursor_ResizeNWSE: return systemCursor(IDC_SIZENWSE);
        case PrAppCursor_AllScroll: return systemCursor(IDC_SIZEALL);
        default: return systemCursor(IDC_ARROW);
    }
}

// Returns true if `text` is an absolute URL: a scheme (RFC 3986, section 3.1), a colon, and characters that a URL holds
// unescaped. A scheme of one letter is a drive, and the text a path.
bool isUrl(PrStringView text) {
    size_t colon = 0;
    while (colon < text.size && text.data[colon] != ':')
        ++colon;
    if (colon < 2 || colon == text.size)
        return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = text.data[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool other = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!letter && (i == 0 || !other))
            return false;
    }
    for (size_t i = colon + 1; i < text.size; ++i) {
        unsigned char c = static_cast<unsigned char>(text.data[i]);
        if (c <= 0x20 || c == 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' ||
                c == '{' || c == '|' || c == '}')
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Delivery

void windowDestroy(PrAppWindow* base);
PrAppFrame frameState(PrAppWindow* base);
void requestFrame(PrAppWindow* base);
void applyOutput(Win32Window* window, const PrAppOutput& output);
void forgetTextInput(Win32Window* window);
void startDrag(Win32Window* window);

// Calls the program with an event, as no other callback runs. A window that the program destroyed during the call is
// freed.
void callEvent(Win32App* app, Win32Window* PR_NULLABLE window, const PrAppEvent& event) {
    app->inCallback = true;
    if (window == nullptr)
        app->handler.event(app->handler.context, app, &event);
    else if (!appWindowDeliverEvent(window->delivery, window, &event, frameState))
        windowDestroy(window);
    app->inCallback = false;
}

// Calls the callback of an answer's request, as no other callback runs. A dialog's source loses the answer's reference
// once the callback returns, so that the program keeps it only by retaining it.
void callAnswer(Win32App* app, Win32Answer& answer) {
    app->inCallback = true;
    switch (answer.kind) {
        case Win32AnswerKind_Store:
            appDataRequestsAnswerFromStore(app->requests, answer.id, answer.status,
                answer.external != nullptr ? answer.external : &answer.store);
            break;
        case Win32AnswerKind_Bytes:
            appDataRequestsAnswerBytes(app->requests, answer.id, answer.status,
                PrByteView{answer.bytes.data, answer.bytes.count});
            break;
        case Win32AnswerKind_Status:
            appDataRequestsAnswerStatus(app->requests, answer.id, answer.status);
            break;
        default:
            appDataRequestsAnswerSource(app->requests, answer.id, answer.status, answer.source);
            appDataSourceRelease(app->sources, answer.source);
            answer.source = kPrAppDataSourceNone;
            break;
    }
    app->inCallback = false;
}

// Ends a callback, and delivers the events that arrived during it, and those that arrive during their own delivery, in
// order. The events of a run that stopped are dropped, and so are those of a window that closed (forgetWindow).
void endCallback(Win32App* app) {
    app->inCallback = false;
    for (uint32_t i = 0; i < app->held.count && app->running; ++i) {
        if (app->held.data[i].dropped)
            continue;
        // An event that its delivery holds goes to the end of the array, which may move the array but not the text.
        PrStringView text{
            app->held.data[i].text.data != nullptr ? app->held.data[i].text.data : "",
            app->held.data[i].text.count,
        };
        if (Win32Answer* answer = app->held.data[i].answer) {
            // The array may move as the callback runs, and the answer that it owns stays in place.
            callAnswer(app, *answer);
            continue;
        }
        PrAppEvent event = app->held.data[i].event;
        prAppEventSetText(&event, text);
        callEvent(app, app->held.data[i].window, event);
    }
    for (uint32_t i = 0; i < app->held.count; ++i)
        heldReset(app->held.data[i], app->sources);
    app->held.clear();
}

// Delivers an event to a window, or to the application if `window` is null, or holds it until the running callback
// returns. An event that hands over a source releases its reference once delivered.
void deliver(Win32App* app, Win32Window* PR_NULLABLE window, const PrAppEvent& event) {
    if (app->running && app->inCallback) {
        app->held.push(heldOf(window, event));
        return;
    }
    if (app->running)
        callEvent(app, window, event);
    appDataSourceRelease(app->sources, sourceOf(event));
    if (app->running)
        endCallback(app);
}

// Delivers an answer now, or holds it until the running callback returns, as deliver does with an event. The
// application owns `answer` from here on.
void answerDeliver(Win32App* app, Win32Answer* answer) {
    if (!app->running) {
        appDataSourceRelease(app->sources, answer->source);
        answerDestroy(answer);
        return;
    }
    if (app->inCallback) {
        app->held.push(HeldEvent{.answer = answer});
        return;
    }
    callAnswer(app, *answer);
    answerDestroy(answer);
    endCallback(app);
}

void deliver(Win32Window* window, const PrAppEvent& event) {
    deliver(appOf(window), window, event);
}

// Drops the held events of a window that is closing, and removes the window from the application.
void forgetWindow(Win32App* app, Win32Window* window) {
    // An event stays in place, since the delivery that is under way counts through the array.
    for (uint32_t i = 0; i < app->held.count; ++i) {
        if (app->held.data[i].window == window)
            app->held.data[i].dropped = true;
    }
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i] == window) {
            app->windows.erase(i);
            break;
        }
    }
    if (app->lastActive == window)
        app->lastActive = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Displays and preferences

void releaseDisplayObjects(Win32Window* window) {
    if (window->output != nullptr)
        window->output->Release();
    if (window->factory != nullptr)
        window->factory->Release();
    window->output = nullptr;
    window->factory = nullptr;
    window->monitor = nullptr;
}

// Reads the headroom and gamut of the display that shows the window again, if the window moved to another display, the
// displays changed, or the last read is older than kDisplayRefreshSeconds. DXGI describes an output as it was when its
// factory was created, so a stale factory is replaced. A display shows more than SDR only while Windows composes it in
// HDR, and then its headroom is its peak luminance over SDR white. In SDR, Windows composes in sRGB. Returns the
// headroom and gamut of the display that `output` shows, on `monitor`. A display shows more than SDR only while Windows
// composes it in HDR, and then its headroom is its peak luminance over SDR white. In SDR, Windows composes in sRGB.
PrAppDisplay displayOfOutput(IDXGIOutput6* PR_NULLABLE output, HMONITOR monitor) {
    PrAppDisplay display = prAppDisplayDefault();
    DXGI_OUTPUT_DESC1 desc;
    if (output != nullptr && SUCCEEDED(output->GetDesc1(&desc)) &&
            desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        float headroom = desc.MaxLuminance / sdrWhiteNits(monitor);
        display.headroom = headroom > 1 ? headroom : 1;
        display.potentialHeadroom = display.headroom;
        display.gamut = gamutOf(desc);
    }
    return display;
}

void refreshDisplay(Win32Window* window, double now) {
    HMONITOR monitor = MonitorFromWindow(window->window, MONITOR_DEFAULTTONEAREST);
    if (monitor == window->monitor && now - window->displayRead < kDisplayRefreshSeconds)
        return;
    window->displayRead = now;
    if (window->factory != nullptr && !window->factory->IsCurrent())
        releaseDisplayObjects(window);
    if (monitor != window->monitor) {
        if (window->output != nullptr)
            window->output->Release();
        window->output = nullptr;
        if (window->factory == nullptr &&
            FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&window->factory))))
        window->factory = nullptr;
        if (window->factory != nullptr)
            window->output = outputOf(window->factory, monitor);
        window->monitor = monitor;
    }
    window->display = displayOfOutput(window->output, monitor);
}

// ---------------------------------------------------------------------------------------------------------------------
// The display list

// A monitor as the backend places windows on it: its bounds and its work area in physical pixels on the virtual screen,
// and its scale.
struct Monitor {
    HMONITOR handle;
    RECT bounds;
    RECT work;
    UINT dpi;
    bool primary;
    PrAppDisplayId id;
    wchar_t device[CCHDEVICENAME];      // the GDI name of the display device, such as \\.\DISPLAY1
};

// Returns the id of the monitor whose device is `device`: the hash of the interface name of the display device that it
// shows, which stays the same across connections, or of the device's name if it has none. It is never zero.
PrAppDisplayId monitorIdOf(const wchar_t* device) {
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    const wchar_t* name = device;
    if (EnumDisplayDevicesW(device, 0, &adapter, EDD_GET_DEVICE_INTERFACE_NAME) && adapter.DeviceID[0] != 0)
        name = adapter.DeviceID;
    RawArray<char> utf8;
    PrStringView text = win32Narrow(name, lstrlenW(name), utf8);
    PrAppDisplayId id = prHashBytes(text.data, text.size, 0);
    utf8.reset();
    return id != kPrAppDisplayNone ? id : 1;
}

bool monitorOf(HMONITOR handle, Monitor* out) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info))
        return false;
    UINT x = 96;
    UINT y = 96;
    if (FAILED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &x, &y)) || x == 0)
        x = 96;
    *out = Monitor{
        .handle = handle,
        .bounds = info.rcMonitor,
        .work = info.rcWork,
        .dpi = x,
        .primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0,
        .id = monitorIdOf(info.szDevice),
    };
    wcsncpy_s(out->device, info.szDevice, _TRUNCATE);
    return true;
}

// The monitors, the primary first.
void monitorsOf(RawArray<Monitor>& out) {
    out.clear();
    auto add = [](HMONITOR handle, HDC, LPRECT, LPARAM context) -> BOOL {
        RawArray<Monitor>& monitors = *reinterpret_cast<RawArray<Monitor>*>(context);
        Monitor monitor{};
        if (monitorOf(handle, &monitor)) {
            monitors.push(monitor);
            if (monitor.primary && monitors.count > 1) {
                Monitor first = monitors.data[0];
                monitors.data[0] = monitor;
                monitors.data[monitors.count - 1] = first;
            }
        }
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, add, reinterpret_cast<LPARAM>(&out));
}

// Finds the monitor with `id`, or returns false if none is connected.
bool monitorWithId(PrAppDisplayId id, Monitor* out) {
    RawArray<Monitor> monitors;
    monitorsOf(monitors);
    bool found = false;
    for (uint32_t i = 0; i < monitors.count && !found; ++i) {
        found = monitors.data[i].id == id;
        if (found)
            *out = monitors.data[i];
    }
    monitors.reset();
    return found;
}

bool primaryMonitor(Monitor* out) {
    return monitorOf(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), out);
}

// Returns a rectangle of `monitor`, in physical pixels on the virtual screen, in the monitor's logical pixels from its
// top left.
PrRect logicalOn(const Monitor& monitor, RECT rect) {
    float scale = 96.0f / float(monitor.dpi);
    return PrRect{
        float(rect.left - monitor.bounds.left) * scale,
        float(rect.top - monitor.bounds.top) * scale,
        float(rect.right - monitor.bounds.left) * scale,
        float(rect.bottom - monitor.bounds.top) * scale,
    };
}

// Reads the user's preferences, and matches the window's title bar to the color scheme. A contrast theme is a palette
// of the user's, and its scheme is the one that its window background belongs to.
void refreshPreferences(Win32Window* window) {
    PrAppUserPreferences& preferences = window->preferences;
    preferences = prAppUserPreferencesDefault();
    if (personalizeValue(L"AppsUseLightTheme", 1) == 0)
        preferences.colorScheme = PrAppColorScheme_Dark;
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
            (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0) {
        preferences.contrast = PrAppContrast_More;
        COLORREF background = GetSysColor(COLOR_WINDOW);
        uint32_t luma = 2126u * GetRValue(background) + 7152u * GetGValue(background) + 722u * GetBValue(background);
        preferences.colorScheme = luma < 10000u * 128u ? PrAppColorScheme_Dark : PrAppColorScheme_Light;
    }
    BOOL animation = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0))
        preferences.reducedMotion = !animation;
    preferences.reducedTransparency = personalizeValue(L"EnableTransparency", 1) == 0;
    // The accent is an ABGR value of the DWM's settings. Windows always has one.
    DWORD accent = 0;
    DWORD size = sizeof(accent);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
            nullptr, &accent, &size) == ERROR_SUCCESS)
        preferences.accentColor = PrColor{
            float(accent & 0xFF) / 255.0f, float((accent >> 8) & 0xFF) / 255.0f,
            float((accent >> 16) & 0xFF) / 255.0f, 1,
        };
    // The text size of the Accessibility settings, as a percentage of the default.
    DWORD textScale = 100;
    size = sizeof(textScale);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD,
            nullptr, &textScale, &size) == ERROR_SUCCESS && textScale >= 100)
        preferences.textScale = float(textScale) / 100.0f;
    BOOL dark = preferences.colorScheme == PrAppColorScheme_Dark;
    DwmSetWindowAttribute(window->window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames

// The window's client extent in logical pixels.
PrVec2 extentOf(Win32Window* window) {
    RECT client;
    if (!GetClientRect(window->window, &client))
        return PrVec2{0, 0};
    float scale = 96.0f / float(window->dpi);
    return PrVec2{float(client.right) * scale, float(client.bottom) * scale};
}

// The pacer: each time a frame is requested, it waits for the display's refresh and posts kMessageFrame to the
// application's message window. Waiting on a thread of its own keeps the message loop free for input, and the message
// reaches the application also during a modal loop, which dispatches posted messages as the main loop does.
void pace(void* PR_NULLABLE context) {
    Win32App* app = static_cast<Win32App*>(context);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr)
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    for (;;) {
        WaitForSingleObject(app->pacerRequest, INFINITE);
        if (app->pacerQuit.load())
            break;
        waitForRefresh(timer);
        if (app->pacerQuit.load())
            break;
        PostMessageW(app->messageWindow, kMessageFrame, 0, 0);
    }
    if (timer != nullptr)
        CloseHandle(timer);
}

// Arranges for a window's frame to run. A window that is hidden or minimized is not composed, so its frames are timed
// instead of paced to the display.
void scheduleFrame(Win32Window* window) {
    if (!IsWindowVisible(window->window) || IsIconic(window->window))
        SetTimer(window->window, kTimerFrame, kHiddenFrameMilliseconds, nullptr);
    else
        SetEvent(appOf(window)->pacerRequest);
}

// The window as a frame at `now` finds it, with the display refreshed. The height of the system's title bar at the
// window's scale, in physical pixels: the caption and the resize border above it.
int captionHeight(const Win32Window* window) {
    return GetSystemMetricsForDpi(SM_CYCAPTION, window->dpi) + GetSystemMetricsForDpi(SM_CYFRAME, window->dpi) +
        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, window->dpi);
}

// A custom title bar. Windows draws no buttons in it, so the program draws them, in a band as tall as the system's own
// title bar.
PrAppTitleBar titleBarOf(const Win32Window* window) {
    if (!window->customTitleBar || window->fullscreen)
        return PrAppTitleBar{};
    PrAppTitleBar titleBar{};
    titleBar.height = float(captionHeight(window)) * 96.0f / float(window->dpi);
    titleBar.flags = PrAppTitleBarFlag_ProgramButtons;
    return titleBar;
}

PrAppFrame frameAt(Win32Window* window, double now) {
    PrAppFrame frame{};
    frame.time = now;
    frame.delta = window->framed ? now - window->lastFrame : 0;
    frame.extent = extentOf(window);
    refreshDisplay(window, now);
    frame.display = window->display;
    frame.display.pixelRatio = float(window->dpi) / 96.0f;
    frame.preferences = window->preferences;
    frame.titleBar = titleBarOf(window);
    return frame;
}

void runFrame(Win32Window* window) {
    Win32App* app = appOf(window);
    // A hidden window has no frames, but for the one that it shows with.
    if (!app->running || (!window->shown && !window->showing))
        return;
    if (app->inCallback) {
        // A callback that pumps messages, as some system calls do, cannot run a frame within it, so the frame waits.
        window->frameRequested = true;
        scheduleFrame(window);
        return;
    }
    window->frameRequested = false;
    uint64_t serial = window->serial;
    double now = secondsNow();
    PrAppFrame frame = frameAt(window, now);
    window->framed = true;
    window->lastFrame = now;
    app->inCallback = true;
    PrAppOutput output{};
    bool open = appWindowDeliverFrame(window->delivery, window, &frame, &output);
    if (open)
        applyOutput(window, output);
    else
        windowDestroy(window);
    endCallback(app);
    if (open && isOpen(app, window, serial))
        startDrag(window);
}

// Returns true if `window` is one of the application's open windows, and the window of `serial` rather than one that
// took its memory after it was freed.
bool isOpen(const Win32App* app, const Win32Window* window, uint64_t serial) {
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i] == window)
            return app->windows.data[i]->serial == serial;
    }
    return false;
}

// Runs the frame of each window that requested one when the refresh came. A frame may close another of the windows,
// whose frame then does not run.
void runRequestedFrames(Win32App* app) {
    app->frameDue = false;
    RawArray<Win32Window*> due;
    RawArray<uint64_t> serials;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (window->frameRequested && !window->inSizeMove) {
            due.push(window);
            serials.push(window->serial);
        }
    }
    for (uint32_t i = 0; i < due.count; ++i) {
        if (isOpen(app, due.data[i], serials.data[i]))
            runFrame(due.data[i]);
    }
    due.reset();
    serials.reset();
}

// Returns true if a window of the application requested a frame.
bool anyFrameRequested(const Win32App* app) {
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i]->frameRequested)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Input

PrVec2 positionOf(Win32Window* window, int x, int y) {
    float scale = 96.0f / float(window->dpi);
    return PrVec2{float(x) * scale, float(y) * scale};
}

// Returns true if the key state of a pointer message has a button down.
bool anyButtonDown(WPARAM keyState) {
    WORD buttons = LOWORD(keyState);
    return (buttons & MK_LBUTTON) != 0 || (buttons & MK_RBUTTON) != 0 || (buttons & MK_MBUTTON) != 0 ||
        (buttons & MK_XBUTTON1) != 0 || (buttons & MK_XBUTTON2) != 0;
}

// Returns the number of presses in the multi-click that a press of `button` at `at`, in client pixels, continues or
// starts, by the system's settings.
uint32_t countClick(Win32Window* window, uint8_t button, POINT at, double time) {
    bool continues = window->clicks != 0 && button == window->clickButton &&
        time - window->clickTime <= double(GetDoubleClickTime()) / 1000 &&
        abs(at.x - window->clickPoint.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
        abs(at.y - window->clickPoint.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
    window->clicks = continues ? window->clicks + 1 : 1;
    window->clickTime = time;
    window->clickPoint = at;
    window->clickButton = button;
    return window->clicks;
}

// Answers WM_NCHITTEST for a custom title bar. The system's answer keeps the resize borders at the sides and the
// bottom, and the top border, which the content covers, is answered here. The rest of the content is the program's
// title bar regions.
LRESULT customHitTest(Win32Window* window, WPARAM wParam, LPARAM lParam) {
    LRESULT hit = DefWindowProcW(window->window, WM_NCHITTEST, wParam, lParam);
    if (hit != HTCLIENT)
        return hit;
    POINT at{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
    ScreenToClient(window->window, &at);
    RECT client{};
    GetClientRect(window->window, &client);
    int border =
        GetSystemMetricsForDpi(SM_CYFRAME, window->dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, window->dpi);
    if (at.y < border && !IsZoomed(window->window) && !isFixed(window)) {
        if (at.x < border)
            return HTTOPLEFT;
        return at.x >= client.right - border ? HTTOPRIGHT : HTTOP;
    }
    switch (appTitleBarRegionAt(window->titleBar, positionOf(window, at.x, at.y))) {
        case PrAppTitleBarRegionKind_Drag: return HTCAPTION;
        case PrAppTitleBarRegionKind_Minimize: return HTMINBUTTON;
        case PrAppTitleBarRegionKind_Maximize: return HTMAXBUTTON;
        case PrAppTitleBarRegionKind_Close: return HTCLOSE;
        default: return HTCLIENT;
    }
}

// Acts on the release of a custom title bar's button whose press it saw. The program draws the buttons, so the system's
// handling of a press, which draws the classic buttons, is skipped.
void customButtonReleased(Win32Window* window, LRESULT button) {
    LRESULT pressed = window->pressedButton;
    window->pressedButton = HTNOWHERE;
    if (button != pressed)
        return;
    switch (button) {
        case HTMINBUTTON: ShowWindow(window->window, SW_MINIMIZE); break;
        case HTMAXBUTTON: ShowWindow(window->window, IsZoomed(window->window) ? SW_RESTORE : SW_MAXIMIZE); break;
        case HTCLOSE: PostMessageW(window->window, WM_CLOSE, 0, 0); break;
        default: break;
    }
}

// Asks the system to report the pointer's leaving of the client area, or of the frame, once for each time it comes.
void trackLeave(Win32Window* window, bool frame) {
    bool& tracking = frame ? window->trackingFrame : window->trackingClient;
    if (tracking)
        return;
    TRACKMOUSEEVENT track{
        .cbSize = sizeof(TRACKMOUSEEVENT),
        .dwFlags = DWORD(TME_LEAVE | (frame ? TME_NONCLIENT : 0)),
        .hwndTrack = window->window,
    };
    tracking = TrackMouseEvent(&track) != FALSE;
}

void deliverCrossing(Win32Window* window, PrAppEventKind kind, PrVec2 position) {
    PrAppEvent event{};
    event.kind = kind;
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.pointer.position = position;
    event.pointer.primary = true;
    event.pointer.id = kPrAppPointerMouse;
    deliver(window, event);
}

// Returns true if the pointer is over the window's content: its client area, or a part of its custom title bar.
bool pointerOverContent(Win32Window* window) {
    POINT at{};
    if (!GetCursorPos(&at) || WindowFromPoint(at) != window->window)
        return false;
    LRESULT hit = SendMessageW(window->window, WM_NCHITTEST, 0, MAKELPARAM(uint16_t(at.x), uint16_t(at.y)));
    bool titleBar = hit == HTCAPTION || hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE;
    return hit == HTCLIENT || (window->customTitleBar && titleBar);
}

// Takes the system's report that the pointer left the client area or the frame. The pointer leaves the content only if
// it is over neither, and with a button down, only once the buttons are up.
void pointerLeft(Win32Window* window, bool frame) {
    (frame ? window->trackingFrame : window->trackingClient) = false;
    if (!window->inside || window->locked || pointerOverContent(window))
        return;
    if (window->buttons != 0) {
        window->leaving = true;
        return;
    }
    window->inside = false;
    deliverCrossing(window, PrAppEventKind_PointerLeave, window->pointer);
}

// Locks the pointer to the window where it is, or releases it. The cursor hides, and stays within the window, where
// each move returns it to the lock's place, so that the next move reports how far it went. Starts or stops the mouse's
// raw input to the window, which reports the device's motion without the system's acceleration.
void setRawInput(Win32Window* window, bool raw) {
    if (window->rawInput == raw)
        return;
    window->rawInput = raw;
    RAWINPUTDEVICE device{
        .usUsagePage = 0x01,    // generic desktop controls
        .usUsage = 0x02,        // mouse
        .dwFlags = raw ? 0u : DWORD(RIDEV_REMOVE),
        .hwndTarget = raw ? window->window : nullptr,
    };
    RegisterRawInputDevices(&device, 1, sizeof(device));
}

// Delivers the motion of the mouse's raw input as a PointerMove, in the device's own units.
void deliverRawInput(Win32Window* window, LPARAM lParam) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) ==
        UINT(-1) || input.header.dwType != RIM_TYPEMOUSE)
    return;
    const RAWMOUSE& mouse = input.data.mouse;
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0 || (mouse.lLastX == 0 && mouse.lLastY == 0))
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_PointerMove;
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.pointer.position = window->lockedAt;
    event.pointer.type = PrAppPointerType_Mouse;
    event.pointer.primary = true;
    event.pointer.id = kPrAppPointerMouse;
    event.pointer.movement = PrVec2{float(mouse.lLastX), float(mouse.lLastY)};
    deliver(window, event);
}

void setLock(Win32Window* window, bool locked) {
    if (window->locked == locked)
        return;
    if (locked && (appOf(window)->headless || GetForegroundWindow() != window->window))
        return;
    window->locked = locked;
    if (locked) {
        window->lockedAt = window->pointer;
        POINT at{
            MulDiv(int(window->pointer.x), int(window->dpi), 96),
            MulDiv(int(window->pointer.y), int(window->dpi), 96),
        };
        ClientToScreen(window->window, &at);
        window->lockedScreen = at;
        RECT client{};
        GetClientRect(window->window, &client);
        MapWindowPoints(window->window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        ClipCursor(&client);
        SetCursor(nullptr);
    } else {
        ClipCursor(nullptr);
        SetCursor(window->cursor);
    }
    setRawInput(window, locked && window->pointerMode == PrAppPointerMode_LockedRaw);
    requestFrame(window);
}

// Delivers a mouse message of the client area, or of a custom title bar if `frame` is set, which the system reports as
// the frame's.
void deliverPointer(Win32Window* window, UINT message, WPARAM wParam, LPARAM lParam, bool frame = false) {
    PrAppEvent event{};
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.pointer.position = positionOf(window, int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam)));
    if (message == WM_MOUSEMOVE) {
        if (window->locked) {
            // The move that returns the cursor to the lock's place moved nothing.
            POINT at{};
            GetCursorPos(&at);
            float scale = 96.0f / float(window->dpi);
            PrVec2 movement{float(at.x - window->lockedScreen.x) * scale, float(at.y - window->lockedScreen.y) * scale};
            if (movement.x == 0 && movement.y == 0)
                return;
            SetCursorPos(window->lockedScreen.x, window->lockedScreen.y);
            // A raw lock reports the device's motion from its raw input instead.
            if (window->rawInput)
                return;
            event.pointer.position = window->lockedAt;
            event.pointer.movement = movement;
        } else if (window->inside) {
            event.pointer.movement = PrVec2{
                event.pointer.position.x - window->pointer.x,
                event.pointer.position.y - window->pointer.y,
            };
        }
    }
    trackLeave(window, frame);
    if (!window->inside && !window->locked) {
        window->inside = true;
        deliverCrossing(window, PrAppEventKind_PointerEnter, event.pointer.position);
    }
    window->leaving = window->leaving && !pointerOverContent(window);
    switch (message) {
        case WM_MOUSEMOVE: event.kind = PrAppEventKind_PointerMove; break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: event.kind = PrAppEventKind_PointerDown; break;
        default: event.kind = PrAppEventKind_PointerUp; break;
    }
    switch (message) {
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: event.pointer.button = 1; break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: event.pointer.button = 2; break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: event.pointer.button = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4; break;
        default: break;
    }
    window->pointer = event.pointer.position;
    uint8_t bit = uint8_t(1u << event.pointer.button);
    if (event.kind == PrAppEventKind_PointerDown) {
        window->buttons |= bit;
        POINT at{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
        event.pointer.clicks = countClick(window, event.pointer.button, at, event.time);
    }
    else if (event.kind == PrAppEventKind_PointerUp)
        window->buttons &= uint8_t(~bit);
    event.pointer.primary = true;
    event.pointer.id = kPrAppPointerMouse;
    event.pointer.pressure = window->buttons != 0 ? 0.5f : 0.0f;
    // The window captures the pointer while a button is down, so that a drag reports its moves and its release outside
    // the window. The buttons are recorded first, since a release of the capture reports a loss of it.
    if (event.kind == PrAppEventKind_PointerDown && GetCapture() != window->window)
        SetCapture(window->window);
    else if (event.kind == PrAppEventKind_PointerUp && !anyButtonDown(wParam) && GetCapture() == window->window)
        ReleaseCapture();
    deliver(window, event);
    // A pointer that left during the press leaves once its last button is up.
    if (event.kind == PrAppEventKind_PointerUp && window->leaving && window->buttons == 0) {
        window->leaving = false;
        window->inside = false;
        deliverCrossing(window, PrAppEventKind_PointerLeave, window->pointer);
    }
}

// Delivers the WM_POINTER message of a pen or a touch, and returns true. Returns false for a mouse, whose messages
// DefWindowProc turns into the mouse's own. A pen's eraser, at its end or as its button, presses
// kPrAppPointerButtonEraser, and its barrel button presses the secondary button. The pointer's id is the system's plus
// one, so that it never equals kPrAppPointerMouse.
bool deliverPenOrTouch(Win32Window* window, UINT message, WPARAM wParam) {
    UINT32 id = GET_POINTERID_WPARAM(wParam);
    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!GetPointerType(id, &type) || (type != PT_PEN && type != PT_TOUCH))
        return false;
    PrAppEvent event{};
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.pointer.id = id + 1;
    POINTER_INFO info{};
    bool pressureKnown = false;
    float scale = 96.0f / float(window->dpi);
    if (type == PT_PEN) {
        POINTER_PEN_INFO pen{};
        if (!GetPointerPenInfo(id, &pen))
            return false;
        info = pen.pointerInfo;
        event.pointer.type = PrAppPointerType_Pen;
        // The system reports pressure in [0, 1024], and tilt and rotation in degrees.
        if ((pen.penMask & PEN_MASK_PRESSURE) != 0) {
            event.pointer.pressure = float(pen.pressure) / 1024.0f;
            pressureKnown = true;
        }
        if ((pen.penMask & PEN_MASK_TILT_X) != 0)
            event.pointer.tilt.x = float(pen.tiltX);
        if ((pen.penMask & PEN_MASK_TILT_Y) != 0)
            event.pointer.tilt.y = float(pen.tiltY);
        if ((pen.penMask & PEN_MASK_ROTATION) != 0)
            event.pointer.twist = float(pen.rotation % 360);
        if ((pen.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0)
            event.pointer.button = kPrAppPointerButtonEraser;
        else if ((pen.penFlags & PEN_FLAG_BARREL) != 0)
            event.pointer.button = 1;
    } else {
        POINTER_TOUCH_INFO touch{};
        if (!GetPointerTouchInfo(id, &touch))
            return false;
        info = touch.pointerInfo;
        event.pointer.type = PrAppPointerType_Touch;
        if ((touch.touchMask & TOUCH_MASK_PRESSURE) != 0) {
            event.pointer.pressure = float(touch.pressure) / 1024.0f;
            pressureKnown = true;
        }
        if ((touch.touchMask & TOUCH_MASK_CONTACTAREA) != 0) {
            event.pointer.contactSize = PrVec2{
                float(touch.rcContact.right - touch.rcContact.left) * scale,
                float(touch.rcContact.bottom - touch.rcContact.top) * scale,
            };
        }
    }
    bool contact = (info.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    if (!pressureKnown || !contact)
        event.pointer.pressure = contact ? 0.5f : 0.0f;
    event.pointer.primary = (info.pointerFlags & POINTER_FLAG_PRIMARY) != 0;
    POINT at = info.ptPixelLocation;
    ScreenToClient(window->window, &at);
    event.pointer.position = positionOf(window, at.x, at.y);
    switch (message) {
        case WM_POINTERDOWN:
            event.kind = PrAppEventKind_PointerDown;
            event.pointer.clicks = countClick(window, event.pointer.button, at, event.time);
            break;
        case WM_POINTERUP:
            event.kind = PrAppEventKind_PointerUp;
            if ((info.pointerFlags & POINTER_FLAG_CANCELED) != 0)
                event.kind = PrAppEventKind_PointerCancel;
            break;
        case WM_POINTERCAPTURECHANGED: event.kind = PrAppEventKind_PointerCancel; break;
        default: event.kind = PrAppEventKind_PointerMove; break;
    }
    deliver(window, event);
    return true;
}

// Releases the buttons that are down where the pointer last was, for a window that lost the pointer's capture, as to
// another window or a system dialog, which reports no release of its own.
void releaseButtons(Win32Window* window) {
    for (uint8_t button = 0; button < kPointerButtons; ++button) {
        uint8_t bit = uint8_t(1u << button);
        if ((window->buttons & bit) == 0)
            continue;
        window->buttons &= uint8_t(~bit);
        PrAppEvent event{};
        event.kind = PrAppEventKind_PointerUp;
        event.time = secondsNow();
        event.modifiers = modifiersNow();
        event.pointer.button = button;
        event.pointer.position = window->pointer;
        event.pointer.primary = true;
        event.pointer.id = kPrAppPointerMouse;
        event.pointer.pressure = window->buttons != 0 ? 0.5f : 0.0f;
        deliver(window, event);
    }
}

// Releases the keys that are down, for a window that lost the keyboard, where the keys' releases go instead. A lock key
// reports its state, not its movement, and is left as it is.
void releaseKeys(Win32Window* window) {
    for (uint32_t code = 0; code < countOf(window->keys); ++code) {
        HeldKey& held = window->keys[code];
        if (!held.down)
            continue;
        held.down = false;
        PrAppEvent event{};
        event.kind = PrAppEventKind_KeyUp;
        event.time = secondsNow();
        event.modifiers = modifiersNow();
        event.keyboard.key = held.key;
        event.keyboard.code = PrAppKeyCode(code);
        event.keyboard.text = "";
        deliver(window, event);
    }
}

// Delivers a wheel's turn in lines, positive right and down, as many per notch as the user sets. A notch is
// WHEEL_DELTA, and a precise wheel or a touchpad reports parts of one. A setting of a page per notch scrolls the
// window's height.
void deliverScroll(Win32Window* window, WPARAM wParam, LPARAM lParam, bool horizontal) {
    POINT point{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
    ScreenToClient(window->window, &point);
    UINT lines = 3;
    SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    PrVec2 extent = extentOf(window);
    float perNotch =
        lines == WHEEL_PAGESCROLL ? (horizontal ? extent.x : extent.y) / kPrAppScrollLineHeight : float(lines);
    float notches = float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
    PrAppEvent event{};
    event.kind = PrAppEventKind_Scroll;
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.scroll.position = positionOf(window, point.x, point.y);
    event.scroll.unit = PrAppScrollUnit_Line;
    // A vertical notch away from the user is positive and scrolls up. A horizontal tilt to the right is positive.
    if (horizontal)
        event.scroll.delta.x = notches * perNotch;
    else
        event.scroll.delta.y = -notches * perNotch;
    deliver(window, event);
}

void deliverKey(Win32Window* window, UINT message, WPARAM virtualKey, LPARAM flags) {
    Win32App* app = appOf(window);
    bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    PrAppEvent event{};
    event.kind = down ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp;
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.keyboard.code = codeOf(scanOf(virtualKey, flags));
    event.keyboard.repeat = down && (flags & (LPARAM(1) << 30)) != 0;
    event.keyboard.key = namedKeyOf(virtualKey);
    event.keyboard.text = "";
    if (virtualKey == VK_CAPITAL || virtualKey == VK_NUMLOCK) {
        // A lock key reports KeyDown as it turns on and KeyUp as it turns off, whichever way the key moves.
        if (!down || event.keyboard.repeat)
            return;
        event.kind = (GetKeyState(int(virtualKey)) & 1) != 0 ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp;
        deliver(window, event);
        return;
    }
    // An input method takes the key, which types nothing itself.
    if (virtualKey == VK_PROCESSKEY) {
        event.keyboard.key = PrAppKey_Unknown;
        deliver(window, event);
        return;
    }
    if (event.keyboard.key == PrAppKey_Unknown) {
        const KeyText& stashed = app->keyText;
        KeyText text;
        if (stashed.message == message && stashed.virtualKey == virtualKey && stashed.flags == flags)
            text = stashed;
        else
            text = translateKey(message, virtualKey, flags);
        if (text.dead) {
            event.keyboard.key = PrAppKey_Dead;
        } else if (text.count != 0) {
            event.keyboard.text = win32Narrow(text.units, text.count, app->utf8);
            wchar_t first = text.units[0];
            if (first >= L'a' && first <= L'z')
                first = wchar_t(first - L'a' + L'A');
            event.keyboard.key = first < 128 ? PrAppKey(first) : PrAppKey_Unknown;
        }
    }
    app->keyText = KeyText{};
    if (event.keyboard.code != PrAppKeyCode_Unidentified)
        window->keys[event.keyboard.code] = HeldKey{.down = down, .key = event.keyboard.key};
    deliver(window, event);
}

void deliverText(Win32Window* window, PrAppEventKind kind, PrStringView text, PrAppTextRange selection) {
    PrAppEvent event{};
    event.kind = kind;
    event.time = secondsNow();
    event.input.text = text;
    event.input.selection = selection;
    deliver(window, event);
}

// Delivers a character that WM_CHAR types. A character beyond the BMP arrives as two messages, one per surrogate. A
// control character types nothing: the program reads its key.
void deliverCharacter(Win32Window* window, wchar_t unit) {
    if (IS_HIGH_SURROGATE(unit)) {
        window->highSurrogate = unit;
        return;
    }
    wchar_t units[2] = {unit, 0};
    int count = 1;
    if (IS_LOW_SURROGATE(unit)) {
        if (window->highSurrogate == 0)
            return;
        units[0] = window->highSurrogate;
        units[1] = unit;
        count = 2;
    }
    window->highSurrogate = 0;
    if (!isPrintable(units, count))
        return;
    deliverText(window, PrAppEventKind_Text, win32Narrow(units, count, appOf(window)->utf8), PrAppTextRange{});
}

void deliverCodePoint(Win32Window* window, uint32_t codePoint) {
    wchar_t units[2];
    int count = 1;
    if (codePoint >= 0x10000) {
        units[0] = wchar_t(0xD800 + ((codePoint - 0x10000) >> 10));
        units[1] = wchar_t(0xDC00 + ((codePoint - 0x10000) & 0x3FF));
        count = 2;
    } else {
        units[0] = wchar_t(codePoint);
    }
    if (isPrintable(units, count))
        deliverText(window, PrAppEventKind_Text, win32Narrow(units, count, appOf(window)->utf8), PrAppTextRange{});
}

// Places the input method's windows at the caret that the program last gave, in the window's client area.
void placeInputMethod(Win32Window* window) {
    HIMC context = ImmGetContext(window->window);
    if (context == nullptr)
        return;
    float scale = float(GetDpiForWindow(window->window)) / USER_DEFAULT_SCREEN_DPI;
    POINT below{LONG(window->textInputRect.left * scale), LONG(window->textInputRect.bottom * scale)};
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = below;
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = below;
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(window->window, context);
}

// Reads one of the composition's strings into the scratch storage and returns its length in UTF-16 units.
int compositionString(Win32App* app, HIMC context, DWORD index) {
    LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (bytes <= 0)
        return 0;
    app->utf16.resize(uint32_t(bytes) / sizeof(wchar_t) + 1);
    ImmGetCompositionStringW(context, index, app->utf16.data, DWORD(bytes));
    return int(uint32_t(bytes) / sizeof(wchar_t));
}

// The part of the composition that the input method converts, from its clause attributes, or the caret if no clause is
// a target. The range is in UTF-16 units.
void compositionSelection(HIMC context, int count, int* start, int* end) {
    RawArray<uint8_t> attributes;
    LONG size = ImmGetCompositionStringW(context, GCS_COMPATTR, nullptr, 0);
    *start = -1;
    *end = -1;
    if (size > 0) {
        attributes.resize(uint32_t(size));
        ImmGetCompositionStringW(context, GCS_COMPATTR, attributes.data, DWORD(size));
        for (int i = 0; i < count && i < size; ++i) {
            uint8_t attribute = attributes.data[i];
            if (attribute == ATTR_TARGET_CONVERTED || attribute == ATTR_TARGET_NOTCONVERTED) {
                if (*start < 0)
                    *start = i;
                *end = i + 1;
            }
        }
    }
    attributes.reset();
    if (*start < 0) {
        LONG caret = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
        *start = caret >= 0 && caret <= count ? int(caret) : count;
        *end = *start;
    }
}

// Delivers a change to the composition. A result ends the composition and follows as a Text event, and a new
// composition may begin in the same message.
void deliverComposition(Win32Window* window, LPARAM flags) {
    Win32App* app = appOf(window);
    HIMC context = ImmGetContext(window->window);
    if (context == nullptr)
        return;
    if ((flags & GCS_RESULTSTR) != 0) {
        int count = compositionString(app, context, GCS_RESULTSTR);
        PrStringView text = win32Narrow(app->utf16.data, count, app->utf8);
        if (window->composing) {
            window->composing = false;
            deliverText(window, PrAppEventKind_CompositionEnd, text, PrAppTextRange{});
        }
        if (text.size != 0)
            deliverText(window, PrAppEventKind_Text, text, PrAppTextRange{});
    }
    if ((flags & GCS_COMPSTR) != 0) {
        int count = compositionString(app, context, GCS_COMPSTR);
        if (count == 0) {
            if (window->composing) {
                window->composing = false;
                deliverText(window, PrAppEventKind_CompositionEnd, "", PrAppTextRange{});
            }
        } else {
            int start = 0;
            int end = 0;
            compositionSelection(context, count, &start, &end);
            PrAppTextRange selection{win32Utf8Length(app->utf16.data, start), win32Utf8Length(app->utf16.data, end)};
            window->composing = true;
            deliverText(window, PrAppEventKind_CompositionUpdate, win32Narrow(app->utf16.data, count, app->utf8),
                selection);
        }
    }
    ImmReleaseContext(window->window, context);
}

// The custom type under which a drop's store keeps the bytes of its image's CF_DIBV5, which a read of the image
// decodes.
constexpr PrStringView kDropImageType = "org.prax.app.dib";

// Adds the file at the absolute path `path` to `out`, with its name, its size and whether it is a directory.
void addFile(Win32App* app, const wchar_t* path, AppDataStore& out) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    bool known = GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) != FALSE;
    PrStringView narrowed = win32Narrow(path, int(wcslen(path)), app->utf8);
    appDataStoreAddFile(out, PrAppDataFile{
        .name = appPathName(narrowed),
        .path = narrowed,
        .size = known ? (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow : 0,
        .directory = known && (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
    });
}

// Adds the files that `drop` lists to `out`.
void addDroppedFiles(Win32App* app, HDROP drop, AppDataStore& out) {
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, nullptr, 0);
        RawArray<wchar_t> path{};
        path.resize(length + 1);
        if (DragQueryFileW(drop, i, path.data, length + 1) == length)
            addFile(app, path.data, out);
        path.reset();
    }
}

// Decodes the bytes of a CF_DIBV5 into `out`'s image.
void setDibImage(const uint8_t* bytes, size_t size, AppDataStore& out) {
    RawArray<uint8_t> pixels{};
    PrUVec2 extent = dibPixels(bytes, size, pixels);
    if (extent.x != 0 && extent.y != 0)
        appDataStoreSetImage(out, PrPixelView{pixels.data, size_t(extent.x) * 4, extent});
    pixels.reset();
}

// Returns the number that follows `name` in a CF_HTML header, or zero.
uint32_t cfHtmlOffset(PrStringView html, PrStringView name) {
    size_t at = prStringFind(html, name);
    if (at == html.size)
        return 0;
    uint32_t value = 0;
    for (at += name.size; at < html.size && html.data[at] >= '0' && html.data[at] <= '9'; ++at)
        value = value * 10 + uint32_t(html.data[at] - '0');
    return value;
}

// Returns the fragment of CF_HTML's document, or the whole of it if its header names none.
PrStringView cfHtmlFragment(PrStringView html) {
    uint32_t start = cfHtmlOffset(html, "StartFragment:");
    uint32_t end = cfHtmlOffset(html, "EndFragment:");
    if (start == 0 || end <= start || end > html.size)
        return html;
    return PrStringView{html.data + start, end - start};
}

// Returns the forms that the clipboard holds.
PrAppDataFormFlags clipboardForms(const Win32App* app) {
    PrAppDataFormFlags forms = 0;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT))
        forms |= PrAppDataFormFlag_Text;
    if (app->htmlFormat != 0 && IsClipboardFormatAvailable(app->htmlFormat))
        forms |= PrAppDataFormFlag_Html;
    if (IsClipboardFormatAvailable(CF_HDROP))
        forms |= PrAppDataFormFlag_Files;
    if (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB))
        forms |= PrAppDataFormFlag_Image;
    return forms;
}

// Reads the clipboard's content of the form that `answer` asks for into `out`. The clipboard holds no URLs apart from
// its text, and custom content by the clipboard format that its type registers.
void readClipboard(Win32App* app, AppDataAnswer answer, PrStringView type, AppDataStore& out) {
    if (!openClipboard(app->messageWindow))
        return;
    switch (answer) {
        case PrAppDataReadKind_Forms:
            out.forms = clipboardForms(app);
            break;
        case PrAppDataReadKind_Text: {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* units = data != nullptr ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
            if (units != nullptr) {
                // The text ends at its NUL, or at the end of its memory if another program wrote none.
                size_t capacity = GlobalSize(data) / sizeof(wchar_t);
                size_t count = 0;
                while (count < capacity && units[count] != 0)
                    ++count;
                int length = int(count < size_t(INT32_MAX) ? count : size_t(INT32_MAX));
                appDataStoreSetText(out, win32Narrow(units, length, app->utf8));
                GlobalUnlock(data);
            }
            break;
        }
        case PrAppDataReadKind_Html: {
            HANDLE data = app->htmlFormat != 0 ? GetClipboardData(app->htmlFormat) : nullptr;
            const char* bytes = data != nullptr ? static_cast<const char*>(GlobalLock(data)) : nullptr;
            if (bytes != nullptr) {
                PrStringView html{bytes, strnlen(bytes, GlobalSize(data))};
                appDataStoreSetHtml(out, cfHtmlFragment(html));
                GlobalUnlock(data);
            }
            break;
        }
        case PrAppDataReadKind_Image: {
            // The system makes a CF_DIB of a CF_DIBV5 and the reverse, and either of a CF_BITMAP.
            HANDLE data = GetClipboardData(CF_DIBV5);
            const uint8_t* bytes = data != nullptr ? static_cast<const uint8_t*>(GlobalLock(data)) : nullptr;
            if (bytes != nullptr) {
                setDibImage(bytes, GlobalSize(data), out);
                GlobalUnlock(data);
            }
            break;
        }
        case PrAppDataReadKind_Files: {
            HANDLE data = GetClipboardData(CF_HDROP);
            if (data != nullptr)
                addDroppedFiles(app, static_cast<HDROP>(data), out);
            break;
        }
        case PrAppDataReadKind_Custom: {
            RawArray<wchar_t> name{};
            win32Widen(type, name);
            UINT format = RegisterClipboardFormatW(name.data);
            name.reset();
            HANDLE data = format != 0 ? GetClipboardData(format) : nullptr;
            const uint8_t* bytes = data != nullptr ? static_cast<const uint8_t*>(GlobalLock(data)) : nullptr;
            if (bytes != nullptr) {
                appDataStoreAddCustom(out, type, PrByteView{bytes, GlobalSize(data)});
                GlobalUnlock(data);
            }
            break;
        }
        default:
            break;
    }
    CloseClipboard();
}

// Answers the request `id` of content, on the application's thread: from the clipboard, or from a source's store. A
// source's store does not change, and the request's reference keeps it, so its answer copies only a decoded image. A
// request that was cancelled answers nothing.
void answerReadFor(Win32App* app, uint32_t id, PrStringView type) {
    PrAppDataSource source = kPrAppDataSourceNone;
    AppDataAnswer kind = PrAppDataReadKind_Text;
    if (!appDataRequestsFind(app->requests, id, &source, &kind))
        return;
    Win32Answer* answer = answerOf(Win32AnswerKind_Store, id, PrStatus_Ok);
    AppDataStore* store = appDataSourceStore(app->sources, source);
    if (source == kPrAppDataSourceClipboard) {
        readClipboard(app, kind, type, answer->store);
    } else if (source == kPrAppDataSourceSelection || source == kPrAppDataSourceNone) {
    } else if (store == nullptr) {
        answer->status = PrStatus_NotFound;
    } else if (kind == PrAppDataReadKind_Image) {
        PrByteView dib = appDataStoreCustom(*store, kDropImageType);
        if (dib.size != 0)
            setDibImage(dib.data, dib.size, answer->store);
    } else {
        answer->external = store;
    }
    answerDeliver(app, answer);
}

// ---------------------------------------------------------------------------------------------------------------------
// Drags
//
// One application runs at a time, and the system carries one drag at a time. So the drop target of the windows, the
// data object of a drag and its drop source are single objects of this file. They are not reference counted, because
// they outlive every reference that OLE takes.

// The name of the clipboard format of a drag's token (Win32Drag).
constexpr const wchar_t* kPayloadFormatName = L"org.prax.app.drag-payload";

// Returns the drag operations of an OLE drop effect.
PrAppDragOperationFlags operationsOfEffect(DWORD effect) {
    PrAppDragOperationFlags operations = 0;
    if ((effect & DROPEFFECT_COPY) != 0)
        operations |= PrAppDragOperationFlag_Copy;
    if ((effect & DROPEFFECT_MOVE) != 0)
        operations |= PrAppDragOperationFlag_Move;
    if ((effect & DROPEFFECT_LINK) != 0)
        operations |= PrAppDragOperationFlag_Link;
    return operations;
}

// Returns the OLE drop effect of a set of drag operations.
DWORD effectOf(PrAppDragOperationFlags operations) {
    DWORD effect = DROPEFFECT_NONE;
    if ((operations & PrAppDragOperationFlag_Copy) != 0)
        effect |= DROPEFFECT_COPY;
    if ((operations & PrAppDragOperationFlag_Move) != 0)
        effect |= DROPEFFECT_MOVE;
    if ((operations & PrAppDragOperationFlag_Link) != 0)
        effect |= DROPEFFECT_LINK;
    return effect;
}

// Returns the OLE drop effect of one operation.
DWORD effectOfOperation(PrAppDragOperation operation) {
    if (operation == PrAppDragOperation_None)
        return DROPEFFECT_NONE;
    return effectOf(PrAppDragOperationFlags(1u << operation));
}

// Returns the operation of the drop effect that a target chose. A target chooses one effect, and a move outranks the
// others if it reports several.
PrAppDragOperation operationOfEffect(DWORD effect) {
    if ((effect & DROPEFFECT_MOVE) != 0)
        return PrAppDragOperation_Move;
    if ((effect & DROPEFFECT_COPY) != 0)
        return PrAppDragOperation_Copy;
    if ((effect & DROPEFFECT_LINK) != 0)
        return PrAppDragOperation_Link;
    return PrAppDragOperation_None;
}

// Narrows the source's operations to the one that the modifier keys in `keys` ask for. As in the shell, Control copies,
// Shift moves, and both together link.
PrAppDragOperationFlags narrowedByKeys(PrAppDragOperationFlags operations, DWORD keys) {
    bool control = (keys & MK_CONTROL) != 0;
    bool shift = (keys & MK_SHIFT) != 0;
    if (control && shift)
        return operations & PrAppDragOperationFlag_Link;
    if (control)
        return operations & PrAppDragOperationFlag_Copy;
    if (shift)
        return operations & PrAppDragOperationFlag_Move;
    return operations;
}

// Returns a copy of `size` bytes in movable memory that the caller owns, or null.
HGLOBAL PR_NULLABLE globalOfBytes(const void* bytes, size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    void* at = memory != nullptr ? GlobalLock(memory) : nullptr;
    if (at == nullptr) {
        if (memory != nullptr)
            GlobalFree(memory);
        return nullptr;
    }
    prMemCopy(at, bytes, size);
    GlobalUnlock(memory);
    return memory;
}

// Returns a device-independent bitmap of straight-alpha sRGB Rgba8 `pixels`, in movable memory that the caller owns, or
// null.
HGLOBAL PR_NULLABLE dibOf(PrPixelView pixels) {
    size_t rowBytes = size_t(pixels.extent.x) * 4;
    size_t bytes = sizeof(BITMAPV5HEADER) + rowBytes * pixels.extent.y;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    uint8_t* at = memory != nullptr ? static_cast<uint8_t*>(GlobalLock(memory)) : nullptr;
    if (at == nullptr) {
        if (memory != nullptr)
            GlobalFree(memory);
        return nullptr;
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = LONG(pixels.extent.x);
    header.bV5Height = LONG(pixels.extent.y);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = DWORD(rowBytes * pixels.extent.y);
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;
    prMemCopy(at, &header, sizeof(header));
    // A bitmap with a positive height holds its rows from the bottom up.
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        const uint8_t* from = pixels.data + y * pixels.stride;
        uint8_t* to = at + sizeof(header) + size_t(pixels.extent.y - 1 - y) * rowBytes;
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            to[x * 4] = from[x * 4 + 2];
            to[x * 4 + 1] = from[x * 4 + 1];
            to[x * 4 + 2] = from[x * 4];
            to[x * 4 + 3] = from[x * 4 + 3];
        }
    }
    GlobalUnlock(memory);
    return memory;
}

// Returns the paths of `content` as a CF_HDROP list in movable memory that the caller owns, or null.
HGLOBAL PR_NULLABLE fileListOf(Win32App* app, const PrAppDataContent& content) {
    // The list is the header, then each path with its NUL, then one more NUL.
    RawArray<wchar_t> units{};
    for (uint32_t i = 0; i < content.pathCount; ++i) {
        win32Widen(content.paths[i], app->utf16);
        units.append(app->utf16.data, app->utf16.count);
    }
    units.push(0);
    DROPFILES header{};
    header.pFiles = sizeof(header);
    header.fWide = TRUE;
    size_t bytes = sizeof(header) + size_t(units.count) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    uint8_t* at = memory != nullptr ? static_cast<uint8_t*>(GlobalLock(memory)) : nullptr;
    if (at != nullptr) {
        prMemCopy(at, &header, sizeof(header));
        prMemCopy(at + sizeof(header), units.data, size_t(units.count) * sizeof(wchar_t));
        GlobalUnlock(memory);
    } else if (memory != nullptr) {
        GlobalFree(memory);
        memory = nullptr;
    }
    units.reset();
    return memory;
}

// Returns a request for `format` in global memory.
FORMATETC formatOf(CLIPFORMAT format) {
    return FORMATETC{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

// Returns the clipboard format that carries a form of a drag's content.
CLIPFORMAT clipFormatOf(const Win32App* app, PrAppDataFormFlags form) {
    switch (form) {
        case PrAppDataFormFlag_Text: return CF_UNICODETEXT;
        case PrAppDataFormFlag_Files: return CF_HDROP;
        case PrAppDataFormFlag_Image: return CF_DIBV5;
        default: return CLIPFORMAT(app->payloadFormat);
    }
}

// The forms of a drag's content, in the order that the data object offers them.
constexpr PrAppDataFormFlags kDragForms[] = {
    PrAppDataFormFlag_Payload, PrAppDataFormFlag_Files, PrAppDataFormFlag_Image, PrAppDataFormFlag_Text,
};

// A format that SetData added to the data object, as the shell's helper adds the drag's image.
struct DataFormat {
    FORMATETC format;
    STGMEDIUM medium;
};

// Returns the form of the drag's content that `format` carries, or zero if the drag has no such form.
PrAppDataFormFlags formOf(const Win32App* app, const FORMATETC& format) {
    if ((format.tymed & TYMED_HGLOBAL) == 0 || format.dwAspect != DVASPECT_CONTENT)
        return 0;
    for (PrAppDataFormFlags form : kDragForms) {
        if ((app->drag.content.forms & form) != 0 && format.cfFormat == clipFormatOf(app, form))
            return form;
    }
    return 0;
}

// Returns the form of the drag's content in global memory that the caller owns, or null.
HGLOBAL PR_NULLABLE globalOfForm(Win32App* app, PrAppDataFormFlags form) {
    PrAppDataContent content = appDataStoreContent(app->drag.content);
    switch (form) {
        case PrAppDataFormFlag_Text:
            win32Widen(content.text, app->utf16);
            return globalOfBytes(app->utf16.data, size_t(app->utf16.count) * sizeof(wchar_t));
        case PrAppDataFormFlag_Files: return fileListOf(app, content);
        case PrAppDataFormFlag_Image: return content.image.data != nullptr ? dibOf(content.image) : nullptr;
        case PrAppDataFormFlag_Payload: return globalOfBytes(&app->drag.token, sizeof(app->drag.token));
        default: return nullptr;
    }
}

// The data object of a drag that a window started. It offers each form of the content in Win32App::drag, and the
// formats that SetData added. The content stays until the next drag, for a target that reads it after the drop.
struct DataObject final : IDataObject {
    Win32App* PR_NULLABLE app;
    RawArray<DataFormat> added;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDataObject)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        *medium = STGMEDIUM{};
        if (app == nullptr)
            return DV_E_FORMATETC;
        PrAppDataFormFlags form = formOf(app, *format);
        if (form != 0) {
            HGLOBAL global = globalOfForm(app, form);
            if (global == nullptr)
                return E_OUTOFMEMORY;
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = global;
            return S_OK;
        }
        for (uint32_t i = 0; i < added.count; ++i) {
            const DataFormat& entry = added.data[i];
            if (entry.format.cfFormat != format->cfFormat || (format->tymed & entry.medium.tymed) == 0)
                continue;
            if (entry.medium.tymed == TYMED_HGLOBAL) {
                medium->hGlobal = OleDuplicateData(entry.medium.hGlobal, entry.format.cfFormat, 0);
                if (medium->hGlobal == nullptr)
                    return E_OUTOFMEMORY;
            } else if (entry.medium.tymed == TYMED_ISTREAM) {
                medium->pstm = entry.medium.pstm;
                medium->pstm->AddRef();
            } else {
                return DV_E_TYMED;
            }
            medium->tymed = entry.medium.tymed;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (app != nullptr && formOf(app, *format) != 0)
            return S_OK;
        for (uint32_t i = 0; i < added.count; ++i) {
            if (added.data[i].format.cfFormat == format->cfFormat && (format->tymed & added.data[i].medium.tymed) != 0)
                return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    // Takes ownership of the medium of a format that another party adds, such as the shell's helper. A call that keeps
    // ownership of its medium is refused.
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!release)
            return E_NOTIMPL;
        DataFormat entry{.format = *format, .medium = *medium};
        entry.format.ptd = nullptr;
        for (uint32_t i = 0; i < added.count; ++i) {
            if (added.data[i].format.cfFormat == format->cfFormat) {
                ReleaseStgMedium(&added.data[i].medium);
                added.data[i] = entry;
                return S_OK;
            }
        }
        added.push(entry);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        *out = nullptr;
        if (direction != DATADIR_GET || app == nullptr)
            return E_NOTIMPL;
        RawArray<FORMATETC> formats{};
        for (PrAppDataFormFlags form : kDragForms) {
            if ((app->drag.content.forms & form) != 0)
                formats.push(formatOf(clipFormatOf(app, form)));
        }
        for (uint32_t i = 0; i < added.count; ++i) {
            FORMATETC format = added.data[i].format;
            format.tymed = added.data[i].medium.tymed;
            formats.push(format);
        }
        HRESULT result = SHCreateStdEnumFmtEtc(formats.count, formats.data, out);
        formats.reset();
        return result;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
};

// Releases the formats that SetData added to the data object.
void dataObjectClear(DataObject& object) {
    for (uint32_t i = 0; i < object.added.count; ++i)
        ReleaseStgMedium(&object.added.data[i].medium);
    object.added.reset();
}

// The source of a drag that a window started. The drag drops when the primary button is released, and Escape cancels
// it.
struct DropSource final : IDropSource {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDropSource)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape)
            return DRAGDROP_S_CANCEL;
        if ((keys & MK_LBUTTON) == 0)
            return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

// Returns the application's window whose top-level window holds the screen point, or null.
Win32Window* PR_NULLABLE windowAt(Win32App* app, POINTL point) {
    HWND hwnd = WindowFromPoint(POINT{point.x, point.y});
    hwnd = hwnd != nullptr ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    for (uint32_t i = 0; i < app->windows.count && hwnd != nullptr; ++i) {
        if (app->windows.data[i]->window == hwnd)
            return app->windows.data[i];
    }
    return nullptr;
}

// Returns a drag as a window's events report it before the drop: the forms of its content that `data` offers, and the
// payload of the drag that the application has in flight if `data` carries its token.
PrAppDragEvent dragOf(Win32App* app, Win32Window* window, IDataObject* PR_NULLABLE data,
        PrAppDragOperationFlags operations) {
    PrAppDragEvent drag{.operations = operations};
    if (data == nullptr)
        return drag;
    for (PrAppDataFormFlags form : kDragForms) {
        if (form == PrAppDataFormFlag_Payload)
            continue;
        FORMATETC format = formatOf(clipFormatOf(app, form));
        if (data->QueryGetData(&format) == S_OK)
            drag.forms |= form;
    }
    FORMATETC format = formatOf(CLIPFORMAT(app->payloadFormat));
    STGMEDIUM medium{};
    if (app->drag.active && data->GetData(&format, &medium) == S_OK) {
        const void* at = GlobalLock(medium.hGlobal);
        uint64_t token = 0;
        if (at != nullptr) {
            if (GlobalSize(medium.hGlobal) >= sizeof(token))
                prMemCopy(&token, at, sizeof(token));
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
        if (token == app->drag.token) {
            drag.fromThisWindow = app->drag.window == window;
            if ((app->drag.content.forms & PrAppDataFormFlag_Payload) != 0) {
                drag.forms |= PrAppDataFormFlag_Payload;
                drag.payload = app->drag.content.payload;
            }
        }
    }
    return drag;
}

// Reads each form of the content that `flags` names from `data` into `store`.
void readDrop(Win32App* app, IDataObject* data, PrAppDataFormFlags flags, AppDataStore& store) {
    STGMEDIUM medium{};
    FORMATETC text = formatOf(CF_UNICODETEXT);
    if ((flags & PrAppDataFormFlag_Text) != 0 && data->GetData(&text, &medium) == S_OK) {
        const wchar_t* units = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
        if (units != nullptr) {
            // The text ends at its NUL, or at the end of its memory if the source wrote none.
            size_t capacity = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
            size_t count = 0;
            while (count < capacity && units[count] != 0)
                ++count;
            int length = int(count < size_t(INT32_MAX) ? count : size_t(INT32_MAX));
            appDataStoreSetText(store, win32Narrow(units, length, app->utf8));
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    FORMATETC files = formatOf(CF_HDROP);
    if ((flags & PrAppDataFormFlag_Files) != 0 && data->GetData(&files, &medium) == S_OK) {
        addDroppedFiles(app, static_cast<HDROP>(medium.hGlobal), store);
        ReleaseStgMedium(&medium);
    }
    FORMATETC image = formatOf(CF_DIBV5);
    if ((flags & PrAppDataFormFlag_Image) != 0 && data->GetData(&image, &medium) == S_OK) {
        const uint8_t* bytes = static_cast<const uint8_t*>(GlobalLock(medium.hGlobal));
        if (bytes != nullptr) {
            appDataStoreAddCustom(store, kDropImageType, PrByteView{bytes, GlobalSize(medium.hGlobal)});
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
}

// The drop target of every window of the application. A drag over a window is answered with the drop operation of the
// window's latest output, and each of the drag's events requests a frame, so that the program answers the drag's latest
// position.
struct DropTarget final : IDropTarget {
    Win32App* PR_NULLABLE app;
    Win32Window* PR_NULLABLE over;      // the window under the drag, from DragEnter to DragLeave or Drop
    uint64_t overSerial;                // the serial of `over`
    IDataObject* PR_NULLABLE data;      // the drag's data, held from DragEnter to DragLeave or Drop
    PrAppDragOperationFlags allowed;    // the operations that the source allows

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDropTarget)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    // Delivers a drag event to the window under the drag, and returns the window's answer.
    DWORD deliverDrag(PrAppEventKind kind, DWORD keys, POINTL point) {
        Win32Window* window = over;
        if (app == nullptr || window == nullptr)
            return DROPEFFECT_NONE;
        PrAppDragEvent drag = dragOf(app, window, data, narrowedByKeys(allowed, keys));
        POINT client{point.x, point.y};
        ScreenToClient(window->window, &client);
        drag.position = positionOf(window, client.x, client.y);
        deliver(window, PrAppEvent{.kind = kind, .modifiers = modifiersNow(), .time = secondsNow(), .drag = drag});
        if (!isOpen(app, window, overSerial)) {
            over = nullptr;
            return DROPEFFECT_NONE;
        }
        requestFrame(window);
        return effectOfOperation(appDragAnswer(window->dropOperation, drag.operations));
    }

    // Ends the drag over the window, and releases its data.
    void leave() {
        Win32Window* window = over;
        over = nullptr;
        if (app != nullptr && window != nullptr && isOpen(app, window, overSerial)) {
            PrAppEvent event{
                .kind = PrAppEventKind_DragLeave,
                .modifiers = modifiersNow(),
                .time = secondsNow(),
                .drag = dragOf(app, window, data, allowed),
            };
            deliver(window, event);
            requestFrame(window);
        }
        if (data != nullptr)
            data->Release();
        data = nullptr;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object, DWORD keys, POINTL point, DWORD* effect) override {
        if (data != nullptr)
            data->Release();
        data = object;
        if (data != nullptr)
            data->AddRef();
        over = app != nullptr ? windowAt(app, point) : nullptr;
        overSerial = over != nullptr ? over->serial : 0;
        allowed = operationsOfEffect(*effect);
        DWORD answer = deliverDrag(PrAppEventKind_DragEnter, keys, point);
        if (app != nullptr && app->dropHelper != nullptr && over != nullptr) {
            POINT screen{point.x, point.y};
            app->dropHelper->DragEnter(over->window, object, &screen, answer);
        }
        *effect = answer;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override {
        allowed = operationsOfEffect(*effect);
        DWORD answer = deliverDrag(PrAppEventKind_DragOver, keys, point);
        if (app != nullptr && app->dropHelper != nullptr) {
            POINT screen{point.x, point.y};
            app->dropHelper->DragOver(&screen, answer);
        }
        *effect = answer;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (app != nullptr && app->dropHelper != nullptr)
            app->dropHelper->DragLeave();
        leave();
        return S_OK;
    }

    // Reads every form of the drag's content into the application's drop, and delivers the drop, if the window answers
    // it. The program reads the content by the drop's source. The data object is the system's only until this returns.
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* object, DWORD keys, POINTL point, DWORD* effect) override {
        Win32Window* window = over;
        allowed = operationsOfEffect(*effect);
        PrAppDragOperationFlags operations = narrowedByKeys(allowed, keys);
        PrAppDragOperation operation = PrAppDragOperation_None;
        if (app != nullptr && window != nullptr && isOpen(app, window, overSerial))
            operation = appDragAnswer(window->dropOperation, operations);
        *effect = effectOfOperation(operation);
        if (app != nullptr && app->dropHelper != nullptr) {
            POINT screen{point.x, point.y};
            app->dropHelper->Drop(object, &screen, *effect);
        }
        if (operation == PrAppDragOperation_None) {
            leave();
            return S_OK;
        }
        PrAppDragEvent drag = dragOf(app, window, object, operations);
        drag.operation = operation;
        AppDataStore* store = allocZeroed<AppDataStore>();
        readDrop(app, object, drag.forms, *store);
        // The image's bytes are custom content of the store, which the drop shows as its image.
        store->forms = drag.forms;
        drag.drop = appDataSourceCreate(app->sources, store, nullptr);
        POINT client{point.x, point.y};
        ScreenToClient(window->window, &client);
        drag.position = positionOf(window, client.x, client.y);
        PrAppEvent event{.kind = PrAppEventKind_Drop, .modifiers = modifiersNow(), .time = secondsNow(), .drag = drag};
        over = nullptr;
        if (data != nullptr)
            data->Release();
        data = nullptr;
        uint64_t serial = overSerial;
        deliver(window, event);
        if (isOpen(app, window, serial))
            requestFrame(window);
        return S_OK;
    }
};

DropTarget gDropTarget{};
DataObject gDataObject{};
DropSource gDropSource{};

// Gives the data object the preview of the window's drag, through the shell's helper, which shows it under the pointer.
void setDragPreview(Win32Window* window) {
    const AppDragPending& drag = window->drag;
    IDragSourceHelper* helper = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))))
        return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = LONG(drag.previewExtent.x);
    info.bmiHeader.biHeight = -LONG(drag.previewExtent.y);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap != nullptr && bits != nullptr) {
        // The helper reads premultiplied BGRA, from the top row down.
        uint8_t* to = static_cast<uint8_t*>(bits);
        size_t count = size_t(drag.previewExtent.x) * drag.previewExtent.y;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* from = drag.preview.data + i * 4;
            uint32_t alpha = from[3];
            to[i * 4] = uint8_t((from[2] * alpha + 127) / 255);
            to[i * 4 + 1] = uint8_t((from[1] * alpha + 127) / 255);
            to[i * 4 + 2] = uint8_t((from[0] * alpha + 127) / 255);
            to[i * 4 + 3] = uint8_t(alpha);
        }
        float scale = float(window->dpi) / 96.0f;
        SHDRAGIMAGE image{};
        image.sizeDragImage = SIZE{LONG(drag.previewExtent.x), LONG(drag.previewExtent.y)};
        image.ptOffset = POINT{LONG(drag.previewOffset.x * scale), LONG(drag.previewOffset.y * scale)};
        image.hbmpDragImage = bitmap;
        image.crColorKey = CLR_NONE;
        // The helper owns the bitmap once it takes it.
        if (FAILED(helper->InitializeFromBitmap(&image, &gDataObject)))
            DeleteObject(bitmap);
    } else if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    helper->Release();
}

// Delivers the end of the window's drag, with the operation that the target performed.
void endDrag(Win32Window* window, PrAppDragOperation operation) {
    PrAppEvent event{
        .kind = PrAppEventKind_DragEnd,
        .modifiers = modifiersNow(),
        .time = secondsNow(),
        .drag = {
            .forms = window->drag.content.flags,
            .operations = window->drag.operations,
            .operation = operation,
            .fromThisWindow = true,
            .payload = window->drag.content.payload,
        },
    };
    deliver(window, event);
}

// Starts the drag that prAppWindowDragStart asked for, once no callback runs. A drag starts only while the primary
// button's press is still down. OLE's loop carries the drag and returns once the drag ends. The loop dispatches the
// windows' messages, so frames and events go on while it runs.
void startDrag(Win32Window* window) {
    if (!window->drag.pending)
        return;
    window->drag.pending = false;
    Win32App* app = appOf(window);
    uint64_t serial = window->serial;
    PrAppDataContent content = appDataStoreContent(window->drag.content);
    bool down = (window->buttons & 1u) != 0 && GetKeyState(VK_LBUTTON) < 0;
    bool startable = app->ole && !app->headless && !app->drag.active && !window->inSizeMove && down;
    if (!startable || content.flags == 0) {
        endDrag(window, PrAppDragOperation_None);
        return;
    }
    dataObjectClear(gDataObject);
    appDataStoreSet(app->drag.content, content);
    app->drag.active = true;
    app->drag.token = prRandomSeed();
    app->drag.window = window;
    app->drag.operations = window->drag.operations;
    if (window->drag.previewExtent.x != 0 && window->drag.previewExtent.y != 0)
        setDragPreview(window);
    // The system takes the pointer, so no release reaches the window to end the press.
    window->buttons &= uint8_t(~1u);
    PrAppEvent cancel{
        .kind = PrAppEventKind_PointerCancel,
        .modifiers = modifiersNow(),
        .time = secondsNow(),
        .pointer =
            {.position = window->pointer, .type = PrAppPointerType_Mouse, .primary = true, .id = kPrAppPointerMouse},
    };
    deliver(window, cancel);
    DWORD effect = DROPEFFECT_NONE;
    HRESULT result = DRAGDROP_S_CANCEL;
    if (isOpen(app, window, serial))
        result = DoDragDrop(&gDataObject, &gDropSource, effectOf(app->drag.operations), &effect);
    Win32Window* source = app->drag.window;
    app->drag.active = false;
    app->drag.window = nullptr;
    // A window that closed during the drag receives no DragEnd. Its destruction cleared the drag's window.
    if (source != nullptr && isOpen(app, source, serial))
        endDrag(source, result == DRAGDROP_S_DROP ? operationOfEffect(effect) : PrAppDragOperation_None);
}

// Brings a window forward, as after a click on the application's icon in the notification area, which allows the
// process to take the foreground.
void bringForward(Win32Window* window) {
    if (IsIconic(window->window))
        ShowWindow(window->window, SW_RESTORE);
    SetForegroundWindow(window->window);
}

// Handles a user action on the icon in the notification area. A click on the balloon of the last notification delivers
// the click to the application. A click on the icon brings the last active window forward.
void notifyIconAction(Win32App* app, UINT action) {
    switch (action) {
        case NIN_BALLOONUSERCLICK: {
            PrAppEvent event{};
            event.kind = PrAppEventKind_Notification;
            event.tag = app->notificationTag;
            event.time = secondsNow();
            deliver(app, nullptr, event);
            break;
        }
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (app->lastActive != nullptr)
                bringForward(app->lastActive);
            else if (app->windows.count != 0)
                bringForward(app->windows.data[app->windows.count - 1]);
            break;
        default:
            break;
    }
}

// Posts one DisplaysChanged event for the messages that each window receives about a change.
void noteDisplaysChanged(Win32App* app) {
    if (app->displaysChanged || app->messageWindow == nullptr)
        return;
    app->displaysChanged = true;
    PostMessageW(app->messageWindow, kMessageDisplaysChanged, 0, 0);
}

// The procedure of the application's message window.
struct Win32Request;
void answerRequest(Win32App* app, Win32Request* request);
void hotkeyPressed(Win32App* app, int id);
void showDialog(Win32App* app, Win32DialogRequest* request);
bool serviceMessage(Win32App* app, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result);
void lifecycleMayChange(Win32App* app);
void keyLabelsReset(Win32App* app);
void deliverKind(Win32App* app, PrAppEventKind kind);
void applyTaskbar(Win32Window* window);
void closePopups(Win32App* app, Win32Window* PR_NULLABLE except);
void menuOpening(Win32App* app, Win32Window* PR_NULLABLE window, HMENU menu);
void menuCommand(Win32App* app, Win32Window* PR_NULLABLE window, UINT id);
void showContextMenu(Win32Window* window);
void statusItemAction(Win32App* app, UINT id, UINT action);
bool translateAccelerator(Win32App* app, MSG& message);
void setBlocking(Win32Window* window, bool blocking);
void setPasswordInput(Win32Window* window, bool password);
void setRawInput(Win32Window* window, bool raw);
void menuTreeReset(Win32MenuTree& tree);
void showMenuBar(Win32Window* window);
HICON PR_NULLABLE iconOf(PrPixelView pixels);
void copyField(Win32App* app, PrStringView text, wchar_t* out, size_t capacity);
void closeCancelledDialog(Win32App* app);

LRESULT CALLBACK messageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    Win32App* app = reinterpret_cast<Win32App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app == nullptr)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == app->taskbarCreated && app->taskbarCreated != 0)
        app->notifyIcon = false;
    LRESULT handled = 0;
    if (serviceMessage(app, message, wParam, lParam, &handled))
        return handled;
    switch (message) {
        case kMessageFrame:
            // The run's loop holds the pacer's message back, so another loop dispatched it: the system's modal loop for
            // a move or a resize. A frame that waits behind input goes to the window's frame timer, whose message comes
            // only when no other waits, so that the loop sees the input, such as the button's release that ends a
            // resize, even where a frame takes longer than a refresh.
            if (HIWORD(GetQueueStatus(QS_INPUT)) == 0) {
                runRequestedFrames(app);
                return 0;
            }
            for (uint32_t i = 0; i < app->windows.count; ++i) {
                Win32Window* window = app->windows.data[i];
                if (window->frameRequested)
                    SetTimer(window->window, kTimerFrame, USER_TIMER_MINIMUM, nullptr);
            }
            return 0;
        case kMessageWake: {
            app->wakePending.store(false);
            PrAppEvent event{};
            event.kind = PrAppEventKind_Wake;
            event.time = secondsNow();
            deliver(app, nullptr, event);
            return 0;
        }
        case kMessageNotifyIcon:
            // With NOTIFYICON_VERSION_4, the low word of lParam is the user's action.
            notifyIconAction(app, LOWORD(lParam));
            return 0;
        case kMessageStatusItem:
            statusItemAction(app, HIWORD(lParam), LOWORD(lParam));
            return 0;
        case WM_INITMENUPOPUP:
            menuOpening(app, nullptr, reinterpret_cast<HMENU>(wParam));
            return 0;
        case WM_COMMAND:
            menuCommand(app, nullptr, LOWORD(wParam));
            return 0;
        case kMessageDisplaysChanged: {
            app->displaysChanged = false;
            // The names that the last event's displays had stay valid until this event, which may change them.
            displayNamesReset(app);
            PrAppEvent event{};
            event.kind = PrAppEventKind_DisplaysChanged;
            event.time = secondsNow();
            deliver(app, nullptr, event);
            return 0;
        }
        case kMessageRead:
            answerRequest(app, reinterpret_cast<Win32Request*>(lParam));
            return 0;
        case WM_HOTKEY:
            hotkeyPressed(app, int(wParam));
            return 0;
        case kMessageDialog:
            // A dialog's modal loop waits for the callback that runs, so that no callback runs within another.
            if (app->inCallback || app->dialog != nullptr)
                PostMessageW(hwnd, kMessageDialog, wParam, lParam);
            else
                showDialog(app, reinterpret_cast<Win32DialogRequest*>(lParam));
            return 0;
        case kMessageDialogCancel:
            closeCancelledDialog(app);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    Win32Window* window = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (window == nullptr)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    Win32App* app = appOf(window);
    if (message == app->taskbarButtonCreated && app->taskbarButtonCreated != 0) {
        applyTaskbar(window);
        return 0;
    }
    switch (message) {
        case WM_CLOSE: {
            // The program closes the window if it chooses to.
            PrAppEvent event{};
            event.kind = PrAppEventKind_CloseRequested;
            event.time = secondsNow();
            deliver(window, event);
            return 0;
        }
        case WM_ERASEBKGND:
            // The frames fill the window, and an erase would flash between them.
            return 1;
        case WM_SIZE:
            // A frame at each step of a resize presents at the window's new size before the compositor shows it. A
            // minimized window has no frame to run, and its next frame sees the new state.
            if (wParam != SIZE_MINIMIZED)
                runFrame(window);
            else
                requestFrame(window);
            lifecycleMayChange(app);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            if (window->frameRequested && !window->inSizeMove)
                runFrame(window);
            return 0;
        }
        case WM_NCCALCSIZE:
            // With a custom title bar, the content covers the caption, and keeps the system's resize borders at the
            // sides and the bottom. A maximized window overhangs its display by its border, which its content leaves
            // out.
            if (window->customTitleBar && !window->fullscreen && wParam == TRUE) {
                NCCALCSIZE_PARAMS* sizes = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                LONG top = sizes->rgrc[0].top;
                DefWindowProcW(hwnd, message, wParam, lParam);
                sizes->rgrc[0].top = top;
                if (IsZoomed(hwnd))
                    sizes->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, window->dpi) +
                        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, window->dpi);
                return 0;
            }
            break;
        case WM_NCHITTEST:
            if (window->customTitleBar && !window->fullscreen)
                return customHitTest(window, wParam, lParam);
            break;
        case WM_NCLBUTTONDOWN:
            if (window->customTitleBar && (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)) {
                window->pressedButton = LRESULT(wParam);
                return 0;
            }
            break;
        case WM_NCLBUTTONUP:
            if (window->customTitleBar && (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)) {
                customButtonReleased(window, LRESULT(wParam));
                return 0;
            }
            break;
        case WM_NCMOUSEMOVE:
            // The pointer over a custom title bar is in the program's content, which shows a button's hover, and which
            // the pointer leaves only as it leaves the title bar too.
            if (window->customTitleBar &&
                    (wParam == HTCAPTION || wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)) {
                POINT at{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
                ScreenToClient(hwnd, &at);
                deliverPointer(window, WM_MOUSEMOVE, 0, MAKELPARAM(at.x, at.y), true);
                if (wParam != HTCAPTION)
                    return 0;
            }
            break;
        case WM_MOUSELEAVE:
            pointerLeft(window, false);
            return 0;
        case WM_NCMOUSELEAVE:
            pointerLeft(window, true);
            break;
        case WM_GETMINMAXINFO: {
            // The system's least width fits the title bar's buttons, and the window may be as small as its frame around
            // its least extent.
            UINT dpi = window->dpi != 0 ? window->dpi : 96;
            MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            PrUVec2 least{
                window->minExtent.x > 1 ? window->minExtent.x : 1,
                window->minExtent.y > 1 ? window->minExtent.y : 1,
            };
            limits->ptMinTrackSize = frameSizeOf(window, least, dpi);
            if (window->maxExtent.x != 0 || window->maxExtent.y != 0) {
                POINT most = frameSizeOf(window, window->maxExtent, dpi);
                if (window->maxExtent.x != 0)
                    limits->ptMaxTrackSize.x = most.x;
                if (window->maxExtent.y != 0)
                    limits->ptMaxTrackSize.y = most.y;
            }
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            window->inSizeMove = true;
            break;
        case WM_EXITSIZEMOVE:
            window->inSizeMove = false;
            break;
        case WM_MOVE:
            if (app->running && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != window->monitor)
                requestFrame(window);
            break;
        case WM_DPICHANGED: {
            window->dpi = HIWORD(wParam);
            noteDisplaysChanged(app);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            requestFrame(window);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            releaseDisplayObjects(window);
            requestFrame(window);
            noteDisplaysChanged(app);
            break;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA)
                noteDisplaysChanged(app);
            [[fallthrough]];
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            refreshPreferences(window);
            requestFrame(window);
            break;
        case WM_SETCURSOR:
            // A locked pointer hides its cursor.
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(window->locked ? nullptr : window->cursor);
                return TRUE;
            }
            break;
        case WM_SYSCOMMAND:
            // Alt or F10 alone would enter the menu loop of a window that has no menu, which takes the next key.
            if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0)
                return 0;
            break;
        case WM_TIMER:
            if (wParam == kTimerFrameAt) {
                KillTimer(hwnd, kTimerFrameAt);
                requestFrame(window);
                return 0;
            }
            if (wParam != kTimerFrame)
                break;
            KillTimer(hwnd, kTimerFrame);
            if (window->frameRequested)
                runFrame(window);
            return 0;
        case kMessageDragStart:
            // A callback that runs a modal loop dispatches the message within it, and the drag waits for its return.
            if (appOf(window)->inCallback)
                PostMessageW(hwnd, kMessageDragStart, 0, 0);
            else
                startDrag(window);
            return 0;
        case kMessageShow:
            // The window shows after a frame, which waits for the callback that runs.
            if (!window->showing)
                return 0;
            if (appOf(window)->inCallback) {
                PostMessageW(hwnd, kMessageShow, 0, 0);
                return 0;
            }
            uint64_t serial = window->serial;
            runFrame(window);
            if (isOpen(app, window, serial) && window->showing) {
                window->showing = false;
                bool passive = window->overlay || window->role == PrAppWindowRole_Popup;
                ShowWindow(hwnd, passive ? SW_SHOWNOACTIVATE : SW_SHOW);
                setBlocking(window, true);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hwnd)
                releaseButtons(window);
            return 0;
        case WM_KILLFOCUS:
            releaseKeys(window);
            break;
        case WM_ACTIVATEAPP:
            if (!wParam)
                closePopups(app, nullptr);
            lifecycleMayChange(app);
            break;
        case WM_INPUT:
            if (window->rawInput)
                deliverRawInput(window, lParam);
            break;
        case WM_INITMENUPOPUP:
            menuOpening(app, window, reinterpret_cast<HMENU>(wParam));
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == 0 || HIWORD(wParam) == 1) {
                menuCommand(app, window, LOWORD(wParam));
                return 0;
            }
            break;
        case kMessageShowMenu:
            // The menu's modal loop waits for the callback that runs.
            if (app->inCallback) {
                PostMessageW(hwnd, kMessageShowMenu, 0, 0);
                return 0;
            }
            showContextMenu(window);
            return 0;
        case WM_SIZING:
            if (window->aspectRatio > 0) {
                keepAspectRatio(window, wParam, reinterpret_cast<RECT*>(lParam));
                return TRUE;
            }
            break;
        case WM_INPUTLANGCHANGE:
            keyLabelsReset(app);
            deliverKind(app, PrAppEventKind_KeyboardLayoutChanged);
            break;
        case WM_ACTIVATE:
            // Activation changes what prAppWindowIsActive returns, so the next frame should see it.
            if (LOWORD(wParam) != WA_INACTIVE)
                app->lastActive = window;
            else
                setLock(window, false);
            requestFrame(window);
            break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            // A press outside a popup closes it.
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)
                closePopups(app, window);
            deliverPointer(window, message, wParam, lParam);
            return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            deliverPointer(window, message, wParam, lParam);
            return TRUE;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            deliverScroll(window, wParam, lParam, message == WM_MOUSEHWHEEL);
            return 0;
        case WM_POINTERDOWN:
        case WM_POINTERUP:
        case WM_POINTERUPDATE:
        case WM_POINTERCAPTURECHANGED:
            // A pen's or a touch's message handled here makes no mouse message.
            if (deliverPenOrTouch(window, message, wParam))
                return 0;
            break;
        case WM_KEYDOWN:
        case WM_KEYUP:
            deliverKey(window, message, wParam, lParam);
            return 0;
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            // The system keeps its own chords, such as Alt+F4.
            deliverKey(window, message, wParam, lParam);
            break;
        case WM_CHAR:
            deliverCharacter(window, wchar_t(wParam));
            return 0;
        case WM_SYSCHAR:
            // An Alt chord types nothing. Alt+Space opens the window's menu.
            if (wParam == L' ')
                break;
            return 0;
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
            return 0;
        case WM_UNICHAR:
            if (wParam == UNICODE_NOCHAR)
                return TRUE;
            deliverCodePoint(window, uint32_t(wParam));
            return 0;
        case WM_IME_SETCONTEXT:
            // The program shows the composition, and the input method shows only its candidates.
            lParam &= ~LPARAM(ISC_SHOWUICOMPOSITIONWINDOW);
            break;
        case WM_IME_STARTCOMPOSITION:
            placeInputMethod(window);
            return 0;
        case WM_IME_COMPOSITION:
            deliverComposition(window, lParam);
            return 0;
        case WM_IME_ENDCOMPOSITION:
            if (window->composing) {
                window->composing = false;
                deliverText(window, PrAppEventKind_CompositionEnd, "", PrAppTextRange{});
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void registerClasses(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(instance, kClassName, &existing)) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = instance;
        windowClass.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(IDI_APPLICATION)));
        windowClass.lpszClassName = kClassName;
        RegisterClassExW(&windowClass);
    }
    if (!GetClassInfoExW(instance, kMessageClassName, &existing)) {
        WNDCLASSEXW messageClass{};
        messageClass.cbSize = sizeof(messageClass);
        messageClass.lpfnWndProc = messageProc;
        messageClass.hInstance = instance;
        messageClass.lpszClassName = kMessageClassName;
        RegisterClassExW(&messageClass);
    }
}

// Where placeWindow puts a window that is shown: centered on its display's work area, or where it already is.
enum {
    Placement_Center,
    Placement_Keep,
};
typedef uint8_t Placement;

// Sizes the window's client area to its extent. A window that is shown is clamped to its display's work area, and
// centered on it if `placement` is Center.
void placeWindow(Win32Window* window, Placement placement) {
    Win32App* app = appOf(window);
    window->extent = limitedExtent(window, window->extent);
    POINT size = frameSizeOf(window, window->extent, window->dpi);
    int width = size.x;
    int height = size.y;
    int x = 0;
    int y = 0;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (app->headless || !GetMonitorInfoW(MonitorFromWindow(window->window, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        flags |= SWP_NOMOVE;
    } else {
        RECT work = monitor.rcWork;
        width = width < work.right - work.left ? width : work.right - work.left;
        height = height < work.bottom - work.top ? height : work.bottom - work.top;
        x = work.left + (work.right - work.left - width) / 2;
        y = work.top + (work.bottom - work.top - height) / 2;
        if (placement == Placement_Keep)
            flags |= SWP_NOMOVE;
    }
    SetWindowPos(window->window, nullptr, x, y, width, height, flags);
}

// ---------------------------------------------------------------------------------------------------------------------
// The windows' functions

void windowDestroy(PrAppWindow* base) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (appWindowDeliveryDeferDestroy(window->delivery))
        return;
    Win32App* app = appOf(window);
    forgetWindow(app, window);
    forgetTextInput(window);
    // A file dialog of the window closes as the user's cancel, and a locked pointer is released.
    if (app->dialog != nullptr && app->dialogParent == window->window)
        app->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    if (window->locked)
        ClipCursor(nullptr);
    setBlocking(window, false);
    setPasswordInput(window, false);
    setRawInput(window, false);
    // A window destroys the menu bar that it shows, which the tree frees instead.
    SetMenu(window->window, nullptr);
    menuTreeReset(window->menuBar);
    menuTreeReset(window->contextMenu);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i]->parent == window)
            app->windows.data[i]->parent = nullptr;
    }
    KillTimer(window->window, kTimerFrame);
    KillTimer(window->window, kTimerFrameAt);
    if (GetCapture() == window->window)
        ReleaseCapture();
    if (app->drag.window == window)
        app->drag.window = nullptr;
    if (app->ole)
        RevokeDragDrop(window->window);
    appDragPendingReset(window->drag);
    releaseDisplayObjects(window);
    appTitleBarRegionsReset(window->titleBar);
    prRawStringReset(&window->titleText);
    SetWindowLongPtrW(window->window, GWLP_USERDATA, 0);
    DestroyWindow(window->window);
    if (window->icon != nullptr)
        DestroyIcon(window->icon);
    prMemoryFree(window->title);
    prMemoryFree(window);
}

PrApp* windowApp(const PrAppWindow* base) {
    return static_cast<const Win32Window*>(base)->app;
}

PrAppFrame frameState(PrAppWindow* base) {
    return frameAt(static_cast<Win32Window*>(base), secondsNow());
}

void requestFrame(PrAppWindow* base) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (!appOf(window)->running || window->frameRequested)
        return;
    window->frameRequested = true;
    scheduleFrame(window);
}

void requestFrameAt(PrAppWindow* base, double time) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (!appOf(window)->running)
        return;
    if (time == 0) {
        KillTimer(window->window, kTimerFrameAt);
        return;
    }
    double delay = time - secondsNow();
    // The timer rounds up, so that the frame finds the time reached.
    UINT milliseconds = delay > 0 ? UINT(delay * 1000) + 1 : USER_TIMER_MINIMUM;
    SetTimer(window->window, kTimerFrameAt, milliseconds, nullptr);
}

void setCursor(PrAppWindow* base, PrAppCursor cursor) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (cursor == window->shape)
        return;
    window->shape = cursor;
    HCURSOR selected = cursorFor(cursor);
    window->cursor = selected;
    // WM_SETCURSOR applies the shape on the pointer's next move, and a pointer that rests over the window takes it now.
    POINT point;
    RECT client;
    if (GetCursorPos(&point) && WindowFromPoint(point) == window->window && ScreenToClient(window->window, &point) &&
            GetClientRect(window->window, &client) && PtInRect(&client, point))
        SetCursor(selected);
}

void setTextInputRect(Win32Window* window, PrRect rect) {
    window->textInputRect = rect;
    placeInputMethod(window);
}

// Returns the window whose latest output holds the text being edited in `window`: the window itself if its output has
// some there, or else a window that owns it whose output names it. Returns null if no output does.
const Win32Window* PR_NULLABLE textInputSource(Win32Window* window) {
    if (window->textInputWindow == window)
        return window;
    Win32App* app = appOf(window);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i]->textInputWindow == window)
            return app->windows.data[i];
    }
    return nullptr;
}

// Applies the text being edited in the window: where the input method opens its windows, and whether the input method
// is set aside for a password.
void applyTextInput(Win32Window* window) {
    const Win32Window* source = textInputSource(window);
    if (source != nullptr)
        setTextInputRect(window, source->textInput);
    setPasswordInput(window, source != nullptr && source->textInputKind == PrAppTextInputKind_Password);
}

// Withdraws the text input that `window`, which is being destroyed, names in another window, and the text input that
// other windows name in it.
void forgetTextInput(Win32Window* window) {
    PrAppWindow* named = window->textInputWindow;
    window->textInputWindow = nullptr;
    if (named != nullptr && named != window)
        applyTextInput(static_cast<Win32Window*>(named));
    Win32App* app = appOf(window);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i]->textInputWindow == window)
            app->windows.data[i]->textInputWindow = nullptr;
    }
}

// Applies the output of the window's frame. The text input applies to the window that the output names, which may be
// another than the one before.
void applyOutput(Win32Window* window, const PrAppOutput& output) {
    setCursor(window, output.cursor);
    PrAppWindow* before = window->textInputWindow;
    PrAppWindow* named = output.textInputWindow != nullptr ? output.textInputWindow : window;
    window->textInputWindow = output.hasTextInput ? named : nullptr;
    window->textInputKind = output.textInputKind;
    window->textInput = output.textInput;
    applyTextInput(window);
    if (before != nullptr && before != window)
        applyTextInput(static_cast<Win32Window*>(before));
    PrAppWindow* now = window->textInputWindow;
    if (now != nullptr && now != window && now != before)
        applyTextInput(static_cast<Win32Window*>(now));
    if (output.wantsFrame)
        requestFrame(window);
    requestFrameAt(window, output.frameTime);
    appTitleBarRegionsSet(window->titleBar, output);
    window->dropOperation = output.dropOperation;
}

void setTitle(PrAppWindow* base, PrStringView title) {
    Win32Window* window = static_cast<Win32Window*>(base);
    RawArray<wchar_t> wide;
    win32Widen(title, wide);
    prMemoryFree(window->title);
    window->title = wide.data;
    prRawStringReset(&window->titleText);
    window->titleText = prRawStringCopy(title);
    SetWindowTextW(window->window, window->title);
    requestFrame(window);
}

PrStringView title(const PrAppWindow* base) {
    return prRawStringView(&static_cast<const Win32Window*>(base)->titleText);
}

void setExtent(PrAppWindow* base, PrUVec2 extent) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->extent = extent;
    placeWindow(window, Placement_Keep);
}

void setExtentLimits(PrAppWindow* base, PrUVec2 minExtent, PrUVec2 maxExtent) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->minExtent = minExtent;
    window->maxExtent = maxExtent;
    if (window->fullscreen)
        return;
    SetWindowLongPtrW(window->window, GWL_STYLE, LONG_PTR(windowStyle(window)));
    SetWindowPos(window->window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    PrVec2 now = extentOf(window);
    PrUVec2 current{uint32_t(now.x + 0.5f), uint32_t(now.y + 0.5f)};
    PrUVec2 limited = limitedExtent(window, current);
    if (limited.x != current.x || limited.y != current.y) {
        window->extent = limited;
        placeWindow(window, Placement_Keep);
    }
}

// Moves the window's content to `position` on `monitor`, in its logical pixels, keeping the window within the monitor's
// work area.
void placeOnMonitor(Win32Window* window, const Monitor& monitor, PrVec2 position) {
    POINT size = frameSizeOf(window, window->extent, monitor.dpi);
    RECT frame{
        0, 0, MulDiv(int(window->extent.x), int(monitor.dpi), 96),
        MulDiv(int(window->extent.y), int(monitor.dpi), 96),
    };
    AdjustWindowRectExForDpi(&frame, windowStyle(window), FALSE, 0, monitor.dpi);
    // The frame's left and top are negative: the content's offset into the frame.
    int x = monitor.bounds.left + MulDiv(int(position.x), int(monitor.dpi), 96) + frame.left;
    int y = monitor.bounds.top + MulDiv(int(position.y), int(monitor.dpi), 96) + frame.top;
    x = x + size.x > monitor.work.right ? monitor.work.right - size.x : x;
    y = y + size.y > monitor.work.bottom ? monitor.work.bottom - size.y : y;
    x = x < monitor.work.left ? monitor.work.left : x;
    y = y < monitor.work.top ? monitor.work.top : y;
    SetWindowPos(window->window, nullptr, x, y, size.x, size.y, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Centers the window's content on the work area of `monitor`.
void centerOnMonitor(Win32Window* window, const Monitor& monitor) {
    PrRect work = logicalOn(monitor, monitor.work);
    PrVec2 position{
        work.left + (work.right - work.left - float(window->extent.x)) / 2,
        work.top + (work.bottom - work.top - float(window->extent.y)) / 2,
    };
    placeOnMonitor(window, monitor, position);
}

bool position(const PrAppWindow* base, PrAppDisplayPosition* out) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    Monitor monitor{};
    POINT origin{0, 0};
    if (appOf(window)->headless || !ClientToScreen(window->window, &origin) ||
            !monitorOf(MonitorFromWindow(window->window, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    PrRect at = logicalOn(monitor, RECT{origin.x, origin.y, origin.x, origin.y});
    *out = PrAppDisplayPosition{.display = monitor.id, .position = PrVec2{at.left, at.top}};
    return true;
}

// The window's logical pixels are at its own DPI, and a display's at the display's.
bool pointToDisplay(const PrAppWindow* base, PrVec2 point, PrAppDisplayPosition* out) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    POINT origin{0, 0};
    if (appOf(window)->headless || !ClientToScreen(window->window, &origin))
        return false;
    float scale = float(window->dpi) / USER_DEFAULT_SCREEN_DPI;
    float x = float(origin.x) + point.x * scale;
    float y = float(origin.y) + point.y * scale;
    Monitor monitor{};
    POINT at{LONG(x), LONG(y)};
    if (!monitorOf(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    float toMonitor = USER_DEFAULT_SCREEN_DPI / float(monitor.dpi);
    *out = PrAppDisplayPosition{
        .display = monitor.id,
        .position = PrVec2{(x - float(monitor.bounds.left)) * toMonitor, (y - float(monitor.bounds.top)) * toMonitor},
    };
    return true;
}

bool pointFromDisplay(const PrAppWindow* base, const PrAppDisplayPosition* position, PrVec2* out) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    POINT origin{0, 0};
    Monitor monitor{};
    if (appOf(window)->headless || !ClientToScreen(window->window, &origin) || !monitorWithId(position->display, &monitor))
        return false;
    float fromMonitor = float(monitor.dpi) / USER_DEFAULT_SCREEN_DPI;
    float x = float(monitor.bounds.left) + position->position.x * fromMonitor;
    float y = float(monitor.bounds.top) + position->position.y * fromMonitor;
    float toWindow = USER_DEFAULT_SCREEN_DPI / float(window->dpi);
    *out = PrVec2{(x - float(origin.x)) * toWindow, (y - float(origin.y)) * toWindow};
    return true;
}

void setPosition(PrAppWindow* base, const PrAppDisplayPosition* position) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (appOf(window)->headless || window->fullscreen)
        return;
    if (IsZoomed(window->window) || IsIconic(window->window))
        ShowWindow(window->window, SW_RESTORE);
    Monitor monitor{};
    if (monitorWithId(position->display, &monitor)) {
        placeOnMonitor(window, monitor, position->position);
        return;
    }
    // A display that is gone gives the primary display, at the same position if the window fits there.
    if (!primaryMonitor(&monitor))
        return;
    PrRect work = logicalOn(monitor, monitor.work);
    bool fits = position->position.x >= work.left && position->position.y >= work.top &&
        position->position.x + float(window->extent.x) <= work.right &&
        position->position.y + float(window->extent.y) <= work.bottom;
    if (fits)
        placeOnMonitor(window, monitor, position->position);
    else
        centerOnMonitor(window, monitor);
}

PrAppWindowState state(const PrAppWindow* base) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    if (appOf(window)->headless)
        return PrAppWindowState_Normal;
    if (window->fullscreen)
        return PrAppWindowState_Fullscreen;
    if (IsIconic(window->window))
        return PrAppWindowState_Minimized;
    return IsZoomed(window->window) ? PrAppWindowState_Maximized : PrAppWindowState_Normal;
}

// Covers the window's display with its content, without a frame, or returns it to where it stood before.
void setFullscreen(Win32Window* window, bool fullscreen) {
    if (fullscreen) {
        window->restore.length = sizeof(window->restore);
        GetWindowPlacement(window->window, &window->restore);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(window->window, MONITOR_DEFAULTTONEAREST), &monitor);
        window->fullscreen = true;
        SetWindowLongPtrW(window->window, GWL_STYLE, LONG_PTR(windowStyle(window)));
        RECT area = monitor.rcMonitor;
        SetWindowPos(window->window, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        window->fullscreen = false;
        SetWindowLongPtrW(window->window, GWL_STYLE, LONG_PTR(windowStyle(window) | WS_VISIBLE));
        SetWindowPlacement(window->window, &window->restore);
        SetWindowPos(window->window, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    requestFrame(window);
}

void setState(PrAppWindow* base, PrAppWindowState wanted) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (appOf(window)->headless || state(window) == wanted)
        return;
    if (window->fullscreen)
        setFullscreen(window, false);
    switch (wanted) {
        case PrAppWindowState_Normal:
            if (IsIconic(window->window) || IsZoomed(window->window))
                ShowWindow(window->window, SW_RESTORE);
            break;
        case PrAppWindowState_Minimized:
            ShowWindow(window->window, SW_MINIMIZE);
            break;
        case PrAppWindowState_Maximized:
            if (!isFixed(window))
                ShowWindow(window->window, SW_MAXIMIZE);
            break;
        case PrAppWindowState_Fullscreen:
            if (IsIconic(window->window) || IsZoomed(window->window))
                ShowWindow(window->window, SW_RESTORE);
            setFullscreen(window, true);
            break;
        default:
            break;
    }
}

bool isActive(const PrAppWindow* base) {
    return GetForegroundWindow() == static_cast<const Win32Window*>(base)->window;
}

// Focusing shows a hidden window, and the windows of a hidden application.
void setHidden(PrApp* base, bool hidden);

void focus(PrAppWindow* base) {
    Win32Window* window = static_cast<Win32Window*>(base);
    Win32App* app = appOf(window);
    window->shown = true;
    window->showing = false;
    if (app->headless)
        return;
    if (app->hidden)
        setHidden(app, false);
    if (!IsWindowVisible(window->window))
        ShowWindow(window->window, SW_SHOW);
    bringForward(window);
}

void requestAttention(PrAppWindow* base) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (GetForegroundWindow() == window->window)
        return;
    // The taskbar button flashes until the window comes to the foreground.
    FLASHWINFO flash{.cbSize = sizeof(FLASHWINFO), .hwnd = window->window, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
    FlashWindowEx(&flash);
}

// Records the drag and posts its start to the window, whose procedure starts it once no callback runs.
void dragStart(PrAppWindow* base, const PrAppDragDesc* desc) {
    Win32Window* window = static_cast<Win32Window*>(base);
    appDragPendingSet(window->drag, *desc);
    PostMessageW(window->window, kMessageDragStart, 0, 0);
}

void setVisible(PrAppWindow* base, bool visible) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (window->shown == visible)
        return;
    window->shown = visible;
    window->hiddenByApp = false;
    if (appOf(window)->headless)
        return;
    if (visible) {
        window->showing = true;
        PostMessageW(window->window, kMessageShow, 0, 0);
        return;
    }
    window->showing = false;
    setLock(window, false);
    setBlocking(window, false);
    ShowWindow(window->window, SW_HIDE);
}

bool isVisible(const PrAppWindow* base) {
    return static_cast<const Win32Window*>(base)->shown;
}

// The pointer passes through a layered window that is transparent to hit tests. The window stays opaque.
void setPointerPassthrough(PrAppWindow* base, bool passthrough) {
    Win32Window* window = static_cast<Win32Window*>(base);
    LONG_PTR style = GetWindowLongPtrW(window->window, GWL_EXSTYLE);
    if (passthrough)
        style |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
    else
        style &= ~LONG_PTR(WS_EX_LAYERED | WS_EX_TRANSPARENT);
    SetWindowLongPtrW(window->window, GWL_EXSTYLE, style);
    if (passthrough)
        SetLayeredWindowAttributes(window->window, 0, 255, LWA_ALPHA);
}

// The window locks the pointer for Locked and LockedRaw alike, and confines it within its client area for Confined.
void setPointerMode(PrAppWindow* base, PrAppPointerMode mode) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->pointerMode = mode;
    setLock(window, mode == PrAppPointerMode_Locked || mode == PrAppPointerMode_LockedRaw);
    if (mode == PrAppPointerMode_Confined && GetForegroundWindow() == window->window) {
        RECT client{};
        GetClientRect(window->window, &client);
        MapWindowPoints(window->window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        ClipCursor(&client);
    } else if (!window->locked) {
        ClipCursor(nullptr);
    }
}

PrAppPointerMode pointerMode(const PrAppWindow* base) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    if (window->locked || (window->pointerMode == PrAppPointerMode_Confined && GetForegroundWindow() == window->window))
        return window->pointerMode;
    return PrAppPointerMode_Free;
}

void setAspectRatio(PrAppWindow* base, float ratio) {
    static_cast<Win32Window*>(base)->aspectRatio = ratio > 0 ? ratio : 0;
}

// Keeps the content's aspect ratio while the user drags an edge of the frame. `edge` is WM_SIZING's, and `rect` the
// frame in screen pixels, which this adjusts.
void keepAspectRatio(Win32Window* window, WPARAM edge, RECT* rect) {
    RECT border{};
    AdjustWindowRectExForDpi(&border, DWORD(GetWindowLongPtrW(window->window, GWL_STYLE)), FALSE,
        DWORD(GetWindowLongPtrW(window->window, GWL_EXSTYLE)), window->dpi);
    LONG borderWidth = border.right - border.left;
    LONG borderHeight = border.bottom - border.top;
    LONG width = rect->right - rect->left - borderWidth;
    LONG height = rect->bottom - rect->top - borderHeight;
    if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
        LONG wanted = LONG(float(width) / window->aspectRatio + 0.5f);
        rect->bottom = rect->top + wanted + borderHeight;
        return;
    }
    LONG wanted = LONG(float(height) * window->aspectRatio + 0.5f);
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
        rect->left = rect->right - wanted - borderWidth;
    else
        rect->right = rect->left + wanted + borderWidth;
}

// Moves a popup to where its placement puts it against its parent, within the parent's work area.
void placePopup(Win32Window* window) {
    Win32Window* parent = window->parent;
    if (window->role != PrAppWindowRole_Popup || parent == nullptr || appOf(window)->headless)
        return;
    Monitor monitor{};
    if (!monitorOf(MonitorFromWindow(parent->window, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    float scale = float(parent->dpi) / 96.0f;
    POINT origin{0, 0};
    ClientToScreen(parent->window, &origin);
    const PrRect& a = window->popup.anchor;
    PrRect anchor{
        float(origin.x) + a.left * scale, float(origin.y) + a.top * scale, float(origin.x) + a.right * scale,
        float(origin.y) + a.bottom * scale,
    };
    PrAppPopupPlacement placement = window->popup;
    placement.offset = PrVec2{placement.offset.x * scale, placement.offset.y * scale};
    RECT client{};
    GetClientRect(window->window, &client);
    PrVec2 extent{float(client.right - client.left), float(client.bottom - client.top)};
    PrRect work{
        float(monitor.work.left), float(monitor.work.top), float(monitor.work.right),
        float(monitor.work.bottom),
    };
    bool rightToLeft = (GetWindowLongPtrW(parent->window, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    placement.anchor = anchor;
    PrRect placed = prAppPopupRect(&placement, extent, work,
        PrAppLayoutDirection(rightToLeft ? PrAppLayoutDirection_RightToLeft : PrAppLayoutDirection_LeftToRight));
    SetWindowPos(window->window, nullptr, int(placed.left), int(placed.top), int(placed.right - placed.left),
        int(placed.bottom - placed.top), SWP_NOZORDER | SWP_NOACTIVATE);
}

void setPopupPlacement(PrAppWindow* base, const PrAppPopupPlacement* placement) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->popup = *placement;
    placePopup(window);
}

// Asks each popup to close, but `except`, as for a press elsewhere or the application's deactivation.
void closePopups(Win32App* app, Win32Window* PR_NULLABLE except) {
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (window == except || window->role != PrAppWindowRole_Popup || !IsWindowVisible(window->window))
            continue;
        PrAppEvent event{};
        event.kind = PrAppEventKind_CloseRequested;
        event.time = secondsNow();
        deliver(window, event);
    }
}

// Disables a modal dialog's parent while the dialog shows, or enables it again, and gives it back the keyboard.
void setBlocking(Win32Window* window, bool blocking) {
    if (!window->modal || window->parent == nullptr || window->blocking == blocking)
        return;
    window->blocking = blocking;
    EnableWindow(window->parent->window, blocking ? FALSE : TRUE);
    if (!blocking)
        SetActiveWindow(window->parent->window);
}

// Windows tells only that a window is hidden, minimized or cloaked on another desktop, and not that other windows cover
// it.
bool isOccluded(const PrAppWindow* base) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    if (appOf(window)->headless)
        return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(window->window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked || IsIconic(window->window) || !IsWindowVisible(window->window);
}

void setKeepAbove(PrAppWindow* base, bool above) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (window->role != PrAppWindowRole_Normal)
        return;
    SetWindowPos(window->window, above ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Returns an icon of straight-alpha sRGB Rgba8 pixels, or null for none.
HICON PR_NULLABLE iconOf(PrPixelView pixels) {
    if (pixels.extent.x == 0 || pixels.extent.y == 0 || pixels.data == nullptr)
        return nullptr;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = LONG(pixels.extent.x);
    header.bV5Height = -LONG(pixels.extent.y);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (color == nullptr)
        return nullptr;
    uint8_t* out = static_cast<uint8_t*>(bits);
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(pixels.data) + size_t(y) * pixels.stride;
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            const uint8_t* in = row + size_t(x) * 4;
            uint8_t* pixel = out + (size_t(y) * pixels.extent.x + x) * 4;
            pixel[0] = in[2];
            pixel[1] = in[1];
            pixel[2] = in[0];
            pixel[3] = in[3];
        }
    }
    HBITMAP mask = CreateBitmap(int(pixels.extent.x), int(pixels.extent.y), 1, 1, nullptr);
    ICONINFO info{.fIcon = TRUE, .hbmMask = mask, .hbmColor = color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void setIcon(PrAppWindow* base, PrPixelView pixels) {
    Win32Window* window = static_cast<Win32Window*>(base);
    HICON icon = iconOf(pixels);
    SendMessageW(window->window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(window->window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    if (window->icon != nullptr)
        DestroyIcon(window->icon);
    window->icon = icon;
}

void setProgress(PrAppWindow* base, PrAppProgressState state, float progress) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->progressState = state;
    window->progress = progress;
    applyTaskbar(window);
}

// A password bypasses the input method, which the window sets aside while one is edited.
void setPasswordInput(Win32Window* window, bool password) {
    if (password && window->inputContext == nullptr) {
        window->inputContext = ImmAssociateContext(window->window, nullptr);
    } else if (!password && window->inputContext != nullptr) {
        ImmAssociateContext(window->window, window->inputContext);
        window->inputContext = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Menus

// Returns the virtual key of `key`, or zero for none.
UINT virtualKeyOfKey(PrAppKey key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return UINT(key);
    if (key > ' ' && key < 127) {
        SHORT scan = VkKeyScanW(wchar_t(key));
        return scan != -1 ? UINT(scan & 0xFF) : 0;
    }
    if (key >= PrAppKey_F1 && key <= PrAppKey_F20)
        return VK_F1 + UINT(key - PrAppKey_F1);
    switch (key) {
        case PrAppKey_Enter: return VK_RETURN;
        case PrAppKey_Escape: return VK_ESCAPE;
        case PrAppKey_Backspace: return VK_BACK;
        case PrAppKey_Tab: return VK_TAB;
        case PrAppKey_Delete: return VK_DELETE;
        case PrAppKey_Insert: return VK_INSERT;
        case PrAppKey_Left: return VK_LEFT;
        case PrAppKey_Right: return VK_RIGHT;
        case PrAppKey_Up: return VK_UP;
        case PrAppKey_Down: return VK_DOWN;
        case PrAppKey_Home: return VK_HOME;
        case PrAppKey_End: return VK_END;
        case PrAppKey_PageUp: return VK_PRIOR;
        case PrAppKey_PageDown: return VK_NEXT;
        default: return 0;
    }
}

// What Windows does for an item of a role: the title and the key of an item that the program leaves without them.
struct Win32MenuRole {
    PrAppMenuRole role;
    const char* title;
    PrAppKey key;
    PrAppModifierFlags modifiers;
};

const Win32MenuRole* PR_NULLABLE menuRoleOf(PrAppMenuRole role) {
    static const Win32MenuRole kRoles[] = {
        {PrAppMenuRole_About, "About", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Hide, "Hide", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Quit, "Exit", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Undo, "Undo", PrAppKey('Z'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Redo, "Redo", PrAppKey('Y'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Cut, "Cut", PrAppKey('X'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Copy, "Copy", PrAppKey('C'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Paste, "Paste", PrAppKey('V'), PrAppModifierFlag_Control},
        {PrAppMenuRole_SelectAll, "Select All", PrAppKey('A'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Minimize, "Minimize", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Zoom, "Maximize", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Fullscreen, "Full Screen", PrAppKey_F11, 0},
        {PrAppMenuRole_WindowList, "Window", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Help, "Help", PrAppKey_Unknown, 0},
    };
    for (const Win32MenuRole& entry : kRoles) {
        if (entry.role == role)
            return &entry;
    }
    return nullptr;
}

// Appends the text of a shortcut to `out`, as a menu shows it after a tab: "Ctrl+Shift+O".
void appendShortcut(RawArray<char>& out, PrAppKey key, PrAppModifierFlags modifiers) {
    auto append = [&out](PrStringView text) { out.append(text.data, uint32_t(text.size)); };
    append("\t");
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        append("Ctrl+");
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        append("Alt+");
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        append("Shift+");
    if (key > ' ' && key < 127) {
        char character = char(key);
        out.push(character);
        return;
    }
    if (key >= PrAppKey_F1 && key <= PrAppKey_F20) {
        char name[4] = {'F', 0, 0, 0};
        uint32_t number = uint32_t(key - PrAppKey_F1) + 1;
        name[1] = char('0' + (number >= 10 ? number / 10 : number));
        if (number >= 10)
            name[2] = char('0' + number % 10);
        append(PrStringView{name, number >= 10 ? 3u : 2u});
        return;
    }
    append(prAppKeyName(key));
}

// Returns the id of a new command. WM_COMMAND carries 16 bits, and ids below 0x100 are left to the system.
UINT newCommandId(Win32App* app) {
    app->lastCommandId = app->lastCommandId >= 0xEFFF || app->lastCommandId < 0x100 ? 0x100 : app->lastCommandId + 1;
    return app->lastCommandId;
}

// Removes every item of `menu`, keeping its submenus, which RemoveMenu detaches rather than destroys.
void menuEmpty(HMENU menu) {
    while (GetMenuItemCount(menu) > 0)
        RemoveMenu(menu, 0, MF_BYPOSITION);
}

HMENU menuFill(Win32App* app, Win32MenuTree& tree, RawArray<Win32Menu>& previous, RawArray<ACCEL>& accelerators,
    const PrAppMenu& description);

// Appends the item that `description` describes to `menu`.
void menuItemAppend(Win32App* app, Win32MenuTree& tree, RawArray<Win32Menu>& previous, RawArray<ACCEL>& accelerators,
        HMENU menu, const PrAppMenuItem& description) {
    if (description.role == PrAppMenuRole_Separator) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        return;
    }
    const Win32MenuRole* role = menuRoleOf(description.role);
    RawArray<char> text;
    PrStringView title = description.title;
    if (title.size == 0 && description.submenu != nullptr)
        title = description.submenu->title;
    if (title.size == 0 && role != nullptr)
        title = PrStringView{role->title, strlen(role->title)};
    text.append(title.data, uint32_t(title.size));
    PrAppKey key = description.key;
    PrAppModifierFlags modifiers = description.modifiers;
    if (key == PrAppKey_Unknown && role != nullptr) {
        key = role->key;
        modifiers = role->modifiers;
    }
    UINT flags = MF_STRING;
    if ((description.flags & PrAppMenuItemFlag_Disabled) != 0)
        flags |= MF_GRAYED;
    if ((description.flags & PrAppMenuItemFlag_Checked) != 0)
        flags |= MF_CHECKED;
    if (description.submenu != nullptr) {
        HMENU submenu = menuFill(app, tree, previous, accelerators, *description.submenu);
        win32Widen(PrStringView{text.data, text.count}, app->utf16);
        AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), app->utf16.data);
        text.reset();
        return;
    }
    UINT id = newCommandId(app);
    tree.commands.push(Win32Command{.id = id, .tag = description.tag, .role = description.role});
    UINT virtualKey = key != PrAppKey_Unknown ? virtualKeyOfKey(key) : 0;
    if (virtualKey != 0) {
        appendShortcut(text, key, modifiers);
        BYTE virt = FVIRTKEY;
        if ((modifiers & PrAppModifierFlag_Control) != 0)
            virt |= FCONTROL;
        if ((modifiers & PrAppModifierFlag_Alt) != 0)
            virt |= FALT;
        if ((modifiers & PrAppModifierFlag_Shift) != 0)
            virt |= FSHIFT;
        accelerators.push(ACCEL{.fVirt = virt, .key = WORD(virtualKey), .cmd = WORD(id)});
    }
    win32Widen(PrStringView{text.data, text.count}, app->utf16);
    AppendMenuW(menu, flags, id, app->utf16.data);
    text.reset();
}

// Fills the menu of `description`'s tag, from `previous` if one has the tag and new otherwise, and returns it.
HMENU menuFill(Win32App* app, Win32MenuTree& tree, RawArray<Win32Menu>& previous, RawArray<ACCEL>& accelerators,
        const PrAppMenu& description) {
    HMENU handle = nullptr;
    for (uint32_t i = 0; i < previous.count; ++i) {
        if (previous.data[i].tag == description.tag) {
            handle = previous.data[i].handle;
            previous.erase(i);
            break;
        }
    }
    if (handle == nullptr)
        handle = CreatePopupMenu();
    tree.menus.push(Win32Menu{.handle = handle, .tag = description.tag});
    for (uint32_t i = 0; i < description.count; ++i)
        menuItemAppend(app, tree, previous, accelerators, handle, description.items[i]);
    return handle;
}

// Makes `tree` the menus that `description` describes: a menu bar of its items' submenus, if `bar`, or else the menu
// itself. The menus of the same tags are filled in place.
void menuTreeSet(Win32App* app, Win32MenuTree& tree, const PrAppMenu& description, bool bar) {
    RawArray<Win32Menu> previous;
    previous.append(tree.menus.data, tree.menus.count);
    tree.menus.clear();
    tree.commands.clear();
    // The old menus are emptied first, so that none is a submenu of another while the new tree takes them.
    for (uint32_t i = 0; i < previous.count; ++i)
        menuEmpty(previous.data[i].handle);
    RawArray<ACCEL> accelerators;
    tree.bar = bar;
    if (bar) {
        if (tree.root == nullptr)
            tree.root = CreateMenu();
        menuEmpty(tree.root);
        for (uint32_t i = 0; i < description.count; ++i)
            menuItemAppend(app, tree, previous, accelerators, tree.root, description.items[i]);
    } else {
        // A menu's root is one of its menus, which the menus of the same tag reuse or the loop below frees.
        tree.root = menuFill(app, tree, previous, accelerators, description);
    }
    for (uint32_t i = 0; i < previous.count; ++i) {
        if (previous.data[i].handle != tree.root)
            DestroyMenu(previous.data[i].handle);
    }
    previous.reset();
    if (tree.accelerators != nullptr)
        DestroyAcceleratorTable(tree.accelerators);
    tree.accelerators = nullptr;
    if (accelerators.count != 0)
        tree.accelerators = CreateAcceleratorTableW(accelerators.data, int(accelerators.count));
    accelerators.reset();
}

// Frees a tree's menus. A menu bar that a window shows must leave the window first.
void menuTreeReset(Win32MenuTree& tree) {
    for (uint32_t i = 0; i < tree.menus.count; ++i)
        menuEmpty(tree.menus.data[i].handle);
    for (uint32_t i = 0; i < tree.menus.count; ++i)
        DestroyMenu(tree.menus.data[i].handle);
    if (tree.bar && tree.root != nullptr)
        DestroyMenu(tree.root);
    tree.menus.reset();
    tree.commands.reset();
    tree.root = nullptr;
    if (tree.accelerators != nullptr)
        DestroyAcceleratorTable(tree.accelerators);
    tree.accelerators = nullptr;
}

void menuCopyReset(Win32MenuCopy* PR_NULLABLE copy) {
    if (copy == nullptr)
        return;
    for (uint32_t i = 0; i < copy->submenus.count; ++i)
        menuCopyReset(copy->submenus.data[i]);
    copy->submenus.reset();
    copy->items.reset();
    appStringsReset(copy->strings);
    prMemoryFree(copy);
}

// Returns a copy of `description` that owns its strings and its submenus.
Win32MenuCopy* menuCopyOf(const PrAppMenu& description) {
    Win32MenuCopy* copy = allocZeroed<Win32MenuCopy>();
    appStringsAdd(copy->strings, description.title);
    for (uint32_t i = 0; i < description.count; ++i)
        appStringsAdd(copy->strings, description.items[i].title);
    const PrStringView* titles = appStringsViews(copy->strings);
    copy->menu = PrAppMenu{.tag = description.tag, .title = titles[0]};
    for (uint32_t i = 0; i < description.count; ++i) {
        PrAppMenuItem item = description.items[i];
        item.title = titles[i + 1];
        if (item.submenu != nullptr) {
            Win32MenuCopy* submenu = menuCopyOf(*item.submenu);
            copy->submenus.push(submenu);
            item.submenu = &submenu->menu;
        }
        copy->items.push(item);
    }
    copy->menu.items = copy->items.data;
    copy->menu.count = copy->items.count;
    return copy;
}

// Shows the application's menu bar on a Normal window that has none of its own. A custom title bar has no room for a
// menu bar, so its window shows none.
void showMenuBar(Win32Window* window) {
    Win32App* app = appOf(window);
    if (window->ownMenuBar || window->role != PrAppWindowRole_Normal || window->customTitleBar)
        return;
    if (app->menuBar == nullptr) {
        if (GetMenu(window->window) != nullptr)
            SetMenu(window->window, nullptr);
        menuTreeReset(window->menuBar);
        return;
    }
    menuTreeSet(app, window->menuBar, app->menuBar->menu, true);
    if (GetMenu(window->window) != window->menuBar.root)
        SetMenu(window->window, window->menuBar.root);
    DrawMenuBar(window->window);
}

void setMenuBar(PrApp* base, const PrAppMenu* PR_NULLABLE menu) {
    Win32App* app = static_cast<Win32App*>(base);
    menuCopyReset(app->menuBar);
    app->menuBar = menu != nullptr ? menuCopyOf(*menu) : nullptr;
    for (uint32_t i = 0; i < app->windows.count; ++i)
        showMenuBar(app->windows.data[i]);
}

void windowSetMenuBar(PrAppWindow* base, const PrAppMenu* PR_NULLABLE menu) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->ownMenuBar = menu != nullptr;
    if (menu == nullptr) {
        showMenuBar(window);
        return;
    }
    if (window->customTitleBar)
        return;
    menuTreeSet(appOf(window), window->menuBar, *menu, true);
    if (GetMenu(window->window) != window->menuBar.root)
        SetMenu(window->window, window->menuBar.root);
    DrawMenuBar(window->window);
}

void showMenu(PrAppWindow* base, const PrAppMenu* menu, PrVec2 position) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (appOf(window)->headless)
        return;
    menuTreeSet(appOf(window), window->contextMenu, *menu, false);
    window->contextPending = true;
    window->contextAt = position;
    PostMessageW(window->window, kMessageShowMenu, 0, 0);
}

// Shows the context menu that waits, which runs the menu's modal loop until the user chooses or dismisses it.
void showContextMenu(Win32Window* window) {
    if (!window->contextPending || window->contextMenu.root == nullptr)
        return;
    window->contextPending = false;
    POINT at{MulDiv(int(window->contextAt.x), int(window->dpi), 96), MulDiv(int(window->contextAt.y), int(window->dpi), 96)};
    ClientToScreen(window->window, &at);
    TrackPopupMenuEx(window->contextMenu.root, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, at.x, at.y,
        window->window, nullptr);
}

// Finds the menu `handle` among the trees that `window` shows, or among the status items' menus for a null window.
// `ofWindow` is true for a menu whose events go to the window: its own menu bar or its context menu.
bool menuTagOf(Win32App* app, Win32Window* PR_NULLABLE window, HMENU handle, uint64_t* out, bool* ofWindow) {
    auto find = [handle, out](const Win32MenuTree& tree) {
        for (uint32_t i = 0; i < tree.menus.count; ++i) {
            if (tree.menus.data[i].handle == handle) {
                *out = tree.menus.data[i].tag;
                return true;
            }
        }
        return false;
    };
    *ofWindow = false;
    if (window != nullptr && find(window->contextMenu)) {
        *ofWindow = true;
        return true;
    }
    if (window != nullptr && find(window->menuBar)) {
        *ofWindow = window->ownMenuBar;
        return true;
    }
    for (uint32_t i = 0; window == nullptr && i < app->statusItems.count; ++i) {
        if (find(app->statusItems.data[i]->menu))
            return true;
    }
    return false;
}

// Delivers MenuOpening for a menu that is about to open, to the window whose menu it is, or to the application for its
// menu bar and its status items.
void menuOpening(Win32App* app, Win32Window* PR_NULLABLE window, HMENU menu) {
    uint64_t tag = 0;
    bool ofWindow = false;
    if (!menuTagOf(app, window, menu, &tag, &ofWindow))
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_MenuOpening;
    event.time = secondsNow();
    event.tag = tag;
    deliver(app, ofWindow ? window : nullptr, event);
}

// Reads the clipboard's text, for a Paste item that the program's window handles.
void clipboardText(Win32App* app, RawArray<char>& out) {
    out.clear();
    if (!openClipboard(app->messageWindow))
        return;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* units = static_cast<const wchar_t*>(GlobalLock(handle))) {
            PrStringView text = win32Narrow(units, int(wcslen(units)), app->utf8);
            out.append(text.data, uint32_t(text.size));
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
}

void setState(PrAppWindow* base, PrAppWindowState wanted);
PrAppWindowState state(const PrAppWindow* base);

// Acts on the command `id` of a menu: a role's action, or a MenuItem with the item's tag, to the window whose menu it
// is, or to the application for its menu bar and its status items.
void menuCommand(Win32App* app, Win32Window* PR_NULLABLE window, UINT id) {
    const Win32Command* command = nullptr;
    bool ofWindow = false;
    auto find = [id, &command](const Win32MenuTree& tree) {
        for (uint32_t i = 0; i < tree.commands.count; ++i) {
            if (tree.commands.data[i].id == id) {
                command = &tree.commands.data[i];
                return true;
            }
        }
        return false;
    };
    if (window != nullptr && find(window->contextMenu))
        ofWindow = true;
    else if (window != nullptr && find(window->menuBar))
        ofWindow = window->ownMenuBar;
    for (uint32_t i = 0; command == nullptr && i < app->statusItems.count; ++i)
        find(app->statusItems.data[i]->menu);
    if (command == nullptr)
        return;
    Win32Window* focused = window != nullptr ? window : app->lastActive;
    PrAppEvent event{};
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.tag = command->tag;
    switch (command->role) {
        case PrAppMenuRole_Quit:
            app->quitPending = true;
            app->quitAnswered = false;
            event.kind = PrAppEventKind_QuitRequested;
            deliver(app, nullptr, event);
            return;
        case PrAppMenuRole_Hide:
            app->setHiddenFn(app, true);
            return;
        case PrAppMenuRole_Minimize:
            if (focused != nullptr)
                ShowWindow(focused->window, SW_MINIMIZE);
            return;
        case PrAppMenuRole_Zoom:
            if (focused != nullptr)
                ShowWindow(focused->window, IsZoomed(focused->window) ? SW_RESTORE : SW_MAXIMIZE);
            return;
        case PrAppMenuRole_Fullscreen:
            if (focused != nullptr) {
                bool full = state(focused) == PrAppWindowState_Fullscreen;
                setState(focused, full ? PrAppWindowState_Normal : PrAppWindowState_Fullscreen);
            }
            return;
        case PrAppMenuRole_Paste:
            // A paste goes to the window that has the keyboard, as the text of a Paste event.
            if (focused != nullptr) {
                RawArray<char> text;
                clipboardText(app, text);
                event.kind = PrAppEventKind_Paste;
                event.input.text = PrStringView{text.data != nullptr ? text.data : "", text.count};
                deliver(focused, event);
                text.reset();
            }
            return;
        case PrAppMenuRole_Undo:
        case PrAppMenuRole_Redo:
        case PrAppMenuRole_Cut:
        case PrAppMenuRole_Copy:
        case PrAppMenuRole_SelectAll:
            // An editing item acts on the window that has the keyboard.
            event.kind = PrAppEventKind_MenuItem;
            if (focused != nullptr)
                deliver(focused, event);
            return;
        default:
            event.kind = PrAppEventKind_MenuItem;
            deliver(app, ofWindow ? window : nullptr, event);
            return;
    }
}

// Translates a key of a menu's shortcut into its command, for the window that the key goes to. Returns true if the key
// was a shortcut.
bool translateAccelerator(Win32App* app, MSG& message) {
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN)
        return false;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (window->window != message.hwnd)
            continue;
        HACCEL table = window->menuBar.accelerators;
        return table != nullptr && TranslateAcceleratorW(window->window, table, &message) != 0;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Status items

void statusItemApply(Win32StatusItem* item, const PrAppStatusItemDesc& desc, DWORD action) {
    Win32App* app = item->app;
    if (item->icon != nullptr)
        DestroyIcon(item->icon);
    item->icon = iconOf(desc.icon);
    item->tag = desc.tag;
    item->hasMenu = desc.menu != nullptr;
    if (item->hasMenu)
        menuTreeSet(app, item->menu, *desc.menu, false);
    else
        menuTreeReset(item->menu);
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app->messageWindow;
    icon.uID = item->id;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = kMessageStatusItem;
    icon.hIcon = item->icon;
    if (icon.hIcon == nullptr)
        icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(IDI_APPLICATION)));
    PrStringView tip = desc.tooltip.size != 0 ? desc.tooltip : desc.title;
    copyField(app, tip, icon.szTip, countOf(icon.szTip));
    Shell_NotifyIconW(action, &icon);
    if (action == NIM_ADD) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
    }
}

void statusItemDestroy(PrAppStatusItem* base) {
    Win32StatusItem* item = static_cast<Win32StatusItem*>(base);
    Win32App* app = item->app;
    for (uint32_t i = 0; i < app->statusItems.count; ++i) {
        if (app->statusItems.data[i] == item) {
            app->statusItems.erase(i);
            break;
        }
    }
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app->messageWindow;
    icon.uID = item->id;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    menuTreeReset(item->menu);
    if (item->icon != nullptr)
        DestroyIcon(item->icon);
    prMemoryFree(item);
}

void statusItemSet(PrAppStatusItem* base, const PrAppStatusItemDesc* desc) {
    statusItemApply(static_cast<Win32StatusItem*>(base), *desc, NIM_MODIFY);
}

PrStatus statusItemCreate(PrApp* base, const PrAppStatusItemDesc* desc, PrAppStatusItem* PR_NULLABLE* PR_NONNULL out) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return PrStatus_InvalidState;
    if (app->headless)
        return PrStatus_NotSupported;
    Win32StatusItem* item = allocZeroed<Win32StatusItem>();
    item->destroyFn = statusItemDestroy;
    item->setFn = statusItemSet;
    item->app = app;
    item->id = kNotifyIconId + 1 + ++app->lastStatusItemId;
    app->statusItems.push(item);
    statusItemApply(item, *desc, NIM_ADD);
    *out = item;
    return PrStatus_Ok;
}

// Acts on a click on a status item: opens its menu at the pointer, or delivers StatusItem.
void statusItemAction(Win32App* app, UINT id, UINT action) {
    if (action != NIN_SELECT && action != NIN_KEYSELECT && action != WM_CONTEXTMENU)
        return;
    for (uint32_t i = 0; i < app->statusItems.count; ++i) {
        Win32StatusItem* item = app->statusItems.data[i];
        if (item->id != id)
            continue;
        if (!item->hasMenu) {
            PrAppEvent event{};
            event.kind = PrAppEventKind_StatusItem;
            event.time = secondsNow();
            event.tag = item->tag;
            deliver(app, nullptr, event);
            return;
        }
        // A menu of the notification area closes as the user clicks elsewhere only if its owner is the foreground
        // window, and the message that follows lets the shell see the menu end.
        POINT at{};
        GetCursorPos(&at);
        SetForegroundWindow(app->messageWindow);
        TrackPopupMenuEx(item->menu.root, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, app->messageWindow, nullptr);
        PostMessageW(app->messageWindow, WM_NULL, 0, 0);
        return;
    }
}

PrStatus surface(const PrAppWindow* base, PrAppSurface* out) {
    out->kind = PrAppSurfaceKind_Hwnd;
    out->handle = static_cast<const Win32Window*>(base)->window;
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Services: the system's notices, timers, the session's end, the lifecycle and a single instance

// Delivers an application event of `kind` that carries no details.
void deliverKind(Win32App* app, PrAppEventKind kind) {
    if (!app->running)
        return;
    PrAppEvent event{};
    event.kind = kind;
    event.time = secondsNow();
    deliver(app, nullptr, event);
}

// Returns the application's lifecycle: Active while one of its windows is the foreground window, Background while none
// of its windows shows, and Inactive otherwise.
PrAppLifecycle computedLifecycle(const Win32App* app) {
    if (app->headless)
        return PrAppLifecycle_Background;
    HWND foreground = GetForegroundWindow();
    bool shows = false;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        HWND window = app->windows.data[i]->window;
        if (window == foreground)
            return PrAppLifecycle_Active;
        if (IsWindowVisible(window) && !IsIconic(window))
            shows = true;
    }
    return shows ? PrAppLifecycle_Inactive : PrAppLifecycle_Background;
}

void lifecycleMayChange(Win32App* app) {
    PrAppLifecycle now = computedLifecycle(app);
    if (now == app->lifecycle)
        return;
    app->lifecycle = now;
    deliverKind(app, PrAppEventKind_LifecycleChanged);
}

void keyLabelsReset(Win32App* app) {
    for (uint32_t i = 0; i < PrAppKeyCode_Count; ++i) {
        prRawStringReset(&app->keyLabels[i]);
        app->keyLabeled[i] = false;
    }
}

// Takes the single instance's mutex, or hands this launch's command line to the instance that holds it and returns
// false. The first instance's message window carries the instance's name as its title.
bool claimSingleInstance(Win32App* app) {
    if (app->instanceName.count == 0)
        return true;
    RawArray<wchar_t> mutexName;
    mutexName.append(L"Local\\", 6);
    mutexName.append(app->instanceName.data, app->instanceName.count);
    app->instanceMutex = CreateMutexW(nullptr, FALSE, mutexName.data);
    DWORD error = GetLastError();
    mutexName.reset();
    if (app->instanceMutex != nullptr && error != ERROR_ALREADY_EXISTS)
        return true;
    if (app->instanceMutex != nullptr)
        CloseHandle(app->instanceMutex);
    app->instanceMutex = nullptr;
    HWND first = FindWindowW(kMessageClassName, app->instanceName.data);
    if (first != nullptr) {
        const wchar_t* line = GetCommandLineW();
        COPYDATASTRUCT data{
            .dwData = kCopyDataOpen,
            .cbData = DWORD((wcslen(line) + 1) * sizeof(wchar_t)),
            .lpData = const_cast<wchar_t*>(line),
        };
        DWORD process = 0;
        GetWindowThreadProcessId(first, &process);
        AllowSetForegroundWindow(process);
        SendMessageW(first, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
    }
    return false;
}

// Delivers the request of a second launch, whose command line `line` holds, with its arguments past the program.
void openCommandLine(Win32App* app, const wchar_t* line) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(line, &count);
    if (arguments == nullptr)
        return;
    AppStrings strings{};
    for (int i = 1; i < count; ++i) {
        appStringsAdd(strings, win32Narrow(arguments[i], int(wcslen(arguments[i])), app->utf8));
    }
    LocalFree(arguments);
    if (app->lastActive != nullptr)
        bringForward(app->lastActive);
    PrAppEvent event{};
    event.kind = PrAppEventKind_Open;
    event.time = secondsNow();
    event.open.arguments = appStringsViews(strings);
    event.open.argumentCount = strings.ends.count;
    deliver(app, nullptr, event);
    appStringsReset(strings);
}

void startServices(Win32App* app) {
    app->lifecycle = computedLifecycle(app);
    AddClipboardFormatListener(app->messageWindow);
    WTSRegisterSessionNotification(app->messageWindow, NOTIFY_FOR_THIS_SESSION);
    app->lowMemory = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    if (app->lowMemory != nullptr)
        SetTimer(app->messageWindow, kTimerLowMemory, kLowMemoryMilliseconds, nullptr);
}

void stopServices(Win32App* app) {
    for (uint32_t i = 0; i < app->timers.count; ++i)
        KillTimer(app->messageWindow, app->timers.data[i].id);
    app->timers.reset();
    KillTimer(app->messageWindow, kTimerLowMemory);
    if (app->lowMemory != nullptr)
        CloseHandle(app->lowMemory);
    app->lowMemory = nullptr;
    RemoveClipboardFormatListener(app->messageWindow);
    WTSUnRegisterSessionNotification(app->messageWindow);
    if (app->taskbar != nullptr)
        app->taskbar->Release();
    app->taskbar = nullptr;
    if (app->badge != nullptr)
        DestroyIcon(app->badge);
    app->badge = nullptr;
    SetThreadExecutionState(ES_CONTINUOUS);
}

// Polls the memory's state, and delivers LowMemory as it turns low.
void pollMemory(Win32App* app) {
    BOOL low = FALSE;
    if (app->lowMemory == nullptr || !QueryMemoryResourceNotification(app->lowMemory, &low))
        return;
    bool turned = low && !app->memoryLow;
    app->memoryLow = low;
    if (turned)
        deliverKind(app, PrAppEventKind_LowMemory);
}

Win32Timer* PR_NULLABLE timerOf(Win32App* app, uint64_t tag, uint32_t* index) {
    for (uint32_t i = 0; i < app->timers.count; ++i) {
        if (app->timers.data[i].tag == tag) {
            *index = i;
            return &app->timers.data[i];
        }
    }
    return nullptr;
}

UINT millisecondsOf(double seconds) {
    double milliseconds = seconds * 1000.0;
    if (milliseconds < USER_TIMER_MINIMUM)
        return USER_TIMER_MINIMUM;
    return milliseconds > double(USER_TIMER_MAXIMUM) ? USER_TIMER_MAXIMUM : UINT(milliseconds);
}

void timerCancel(PrApp* base, uint64_t tag) {
    Win32App* app = static_cast<Win32App*>(base);
    uint32_t index = 0;
    if (Win32Timer* timer = timerOf(app, tag, &index)) {
        KillTimer(app->messageWindow, timer->id);
        app->timers.erase(index);
    }
}

void timerSet(PrApp* base, uint64_t tag, double delay, double interval) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return;
    timerCancel(base, tag);
    UINT_PTR id = kFirstProgramTimer + ++app->lastTimerId;
    app->timers.push(Win32Timer{.tag = tag, .id = id, .interval = interval > 0 ? interval : 0});
    SetTimer(app->messageWindow, id, millisecondsOf(delay), nullptr);
}

// Delivers the firing of the program's timer `id`. Its first firing sets it to repeat at its interval, before the
// delivery, so that the program may set or cancel the tag during the event.
void timerFired(Win32App* app, UINT_PTR id) {
    for (uint32_t i = 0; i < app->timers.count; ++i) {
        Win32Timer& timer = app->timers.data[i];
        if (timer.id != id)
            continue;
        uint64_t tag = timer.tag;
        if (timer.interval == 0) {
            KillTimer(app->messageWindow, id);
            app->timers.erase(i);
        } else if (!timer.repeating) {
            timer.repeating = true;
            SetTimer(app->messageWindow, id, millisecondsOf(timer.interval), nullptr);
        }
        PrAppEvent event{};
        event.kind = PrAppEventKind_Timer;
        event.time = secondsNow();
        event.tag = tag;
        deliver(app, nullptr, event);
        return;
    }
    KillTimer(app->messageWindow, id);
}

// Asks the program whether the session may end. A program that answers during the event decides at once. Otherwise the
// application blocks the end with a reason that the system shows, until the program answers.
LRESULT queryEndSession(Win32App* app) {
    if (!app->running)
        return TRUE;
    app->quitPending = true;
    app->quitAnswered = false;
    PrAppEvent event{};
    event.kind = PrAppEventKind_QuitRequested;
    event.time = secondsNow();
    event.quit.sessionEnding = true;
    deliver(app, nullptr, event);
    if (app->quitAnswered)
        return app->quitAccepted ? TRUE : FALSE;
    ShutdownBlockReasonCreate(app->messageWindow, L"Waiting for the application to save its work");
    return FALSE;
}

void quitRespond(PrApp* base, bool quit) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->quitPending)
        return;
    app->quitPending = false;
    app->quitAnswered = true;
    app->quitAccepted = quit;
    ShutdownBlockReasonDestroy(app->messageWindow);
    if (quit)
        app->stopFn(app);
}

// Handles the system's notices that reach the message window. Returns true and writes the result for a message that it
// handled.
bool serviceMessage(Win32App* app, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result) {
    *result = 0;
    switch (message) {
        case WM_TIMER:
            if (wParam == kTimerLowMemory)
                pollMemory(app);
            else if (wParam >= kFirstProgramTimer)
                timerFired(app, wParam);
            else
                return false;
            return true;
        case WM_CLIPBOARDUPDATE:
            deliverKind(app, PrAppEventKind_ClipboardChanged);
            return true;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMPOWERSTATUSCHANGE)
                deliverKind(app, PrAppEventKind_PowerChanged);
            else if (wParam == PBT_APMSUSPEND)
                deliverKind(app, PrAppEventKind_Suspend);
            else if (wParam == PBT_APMRESUMEAUTOMATIC)
                deliverKind(app, PrAppEventKind_Resume);
            *result = TRUE;
            return true;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK)
                deliverKind(app, PrAppEventKind_ScreenLocked);
            else if (wParam == WTS_SESSION_UNLOCK)
                deliverKind(app, PrAppEventKind_ScreenUnlocked);
            return true;
        case WM_SETTINGCHANGE:
            if (lParam != 0 && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"intl") == 0)
                deliverKind(app, PrAppEventKind_LocaleChanged);
            return false;
        case WM_QUERYENDSESSION:
            *result = queryEndSession(app);
            return true;
        case WM_COPYDATA: {
            const COPYDATASTRUCT* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data == nullptr || data->dwData != kCopyDataOpen || data->cbData < sizeof(wchar_t))
                return false;
            RawArray<wchar_t> line;
            line.append(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
            line.push(0);
            openCommandLine(app, line.data);
            line.reset();
            *result = TRUE;
            return true;
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The shell and the device

PrStatus trashPath(PrApp* base, PrStringView path) {
    Win32App* app = static_cast<Win32App*>(base);
    win32Widen(path, app->utf16);
    if (GetFileAttributesW(app->utf16.data) == INVALID_FILE_ATTRIBUTES)
        return PrStatus_NotFound;
    // The shell's operation takes a list of paths, each ended by a NUL, and the list by another.
    app->utf16.push(0);
    SHFILEOPSTRUCTW operation{
        .wFunc = FO_DELETE,
        .pFrom = app->utf16.data,
        .fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT,
    };
    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted ? PrStatus_Ok : PrStatus_Io;
}

void recentDocumentAdd(PrApp* base, PrStringView path) {
    Win32App* app = static_cast<Win32App*>(base);
    win32Widen(path, app->utf16);
    SHAddToRecentDocs(SHARD_PATHW, app->utf16.data);
}

void recentDocumentsClear(PrApp*) {
    SHAddToRecentDocs(SHARD_PATHW, nullptr);
}

// Returns the taskbar, created on first use, or null if the shell has none.
ITaskbarList3* PR_NULLABLE taskbarOf(Win32App* app) {
    if (app->taskbar != nullptr || !app->ole)
        return app->taskbar;
    ITaskbarList3* taskbar = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))))
        return nullptr;
    if (FAILED(taskbar->HrInit())) {
        taskbar->Release();
        return nullptr;
    }
    app->taskbar = taskbar;
    return taskbar;
}

// Shows the window's progress and the application's badge on the window's taskbar button.
void applyTaskbar(Win32Window* window) {
    Win32App* app = appOf(window);
    ITaskbarList3* taskbar = taskbarOf(app);
    if (taskbar == nullptr || window->role != PrAppWindowRole_Normal)
        return;
    TBPFLAG flag = TBPF_NOPROGRESS;
    switch (window->progressState) {
        case PrAppProgressState_Normal:
            flag = TBPF_NORMAL;
            break;
        case PrAppProgressState_Indeterminate:
            flag = TBPF_INDETERMINATE;
            break;
        case PrAppProgressState_Paused:
            flag = TBPF_PAUSED;
            break;
        case PrAppProgressState_Error:
            flag = TBPF_ERROR;
            break;
        default:
            break;
    }
    taskbar->SetProgressState(window->window, flag);
    if (flag != TBPF_NOPROGRESS && flag != TBPF_INDETERMINATE) {
        float clamped = window->progress < 0 ? 0 : window->progress > 1 ? 1 : window->progress;
        taskbar->SetProgressValue(window->window, ULONGLONG(clamped * 10000.0f), 10000);
    }
    const wchar_t* description = app->badgeText.count != 0 ? app->badgeText.data : L"";
    taskbar->SetOverlayIcon(window->window, app->badge, description);
}

// Returns an icon of `text` in white on a red circle, the size of a small icon, for the taskbar's overlay.
HICON PR_NULLABLE badgeIconOf(const wchar_t* text) {
    int size = GetSystemMetrics(SM_CXSMICON);
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (color == nullptr)
        return nullptr;
    HDC context = CreateCompatibleDC(nullptr);
    HGDIOBJ previous = SelectObject(context, color);
    HBRUSH red = CreateSolidBrush(RGB(220, 40, 40));
    HGDIOBJ previousBrush = SelectObject(context, red);
    HGDIOBJ previousPen = SelectObject(context, GetStockObject(NULL_PEN));
    Ellipse(context, 0, 0, size + 1, size + 1);
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, RGB(255, 255, 255));
    RECT bounds{0, 0, size, size};
    DrawTextW(context, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(context, previousPen);
    SelectObject(context, previousBrush);
    SelectObject(context, previous);
    DeleteObject(red);
    DeleteDC(context);
    // GDI leaves the alpha at zero, so the circle's pixels become opaque, and the others transparent.
    uint32_t* pixels = static_cast<uint32_t*>(bits);
    float radius = float(size) / 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = float(x) + 0.5f - radius;
            float dy = float(y) + 0.5f - radius;
            bool inside = dx * dx + dy * dy <= radius * radius;
            uint32_t& pixel = pixels[y * size + x];
            pixel = inside ? (pixel | 0xFF000000u) : 0;
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{.fIcon = TRUE, .hbmMask = mask, .hbmColor = color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void setBadge(PrApp* base, PrStringView badge) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->badge != nullptr)
        DestroyIcon(app->badge);
    app->badge = nullptr;
    app->badgeText.clear();
    if (badge.size != 0) {
        win32Widen(badge, app->utf16);
        app->badgeText.append(app->utf16.data, app->utf16.count);
        app->badge = badgeIconOf(app->badgeText.data);
    }
    for (uint32_t i = 0; i < app->windows.count; ++i)
        applyTaskbar(app->windows.data[i]);
}

PrAppPower power(const PrApp*) {
    PrAppPower result{.source = PrAppPowerSource_Unknown, .level = -1};
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status))
        return result;
    if (status.ACLineStatus == 1)
        result.source = PrAppPowerSource_External;
    else if (status.ACLineStatus == 0)
        result.source = PrAppPowerSource_Battery;
    // A battery flag of 128 means that the system has no battery, and a percentage of 255 that it is unknown.
    if (status.BatteryFlag != 128 && status.BatteryLifePercent <= 100)
        result.level = float(status.BatteryLifePercent) / 100.0f;
    result.charging = status.BatteryFlag != 128 && (status.BatteryFlag & 8) != 0;
    result.lowPower = status.SystemStatusFlag == 1;
    return result;
}

void setAwake(PrApp*, PrAppAwakeFlags flags) {
    EXECUTION_STATE state = ES_CONTINUOUS;
    if ((flags & PrAppAwakeFlag_Display) != 0)
        state |= ES_DISPLAY_REQUIRED;
    if ((flags & PrAppAwakeFlag_System) != 0)
        state |= ES_SYSTEM_REQUIRED;
    SetThreadExecutionState(state);
}

// Without a taskbar button, the application's windows are owned by its message window, which keeps them off the
// taskbar.
void setPresence(PrApp* base, PrAppPresence presence) {
    Win32App* app = static_cast<Win32App*>(base);
    app->presence = presence;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (window->role != PrAppWindowRole_Normal)
            continue;
        HWND owner = presence == PrAppPresence_Accessory ? app->messageWindow : nullptr;
        SetWindowLongPtrW(window->window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    }
}

PrAppWindowModel windowModel(const PrApp* base) {
    return static_cast<const Win32App*>(base)->windowModel;
}

PrAppLifecycle lifecycle(const PrApp* base) {
    const Win32App* app = static_cast<const Win32App*>(base);
    return app->running ? app->lifecycle : computedLifecycle(app);
}

UINT virtualKeyOf(PrAppKeyCode code);

// Returns the label of the key at `code`: the character that it types in the current layout, which MapVirtualKey gives
// uppercase, or the key's name for a key that types none.
PrStringView keyCodeLabel(const PrApp* base, PrAppKeyCode code) {
    const Win32App* app = static_cast<const Win32App*>(base);
    if (code >= PrAppKeyCode_Count)
        return prAppKeyCodeName(PrAppKeyCode_Unidentified);
    if (!app->keyLabeled[code]) {
        app->keyLabeled[code] = true;
        prRawStringReset(&app->keyLabels[code]);
        UINT virtualKey = virtualKeyOf(code);
        // The high bit marks a dead key, whose character is the accent that it adds.
        UINT character = virtualKey != 0 ? MapVirtualKeyExW(virtualKey, MAPVK_VK_TO_CHAR, GetKeyboardLayout(0)) : 0;
        character &= 0x7FFFFFFF;
        if (character > ' ' && character != 0x7F) {
            wchar_t unit = wchar_t(character);
            app->keyLabels[code] = prRawStringCopy(win32Narrow(&unit, 1, app->utf8));
        }
    }
    if (!prRawStringIsEmpty(&app->keyLabels[code]))
        return prRawStringView(&app->keyLabels[code]);
    return prAppKeyCodeName(code);
}

// ---------------------------------------------------------------------------------------------------------------------
// The application's functions

void destroy(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    keyLabelsReset(app);
    displayNamesReset(app);
    app->badgeText.reset();
    app->instanceName.reset();
    if (app->instanceMutex != nullptr)
        CloseHandle(app->instanceMutex);
    appDataRequestsReset(app->requests);
    appDataSourcesReset(app->sources);
    app->held.reset();
    app->windows.reset();
    app->utf8.reset();
    app->utf16.reset();
    prMemoryFree(app);
}

void run(PrApp* base, PrAppHandler handler) {
    Win32App* app = static_cast<Win32App*>(base);
    app->handler = handler;
    // The shell, which opens URLs, and the input methods expect a single-threaded apartment on the windows' thread, and
    // drags need OLE in it. A thread that already joined another apartment keeps it, and has no drags.
    app->ole = SUCCEEDED(OleInitialize(nullptr));
    app->payloadFormat = RegisterClipboardFormatW(kPayloadFormatName);
    app->htmlFormat = RegisterClipboardFormatW(L"HTML Format");
    if (app->ole && FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&app->dropHelper))))
    app->dropHelper = nullptr;
    gDropTarget.app = app;
    gDataObject.app = app;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    registerClasses(instance);
    app->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // A second launch of a single instance hands its command line to the first, and its run ends without a callback.
    if (!claimSingleInstance(app)) {
        if (app->dropHelper != nullptr)
            app->dropHelper->Release();
        app->dropHelper = nullptr;
        gDropTarget.app = nullptr;
        gDataObject.app = nullptr;
        if (app->ole)
            OleUninitialize();
        app->ole = false;
        return;
    }
    app->taskbarButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    // The message window is a hidden top-level window rather than a message-only one, because only top-level windows
    // receive the system's broadcasts: the session's end, power, and the settings.
    const wchar_t* name = app->instanceName.count != 0 ? app->instanceName.data : L"";
    app->messageWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kMessageClassName, name, WS_POPUP, 0, 0, 0, 0, nullptr,
        nullptr, instance, app);
    if (app->messageWindow == nullptr)
        PR_FATAL("the application's message window could not be created");
    startServices(app);
    app->pacerRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    app->pacerQuit.store(false);
    prThreadStart(&app->pacer, pace, app);
    app->wakePending.store(false);
    app->wakeWindow.store(app->messageWindow);
    app->running = true;
    app->inCallback = true;
    if (!handler.start(handler.context, app))
        app->stopFn(app);
    endCallback(app);
    // A frame runs when nothing else waits in the queue, as WM_PAINT does, since a posted message comes ahead of input:
    // a frame that takes longer than a refresh would otherwise find the next frame's message waiting each time it
    // returned, and the windows would take no input. The pacer's message only marks the frames due here. A modal loop,
    // such as a move's or a resize's, dispatches it to the message window, which runs the frames at once.
    while (app->running) {
        MSG message;
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (app->frameDue && anyFrameRequested(app))
                runRequestedFrames(app);
            else
                WaitMessage();
            continue;
        }
        if (message.message == WM_QUIT) {
            app->stopFn(app);
            break;
        }
        if (message.message == kMessageFrame && message.hwnd == app->messageWindow) {
            app->frameDue = true;
            continue;
        }
        if (translateAccelerator(app, message))
            continue;
        if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)
            app->keyText = translateKey(message.message, message.wParam, message.lParam);
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    app->inCallback = true;
    handler.shutdown(handler.context, app);
    endCallback(app);
    stopServices(app);
    while (app->statusItems.count != 0)
        statusItemDestroy(app->statusItems.data[app->statusItems.count - 1]);
    app->statusItems.reset();
    menuCopyReset(app->menuBar);
    app->menuBar = nullptr;
    // The windows that the program left open close with the run.
    while (app->windows.count != 0)
        prAppWindowDestroy(app->windows.data[app->windows.count - 1]);
    app->wakeWindow.store(nullptr);
    app->pacerQuit.store(true);
    SetEvent(app->pacerRequest);
    prThreadJoin(&app->pacer);
    CloseHandle(app->pacerRequest);
    app->pacerRequest = nullptr;
    if (app->notifyIcon) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = app->messageWindow;
        icon.uID = kNotifyIconId;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        app->notifyIcon = false;
    }
    for (uint32_t i = 0; i < app->hotkeys.count; ++i)
        UnregisterHotKey(app->messageWindow, app->hotkeys.data[i].id);
    app->hotkeys.reset();
    SetWindowLongPtrW(app->messageWindow, GWLP_USERDATA, 0);
    DestroyWindow(app->messageWindow);
    app->messageWindow = nullptr;
    app->frameDue = false;
    if (app->dropHelper != nullptr)
        app->dropHelper->Release();
    app->dropHelper = nullptr;
    appDataStoreReset(app->drag.content);
    dataObjectClear(gDataObject);
    gDropTarget.app = nullptr;
    gDataObject.app = nullptr;
    if (app->ole)
        OleUninitialize();
    app->ole = false;
}

void stop(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return;
    app->running = false;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        KillTimer(window->window, kTimerFrame);
        KillTimer(window->window, kTimerFrameAt);
        // The system's modal loop for a move or a resize ends when it is canceled, and the main loop then sees the
        // stop.
        if (window->inSizeMove)
            PostMessageW(window->window, WM_CANCELMODE, 0, 0);
    }
    PostMessageW(app->messageWindow, WM_NULL, 0, 0);
}

void wake(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->wakePending.exchange(true))
        return;
    HWND window = app->wakeWindow.load();
    if (window == nullptr || !PostMessageW(window, kMessageWake, 0, 0))
        app->wakePending.store(false);
}

// Copies `text` to `out`, a NUL-terminated field of `capacity` UTF-16 units. Text that does not fit is cut before the
// first character that does not fit, so a surrogate pair is never split.
void copyField(Win32App* app, PrStringView text, wchar_t* out, size_t capacity) {
    win32Widen(text, app->utf16);
    size_t count = app->utf16.count - 1;
    if (count > capacity - 1) {
        count = capacity - 1;
        wchar_t last = app->utf16.data[count - 1];
        if (last >= 0xD800 && last <= 0xDBFF)
            --count;
    }
    prMemCopy(out, app->utf16.data, count * sizeof(wchar_t));
    out[count] = 0;
}

// Adds the application's icon to the notification area, if it is not there, with the tip of its first window's title.
// Notifications appear as balloons on this icon. Returns false if the shell refuses the icon.
bool addNotifyIcon(Win32App* app) {
    if (app->notifyIcon)
        return true;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app->messageWindow;
    icon.uID = kNotifyIconId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = kMessageNotifyIcon;
    icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(IDI_APPLICATION)));
    if (app->windows.count != 0)
        lstrcpynW(icon.szTip, app->windows.data[0]->title, int(countOf(icon.szTip)));
    if (!Shell_NotifyIconW(NIM_ADD, &icon))
        return false;
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
    app->notifyIcon = true;
    return true;
}

// A notification is a balloon on the application's icon in the notification area, which Windows 10 and later show as a
// toast. A real toast needs an application user model ID, which only a packaged or installed application has. A balloon
// needs nothing from the process. A new balloon replaces the previous one.
PrStatus notificationPost(PrApp* base, const PrAppNotificationDesc* desc) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return PrStatus_InvalidState;
    // A headless application posts nothing, because no user sees it.
    if (app->headless)
        return PrStatus_NotSupported;
    // A balloon without text removes the balloon, so a notification with only a title shows the title as its text. A
    // notification with neither posts nothing.
    PrStringView text = desc->body.size != 0 ? desc->body : desc->title;
    PrStringView title = desc->body.size != 0 ? desc->title : PrStringView{};
    if (text.size == 0)
        return PrStatus_Ok;
    if (!addNotifyIcon(app))
        return PrStatus_Io;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app->messageWindow;
    icon.uID = kNotifyIconId;
    icon.uFlags = NIF_INFO;
    icon.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    copyField(app, title, icon.szInfoTitle, countOf(icon.szInfoTitle));
    copyField(app, text, icon.szInfo, countOf(icon.szInfo));
    if (!Shell_NotifyIconW(NIM_MODIFY, &icon))
        return PrStatus_Io;
    app->notificationTag = desc->tag;
    return PrStatus_Ok;
}

PrStatus openUrl(PrApp* base, PrStringView url) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!isUrl(url))
        return PrStatus_Syntax;
    win32Widen(url, app->utf16);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", app->utf16.data, nullptr, nullptr, SW_SHOWNORMAL);
    // A result above 32 is success, as the function's older interface defines it.
    return reinterpret_cast<INT_PTR>(result) > 32 ? PrStatus_Ok : PrStatus_Io;
}

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

void postRequest(Win32App* app, const Win32Request& request) {
    Win32Request* posted = allocZeroed<Win32Request>();
    *posted = request;
    HWND window = app->wakeWindow.load();
    if (window == nullptr || !PostMessageW(window, kMessageRead, 0, LPARAM(posted))) {
        prRawStringReset(&posted->type);
        posted->bytes.reset();
        prMemoryFree(posted);
    }
}

uint32_t addRequest(Win32App* app, PrAppDataSource source, AppDataAnswer kind, void (*done)(void),
        void* PR_NULLABLE context, PrStringView type = {}) {
    return appDataRequestsAdd(app->requests, source, kind, done, context, type);
}

void dataRetain(PrApp* base, PrAppDataSource source) {
    appDataSourceRetain(static_cast<Win32App*>(base)->sources, source);
}

void dataRelease(PrApp* base, PrAppDataSource source) {
    appDataSourceRelease(static_cast<Win32App*>(base)->sources, source);
}

void dataRead(PrApp* base, const PrAppDataRead* read) {
    Win32App* app = static_cast<Win32App*>(base);
    uint32_t id = addRequest(app, read->source, read->kind, read->done, read->context, read->type);
    if (read->kind == PrAppDataReadKind_File) {
        postRequest(app, Win32Request{
            .id = id,
            .file = true,
            .index = read->index,
            .offset = read->offset,
            .size = read->size,
        });
    } else {
        postRequest(app, Win32Request{.id = id, .type = prRawStringCopy(read->type)});
    }
}

void dataWriteFile(PrApp* base, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
        PrAppDataStatusFn done, void* PR_NULLABLE context) {
    Win32App* app = static_cast<Win32App*>(base);
    uint32_t id = addRequest(app, source, AppDataAnswer_Status, (void (*)(void))done, context);
    Win32Request request{.id = id, .file = true, .write = true, .index = index, .offset = offset};
    if (bytes.size != 0)
        request.bytes.append(bytes.data, uint32_t(bytes.size));
    postRequest(app, request);
}

// Reads or writes a file of a request's source, on the application's thread, and answers. A write at offset zero
// empties the file first.
void answerFileFor(Win32App* app, Win32Request& request) {
    PrAppDataSource source = kPrAppDataSourceNone;
    AppDataAnswer kind = PrAppDataReadKind_File;
    if (!appDataRequestsFind(app->requests, request.id, &source, &kind))
        return;
    bool status = request.write || kind == AppDataAnswer_Status;
    Win32Answer* answer = answerOf(status ? Win32AnswerKind_Status : Win32AnswerKind_Bytes, request.id,
        request.index == UINT32_MAX ? PrStatus_NotSupported : PrStatus_NotFound);
    AppDataStore* store = appDataSourceStore(app->sources, source);
    uint32_t count = 0;
    const PrAppDataFile* files = store != nullptr ? appDataStoreFiles(*store, &count) : nullptr;
    if (request.index < count && files[request.index].path.size != 0 && !files[request.index].directory) {
        RawArray<wchar_t> path{};
        win32Widen(files[request.index].path, path);
        DWORD disposition = request.write ? (request.offset == 0 ? CREATE_ALWAYS : OPEN_ALWAYS) : OPEN_EXISTING;
        HANDLE file = CreateFileW(path.data, request.write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr,
            disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        path.reset();
        answer->status = PrStatus_Io;
        LARGE_INTEGER at{};
        at.QuadPart = LONGLONG(request.offset);
        if (file != INVALID_HANDLE_VALUE && SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) {
            DWORD done = 0;
            if (request.write) {
                if (WriteFile(file, request.bytes.data, request.bytes.count, &done, nullptr) &&
                        done == request.bytes.count)
                    answer->status = PrStatus_Ok;
            } else {
                answer->bytes.resize(uint32_t(request.size < UINT32_MAX ? request.size : UINT32_MAX));
                if (ReadFile(file, answer->bytes.data, answer->bytes.count, &done, nullptr)) {
                    answer->bytes.resize(done);
                    answer->status = PrStatus_Ok;
                }
            }
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
    answerDeliver(app, answer);
}

// Answers a posted request, and frees it.
void answerRequest(Win32App* app, Win32Request* request) {
    if (request->file)
        answerFileFor(app, *request);
    else
        answerReadFor(app, request->id, prRawStringView(&request->type));
    prRawStringReset(&request->type);
    request->bytes.reset();
    prMemoryFree(request);
}

void share(PrAppWindow* window, const PrAppDataContent*, PrRect, PrAppShareFn done, void* PR_NULLABLE context) {
    Win32App* app = appOf(static_cast<Win32Window*>(window));
    uint32_t id = addRequest(app, kPrAppDataSourceNone, AppDataAnswer_Status, (void (*)(void))done, context);
    postRequest(app, Win32Request{.id = id, .file = true, .index = UINT32_MAX});
}

// A cancel may be of the dialog that shows, which the message window closes on the application's thread.
void dataCancel(PrApp* base, void* PR_NULLABLE context) {
    Win32App* app = static_cast<Win32App*>(base);
    appDataRequestsCancel(app->requests, context);
    if (HWND message = app->wakeWindow.load())
        PostMessageW(message, kMessageDialogCancel, 0, 0);
}

bool pointerPosition(const PrApp* base, PrAppDisplayPosition* out) {
    const Win32App* app = static_cast<const Win32App*>(base);
    POINT at{};
    Monitor monitor{};
    if (app->headless || !GetCursorPos(&at) || !monitorOf(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    PrRect logical = logicalOn(monitor, RECT{at.x, at.y, at.x, at.y});
    *out = PrAppDisplayPosition{.display = monitor.id, .position = PrVec2{logical.left, logical.top}};
    return true;
}

// Windows has no hidden application, so the application hides its windows that show, and shows them again.
void setHidden(PrApp* base, bool hidden) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->headless || app->hidden == hidden)
        return;
    app->hidden = hidden;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (hidden && window->shown && IsWindowVisible(window->window)) {
            window->hiddenByApp = true;
            setLock(window, false);
            ShowWindow(window->window, SW_HIDE);
        } else if (!hidden && window->hiddenByApp) {
            window->hiddenByApp = false;
            ShowWindow(window->window, window->overlay ? SW_SHOWNOACTIVATE : SW_SHOW);
            requestFrame(window);
        }
    }
}

bool isHidden(const PrApp* base) {
    return static_cast<const Win32App*>(base)->hidden;
}

PrStatus revealPath(PrApp* base, PrStringView path) {
    Win32App* app = static_cast<Win32App*>(base);
    RawArray<wchar_t> wide{};
    win32Widen(path, wide);
    for (uint32_t i = 0; i < wide.count; ++i) {
        if (wide.data[i] == L'/')
            wide.data[i] = L'\\';
    }
    PrStatus status = PrStatus_Ok;
    PIDLIST_ABSOLUTE item = nullptr;
    if (GetFileAttributesW(wide.data) == INVALID_FILE_ATTRIBUTES)
        status = PrStatus_NotFound;
    else if (app->headless)
        status = PrStatus_NotSupported;
    else if (FAILED(SHParseDisplayName(wide.data, nullptr, &item, 0, nullptr)))
        status = PrStatus_Io;
    else if (FAILED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0)))
        status = PrStatus_Io;
    if (item != nullptr)
        CoTaskMemFree(item);
    wide.reset();
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------
// Global hotkeys, which the message window receives as WM_HOTKEY.

UINT hotkeyModifiersOf(PrAppModifierFlags modifiers) {
    UINT flags = MOD_NOREPEAT;
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        flags |= MOD_SHIFT;
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        flags |= MOD_CONTROL;
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        flags |= MOD_ALT;
    if ((modifiers & PrAppModifierFlag_Super) != 0)
        flags |= MOD_WIN;
    return flags;
}

// Returns the virtual key of the key at `code` in the current layout, or zero for none.
UINT virtualKeyOf(PrAppKeyCode code) {
    for (const ScanKey& key : kScanKeys) {
        if (key.code == code)
            return MapVirtualKeyW(key.scan, MAPVK_VSC_TO_VK_EX);
    }
    return 0;
}

void hotkeyUnregister(PrApp* base, uint64_t tag) {
    Win32App* app = static_cast<Win32App*>(base);
    for (uint32_t i = 0; i < app->hotkeys.count; ++i) {
        if (app->hotkeys.data[i].tag == tag) {
            UnregisterHotKey(app->messageWindow, app->hotkeys.data[i].id);
            app->hotkeys.erase(i);
            return;
        }
    }
}

PrStatus hotkeyRegister(PrApp* base, PrAppHotkey hotkey, uint64_t tag) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return PrStatus_InvalidState;
    UINT key = virtualKeyOf(hotkey.code);
    if (app->headless || key == 0)
        return PrStatus_NotSupported;
    hotkeyUnregister(app, tag);
    // An application's hotkey ids are in [1, 0xBFFF].
    app->lastHotkeyId = app->lastHotkeyId >= 0xBFFF ? 1 : app->lastHotkeyId + 1;
    if (!RegisterHotKey(app->messageWindow, app->lastHotkeyId, hotkeyModifiersOf(hotkey.modifiers), key))
        return PrStatus_NotAllowed;
    app->hotkeys.push(Win32Hotkey{.tag = tag, .id = app->lastHotkeyId});
    return PrStatus_Ok;
}

// Delivers the press of the hotkey whose id is `id`.
void hotkeyPressed(Win32App* app, int id) {
    for (uint32_t i = 0; i < app->hotkeys.count; ++i) {
        if (app->hotkeys.data[i].id != id)
            continue;
        PrAppEvent event{};
        event.kind = PrAppEventKind_Hotkey;
        event.time = secondsNow();
        event.modifiers = modifiersNow();
        event.tag = app->hotkeys.data[i].tag;
        deliver(app, nullptr, event);
        return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// File dialogs, which the shell's common item dialogs show. A dialog's answer is a read of files that no source names.

// Adds `text` to the request's strings, with its NUL.
void requestAdd(Win32DialogRequest& request, PrStringView text) {
    RawArray<wchar_t> wide{};
    win32Widen(text, wide);
    request.starts.push(request.strings.count);
    request.strings.append(wide.data, wide.count);
    wide.reset();
}

void requestReset(Win32DialogRequest* request) {
    request->strings.reset();
    request->starts.reset();
    prMemoryFree(request);
}

// Records the dialog, which shows from the message window once no callback runs. The types' extensions become the
// shell's patterns, as "*.png;*.jpg".
void fileDialog(PrApp* base, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context) {
    Win32App* app = static_cast<Win32App*>(base);
    uint32_t id = addRequest(app, kPrAppDataSourceNone, AppDataAnswer_Source, (void (*)(void))done, context);
    Win32DialogRequest* request = allocZeroed<Win32DialogRequest>();
    request->id = id;
    request->kind = desc->kind;
    request->flags = desc->flags;
    request->parent = desc->parent != nullptr ? static_cast<Win32Window*>(desc->parent)->window : nullptr;
    requestAdd(*request, desc->title);
    requestAdd(*request, desc->directory);
    requestAdd(*request, desc->name);
    RawArray<char> pattern{};
    for (uint32_t i = 0; i < desc->typeCount; ++i) {
        pattern.clear();
        PrStringView rest = desc->types[i].extensions;
        while (rest.size != 0) {
            size_t comma = 0;
            while (comma < rest.size && rest.data[comma] != ',')
                ++comma;
            PrStringView extension = prStringTrim(PrStringView{rest.data, comma});
            if (extension.size != 0) {
                if (pattern.count != 0)
                    pattern.push(';');
                pattern.append("*.", 2);
                pattern.append(extension.data, uint32_t(extension.size));
            }
            rest = comma < rest.size ? PrStringView{rest.data + comma + 1, rest.size - comma - 1} : PrStringView{};
        }
        requestAdd(*request, desc->types[i].name);
        requestAdd(*request, PrStringView{pattern.data != nullptr ? pattern.data : "", pattern.count});
    }
    pattern.reset();
    if (app->messageWindow == nullptr || !PostMessageW(app->messageWindow, kMessageDialog, 0, LPARAM(request)))
        requestReset(request);
}

// Adds the file of `item` to `content`.
void addItemPath(Win32App* app, IShellItem* item, AppDataStore& content) {
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    addFile(app, path, content);
    CoTaskMemFree(path);
}

// Shows the dialog of `request` in its modal loop, and answers its read with what the user chose. A dialog whose read
// was cancelled before it showed does not show.
void showDialog(Win32App* app, Win32DialogRequest* request) {
    PrAppDataSource source = kPrAppDataSourceNone;
    AppDataAnswer kind = AppDataAnswer_Source;
    if (!appDataRequestsFind(app->requests, request->id, &source, &kind)) {
        requestReset(request);
        return;
    }
    AppDataStore* content = allocZeroed<AppDataStore>();
    bool save = request->kind == PrAppFileDialogKind_Save;
    IFileDialog* dialog = nullptr;
    HRESULT result = E_FAIL;
    if (!app->headless && app->ole) {
        const CLSID& kind = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
        result = CoCreateInstance(kind, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    }
    if (FAILED(result) || dialog == nullptr) {
        appDataStoreReset(*content);
        prMemoryFree(content);
        answerDeliver(app, answerOf(Win32AnswerKind_Source, request->id, PrStatus_NotSupported));
        requestReset(request);
        return;
    }
    auto text = [&](uint32_t index) { return request->strings.data + request->starts.data[index]; };
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM;
    bool directories = !save && (request->flags & PrAppFileDialogFlag_Directories) != 0;
    if (!save && (request->flags & PrAppFileDialogFlag_Multiple) != 0)
        options |= FOS_ALLOWMULTISELECT;
    if (directories)
        options |= FOS_PICKFOLDERS;
    if (save)
        options |= FOS_OVERWRITEPROMPT;
    dialog->SetOptions(options);
    if (text(0)[0] != 0)
        dialog->SetTitle(text(0));
    IShellItem* folder = nullptr;
    if (text(1)[0] != 0 && SUCCEEDED(SHCreateItemFromParsingName(text(1), nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder);
        folder->Release();
    }
    if (save && text(2)[0] != 0)
        dialog->SetFileName(text(2));
    uint32_t types = (request->starts.count - 3) / 2;
    if (types != 0 && !directories) {
        RawArray<COMDLG_FILTERSPEC> specs{};
        for (uint32_t i = 0; i < types; ++i)
            specs.push(COMDLG_FILTERSPEC{text(3 + 2 * i), text(4 + 2 * i)});
        dialog->SetFileTypes(types, specs.data);
        specs.reset();
        // A Save dialog adds the first type's first extension, after its "*.", to a name that has none.
        const wchar_t* pattern = text(4);
        if (save && pattern[0] == L'*' && pattern[1] == L'.') {
            wchar_t extension[32]{};
            for (uint32_t i = 0; i + 1 < countOf(extension) && pattern[i + 2] != 0 && pattern[i + 2] != L';'; ++i)
                extension[i] = pattern[i + 2];
            dialog->SetDefaultExtension(extension);
        }
    }
    app->dialog = dialog;
    app->dialogRead = request->id;
    app->dialogParent = request->parent;
    result = dialog->Show(request->parent);
    app->dialog = nullptr;
    app->dialogParent = nullptr;
    if (SUCCEEDED(result) && save) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            addItemPath(app, item, *content);
            item->Release();
        }
    } else if (SUCCEEDED(result)) {
        IFileOpenDialog* open = nullptr;
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&open))) && SUCCEEDED(open->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item))) {
                    addItemPath(app, item, *content);
                    item->Release();
                }
            }
        }
        if (items != nullptr)
            items->Release();
        if (open != nullptr)
            open->Release();
    }
    dialog->Release();
    // The user's choice is a source of its files, which the answer holds a reference to. A cancel answers None.
    Win32Answer* answer = answerOf(Win32AnswerKind_Source, request->id, PrStatus_Ok);
    uint32_t chosen = 0;
    appDataStoreFiles(*content, &chosen);
    if (chosen != 0) {
        answer->source = appDataSourceCreate(app->sources, content, nullptr);
    } else {
        appDataStoreReset(*content);
        prMemoryFree(content);
    }
    answerDeliver(app, answer);
    requestReset(request);
}

// Closes the dialog that shows if its read was cancelled.
void closeCancelledDialog(Win32App* app) {
    PrAppDataSource source = kPrAppDataSourceNone;
    AppDataAnswer kind = AppDataAnswer_Source;
    if (app->dialog != nullptr && !appDataRequestsFind(app->requests, app->dialogRead, &source, &kind))
        app->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}

// Returns `html` as CF_HTML holds it: a header of the offsets of the document and of its fragment, and the document.
RawArray<char> cfHtmlOf(PrStringView html) {
    constexpr PrStringView kHeader = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    constexpr PrStringView kBefore = "<html><body><!--StartFragment-->";
    constexpr PrStringView kAfter = "<!--EndFragment--></body></html>";
    RawArray<char> out{};
    out.append(kHeader.data, uint32_t(kHeader.size));
    out.append(kBefore.data, uint32_t(kBefore.size));
    if (html.size != 0)
        out.append(html.data, uint32_t(html.size));
    out.append(kAfter.data, uint32_t(kAfter.size));
    out.push('\0');
    // Each offset is ten digits, after its name and its colon, in the header's order.
    uint32_t offsets[] = {
        uint32_t(kHeader.size),
        out.count - 1,
        uint32_t(kHeader.size + kBefore.size),
        uint32_t(kHeader.size + kBefore.size + html.size),
    };
    PrStringView names[] = {"StartHTML:", "EndHTML:", "StartFragment:", "EndFragment:"};
    for (uint32_t i = 0; i < 4; ++i) {
        PrStringView header{out.data, kHeader.size};
        size_t at = prStringFind(header, names[i]) + names[i].size;
        for (uint32_t digit = 10, value = offsets[i]; digit > 0; --digit, value /= 10)
            out.data[at + digit - 1] = char('0' + value % 10);
    }
    return out;
}

// Sets one format of the clipboard, which owns `memory` once it takes it. Returns false if it refuses.
bool setClipboard(UINT format, HGLOBAL PR_NULLABLE memory) {
    if (memory == nullptr)
        return false;
    if (SetClipboardData(format, memory) != nullptr)
        return true;
    GlobalFree(memory);
    return false;
}

PrStatus dataWrite(PrApp* base, PrAppDataSource source, const PrAppDataContent* content) {
    Win32App* app = static_cast<Win32App*>(base);
    if (source == kPrAppDataSourceSelection)
        return PrStatus_NotSupported;
    // The clipboard takes data only from an owner window, which exists during a run.
    if (app->messageWindow == nullptr || !openClipboard(app->messageWindow))
        return PrStatus_Io;
    bool set = EmptyClipboard() != FALSE;
    if (set && (content->forms & PrAppDataFormFlag_Text) != 0) {
        win32Widen(content->text, app->utf16);
        set = setClipboard(CF_UNICODETEXT, globalOfBytes(app->utf16.data, size_t(app->utf16.count) * sizeof(wchar_t)));
    }
    if (set && (content->forms & PrAppDataFormFlag_Html) != 0 && app->htmlFormat != 0) {
        RawArray<char> html = cfHtmlOf(content->html);
        set = setClipboard(app->htmlFormat, globalOfBytes(html.data, html.count));
        html.reset();
    }
    if (set && (content->forms & PrAppDataFormFlag_Files) != 0)
        set = setClipboard(CF_HDROP, fileListOf(app, *content));
    if (set && (content->forms & PrAppDataFormFlag_Image) != 0)
        set = setClipboard(CF_DIBV5, dibOf(content->image));
    for (uint32_t i = 0; set && (content->forms & PrAppDataFormFlag_Custom) != 0 && i < content->customCount; ++i) {
        RawArray<wchar_t> name{};
        win32Widen(content->custom[i].type, name);
        UINT format = RegisterClipboardFormatW(name.data);
        name.reset();
        set = format != 0 &&
            setClipboard(format, globalOfBytes(content->custom[i].bytes.data, content->custom[i].bytes.size));
    }
    CloseClipboard();
    return set ? PrStatus_Ok : PrStatus_Io;
}

// Places a new window as its description asks. By default, a window cascades from the last one that opened, and the
// first is centered on the primary display.
void placeNewWindow(Win32App* app, Win32Window* window, const PrAppWindowDesc* desc) {
    Monitor monitor{};
    if (desc->placement == PrAppWindowPlacement_Position && monitorWithId(desc->position.display, &monitor)) {
        placeOnMonitor(window, monitor, desc->position.position);
        return;
    }
    // A dialog is centered on its parent.
    PrAppDisplayPosition parentAt{};
    if (window->parent != nullptr && position(window->parent, &parentAt) && monitorWithId(parentAt.display, &monitor)) {
        PrUVec2 around = window->parent->extent;
        PrVec2 at{
            parentAt.position.x + (float(around.x) - float(window->extent.x)) / 2,
            parentAt.position.y + (float(around.y) - float(window->extent.y)) / 2,
        };
        placeOnMonitor(window, monitor, at);
        return;
    }
    if (desc->placement == PrAppWindowPlacement_Center) {
        HMONITOR active = nullptr;
        if (app->lastActive != nullptr)
            active = MonitorFromWindow(app->lastActive->window, MONITOR_DEFAULTTOPRIMARY);
        if ((active != nullptr && monitorOf(active, &monitor)) || primaryMonitor(&monitor))
            centerOnMonitor(window, monitor);
        return;
    }
    PrAppDisplayPosition previous{};
    if (app->windows.count != 0 && position(app->windows.data[app->windows.count - 1], &previous) &&
            monitorWithId(previous.display, &monitor)) {
        // The offset is the height of a title bar, so that the last window's title shows below the new window's.
        float step = float(GetSystemMetricsForDpi(SM_CYCAPTION, monitor.dpi)) * 96.0f / float(monitor.dpi) +
            float(GetSystemMetricsForDpi(SM_CYFRAME, monitor.dpi)) * 96.0f / float(monitor.dpi);
        placeOnMonitor(window, monitor, PrVec2{previous.position.x + step, previous.position.y + step});
    }
}

// Returns the orientation of a display whose current bounds are `bounds` and which `turn` rotates. Windows names no
// natural orientation, so a display that is taller than wide is a portrait, flipped if it is turned by 270 degrees, and
// one that is wider is a landscape, flipped if it is turned by 180.
PrAppOrientation orientationOf(DWORD turn, PrRect bounds) {
    if (bounds.bottom - bounds.top > bounds.right - bounds.left)
        return turn == DMDO_270 ? PrAppOrientation_PortraitFlipped : PrAppOrientation_Portrait;
    return turn == DMDO_180 ? PrAppOrientation_LandscapeFlipped : PrAppOrientation_Landscape;
}

// Returns the name of the monitor's display as the system shows it, which the application keeps until the next
// DisplaysChanged event.
PrStringView displayNameOf(const Win32App* app, const Monitor& monitor) {
    for (uint32_t i = 0; i < app->displayNameIds.count; ++i) {
        if (app->displayNameIds.data[i] == monitor.id)
            return PrStringView{app->displayNames.data[i], strlen(app->displayNames.data[i])};
    }
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (!EnumDisplayDevicesW(monitor.device, 0, &device, 0))
        return PrStringView{};
    PrStringView name = win32Narrow(device.DeviceString, int(wcslen(device.DeviceString)), app->utf8);
    char* copy = static_cast<char*>(prMemoryAlloc(name.size + 1));
    prMemCopy(copy, name.data, name.size);
    copy[name.size] = 0;
    app->displayNameIds.push(monitor.id);
    app->displayNames.push(copy);
    return PrStringView{copy, name.size};
}

void displayNamesReset(Win32App* app) {
    for (uint32_t i = 0; i < app->displayNames.count; ++i)
        prMemoryFree(app->displayNames.data[i]);
    app->displayNames.reset();
    app->displayNameIds.reset();
}

// Returns what the application reports of `monitor`. `factory` gives the monitor's output, for its colors, or null.
PrAppDisplayInfo displayInfoOf(const Win32App* app, const Monitor& monitor, IDXGIFactory1* PR_NULLABLE factory) {
    IDXGIOutput6* output = factory != nullptr ? outputOf(factory, monitor.handle) : nullptr;
    PrAppDisplay display = displayOfOutput(output, monitor.handle);
    if (output != nullptr)
        output->Release();
    display.pixelRatio = float(monitor.dpi) / 96.0f;
    PrRect bounds = logicalOn(monitor, monitor.bounds);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    bool moded = EnumDisplaySettingsW(monitor.device, ENUM_CURRENT_SETTINGS, &mode) != 0;
    return PrAppDisplayInfo{
        .id = monitor.id,
        .name = displayNameOf(app, monitor),
        .display = display,
        .extent = PrVec2{bounds.right, bounds.bottom},
        .workArea = logicalOn(monitor, monitor.work),
        .refreshRate = moded && mode.dmDisplayFrequency > 1 ? float(mode.dmDisplayFrequency) : 0.0f,
        .orientation = orientationOf(moded ? mode.dmDisplayOrientation : DMDO_DEFAULT, bounds),
        .main = monitor.primary,
    };
}

// Returns a factory of DXGI, which the caller releases, or null if it cannot be made.
IDXGIFactory1* PR_NULLABLE dxgiFactory() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return nullptr;
    return factory;
}

uint32_t displays(const PrApp* base, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity) {
    const Win32App* app = static_cast<const Win32App*>(base);
    RawArray<Monitor> monitors;
    monitorsOf(monitors);
    IDXGIFactory1* factory = capacity != 0 ? dxgiFactory() : nullptr;
    for (uint32_t i = 0; i < monitors.count && i < capacity; ++i)
        out[i] = displayInfoOf(app, monitors.data[i], factory);
    if (factory != nullptr)
        factory->Release();
    uint32_t count = monitors.count;
    monitors.reset();
    return count;
}

bool displayInfo(const PrApp* base, PrAppDisplayId id, PrAppDisplayInfo* out) {
    Monitor monitor{};
    if (!monitorWithId(id, &monitor))
        return false;
    IDXGIFactory1* factory = dxgiFactory();
    *out = displayInfoOf(static_cast<const Win32App*>(base), monitor, factory);
    if (factory != nullptr)
        factory->Release();
    return true;
}

PrStatus windowCreate(PrApp* base, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return PrStatus_InvalidState;
    if (app->windowModel == PrAppWindowModel_Single && app->windows.count != 0)
        return PrStatus_NotSupported;
    Win32Window* window = allocZeroed<Win32Window>();
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
    window->setPointerPassthroughFn = setPointerPassthrough;
    window->setPointerModeFn = setPointerMode;
    window->pointerModeFn = pointerMode;
    window->dragStartFn = dragStart;
    window->shareFn = share;
    window->surfaceFn = surface;
    window->setAspectRatioFn = setAspectRatio;
    window->setPopupPlacementFn = setPopupPlacement;
    window->isOccludedFn = isOccluded;
    window->setKeepAboveFn = setKeepAbove;
    window->setIconFn = setIcon;
    window->setProgressFn = setProgress;
    window->setMenuBarFn = windowSetMenuBar;
    window->showMenuFn = showMenu;
    window->app = app;
    window->serial = ++app->lastSerial;
    window->delivery.handler = desc->handler;
    RawArray<wchar_t> title;
    win32Widen(desc->title, title);
    window->title = title.data;
    window->titleText = prRawStringCopy(desc->title);
    window->minExtent = desc->minExtent;
    window->maxExtent = desc->maxExtent;
    window->role = desc->role;
    window->overlay = desc->role == PrAppWindowRole_Overlay;
    bool popup = desc->role == PrAppWindowRole_Popup;
    bool dialog = desc->role == PrAppWindowRole_Dialog;
    if (desc->parent != nullptr && (popup || dialog))
        window->parent = static_cast<Win32Window*>(desc->parent);
    window->modal = dialog && desc->modal && window->parent != nullptr;
    window->popup = desc->popup;
    window->shown = !desc->hidden;
    window->customTitleBar = desc->decoration == PrAppWindowDecoration_Custom && !window->overlay && !popup;
    window->pressedButton = HTNOWHERE;
    window->extent = desc->extent;
    // A dialog has no minimize button, since it follows its parent, and a window without decoration has no title bar.
    window->style = styleOf(app);
    if (dialog)
        window->style &= ~DWORD(WS_MINIMIZEBOX);
    if (desc->decoration == PrAppWindowDecoration_None && !popup && !window->overlay)
        window->style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    DWORD style = window->style;
    if (isFixed(window))
        style &= ~DWORD(WS_THICKFRAME | WS_MAXIMIZEBOX);
    // An overlay is a popup above every other window, which the taskbar and the window switcher leave out, and which a
    // press does not activate. A popup is the same, but above its parent alone, which owns it.
    DWORD exStyle = 0;
    if (window->overlay) {
        style = WS_POPUP;
        exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    } else if (popup) {
        style = WS_POPUP;
        exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    }
    if (window->overlay || popup)
        window->style = WS_POPUP;
    // An owned window stays above its owner and off the taskbar. An Accessory application's windows are owned by its
    // message window, which keeps them off the taskbar.
    HWND owner = window->parent != nullptr ? window->parent->window : nullptr;
    if (owner == nullptr && desc->role == PrAppWindowRole_Normal && app->presence == PrAppPresence_Accessory)
        owner = app->messageWindow;
    window->window = CreateWindowExW(exStyle, kClassName, window->title, style, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, GetModuleHandleW(nullptr), window);
    if (window->window == nullptr) {
        prMemoryFree(window->title);
        prRawStringReset(&window->titleText);
        prMemoryFree(window);
        return PrStatus_Io;
    }
    window->dpi = GetDpiForWindow(window->window);
    if (window->dpi == 0)
        window->dpi = 96;
    // The window's frame is computed again, now that the window can answer WM_NCCALCSIZE for a custom title bar.
    if (window->customTitleBar)
        SetWindowPos(window->window, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    placeWindow(window, Placement_Center);
    if (!app->headless && popup)
        placePopup(window);
    else if (!app->headless)
        placeNewWindow(app, window, desc);
    window->cursor = cursorFor(window->shape);
    if (app->ole)
        RegisterDragDrop(window->window, &gDropTarget);
    refreshPreferences(window);
    window->display = prAppDisplayDefault();
    app->windows.push(window);
    if (desc->pointerPassthrough)
        setPointerPassthrough(window, true);
    showMenuBar(window);
    // An overlay and a popup show without taking the foreground.
    if (!app->headless && window->shown && (window->overlay || popup)) {
        ShowWindow(window->window, SW_SHOWNOACTIVATE);
    } else if (!app->headless && window->shown) {
        ShowWindow(window->window, SW_SHOW);
        SetForegroundWindow(window->window);
        setBlocking(window, true);
    }
    if (!app->headless && window->shown && desc->state != PrAppWindowState_Normal)
        setState(window, desc->state);
    requestFrame(window);
    *out = window;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrApp* prAppWin32Create(const PrAppOptions* options) {
    // The call fails if the process already chose an awareness, which then stands.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Win32App* app = allocZeroed<Win32App>();
    app->destroyFn = destroy;
    app->runFn = run;
    app->stopFn = stop;
    app->wakeFn = wake;
    app->notificationPostFn = notificationPost;
    app->openUrlFn = openUrl;
    app->cancelFn = dataCancel;
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
    app->quitRespondFn = quitRespond;
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
    app->setMenuBarFn = setMenuBar;
    app->statusItemCreateFn = statusItemCreate;
    appDataSourcesInit(app->sources);
    appDataRequestsInit(app->requests, app->sources);
    app->headless = options->headless;
    app->presence = options->presence;
    app->windowModel = options->windowModel;
    if (options->singleInstance.size != 0)
        win32Widen(options->singleInstance, app->instanceName);
    return app;
}
