// The Canvas 2D GPU backend: a memory device (pr/gpu_memory.h) whose textures of Rgba8 and Bgra8 are also canvases of a
// web page. Native draws draw into a texture through its canvas's 2D context (PrGpuNativeKind_Canvas2d), as lib/draw's
// Canvas 2D renderer does, or into its texels (PrGpuNativeKind_Pixels), as lib/draw's CPU renderer does. The device
// keeps the two in step.
//
// A swap chain presents to a surface of PrSurfaceKind_HtmlCanvas. Its textures are that canvas itself, so what is drawn
// through the canvas shows without a copy. They hold Rgba8 in sRGB for every intent, and the canvas shows them opaque.
//
// A PrGpuCanvas is an index into `prGpuCanvases`, a table that the page's side of the backend keeps (gpu_canvas2d.js).
// An entry either holds `canvas`, an OffscreenCanvas or a canvas element, and `context`, its 2D context, or it holds an
// image that a context can draw, such as a VideoFrame, as `canvas` and no context. Page code that uses the table names
// it as a dependency of its library (`__deps: ["$prGpuCanvases"]`).
//
// The device imports PrGpuImportKind_WebImage images. The handle is the PrGpuCanvas of an image's entry, which page
// code made (prGpuCanvasAddImage). The texture's copy is a reference of its own to the image: a clone of a VideoFrame,
// which the device closes, or else the image itself. The texture is in Rgba8. A canvas holds premultiplied colors in
// the sRGB encoding, as the texture's texels do, so a native draw draws the same into either.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a device. Returns NotSupported if the program runs without a page's canvases, such as under Node.js.
PrStatus prGpuCanvas2dCreate(PrGpuDevice* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
