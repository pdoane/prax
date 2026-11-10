// Applications: a window, its event loop, its input, and presenting to it.
//
// An application is used from the thread that runs it, except for prAppWake, which any thread may call while it runs.
// One application runs at a time in a process.

#pragma once

#include "pr/app_display.h"
#include "pr/app_surface.h"
#include "pr/app_user_preferences.h"
#include "pr/base.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A window and its event loop, created by its backend's constructor.
typedef struct PrApp PrApp;

// The kinds of input event.
enum {
    PrAppEventKind_PointerMove,
    PrAppEventKind_PointerDown, // a button pressed
    PrAppEventKind_PointerUp,   // a button released
    PrAppEventKind_Scroll,
    PrAppEventKind_KeyDown,
    PrAppEventKind_KeyUp,
    PrAppEventKind_Text,           // text typed, as UTF-8
    PrAppEventKind_Clipboard,      // the clipboard's text, in response to a request or a paste
    PrAppEventKind_ClipboardImage, // the clipboard's image, in response to a request
    PrAppEventKind_Wake,           // prAppWake was called

    // An input method's composition. An update gives the whole text being composed, and the first update starts a
    // composition. The end gives the text that the composition commits, which is empty if it was canceled. The text
    // committed follows as a Text event, so that a program that takes no composition still receives it.
    PrAppEventKind_CompositionUpdate,
    PrAppEventKind_CompositionEnd,

    PrAppEventKind_Notification,   // the user clicked a notification posted with prAppNotificationPost
};
typedef uint8_t PrAppEventKind;

// Keys by meaning rather than position. A printable key is its ASCII character, uppercase for letters. The other keys
// are numbered from PrAppKey_Left. Each named key is a key value of UI Events (uievents-key), which prAppKeyName gives.
enum {
    PrAppKey_Unknown = 0,
    PrAppKey_Left = 256,
    PrAppKey_Right,
    PrAppKey_Up,
    PrAppKey_Down,
    PrAppKey_Home,
    PrAppKey_End,
    PrAppKey_PageUp,
    PrAppKey_PageDown,
    PrAppKey_Enter,
    PrAppKey_Escape,
    PrAppKey_Backspace,
    PrAppKey_Delete,
    PrAppKey_Tab,
    PrAppKey_Insert,
    PrAppKey_Shift,
    PrAppKey_Control,
    PrAppKey_Alt,
    PrAppKey_Meta,
    PrAppKey_CapsLock,
    PrAppKey_NumLock,
    PrAppKey_Fn,
    PrAppKey_ContextMenu,
    PrAppKey_Help,
    PrAppKey_Clear,
    PrAppKey_Dead,              // a dead key, which marks the next key that the layout combines with it
    PrAppKey_F1,
    PrAppKey_F2,
    PrAppKey_F3,
    PrAppKey_F4,
    PrAppKey_F5,
    PrAppKey_F6,
    PrAppKey_F7,
    PrAppKey_F8,
    PrAppKey_F9,
    PrAppKey_F10,
    PrAppKey_F11,
    PrAppKey_F12,
    PrAppKey_F13,
    PrAppKey_F14,
    PrAppKey_F15,
    PrAppKey_F16,
    PrAppKey_F17,
    PrAppKey_F18,
    PrAppKey_F19,
    PrAppKey_F20,
};
typedef uint16_t PrAppKey;

