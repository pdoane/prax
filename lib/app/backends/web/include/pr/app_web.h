// The web application backend: windows that are canvases in a web page, and the page's event loop.
//
// The page's host element is the display. Its CSS size is the display's extent and work area, and the page's device
// pixel ratio is the display's. Each window is a canvas in a frame that the backend adds to the host, and its surface
// is a PrSurfaceKind_HtmlCanvas, the canvas's selector, which a GPU device presents to (pr/gpu_canvas2d.h). Frames come
// from the page's animation frames.
//
// The window of a Single application fills the host, which the page sizes, without a title bar, as on a phone or a
// console. Its Fullscreen covers the machine's display, through the page's Fullscreen API.
//
// The windows of a Multiple application float in the host as on a desktop, where the backend is the window manager. It
// draws the title bar and the resize borders of a System window, and moves a Custom window by the regions of its title
// bar, whose band is the same height. It orders the windows, keeps a Dialog and a Popup above the window that they
// belong to, dims a modal dialog's parent, and asks each popup to close on a press elsewhere. Maximized fills the host,
// Fullscreen fills it without a title bar, and Minimized hides the window behind a button on a shelf along the host's
// bottom edge, which restores it. A Blur backdrop blurs the page behind the window. The host covers the machine's
// display through prAppWebSetHostFullscreen.
//
// The browser shows anything on the machine's whole display only shortly after the user's input.
//
// prAppRun never returns to its caller. From then on, the page's event loop calls the handler, and the program's state
// outlives main().
//
// The keys go to the window whose canvas has the page's focus. The page's title follows the title of that window, or of
// the only window. The window's icon is the page's favicon.
//
// The clipboard is the page's, through the Async Clipboard API (clipboard-apis/#async-clipboard-api). The browser may
// ask the user before a read, and a read that the user refuses answers NotAllowed. A page without the API answers
// NotSupported. The paste shortcut, Control+V or Command+V, is the program's key, so a program reads the clipboard
// itself. A paste that the user chooses from the browser's menu arrives as a Paste event.
//
// Notifications use the page's Notification API, and the browser shows them only after the user allows the page's
// notifications. The first post asks for permission. Some browsers ask only while the page handles user input.
//
// A drag over a canvas from another page or application carries text and files, and no image of its own. The page has
// no paths for the files of a drop or of an Open dialog, which the program reads through their source. A page starts a
// drag only from an element that the user presses. A window cannot start one, and DragEnd follows at once with None.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The options of the web backend.
typedef struct PrAppWebOptions {
    PrStringView host;      // the CSS selector of the element that holds the windows, or empty for the page's body
} PrAppWebOptions;

// The page's body as the host.
PrAppWebOptions prAppWebOptionsDefault(void);

// Creates an application whose windows are canvases in the page. A null `web` means the defaults. The page is never
// headless.
PrApp* prAppWebCreate(const PrAppOptions* options, const PrAppWebOptions* PR_NULLABLE web);

// Shows the host on the machine's whole display, or returns it to the page, or returns true while it shows there. The
// application is one that prAppWebCreate created. DisplaysChanged follows the host's change of extent.
void prAppWebSetHostFullscreen(PrApp* app, bool fullscreen);
bool prAppWebIsHostFullscreen(PrApp* app);

// The functions below are the backend's, which its page code calls. A program does not. `windowId` is the id that the
// backend gave the page for a window, and an event of a window that has closed is dropped. Positions are in CSS px from
// the canvas's top left, and modifiers hold Shift, Control, Alt and Meta from the lowest bit up.

// Delivers a click on a notification of the running application. `tagLow` and `tagHigh` are the low and high 32 bits of
// its tag.
void prAppWebNotificationClicked(uint32_t tagLow, uint32_t tagHigh);

// Answer the clipboard read `id` of the running application. `result` is 0 for a read, 1 if the user or the browser
// refused it, and 2 if the page cannot read the clipboard. A null `text` or `pixels` is an empty clipboard. `pixels`
// holds `width` by `height` straight-alpha sRGB Rgba8 pixels.
void prAppWebClipboardTextRead(uint32_t id, int result, const char* PR_NULLABLE text);
void prAppWebClipboardImageRead(uint32_t id, int result, const uint8_t* PR_NULLABLE pixels, uint32_t width, uint32_t height);

// Delivers a pointer event of the mouse, a pen or a touch over a window. `kind` is 0 for a move, 1 for a press, 2 for a
// release, 3 for a cancel, 4 for an enter and 5 for a leave, and `type` is 0 for the mouse, 1 for a pen and 2 for a
// touch. The others are the PointerEvent's, with the movement in CSS px.
void prAppWebPointer(uint32_t windowId, int kind, int type, int32_t id, bool primary, double x, double y, int button,
    double pressure, double tiltX, double tiltY, double twist, double width, double height, int modifiers,
    double movementX, double movementY);

