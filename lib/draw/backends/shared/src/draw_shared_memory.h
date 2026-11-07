// What the renderers that draw on the CPU share: a memory device to render on (PrGpuNativeKind_Pixels), and frames that
// render natively into the texels of a pass's attachment (prGpuPassNativeDraw) while the pass's work runs.

#pragma once

#include "pr/base.h"
#include "pr/draw_renderer.h"
#include "pr/draw_renderer_backend.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Renders the part of `list` over `pixels` in `format`. The pixels have the render's extent.
using DrawMemoryRenderFn = void (*)(PrDrawRenderer* renderer, PrPixels pixels, PrPixelFormat format, const PrDrawList* list,
    const PrDrawPart& part);

// A frame of a renderer that draws on the CPU: the part of its list, how it renders, and the origin of each render
// since the frame was prepared. The native draws read the origins in the order that they run. A renderer that draws
// only 8 bits renders an RgbaF16 target by clamping its colors to Rgba8, drawing over them, and widening the result
// back.
struct DrawMemoryFrame : PrDrawFrame {
    PrDrawRenderer* renderer;
    PrUVec2 extent;                 // the extent of the target that the frame was prepared for
    const PrDrawList* PR_NULLABLE list;
    PrDrawPart part;
    DrawMemoryRenderFn render;
    bool halfFloat;                 // the renderer draws RgbaF16 itself
    RawArray<PrIVec2> origins;
    uint32_t drawn;
    RawArray<uint8_t> narrow;          // the 8-bit pixels of an RgbaF16 target, for a renderer that draws only 8 bits
};

// Returns the device that a renderer renders on. That is `device` if it is a memory device. If `device` is null, it is
// a new memory device that the renderer owns. `*owned` says which. Returns null for a device of any other kind, or if
// `device` is null and the build has no memory device.
PrGpuDevice* PR_NULLABLE drawMemoryDevice(PrGpuDevice* PR_NULLABLE device, bool* owned);

// Creates a frame that renders with `render`. `halfFloat` is true if `render` draws RgbaF16 itself. The frame's prepare
// keeps the part of the list, which renders when the frame's passes run.
DrawMemoryFrame* drawMemoryFrameCreate(PrDrawRenderer* renderer, DrawMemoryRenderFn render, bool halfFloat);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
