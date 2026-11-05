// The Canvas 2D draw backend, which renders through the browser's 2D canvas. It runs on a device whose textures are a
// web page's canvases (PrGpuNativeKind_Canvas2d, pr/gpu_canvas2d.h). A frame draws into the canvas of its pass's
// attachment while the pass's work runs (prGpuPassNativeDraw), and the browser rasterizes its paths, gradients, images
// and shadows.
//
// The context draws what lib/draw specifies as closely as it can. It always antialiases, draws in 8-bit sRGB with its
// own rasterization, and draws glyphs from their unhinted outlines. A group with an effect draws into a canvas of its
// own. Of a group's filters, only a chain of blurs and drop shadows applies, and a group with any other filter graph
// draws unfiltered. A projected group draws nothing. A brush that the context has no paint for, such as a diamond
// gradient, is evaluated per pixel instead.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How a Canvas 2D renderer is created.
typedef struct PrDrawCanvasOptions {
    // Draws the glyphs that are pictures, or null to draw them as nothing. The decoder must outlive the renderer.
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;
} PrDrawCanvasOptions;

// Returns options with no glyph decoder.
PrDrawCanvasOptions prDrawCanvasOptionsDefault(void);

// Creates a renderer on `device`. Null options mean the defaults. Returns NotSupported if the device's textures are not
// canvases, and Lost if the device is lost.
PrStatus prDrawCanvasCreate(PrGpuDevice* device, const PrDrawCanvasOptions* PR_NULLABLE options,
    PrDrawRenderer* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
