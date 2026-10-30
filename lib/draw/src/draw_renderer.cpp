// Draw renderers: the interface functions, which enforce the contract around each backend's operations.

#include "pr/draw_renderer.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/vec.h"

#if defined(PR_DRAW_HAS_CPU)
#include "pr/draw_cpu.h"
#endif
#if defined(PR_DRAW_HAS_TRIANGLES)
#include "pr/draw_triangles.h"
#endif
#if defined(PR_DRAW_HAS_FRAGMENT)
#include "pr/draw_fragment.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrDrawRendererBackend_Count] = {"default", "cpu", "triangles", "fragment"};

// The backends that the build has, in the order of preference, which Default creates the first of that the device
// allow, and then Default, so that a build without backends creates none.
constexpr PrDrawRendererBackend kBuilt[] = {
#if defined(PR_DRAW_HAS_FRAGMENT)
    PrDrawRendererBackend_Fragment,
#endif
#if defined(PR_DRAW_HAS_TRIANGLES)
    PrDrawRendererBackend_Triangles,
#endif
#if defined(PR_DRAW_HAS_CPU)
    PrDrawRendererBackend_Cpu,
#endif
    PrDrawRendererBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawRendererBackend prDrawRendererBackendFromName(PrStringView name) {
    for (PrDrawRendererBackend backend = 0; backend < PrDrawRendererBackend_Count; ++backend) {
        if (asciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrDrawRendererBackend_Count;
}

PrStringView prDrawRendererBackendName(PrDrawRendererBackend backend) {
    return backend < PrDrawRendererBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prDrawRendererBackendIsBuilt(PrDrawRendererBackend backend) {
    for (PrDrawRendererBackend built : kBuilt) {
        if (built != PrDrawRendererBackend_Default && (built == backend || backend == PrDrawRendererBackend_Default))
            return true;
    }
    return false;
}

bool prDrawRendererBackendIsDevice(PrDrawRendererBackend backend) {
    return backend == PrDrawRendererBackend_Triangles || backend == PrDrawRendererBackend_Fragment;
}

PrDrawRendererOptions prDrawRendererOptionsDefault(void) {
    return PrDrawRendererOptions{};
}

PrStatus prDrawRendererCreate(PrDrawRendererBackend backend, PrGpuDevice* PR_NULLABLE device,
        const PrDrawRendererOptions* PR_NULLABLE options, PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDrawRendererOptions o = options != nullptr ? *options : prDrawRendererOptionsDefault();
    if (backend == PrDrawRendererBackend_Default) {
        for (PrDrawRendererBackend built : kBuilt) {
            backend = built;
            if (device != nullptr || !prDrawRendererBackendIsDevice(built))
                break;
        }
    }
    if (prDrawRendererBackendIsDevice(backend) && device == nullptr)
        return PrStatus_NotSupported;
    switch (backend) {
#if defined(PR_DRAW_HAS_CPU)
        case PrDrawRendererBackend_Cpu: {
            PrDrawCpuOptions cpu = prDrawCpuOptionsDefault();
            cpu.glyphDecoder = o.glyphDecoder;
            *out = prDrawCpuCreate(&cpu);
            return PrStatus_Ok;
        }
#endif
#if defined(PR_DRAW_HAS_TRIANGLES)
        case PrDrawRendererBackend_Triangles: {
            PrDrawTrianglesOptions triangles = prDrawTrianglesOptionsDefault();
            triangles.glyphDecoder = o.glyphDecoder;
            return prDrawTrianglesCreate(device, &triangles, out);
        }
#endif
#if defined(PR_DRAW_HAS_FRAGMENT)
        case PrDrawRendererBackend_Fragment: {
            PrDrawFragmentOptions fragment = prDrawFragmentOptionsDefault();
            fragment.glyphDecoder = o.glyphDecoder;
            return prDrawFragmentCreate(device, &fragment, out);
        }
#endif
        default: return PrStatus_NotSupported;
    }
}

void prDrawRendererDestroy(PrDrawRenderer* PR_NULLABLE renderer) {
    if (renderer != nullptr && renderer->destroy != nullptr)
        renderer->destroy(renderer);
}

bool prDrawRendererIsOnDevice(const PrDrawRenderer* renderer) {
    return renderer->frameRender != nullptr;
}

void prDrawRendererDropResources(PrDrawRenderer* renderer) {
    if (renderer->dropResources != nullptr)
        renderer->dropResources(renderer);
}

PrDrawFrame* prDrawFrameCreate(PrDrawRenderer* renderer) {
    PrDrawFrame* frame = renderer->frameCreate(renderer);
    PR_ASSERT(frame->renderer == renderer);
    frame->extent.x = 0;
    frame->extent.y = 0;
    frame->prepared = false;
    return frame;
}

void prDrawFrameDestroy(PrDrawFrame* PR_NULLABLE frame) {
    if (frame != nullptr)
        frame->renderer->frameDestroy(frame->renderer, frame);
}

PrStatus prDrawFramePrepare(PrDrawFrame* frame, const PrDrawList* list, const PrDrawTarget* target) {
    PrDrawRenderer* renderer = frame->renderer;
    PR_ASSERT(prDrawListDepth(list) == 0);
    PR_ASSERT(!prDrawRendererIsOnDevice(renderer) || target->commands != nullptr);
    PR_ASSERT(target->beneath == nullptr || target->beneath->kind != PrDrawBeneathKind_Texture ||
        target->beneath->texture != 0);
    PR_ASSERT(target->beneath == nullptr || target->beneath->kind != PrDrawBeneathKind_Draw || target->beneath->draw != nullptr);
    frame->prepared = false;
    if (target->extent.x > kPrDrawRenderMaxSize || target->extent.y > kPrDrawRenderMaxSize)
        return PrStatus_Limit;
    if (target->format != PrPixelFormat_Rgba8 && target->format != PrPixelFormat_Bgra8 &&
            target->format != PrPixelFormat_RgbaF16)
        return PrStatus_NotSupported;
    PrStatus status = renderer->framePrepare(renderer, frame, list, target);
    if (status != PrStatus_Ok)
        return status;
    frame->extent.x = target->extent.x;
    frame->extent.y = target->extent.y;
    frame->prepared = true;
    return status;
}

PrDrawStats prDrawFrameStats(const PrDrawFrame* frame) {
    PrDrawStats stats{};
    if (frame->renderer->frameStats != nullptr)
        frame->renderer->frameStats(frame->renderer, frame, &stats);
    return stats;
}

void prDrawFrameRender(PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin) {
    PR_ASSERT(frame->renderer->frameRender != nullptr && frame->prepared);
    frame->renderer->frameRender(frame->renderer, frame, pass, origin);
}

void prDrawFrameRenderPixels(PrDrawFrame* frame, PrPixels pixels, PrPixelFormat format) {
    PR_ASSERT(frame->renderer->frameRenderPixels != nullptr && frame->prepared);
    PR_ASSERT(format == PrPixelFormat_Rgba8 || format == PrPixelFormat_Bgra8 || format == PrPixelFormat_RgbaF16);
    PR_ASSERT(size_t(pixels.extent.x) * prPixelFormatSize(format) <= pixels.stride);
    PR_ASSERT(pixels.extent.x == frame->extent.x && pixels.extent.y == frame->extent.y);
    if (format != PrPixelFormat_RgbaF16 || frame->renderer->rendersHalfFloat) {
        frame->renderer->frameRenderPixels(frame->renderer, frame, pixels, format);
        return;
    }
    // Rendered over the pixels' own colors, clamped, then widened back.
    size_t stride = size_t(pixels.extent.x) * 4;
    uint8_t* narrow = static_cast<uint8_t*>(prMemoryAlloc(stride * pixels.extent.y + 1));
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            PrFloat16 wide[4];
            memCopy(wide, pixels.data + y * pixels.stride + x * 8, sizeof(wide));
            float a = clampF32(prFloat16ToFloat(wide[3]), 0, 1);
            for (uint32_t k = 0; k < 4; ++k) {
                float v = clampF32(prFloat16ToFloat(wide[k]), 0, k < 3 ? a : 1);
                narrow[y * stride + x * 4 + k] = uint8_t(v * 255 + 0.5f);
            }
        }
    }
    frame->renderer->frameRenderPixels(frame->renderer, frame, PrPixels{narrow, stride, pixels.extent},
        PrPixelFormat_Rgba8);
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            PrFloat16 wide[4];
            for (uint32_t k = 0; k < 4; ++k)
                wide[k] = prFloat16FromFloat(float(narrow[y * stride + x * 4 + k]) / 255);
            memCopy(pixels.data + y * pixels.stride + x * 8, wide, sizeof(wide));
        }
    }
    prMemoryFree(narrow);
}

PrStatus prDrawRendererRenderPixels(PrDrawRenderer* renderer, const PrDrawList* list, PrPixels pixels, PrPixelFormat format) {
    PrDrawTarget target{};
    target.extent = pixels.extent;
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    PrStatus status = prDrawFramePrepare(frame, list, &target);
    if (status == PrStatus_Ok)
        prDrawFrameRenderPixels(frame, pixels, format);
    prDrawFrameDestroy(frame);
    return status;
}