// Records which window's canvas locks the pointer, or 0 for none, as the page's pointerlockchange reports.
void prAppWebPointerLockChanged(uint32_t windowId);

// Delivers a wheel over a window: its deltas, in the WheelEvent's `mode`, 0 for pixels, 1 for lines and 2 for pages.
void prAppWebWheel(uint32_t windowId, double x, double y, double deltaX, double deltaY, int mode, int modifiers);

// Records that a window's canvas took the page's focus, or lost it.
void prAppWebWindowFocused(uint32_t windowId, bool focused);

// Moves or resizes a window by a press on the frame that the page draws. `edges` is 1 for the left edge, 2 for the top,
// 4 for the right and 8 for the bottom, or 16 to move the whole window, and `phase` is 0 for the press, 1 for a move
// and 2 for the release. `x` and `y` are in CSS px from the host's top left.
void prAppWebFrameDrag(uint32_t windowId, int edges, int phase, double x, double y);

// Acts on a button of the title bar that the page draws: a PrAppTitleBarRegionKind of a button.
void prAppWebFrameButton(uint32_t windowId, int kind);

// Restores a minimized window and gives it the keyboard, for a press on its button on the shelf.
void prAppWebWindowRestore(uint32_t windowId);

// Asks every popup to close, for a press on the page outside every window.
void prAppWebPressOutside(void);

// Answers the file dialog `id` with the set of files that the page keeps under `key`: `count` files, whose names are in
// `names`, `nameBytes` bytes of names each ended by a NUL, and whose sizes are in `sizes`. A cancel has no files.
void prAppWebFilesChosen(uint32_t id, uint32_t key, const char* names, uint32_t nameBytes, const double* sizes, uint32_t count);

// Delivers an Open event for a launch of the installed page: `url` is the URL that it opens, as UTF-8 that ends with
// NUL, and empty if none, and its files are the set that the page keeps under `key`, as prAppWebFilesChosen describes.
void prAppWebLaunched(const char* url, uint32_t key, const char* names, uint32_t nameBytes, const double* sizes,
    uint32_t count);

// Answers the read `id` of a file's bytes: `size` bytes at `bytes`, and `result` 0 for a read, 1 for a file that the
// page does not hold, and 2 for one that it cannot read.
void prAppWebFileRead(uint32_t id, int result, const uint8_t* PR_NULLABLE bytes, uint32_t size);

// Delivers a DisplaysChanged event, for a change of the host's extent or of the page's device pixel ratio.
void prAppWebDisplaysChanged(void);

// Requests a frame of each window, for a change of the user's preferences, which the frame reports.
void prAppWebPreferencesChanged(void);

// Delivers a LifecycleChanged event if the page's visibility or focus changed its lifecycle.
void prAppWebLifecycleChanged(void);

// Delivers an application event for a notice of the page: 0 for Suspend (freeze), 1 for Resume (resume), 2 for
// LocaleChanged (languagechange), 3 for PowerChanged (the battery's events), and 4 for KeyboardLayoutChanged (the
// keyboard's layoutchange).
void prAppWebAppEvent(int notice);

// Answers the share `id`: `result` is 0 if the user shared or closed the sheet, 1 if the browser refused, and 2 for any
// other failure.
void prAppWebShared(uint32_t id, int result);

// Delivers a reading of the PrAppSensor `sensor`, in its first three or four values.
void prAppWebSensorReading(int sensor, double x, double y, double z, double w);

// Delivers a gesture over a window: a PrAppGestureKind and a PrAppGesturePhase, with the change of scale and the
// clockwise change of angle in degrees since the gesture's last event.
void prAppWebGesture(uint32_t windowId, int kind, int phase, double x, double y, double magnification, double rotation);

// Delivers a paste of `text`, as UTF-8 that ends with NUL, that the user chose from the browser's menu, to the window
// that has the keyboard.
void prAppWebPasted(const char* text);

// Delivers a drag over a window, and returns the window's answer as a PrAppDragOperation. `kind` is 0 for an enter, 1
// for a move over the canvas and 2 for a leave. `content` is the PrAppDataFormFlags of the forms that the drag carries,
// and `operations` is the PrAppDragOperationFlags that its source allows.
int prAppWebDrag(uint32_t windowId, int kind, double x, double y, int content, int operations, int modifiers);

// Returns a window's answer to a drop that its source allows `operations` for, as prAppWebDrag does, without an event.
int prAppWebDropAnswer(uint32_t windowId, int operations);

// Delivers a drop on a window with `operation`, the answer that prAppWebDropAnswer gave. `text` is UTF-8 that ends with
// NUL, and empty if the drop carries none. The drop's files are the set that the page keeps under `key`, as
// prAppWebFilesChosen describes.
void prAppWebDrop(uint32_t windowId, double x, double y, int operation, int modifiers, const char* text, uint32_t key,
    const char* names, uint32_t nameBytes, const double* sizes, uint32_t count);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