// Keys by position on the keyboard, whatever the layout: the keys of a standard 104-key keyboard and of a Mac's, named
// as UI Events names their codes (uievents-code), which prAppKeyCodeName gives. A writing system key is named for the
// character it types on a US layout.
enum {
    PrAppKeyCode_Unidentified,
    PrAppKeyCode_Backquote,
    PrAppKeyCode_Backslash,
    PrAppKeyCode_BracketLeft,
    PrAppKeyCode_BracketRight,
    PrAppKeyCode_Comma,
    PrAppKeyCode_Digit0,
    PrAppKeyCode_Digit1,
    PrAppKeyCode_Digit2,
    PrAppKeyCode_Digit3,
    PrAppKeyCode_Digit4,
    PrAppKeyCode_Digit5,
    PrAppKeyCode_Digit6,
    PrAppKeyCode_Digit7,
    PrAppKeyCode_Digit8,
    PrAppKeyCode_Digit9,
    PrAppKeyCode_Equal,
    PrAppKeyCode_IntlBackslash,
    PrAppKeyCode_IntlRo,
    PrAppKeyCode_IntlYen,
    PrAppKeyCode_KeyA,
    PrAppKeyCode_KeyB,
    PrAppKeyCode_KeyC,
    PrAppKeyCode_KeyD,
    PrAppKeyCode_KeyE,
    PrAppKeyCode_KeyF,
    PrAppKeyCode_KeyG,
    PrAppKeyCode_KeyH,
    PrAppKeyCode_KeyI,
    PrAppKeyCode_KeyJ,
    PrAppKeyCode_KeyK,
    PrAppKeyCode_KeyL,
    PrAppKeyCode_KeyM,
    PrAppKeyCode_KeyN,
    PrAppKeyCode_KeyO,
    PrAppKeyCode_KeyP,
    PrAppKeyCode_KeyQ,
    PrAppKeyCode_KeyR,
    PrAppKeyCode_KeyS,
    PrAppKeyCode_KeyT,
    PrAppKeyCode_KeyU,
    PrAppKeyCode_KeyV,
    PrAppKeyCode_KeyW,
    PrAppKeyCode_KeyX,
    PrAppKeyCode_KeyY,
    PrAppKeyCode_KeyZ,
    PrAppKeyCode_Minus,
    PrAppKeyCode_Period,
    PrAppKeyCode_Quote,
    PrAppKeyCode_Semicolon,
    PrAppKeyCode_Slash,
    PrAppKeyCode_AltLeft,
    PrAppKeyCode_AltRight,
    PrAppKeyCode_Backspace,
    PrAppKeyCode_CapsLock,
    PrAppKeyCode_ContextMenu,
    PrAppKeyCode_ControlLeft,
    PrAppKeyCode_ControlRight,
    PrAppKeyCode_Enter,
    PrAppKeyCode_MetaLeft,
    PrAppKeyCode_MetaRight,
    PrAppKeyCode_ShiftLeft,
    PrAppKeyCode_ShiftRight,
    PrAppKeyCode_Space,
    PrAppKeyCode_Tab,
    PrAppKeyCode_Lang1,
    PrAppKeyCode_Lang2,
    PrAppKeyCode_Delete,
    PrAppKeyCode_End,
    PrAppKeyCode_Help,
    PrAppKeyCode_Home,
    PrAppKeyCode_Insert,
    PrAppKeyCode_PageDown,
    PrAppKeyCode_PageUp,
    PrAppKeyCode_ArrowDown,
    PrAppKeyCode_ArrowLeft,
    PrAppKeyCode_ArrowRight,
    PrAppKeyCode_ArrowUp,
    PrAppKeyCode_NumLock,
    PrAppKeyCode_Numpad0,
    PrAppKeyCode_Numpad1,
    PrAppKeyCode_Numpad2,
    PrAppKeyCode_Numpad3,
    PrAppKeyCode_Numpad4,
    PrAppKeyCode_Numpad5,
    PrAppKeyCode_Numpad6,
    PrAppKeyCode_Numpad7,
    PrAppKeyCode_Numpad8,
    PrAppKeyCode_Numpad9,
    PrAppKeyCode_NumpadAdd,
    PrAppKeyCode_NumpadComma,
    PrAppKeyCode_NumpadDecimal,
    PrAppKeyCode_NumpadDivide,
    PrAppKeyCode_NumpadEnter,
    PrAppKeyCode_NumpadEqual,
    PrAppKeyCode_NumpadMultiply,
    PrAppKeyCode_NumpadSubtract,
    PrAppKeyCode_Escape,
    PrAppKeyCode_Fn,
    PrAppKeyCode_F1,
    PrAppKeyCode_F2,
    PrAppKeyCode_F3,
    PrAppKeyCode_F4,
    PrAppKeyCode_F5,
    PrAppKeyCode_F6,
    PrAppKeyCode_F7,
    PrAppKeyCode_F8,
    PrAppKeyCode_F9,
    PrAppKeyCode_F10,
    PrAppKeyCode_F11,
    PrAppKeyCode_F12,
    PrAppKeyCode_F13,
    PrAppKeyCode_F14,
    PrAppKeyCode_F15,
    PrAppKeyCode_F16,
    PrAppKeyCode_F17,
    PrAppKeyCode_F18,
    PrAppKeyCode_F19,
    PrAppKeyCode_F20,
    PrAppKeyCode_PrintScreen,
    PrAppKeyCode_ScrollLock,
    PrAppKeyCode_Pause,
    PrAppKeyCode_Count,
};
typedef uint8_t PrAppKeyCode;

