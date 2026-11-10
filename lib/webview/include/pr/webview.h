// Web views: the system's browser engine, embedded. A page loads into a viewport of a size in CSS pixels, and scripts
// run in it. The page's rendering is captured, or shown live over a window's surface, where the platform composites it.
//
// A web view is used from the thread that runs the application (pr/app.h), whose event loop runs the engine. Each call
// completes through its callback on that thread, never during the call. Once a view is destroyed, its pending callbacks
// are not called.

#pragma once

#include "pr/app_surface.h"
#include "pr/app_user_preferences.h"
#include "pr/base.h"
#include "pr/geom.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An embedded page, created by its backend.
typedef struct PrWebView PrWebView;

// The library's backends of web views, which the build turns on (README.md), and Default, the platform's own.
enum {
    PrWebViewBackend_Default,
    PrWebViewBackend_WebKit,
    PrWebViewBackend_WebView2,
    PrWebViewBackend_Count,
};
typedef uint8_t PrWebViewBackend;

// Returns the backend that `name` names, as prWebViewBackendName spells it, ignoring case, or Count if it names none.
PrWebViewBackend prWebViewBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prWebViewBackendName(PrWebViewBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prWebViewBackendIsBuilt(PrWebViewBackend backend);

// Called once a page has loaded or failed to load. `status` is Ok if the page loaded.
typedef void (*PrWebViewDoneFn)(void* PR_NULLABLE context, PrStatus status);

// Called with a script's result as JSON, or the null view if it has none. If the script fails, `status` is InvalidData
// and `json` is the message of its error.
typedef void (*PrWebViewResultFn)(void* PR_NULLABLE context, PrStatus status, PrStringView json);

// Called with the pixels of a capture as premultiplied Bgra8. The pixels are valid during the call, and empty if the
// capture failed.
typedef void (*PrWebViewCaptureFn)(void* PR_NULLABLE context, PrStatus status, PrPixelView pixels);

// What a web view is created with.
typedef struct PrWebViewDesc {
    PrUVec2 viewport;           // CSS pixels
    float pixelRatio;           // pixels per CSS pixel that the page renders at, or zero for the display's

    // The preferences that the page's media queries read, of which the engine takes the color scheme, or null to follow
    // the system's as they change.
    const PrAppUserPreferences* PR_NULLABLE preferences;
} PrWebViewDesc;

// The roles of the processes that a browser engine runs a page in.
enum {
    PrWebViewProcessRole_Content,   // runs the page's scripts and lays it out
    PrWebViewProcessRole_Gpu,
    PrWebViewProcessRole_Network,
};
typedef uint8_t PrWebViewProcessRole;

// A process that a browser engine runs a page in.
typedef struct PrWebViewProcess {
    PrWebViewProcessRole role;
    int32_t id;                 // the system's process identifier
} PrWebViewProcess;

// Creates a web view by the backend, as `desc` describes it, empty until a page loads. Its page keeps no data once the
// view is destroyed, and renders at full speed whether or not it is seen. Returns NotSupported if the build does not
// have the backend or the system lacks its engine. On failure, `*out` is null.
PrStatus prWebViewCreate(PrWebViewBackend backend, const PrWebViewDesc* desc, PrWebView* PR_NULLABLE* PR_NONNULL out);

// Frees the view.
void prWebViewDestroy(PrWebView* PR_NULLABLE view);

// Sets the viewport to `viewport` in CSS pixels.
void prWebViewResize(PrWebView* view, PrUVec2 viewport);

// Loads the page at `url`, and calls `done` once it has loaded or failed. A page from a file URL, and its scripts, may
// read the files beside it.
void prWebViewLoad(PrWebView* view, PrStringView url, PrWebViewDoneFn done, void* PR_NULLABLE context);

// Runs `script` in the page as global code, and calls `done` with its result. If the result is a promise, `done` is
// called once the promise settles.
void prWebViewEvaluate(PrWebView* view, PrStringView script, PrWebViewResultFn done, void* PR_NULLABLE context);

// Captures `rect` of the viewport, in CSS pixels, as the page is displayed once its latest changes have rendered, at
// `size` pixels in sRGB, and calls `done` with the pixels.
void prWebViewCapture(PrWebView* view, PrRect rect, PrUVec2 size, PrWebViewCaptureFn done, void* PR_NULLABLE context);

// Shows the page live over `host`, a window's surface (prAppSurface), above what the program presents. The viewport is
// scaled to fill `frame` and clipped to `clip`, both in the host's logical pixels. The platform renders and composites
// the page on its own schedule, as it does any page it shows. The program neither draws it nor waits for it. Whether or
// not it is shown, the page lays out and captures at the size of its viewport. A host of a kind that the backend does
// not show over leaves the page where it is.
void prWebViewShow(PrWebView* view, const PrAppSurface* host, PrRect clip, PrRect frame);

// Removes the page from its host and moves it back to its own window, which no one sees.
void prWebViewHide(PrWebView* view);

// Prints the page to a PDF at `path`, on paper of `paper` CSS pixels with no margins of its own, or on US Letter if
// `paper` is zero, as the page's print styles lay it out with backgrounds drawn, and calls `done` once the file is
// written or the print failed. Returns NotSupported, and calls nothing, if the engine does not print.
PrStatus prWebViewPrint(PrWebView* view, PrStringView path, PrVec2 paper, PrWebViewDoneFn done, void* PR_NULLABLE context);

// Writes up to `capacity` of the processes that the engine runs the page in, other than the program's own, to `out`,
// and returns how many it runs. Processes appear as the engine starts them, once a page begins to load, and an engine
// that runs pages in the program's own process has none.
uint32_t prWebViewProcesses(PrWebView* view, PrWebViewProcess* PR_NULLABLE out, uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
