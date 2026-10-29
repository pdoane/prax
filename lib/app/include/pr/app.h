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

// The system's browser engine, created by an application. A page loads into a viewport of a size in CSS pixels, and
// scripts run in it. The page's rendering is captured, or shown live in the application's window, where the platform
// composites it.
typedef struct PrAppBrowser PrAppBrowser;

// The kinds of input event.
enum {
    PrAppEventKind_PointerMove,
    PrAppEventKind_PointerDown, // a button pressed
    PrAppEventKind_PointerUp,   // a button released
    PrAppEventKind_Scroll,
    PrAppEventKind_KeyDown,
    PrAppEventKind_KeyUp,
    PrAppEventKind_Text,        // text typed, as UTF-8
    PrAppEventKind_Clipboard,   // the clipboard's text, in response to a request or a paste
    PrAppEventKind_Wake,        // prAppWake was called

    // An input method's composition. An update gives the whole text being composed, and the first update starts a
    // composition. The end gives the text that the composition commits, which is empty if it was canceled. The text
    // committed follows as a Text event, so that a program that takes no composition still receives it.
    PrAppEventKind_CompositionUpdate,
    PrAppEventKind_CompositionEnd,
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

// The shapes of the pointer over the window.
enum {
    PrAppCursor_Arrow,
    PrAppCursor_Hand,
    PrAppCursor_Text,
    PrAppCursor_Crosshair,
    PrAppCursor_ResizeX,        // a horizontal resize
    PrAppCursor_ResizeY,        // a vertical resize
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

// An input event. `kind` decides which fields apply. Positions are in the window's logical pixels, from its top left. A
// scroll's delta is positive right and down, in the unit that the device reports (prAppEventScrollPixels resolves it).
// `text` is valid only while the event is delivered.
//
// A key carries `key`, its meaning, and `code`, its position. Its `text` is the character that it types with Shift and
// a layout's third level (AltGr, Option) applied and Control and Command not: `a` for A and for Control+A, `A` for
// Shift+A, `å` for Option+A on a Mac. The text is empty for a key that types no character. A modifier key sends KeyDown
// and KeyUp as it is pressed and released, and a lock key as it turns on and off. A Text event that a key types comes
// after that key's KeyDown and before the next KeyDown, so that a program can drop the text of a key whose press it
// refused.
typedef struct PrAppEvent {
    PrAppEventKind kind;
    PrAppModifierFlags modifiers;
    uint8_t button;             // pointer buttons: 0 for the primary one
    bool repeat;                // KeyDown: true if the event repeats a held key
    PrAppKey key;               // KeyDown, KeyUp
    PrAppKeyCode code;          // KeyDown, KeyUp
    PrAppScrollUnit scrollUnit; // Scroll
    PrVec2 position;            // pointer events
    PrVec2 delta;               // Scroll
    PrStringView text;          // KeyDown, KeyUp, Text, Clipboard, CompositionUpdate, CompositionEnd
    PrAppTextRange selection;   // CompositionUpdate: the part of the text that the input method selects
    double time;                // seconds, from a monotonic clock
} PrAppEvent;

// A Scroll event's delta in logical pixels, with a line of `lineHeight` pixels.
PrVec2 prAppEventScrollPixels(const PrAppEvent* event, float lineHeight);

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

// The callbacks into the program, each passed `context`. All are called on the thread that runs the application, and
// none is nested in another. `start` is called once the window exists, and `shutdown` last of all. After `shutdown`,
// the application makes no more calls. Every callback must be set.
//
// `frame` runs within the window's display transaction: at the display's refresh after a frame is requested, and at
// each step of a resize, whether or not one is requested. What `frame` presents through a swap chain that presents
// within the transaction (prGpuSwapChainCreate) is shown with the window at the frame's extent.
typedef struct PrAppHandler {
    void* PR_NULLABLE context;
    void (*start)(void* PR_NULLABLE context, PrApp* app);
    void (*frame)(void* PR_NULLABLE context, PrApp* app, const PrAppFrame* frame);
    void (*event)(void* PR_NULLABLE context, PrApp* app, const PrAppEvent* event);
    void (*shutdown)(void* PR_NULLABLE context, PrApp* app);
} PrAppHandler;

// The library's backends of applications, which the build turns on (README.md), and Default, the platform's own.
enum {
    PrAppBackend_Default,
    PrAppBackend_Cocoa,
    PrAppBackend_Win32,
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

// Called once a page has loaded or failed to load. `status` is Ok if the page loaded.
typedef void (*PrAppBrowserDoneFn)(void* PR_NULLABLE context, PrStatus status);

// Called with a script's result as JSON, or the null view if it has none. If the script fails, `status` is InvalidData
// and `json` is the message of its error.
typedef void (*PrAppBrowserResultFn)(void* PR_NULLABLE context, PrStatus status, PrStringView json);

// Called with the pixels of a capture as premultiplied Bgra8. The pixels are valid during the call, and empty if the
// capture failed.
typedef void (*PrAppBrowserCaptureFn)(void* PR_NULLABLE context, PrStatus status, PrPixelView pixels);

// What a browser is created with.
typedef struct PrAppBrowserDesc {
    PrUVec2 viewport;           // CSS pixels
    float pixelRatio;           // pixels per CSS pixel that the browser renders the page at, or zero for the display's

    // The preferences that the page's media queries read, of which the engine takes the color scheme, or null to follow
    // the system's as they change.
    const PrAppUserPreferences* PR_NULLABLE preferences;
} PrAppBrowserDesc;

// The roles of the processes that a browser engine runs a page in.
enum {
    PrAppBrowserProcessRole_Content,    // runs the page's scripts and lays it out
    PrAppBrowserProcessRole_Gpu,
    PrAppBrowserProcessRole_Network,
};
typedef uint8_t PrAppBrowserProcessRole;

// A process that a browser engine runs a page in.
typedef struct PrAppBrowserProcess {
    PrAppBrowserProcessRole role;
    int32_t id;                 // the system's process identifier
} PrAppBrowserProcess;

// The start of every browser's state. `app` is the application that created the browser.
struct PrAppBrowser {
    PrApp* app;
};

// The functions of an application, at the start of its backend's state. Each member implements the function that it is
// named for. A null `destroy` leaves nothing to free. An application with no browser engine leaves all the browser
// functions null. Callers use the functions below, never these members.
struct PrApp {
    void (*PR_NULLABLE destroy)(PrApp* app);
    void (*run)(PrApp* app, const PrAppHandler* handler);
    void (*stop)(PrApp* app);
    void (*wake)(PrApp* app);
    void (*requestFrame)(PrApp* app);
    void (*setCursor)(PrApp* app, PrAppCursor cursor);
    PrStatus (*surface)(PrApp* app, PrAppSurface* out);
    PrStatus (*openUrl)(PrApp* app, PrStringView url);
    void (*requestClipboardText)(PrApp* app);
    PrStatus (*setClipboardText)(PrApp* app, PrStringView text);
    PrStatus (*PR_NULLABLE browserCreate)(PrApp* app, const PrAppBrowserDesc* desc,
                                          PrAppBrowser* PR_NULLABLE* PR_NONNULL out);
    void (*PR_NULLABLE browserDestroy)(PrApp* app, PrAppBrowser* browser);
    void (*PR_NULLABLE browserResize)(PrApp* app, PrAppBrowser* browser, PrUVec2 viewport);
    void (*PR_NULLABLE browserLoad)(PrApp* app, PrAppBrowser* browser, PrStringView url,
                                    PrAppBrowserDoneFn done, void* PR_NULLABLE context);
    void (*PR_NULLABLE browserEvaluate)(PrApp* app, PrAppBrowser* browser, PrStringView script,
                                        PrAppBrowserResultFn done, void* PR_NULLABLE context);
    void (*PR_NULLABLE browserCapture)(PrApp* app, PrAppBrowser* browser, PrRect rect, PrUVec2 size,
                                       PrAppBrowserCaptureFn done, void* PR_NULLABLE context);
    void (*PR_NULLABLE browserShow)(PrApp* app, PrAppBrowser* browser, PrRect clip, PrRect frame);
    void (*PR_NULLABLE browserHide)(PrApp* app, PrAppBrowser* browser);
    void (*PR_NULLABLE browserPrint)(PrApp* app, PrAppBrowser* browser, PrStringView path, PrVec2 paper,
                                     PrAppBrowserDoneFn done, void* PR_NULLABLE context);
    uint32_t (*PR_NULLABLE browserProcesses)(PrApp* app, PrAppBrowser* browser, PrAppBrowserProcess* PR_NULLABLE out,
                                             uint32_t capacity);
};

// Creates an application by the backend, as `desc` describes. A backend's own header documents what its windows and
// browsers are. Returns null if the build does not have the backend.
PrApp* PR_NULLABLE prAppCreate(PrAppBackend backend, const PrAppDesc* desc);

// Frees the application, which must not be running. Its browsers must be destroyed first.
void prAppDestroy(PrApp* PR_NULLABLE app);

// Creates the window and delivers events to a copy of `handler` until the program stops. Closing the window also stops
// the program. The call may never return, which is the case on the web.
void prAppRun(PrApp* app, const PrAppHandler* handler);

// Ends the run, which calls the handler's `shutdown`.
void prAppStop(PrApp* app);

// Delivers a Wake event soon. Any thread may call it. Several wakes before the next delivery produce one event.
void prAppWake(PrApp* app);

// Delivers a frame when the display next needs one. Several requests before that frame produce one frame.
void prAppRequestFrame(PrApp* app);

// Sets the shape of the pointer over the window.
void prAppSetCursor(PrApp* app, PrAppCursor cursor);

// Writes the window's native surface to `out`. The surface is valid until shutdown. Returns InvalidState if the
// application is not running.
PrStatus prAppSurface(PrApp* app, PrAppSurface* out);

// Opens `url` with the system's handler for it. Returns Syntax for text that is not a URL, and Io if the system cannot
// open it.
PrStatus prAppOpenUrl(PrApp* app, PrStringView url);

// Requests the clipboard's text. A Clipboard event delivers it later, never during this call.
void prAppRequestClipboardText(PrApp* app);

// Replaces the clipboard's contents with `text`. Returns Io if the system refuses.
PrStatus prAppSetClipboardText(PrApp* app, PrStringView text);

// ---------------------------------------------------------------------------------------------------------------------
// Browsers
//
// Each call completes through its callback on the thread that runs the application, never during the call. Once a
// browser is destroyed, its pending callbacks are not called.

// Creates a browser as `desc` describes it, empty until a page loads. Its page keeps no data once the browser is
// destroyed, and renders at full speed whether or not it is seen. Returns NotSupported if the backend has no browser
// engine. On failure, `*out` is null.
PrStatus prAppBrowserCreate(PrApp* app, const PrAppBrowserDesc* desc, PrAppBrowser* PR_NULLABLE* PR_NONNULL out);

// Frees the browser.
void prAppBrowserDestroy(PrAppBrowser* PR_NULLABLE browser);

// Sets the viewport to `viewport` in CSS pixels.
void prAppBrowserResize(PrAppBrowser* browser, PrUVec2 viewport);

// Loads the page at `url`, and calls `done` once it has loaded or failed. A page from a file URL, and its scripts, may
// read the files beside it.
void prAppBrowserLoad(PrAppBrowser* browser, PrStringView url, PrAppBrowserDoneFn done,
                      void* PR_NULLABLE context);

// Runs `script` in the page as global code, and calls `done` with its result. If the result is a promise, `done` is
// called once the promise settles.
void prAppBrowserEvaluate(PrAppBrowser* browser, PrStringView script, PrAppBrowserResultFn done,
                          void* PR_NULLABLE context);

// Captures `rect` of the viewport, in CSS pixels, as the page is displayed once its latest changes have rendered, at
// `size` pixels in sRGB, and calls `done` with the pixels.
void prAppBrowserCapture(PrAppBrowser* browser, PrRect rect, PrUVec2 size, PrAppBrowserCaptureFn done,
                         void* PR_NULLABLE context);

// Shows the page live in the application's window, over what the program presents. The viewport is scaled to fill
// `frame` and clipped to `clip`, both in the window's logical pixels. The platform renders and composites the page on
// its own schedule, as it does any page it shows. The program neither draws it nor waits for it. Whether or not it is
// shown, the page lays out and captures at the size of its viewport.
void prAppBrowserShow(PrAppBrowser* browser, PrRect clip, PrRect frame);

// Removes the page from the window and moves it back to its own window, which no one sees.
void prAppBrowserHide(PrAppBrowser* browser);

// Prints the page to a PDF at `path`, on paper of `paper` CSS pixels with no margins of its own, or on US Letter if
// `paper` is zero, as the page's print styles lay it out with backgrounds drawn, and calls `done` once the file is
// written or the print failed. Returns NotSupported, and calls nothing, if the engine does not print.
PrStatus prAppBrowserPrint(PrAppBrowser* browser, PrStringView path, PrVec2 paper, PrAppBrowserDoneFn done,
                           void* PR_NULLABLE context);

// Writes up to `capacity` of the processes that the engine runs the page in, other than the application's own, to
// `out`, and returns how many it runs. Processes appear as the engine starts them, once a page begins to load, and an
// engine that runs pages in the application's own process has none.
uint32_t prAppBrowserProcesses(PrAppBrowser* browser, PrAppBrowserProcess* PR_NULLABLE out, uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