// The modifier keys held during an event, and the lock keys that are on.
enum {
    PrAppModifierFlag_Shift = 1 << 0,
    PrAppModifierFlag_Control = 1 << 1,
    PrAppModifierFlag_Alt = 1 << 2,
    PrAppModifierFlag_Super = 1 << 3,
    PrAppModifierFlag_CapsLock = 1 << 4,
};
typedef uint8_t PrAppModifierFlags;

// The shapes of the pointer over the window. They are the predefined cursors of CSS (css-ui-4/#cursor), so that a
// document's cursor maps to one. A backend shows the platform's nearest cursor, or the arrow if the platform has none
// close to a shape.
enum {
    PrAppCursor_Arrow,          // default
    PrAppCursor_None,           // no cursor shows
    PrAppCursor_ContextMenu,    // a context menu is available
    PrAppCursor_Help,
    PrAppCursor_Hand,           // a link or another control that activates
    PrAppCursor_Progress,       // the program is busy, and the user can still interact with it
    PrAppCursor_Wait,           // the program is busy, and the user cannot interact with it
    PrAppCursor_Cell,           // a cell or a set of cells can be selected
    PrAppCursor_Crosshair,
    PrAppCursor_Text,
    PrAppCursor_VerticalText,
    PrAppCursor_Alias,          // a drag makes an alias or a shortcut
    PrAppCursor_Copy,           // a drag copies
    PrAppCursor_Move,           // a drag moves
    PrAppCursor_NoDrop,         // a drop is not allowed here
    PrAppCursor_NotAllowed,
    PrAppCursor_Grab,           // a drag grabs and moves what is under the pointer
    PrAppCursor_Grabbing,       // a drag grabbed what is under the pointer

    // Resizes that move one edge or corner, which the platform may show pointing outward only.
    PrAppCursor_ResizeN,
    PrAppCursor_ResizeE,
    PrAppCursor_ResizeS,
    PrAppCursor_ResizeW,
    PrAppCursor_ResizeNE,
    PrAppCursor_ResizeNW,
    PrAppCursor_ResizeSE,
    PrAppCursor_ResizeSW,

    // Resizes in both directions along an axis.
    PrAppCursor_ResizeX,        // east and west
    PrAppCursor_ResizeY,        // north and south
    PrAppCursor_ResizeNESW,
    PrAppCursor_ResizeNWSE,
    PrAppCursor_ResizeColumn,   // a divider between columns moves left or right
    PrAppCursor_ResizeRow,      // a divider between rows moves up or down

    PrAppCursor_AllScroll,      // the content scrolls in any direction
    PrAppCursor_ZoomIn,
    PrAppCursor_ZoomOut,
    PrAppCursor_Count,
};
typedef uint8_t PrAppCursor;

// The unit of a scroll's delta: pixels from a device that measures them, such as a trackpad, or lines from a wheel that
// turns in steps.
enum {
    PrAppScrollUnit_Pixel,
    PrAppScrollUnit_Line,
};
typedef uint8_t PrAppScrollUnit;

// The height of a line in logical pixels, for a program that scrolls by lines and has no line height of its own.
static const float kPrAppScrollLineHeight = 40.0f;

// A range of a string, in bytes from its start.
typedef struct PrAppTextRange {
    uint32_t start;
    uint32_t end;
} PrAppTextRange;

// A pointer event: PointerMove, PointerDown or PointerUp. The position is in logical pixels from the window's top left.
typedef struct PrAppPointerEvent {
    PrVec2 position;
    uint8_t button;             // the button pressed or released, 0 for the primary button
    uint32_t clicks;            // PointerDown: the number of presses in the current multi-click, including this one
} PrAppPointerEvent;

// A Scroll event at `position`, in logical pixels from the window's top left. The delta is positive to the right and
// down, in the unit that the device reports. prAppEventScrollPixels converts it to pixels.
typedef struct PrAppScrollEvent {
    PrVec2 position;
    PrVec2 delta;
    PrAppScrollUnit unit;
} PrAppScrollEvent;

