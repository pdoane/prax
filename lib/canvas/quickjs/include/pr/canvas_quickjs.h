// JavaScript binding of the 2D rendering context of a canvas.
//
// html - https://html.spec.whatwg.org/

#pragma once

#include "pr/canvas.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines CanvasRenderingContext2D, CanvasGradient, CanvasPattern, Path2D, ImageData and TextMetrics on the global
// object of `ctx`. The DOM's binding must be defined first.
void prJsCanvasBind(JSContext* ctx);

// Reads the options of getContext("2d") (html/#canvasrenderingcontext2dsettings). A value that is not an object means
// the defaults. Returns false if reading them threw.
bool prJsCanvasSettingsRead(JSContext* ctx, JSValueConst value, PrCanvasSettings* out);

// Returns a new object for `context`, which belongs to the canvas whose object is `canvas`. The object keeps `canvas`
// alive, and its canvas attribute returns it.
JSValue prJsCanvasRenderingContext2DNew(JSContext* ctx, PrCanvas* context, JSValueConst canvas);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
