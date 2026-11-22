// Renderers that draw on the CPU, natively into the texels of a memory device.

#include "draw/backends/shared/src/draw_shared_memory.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/gpu_memory.h"
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

void destroyFrame(PrDrawFrame* handle) {
    DrawMemoryFrame* frame = static_cast<DrawMemoryFrame*>(handle);
    frame->origins.reset();
    frame->narrow.reset();
    prMemoryFree(frame);
}

PrStatus prepareFrame(PrDrawFrame* handle, const PrDrawList* list, const PrDrawPart* part, const PrDrawTarget* target) {
    DrawMemoryFrame* frame = static_cast<DrawMemoryFrame*>(handle);
    frame->extent = target->extent;
    frame->list = list;
    frame->part = *part;
    frame->origins.clear();
    frame->drawn = 0;
    return PrStatus_Ok;
}

// Records a render of the frame into the pass at `origin`. The render runs while the pass's work runs.
void renderFrame(PrDrawFrame* handle, PrGpuPass* pass, PrIVec2 origin) {
    DrawMemoryFrame* frame = static_cast<DrawMemoryFrame*>(handle);
    frame->origins.push(origin);
    prGpuPassNativeDraw(pass, PrGpuNativeKind_Pixels, drawFrame, frame);
}

} // namespace

PrGpuDevice* PR_NULLABLE drawMemoryDevice(PrGpuDevice* PR_NULLABLE device, bool* owned) {
    *owned = device == nullptr;
    // A memory device created without a pool is ready on return.
    if (device == nullptr)
        device = prGpuMemoryCreate(nullptr);
    if (device == nullptr)
        return nullptr;
    if (!prGpuDeviceHasNative(device, PrGpuNativeKind_Pixels))
        return nullptr;
    return device;
}

bool drawMemoryReadSource(PrGpuDevice* device, const PrDrawSource& source, PrPixelFormat format, PrPixels out) {
    PrGpuNativePixels native{};
    PrPixelFormat from{};
    if (source.kind != PrDrawSourceKind_Texture ||
            !prGpuTextureNative(device, source.texture, PrGpuNativeKind_Pixels, &native) ||
            !prGpuFormatToPixelFormat(prGpuFormatPlain(native.format), &from))
        return false;
    int32_t left = int32_t(prF32Floor(source.rect.left));
    int32_t top = int32_t(prF32Floor(source.rect.top));
    uint32_t fromBytes = prPixelFormatSize(from);
    uint32_t toBytes = prPixelFormatSize(format);
    for (uint32_t y = 0; y < out.extent.y; ++y) {
        int32_t sy = top + int32_t(y);
        for (uint32_t x = 0; x < out.extent.x; ++x) {
            int32_t sx = left + int32_t(x);
            float c[4] = {0, 0, 0, 0};
            if (sx >= 0 && sy >= 0 && uint32_t(sx) < native.pixels.extent.x && uint32_t(sy) < native.pixels.extent.y) {
                const uint8_t* p = native.pixels.data + size_t(sy) * native.pixels.stride + size_t(sx) * fromBytes;
                if (from == PrPixelFormat_RgbaF16) {
                    PrFloat16 wide[4];
                    prMemCopy(wide, p, sizeof(wide));
                    for (uint32_t k = 0; k < 4; ++k)
                        c[k] = prFloat16ToFloat(wide[k]);
                } else {
                    bool bgra = from == PrPixelFormat_Bgra8;
                    c[0] = float(p[bgra ? 2 : 0]) / 255;
                    c[1] = float(p[1]) / 255;
                    c[2] = float(p[bgra ? 0 : 2]) / 255;
                    c[3] = float(p[3]) / 255;
                }
                if (source.alpha == PrTextureAlpha_None)
                    c[3] = 1;
                c[3] = prF32Clamp(c[3], 0, 1);
                if (source.alpha == PrTextureAlpha_Straight) {
                    for (uint32_t k = 0; k < 3; ++k)
                        c[k] *= c[3];
                }
            }
            uint8_t* o = out.data + size_t(y) * out.stride + size_t(x) * toBytes;
            if (format == PrPixelFormat_RgbaF16) {
                PrFloat16 wide[4];
                for (uint32_t k = 0; k < 4; ++k)
                    wide[k] = prFloat16FromFloat(c[k]);
                prMemCopy(o, wide, sizeof(wide));
            } else {
                bool bgra = format == PrPixelFormat_Bgra8;
                const float ordered[4] = {bgra ? c[2] : c[0], c[1], bgra ? c[0] : c[2], c[3]};
                for (uint32_t k = 0; k < 4; ++k)
                    o[k] = uint8_t(prF32Clamp(ordered[k], 0, 1) * 255 + 0.5f);
            }
        }
    }
    return true;
}

void drawMemoryWriteSource(PrGpuDevice* device, const PrDrawSource& source, PrPixels target, PrPixelFormat format,
        PrIVec2 origin) {
    PrUVec2 extent = prDrawSourceTexels(source);
    PrIRect landed = prIRectIntersect(prIRectAt(origin, extent),
        PrIRect{0, 0, int32_t(target.extent.x), int32_t(target.extent.y)});
    if (prIRectIsEmpty(landed))
        return;
    uint32_t bytes = prPixelFormatSize(format);
    size_t stride = size_t(extent.x) * bytes;
    RawArray<uint8_t> read;
    read.resize(uint32_t(stride * extent.y));
    if (!drawMemoryReadSource(device, source, format, PrPixels{read.data, stride, extent})) {
        read.reset();
        return;
    }
    size_t count = size_t(prIRectWidth(landed)) * bytes;
    for (int32_t y = landed.top; y < landed.bottom; ++y) {
        prMemCopy(target.data + size_t(y) * target.stride + size_t(landed.left) * bytes,
            read.data + size_t(y - origin.y) * stride + size_t(landed.left - origin.x) * bytes, count);
    }
    read.reset();
}

DrawMemoryFrame* drawMemoryFrameCreate(PrDrawRenderer* renderer, DrawMemoryRenderFn render, bool halfFloat) {
    DrawMemoryFrame* frame = allocZeroed<DrawMemoryFrame>();
    frame->destroyFn = destroyFrame;
    frame->prepareFn = prepareFrame;
    frame->renderFn = renderFrame;
    frame->renderer = renderer;
    frame->render = render;
    frame->halfFloat = halfFloat;
    return frame;
}

} // namespace pr_impl