// A KeyDown or KeyUp event. `key` is the key's meaning, and `code` its position.
//
// `text` is the character that the key types with Shift and the layout's third level (AltGr, Option) applied, but
// without Control or Command: `a` for A and for Control+A, `A` for Shift+A, and `å` for Option+A on a Mac. The text is
// empty if the key types no character. A modifier key sends KeyDown when pressed and KeyUp when released. A lock key
// sends KeyDown when it turns on and KeyUp when it turns off. The Text event that a key types comes after the key's
// KeyDown and before the next KeyDown, so a program that refuses a key press can drop its text.
//
// `composing` is true if the input method takes the key to begin, continue or end a composition. The input method
// interprets such a key, as it does an Enter that commits a composition. A program acts only on the composition events
// and the committed text.
typedef struct PrAppKeyboardEvent {
    PrAppKey key;
    PrAppKeyCode code;
    bool repeat;                // KeyDown: true if the event repeats a held key
    bool composing;             // KeyDown
    PrStringView text;
} PrAppKeyboardEvent;

// A Text, Clipboard, CompositionUpdate or CompositionEnd event: the text typed, read from the clipboard, or composed.
typedef struct PrAppInputEvent {
    PrStringView text;
    PrAppTextRange selection;   // CompositionUpdate: the range of the text that the input method selects
} PrAppInputEvent;

// A ClipboardImage event: the clipboard's image, straight-alpha sRGB Rgba8, or empty pixels if it holds no image.
typedef struct PrAppImageEvent {
    PrPixelView pixels;
} PrAppImageEvent;

// A Notification event: the click on a notification posted with prAppNotificationPost.
typedef struct PrAppNotificationEvent {
    uint64_t tag;               // the tag from PrAppNotificationDesc
} PrAppNotificationEvent;

// An event. Its kind decides which member of the union holds its details. Some kinds, such as Wake, have no details. A
// string or pixels that an event holds are valid only during delivery.
typedef struct PrAppEvent {
    PrAppEventKind kind;
    PrAppModifierFlags modifiers;
    double time;                // seconds, from a monotonic clock
    union {
        PrAppPointerEvent pointer;
        PrAppScrollEvent scroll;
        PrAppKeyboardEvent keyboard;
        PrAppInputEvent input;
        PrAppImageEvent image;
        PrAppNotificationEvent notification;
    };
} PrAppEvent;

// Returns a Scroll event's delta in logical pixels, converting each line to `lineHeight` pixels.
PrVec2 prAppEventScrollPixels(const PrAppEvent* event, float lineHeight);

// Returns or replaces the text of a keyboard or input event. Another event has no text: the getter returns an empty
// view, and the setter does nothing.
PrStringView prAppEventText(const PrAppEvent* event);
void prAppEventSetText(PrAppEvent* event, PrStringView text);

// Returns or replaces the position of a pointer or scroll event. The getter writes the position to `out` and returns
// true. Another event has no position: the getter writes zero and returns false, and the setter does nothing.
bool prAppEventPosition(const PrAppEvent* event, PrVec2* out);
void prAppEventSetPosition(PrAppEvent* event, PrVec2 position);

// The key value of UI Events that a named key has, such as "ArrowLeft" for PrAppKey_Left, or the empty view for a
// printable key or PrAppKey_Unknown.
PrStringView prAppKeyName(PrAppKey key);

// The code of UI Events that names a key's position, such as "KeyA", or "Unidentified".
PrStringView prAppKeyCodeName(PrAppKeyCode code);

// The parameters of a frame: the display that shows the window as the frame begins, and the user's preferences then.
// The display's headroom can change from one frame to the next, as its brightness changes. A move to another display
// requests a frame.
typedef struct PrAppFrame {
    double time;                // seconds, from a monotonic clock
    double delta;               // seconds since the last frame, or zero for the first
    PrVec2 extent;              // the window's extent in logical pixels
    PrAppDisplay display;
    PrAppUserPreferences preferences;
} PrAppFrame;

