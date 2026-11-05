// Renderers that draw on the CPU, natively into the texels of a memory device.

#include "draw/backends/shared/src/draw_shared_memory.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"

namespace pr_impl {

namespace {

// Renders the list over half floats, for a renderer that draws only 8 bits. It draws over the colors clamped to 8 bits,
// and then widens the result back.
void renderNarrowed(DrawMemoryFrame* frame, PrPixels pixels) {
    size_t stride = size_t(pixels.extent.x) * 4;
    frame->narrow.resize(uint32_t(stride * pixels.extent.y));
    uint8_t* narrow = frame->narrow.data;
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            PrFloat16 wide[4];
            prMemCopy(wide, pixels.data + y * pixels.stride + x * 8, sizeof(wide));
            float a = prF32Clamp(prFloat16ToFloat(wide[3]), 0, 1);
            for (uint32_t k = 0; k < 4; ++k) {
                float v = prF32Clamp(prFloat16ToFloat(wide[k]), 0, k < 3 ? a : 1);
                narrow[y * stride + x * 4 + k] = uint8_t(v * 255 + 0.5f);
            }
        }
    }
    frame->render(frame->renderer, PrPixels{narrow, stride, pixels.extent}, PrPixelFormat_Rgba8, frame->list,
        frame->part);
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            PrFloat16 wide[4];
            for (uint32_t k = 0; k < 4; ++k)
                wide[k] = prFloat16FromFloat(float(narrow[y * stride + x * 4 + k]) / 255);
            prMemCopy(pixels.data + y * pixels.stride + x * 8, wide, sizeof(wide));
        }
    }
}

// Renders the frame's list into the pass's attachment, at the origin of the frame's next render.
void drawFrame(void* PR_NULLABLE context, const void* native) {
    DrawMemoryFrame* frame = static_cast<DrawMemoryFrame*>(context);
    const PrGpuNativePixels& attachment = *static_cast<const PrGpuNativePixels*>(native);
    PrPixels target = attachment.pixels;
    PrIVec2 origin = frame->origins.data[frame->drawn++];
    PrPixelFormat format{};
    if (!prGpuFormatToPixelFormat(attachment.format, &format))
        return;
    PrPixels at{
        .data = target.data + size_t(origin.y) * target.stride + size_t(origin.x) * prPixelFormatSize(format),
        .stride = target.stride,
        .extent = frame->extent,
    };
    if (format == PrPixelFormat_RgbaF16 && !frame->halfFloat)
        renderNarrowed(frame, at);
    else
        frame->render(frame->renderer, at, format, frame->list, frame->part);
}

} // namespace

PrGpuDevice* PR_NULLABLE drawMemoryDevice(PrGpuDevice* PR_NULLABLE device, bool* owned) {
    *owned = device == nullptr;
    if (device == nullptr && prGpuDeviceCreate(PrGpuBackend_Memory, nullptr, &device) != PrStatus_Ok)
        return nullptr;
    if (!prGpuDeviceHasNative(device, PrGpuNativeKind_Pixels))
        return nullptr;
    return device;
}

DrawMemoryFrame* drawMemoryFrameCreate(PrDrawRenderer* renderer, DrawMemoryRenderFn render, bool halfFloat) {
    DrawMemoryFrame* frame = allocZeroed<DrawMemoryFrame>();
    frame->renderer = renderer;
    frame->render = render;
    frame->halfFloat = halfFloat;
    return frame;
}

void drawMemoryFrameDestroy(DrawMemoryFrame* frame) {
    frame->origins.reset();
    frame->narrow.reset();
    prMemoryFree(frame);
}

void drawMemoryFramePrepare(DrawMemoryFrame* frame, const PrDrawList* list, const PrDrawPart& part) {
    frame->list = list;
    frame->part = part;
    frame->origins.clear();
    frame->drawn = 0;
}

void drawMemoryFrameRender(DrawMemoryFrame* frame, PrGpuPass* pass, PrIVec2 origin) {
    frame->origins.push(origin);
    prGpuPassNativeDraw(pass, PrGpuNativeKind_Pixels, drawFrame, frame);
}

} // namespace pr_impl
