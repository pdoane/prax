// The web application backend: a web page's canvas element, and the page's event loop.
//
// The window is the canvas whose id is `canvas`. The page sizes it with CSS: its CSS size is the window's extent in
// logical pixels, and the page's device pixel ratio is the display's. Frames come from the page's animation frames. The
// window's surface is a PrAppSurfaceKind_HtmlCanvas, which a GPU device presents to (pr/gpu_canvas2d.h).
//
// prAppRun never returns to its caller. From then on, the page's event loop calls the handler, and the program's state
// outlives main(). The clipboard is the page's. The page gives its text only to a paste that the user makes, and gives
// no images to the application.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The CSS selector of the canvas that is a web application's window.
#define PR_APP_WEB_CANVAS "#canvas"

// Creates an application whose window is the page's canvas, as `desc` describes. The page's title becomes `desc`'s
// title. The page chooses the extent, so `desc`'s extent is not used.
PrApp* prAppWebCreate(const PrAppDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