// What a frame reports to the window, which the frame handler returns. A library that draws a surface reports its own
// (prUiOutput, prHtmlViewOutput), and a host with several surfaces combines them (prAppOutputPlaced, prAppOutputMerge).
typedef struct PrAppOutput {
    PrAppCursor cursor;         // the shape of the pointer over the window
    bool wantsFrame;            // true to deliver another frame when the display next needs one
    bool hasTextInput;          // true if text is being edited at `textInput`
    PrRect textInput;           // the caret, in logical px of the window, where the input method's windows open

    // When to deliver a frame, on the clock of PrAppFrame::time, for what changes by itself at a known time, or zero
    // for no time. The application sleeps until then. Each frame's output replaces the last frame's time.
    double frameTime;
} PrAppOutput;

// The callbacks into the program, each passed `context`. All are called on the thread that runs the application, and
// none is nested in another. `start` is called once the window exists, with `window`, the window as the first frame
// finds it (its delta is zero), so that what the program makes for the window can start at its extent, on its display,
// for the user's preferences. `shutdown` is called last of all. After `shutdown`, the application makes no more calls.
// Every callback must be set.
//
// `frame` runs within the window's display transaction: at the display's refresh after a frame is requested, and at
// each step of a resize, whether or not one is requested. What `frame` presents through a swap chain that presents
// within the transaction (prGpuSwapChainCreate) is shown with the window at the frame's extent. What it returns is
// applied to the window when it returns.
typedef struct PrAppHandler {
    void* PR_NULLABLE context;
    void (*start)(void* PR_NULLABLE context, PrApp* app, const PrAppFrame* window);
    PrAppOutput (*frame)(void* PR_NULLABLE context, PrApp* app, const PrAppFrame* frame);
    void (*event)(void* PR_NULLABLE context, PrApp* app, const PrAppEvent* event);
    void (*shutdown)(void* PR_NULLABLE context, PrApp* app);
} PrAppHandler;

// The library's backends of applications, which the build turns on (README.md), and Default, the platform's own.
enum {
    PrAppBackend_Default,
    PrAppBackend_Cocoa,
    PrAppBackend_Win32,
    PrAppBackend_Web,
    PrAppBackend_Count,
};
typedef uint8_t PrAppBackend;

// Returns the backend that `name` names, as prAppBackendName spells it, ignoring case, or Count if it names none.
PrAppBackend prAppBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prAppBackendName(PrAppBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prAppBackendIsBuilt(PrAppBackend backend);

// The initial state of an application's window. The backends' constructors take it.
typedef struct PrAppDesc {
    PrStringView title;
    PrUVec2 extent;             // logical pixels, clamped to the screen
    bool headless;              // the window stays off the screen, and the process is never activated
} PrAppDesc;

// A desktop notification. The title and body are UTF-8.
typedef struct PrAppNotificationDesc {
    PrStringView title;
    PrStringView body;
    uint64_t tag;               // chosen by the program, and reported by the Notification event of a click
} PrAppNotificationDesc;

// The functions of an application, at the start of its backend's state. Each member implements the function that it is
// named for. A null `destroy` leaves nothing to free. Callers use the functions below, never these members. A backend
// delivers each frame with prAppDeliverFrame. `requestFrameAt` replaces the time of the last call, and zero leaves no
// time. A backend that implements `isActive` requests a frame whenever its result changes.
struct PrApp {
    void (*PR_NULLABLE destroy)(PrApp* app);
    void (*run)(PrApp* app, const PrAppHandler* handler);
    void (*stop)(PrApp* app);
    void (*wake)(PrApp* app);
    void (*requestFrame)(PrApp* app);
    void (*requestFrameAt)(PrApp* app, double time);
    void (*setCursor)(PrApp* app, PrAppCursor cursor);
    void (*PR_NULLABLE setTextInputRect)(PrApp* app, PrRect rect);
    void (*setTitle)(PrApp* app, PrStringView title);
    void (*PR_NULLABLE setExtent)(PrApp* app, PrUVec2 extent);
    void (*PR_NULLABLE requestAttention)(PrApp* app);
    bool (*PR_NULLABLE isActive)(PrApp* app);
    PrStatus (*PR_NULLABLE notificationPost)(PrApp* app, const PrAppNotificationDesc* desc);
    PrStatus (*surface)(PrApp* app, PrAppSurface* out);
    PrStatus (*openUrl)(PrApp* app, PrStringView url);
    void (*requestClipboardText)(PrApp* app);
    PrStatus (*setClipboardText)(PrApp* app, PrStringView text);
    void (*requestClipboardImage)(PrApp* app);
    PrStatus (*setClipboardImage)(PrApp* app, PrPixelView pixels);
};

// Creates an application by the backend, as `desc` describes. A backend's own header documents what its windows are.
// Returns null if the build does not have the backend.
PrApp* PR_NULLABLE prAppCreate(PrAppBackend backend, const PrAppDesc* desc);

// Frees the application, which must not be running.
void prAppDestroy(PrApp* PR_NULLABLE app);

// Creates the window and delivers events to a copy of `handler` until the program stops. Closing the window also stops
// the program. The call may never return, which is the case on the web.
void prAppRun(PrApp* app, const PrAppHandler* handler);

// Ends the run, which calls the handler's `shutdown`.
void prAppStop(PrApp* app);

// Calls the handler's `frame` and applies what it returns. Backends call it.
void prAppDeliverFrame(PrApp* app, const PrAppHandler* handler, const PrAppFrame* frame);

// Delivers a Wake event soon. Any thread may call it. Several wakes before the next delivery produce one event.
void prAppWake(PrApp* app);

// Delivers a frame when the display next needs one. Several requests before that frame produce one frame.
void prAppRequestFrame(PrApp* app);

// Returns what a surface of the window reports, in the surface's logical px, in logical px of the window: the surface's
// origin in the window, and logical px of the window per px of the surface.
PrAppOutput prAppOutputPlaced(PrAppOutput output, PrVec2 origin, float scale);

// Returns what two surfaces of one window report together. The pointer's surface gives the cursor. The second surface's
// text input is taken if it has one, and otherwise the first's. Either surface can want a frame, and the earlier of
// their frame times is taken.
PrAppOutput prAppOutputMerge(PrAppOutput first, PrAppOutput second, bool pointerInSecond);

// Sets the title of the window, which PrAppDesc::title sets first. The window copies `title`.
void prAppSetTitle(PrApp* app, PrStringView title);

// Resizes the window's content to `extent` logical pixels, clamped to the screen. PrAppDesc::extent sets the initial
// size. The window keeps its position, and the next frame has the new extent. This does nothing on a backend whose
// window is sized by the page or the system, such as the web backend.
void prAppSetExtent(PrApp* app, PrUVec2 extent);

// Asks for the user's attention if the window is not active, for example by bouncing its Dock icon or flashing its
// taskbar button. This does nothing on a platform that has no such request.
void prAppRequestAttention(PrApp* app);

// Returns true if the window is the active window, which receives keyboard input, of the application that the user is
// using. Returns true if the backend cannot tell. A change requests a frame, so the next frame sees the new value.
bool prAppIsActive(PrApp* app);

// Posts a desktop notification. The system copies the title and body. A click on the notification brings the window
// forward and delivers a Notification event with its tag.
//
// The system may ask the user's permission on the first post, and the notification shows once permission is given. The
// system decides how a notification shows, and may hold it back, for example while the user has asked not to be
// disturbed.
//
// Returns NotSupported if the backend or the process cannot post notifications, NotAllowed if the user refused them,
// InvalidState if the application is not running, and Io if the system refuses the notification.
PrStatus prAppNotificationPost(PrApp* app, const PrAppNotificationDesc* desc);

// Writes the window's native surface to `out`. The surface is valid until shutdown. Returns InvalidState if the
// application is not running.
PrStatus prAppSurface(PrApp* app, PrAppSurface* out);

// Opens `url` with the system's handler for it. Returns Syntax for text that is not a URL, and Io if the system cannot
// open it.
PrStatus prAppOpenUrl(PrApp* app, PrStringView url);

// Requests the clipboard's text. A Clipboard event delivers it later, never during this call. Any thread may call it.
void prAppRequestClipboardText(PrApp* app);

// Replaces the clipboard's contents with `text`. Returns Io if the system refuses. Any thread may call it.
PrStatus prAppSetClipboardText(PrApp* app, PrStringView text);

// Requests the clipboard's image. A ClipboardImage event delivers it later, never during this call.
void prAppRequestClipboardImage(PrApp* app);

// Replaces the clipboard's contents with the image of `pixels`, which are straight-alpha sRGB Rgba8 of nonzero size.
// Returns Io if the system refuses.
PrStatus prAppSetClipboardImage(PrApp* app, PrPixelView pixels);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
