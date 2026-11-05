// Draw renderers: the interface functions, which enforce the contract around each backend's operations.

#include "pr/draw_renderer.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/vec.h"

#if defined(PR_DRAW_HAS_CANVAS)
#include "pr/draw_canvas.h"
#endif
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

constexpr PrStringView kBackendNames[PrDrawRendererBackend_Count] = {"default", "cpu", "canvas", "triangles", "fragment"};

// The backends in the build, in the order of preference, and then Default. Default creates the first of these that the
// device runs. Listing Default last means that a build without backends creates none.
constexpr PrDrawRendererBackend kBuilt[] = {
#if defined(PR_DRAW_HAS_FRAGMENT)
    PrDrawRendererBackend_Fragment,
#endif
#if defined(PR_DRAW_HAS_TRIANGLES)
    PrDrawRendererBackend_Triangles,
#endif
#if defined(PR_DRAW_HAS_CANVAS)
    PrDrawRendererBackend_Canvas,
#endif
#if defined(PR_DRAW_HAS_CPU)
    PrDrawRendererBackend_Cpu,
#endif
    PrDrawRendererBackend_Default,
};

// Creates a renderer by `backend`, which is not Default. Only the CPU renderer makes a device of its own.
PrStatus create(PrDrawRendererBackend backend, PrGpuDevice* PR_NULLABLE device, const PrDrawRendererOptions& o,
        PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    if (backend != PrDrawRendererBackend_Cpu && device == nullptr)
        return PrStatus_NotSupported;
    switch (backend) {
#if defined(PR_DRAW_HAS_CPU)
        case PrDrawRendererBackend_Cpu: {
            PrDrawCpuOptions cpu = prDrawCpuOptionsDefault();
            cpu.glyphDecoder = o.glyphDecoder;
            cpu.device = device;
            *out = prDrawCpuCreate(&cpu);
            return *out != nullptr ? PrStatus_Ok : PrStatus_NotSupported;
        }
#endif
#if defined(PR_DRAW_HAS_CANVAS)
        case PrDrawRendererBackend_Canvas: {
            PrDrawCanvasOptions canvas = prDrawCanvasOptionsDefault();
            canvas.glyphDecoder = o.glyphDecoder;
            return prDrawCanvasCreate(device, &canvas, out);
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

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawRendererBackend prDrawRendererBackendFromName(PrStringView name) {
    for (PrDrawRendererBackend backend = 0; backend < PrDrawRendererBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
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

PrDrawRendererOptions prDrawRendererOptionsDefault(void) {
    return PrDrawRendererOptions{};
}

PrStatus prDrawRendererCreate(PrDrawRendererBackend backend, PrGpuDevice* PR_NULLABLE device,
        const PrDrawRendererOptions* PR_NULLABLE options, PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDrawRendererOptions o = options != nullptr ? *options : prDrawRendererOptionsDefault();
    if (backend != PrDrawRendererBackend_Default)
        return create(backend, device, o, out);
    // Default takes the first backend that the device runs.
    for (PrDrawRendererBackend built : kBuilt) {
        if (built == PrDrawRendererBackend_Default)
            break;
        PrStatus status = create(built, device, o, out);
        if (status != PrStatus_NotSupported)
            return status;
    }
    return PrStatus_NotSupported;
}

void prDrawRendererDestroy(PrDrawRenderer* PR_NULLABLE renderer) {
    if (renderer != nullptr && renderer->destroy != nullptr)
        renderer->destroy(renderer);
}

PrGpuDevice* prDrawRendererDevice(const PrDrawRenderer* renderer) {
    return renderer->device;
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
    PR_ASSERT(target->commands != nullptr);
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
    PR_ASSERT(frame->prepared);
    frame->renderer->frameRender(frame->renderer, frame, pass, origin);
}

PrStatus prDrawRendererRenderPixels(PrDrawRenderer* renderer, const PrDrawList* list, PrPixels pixels, PrPixelFormat format) {
    PR_ASSERT(format == PrPixelFormat_Rgba8 || format == PrPixelFormat_Bgra8 || format == PrPixelFormat_RgbaF16);
    PR_ASSERT(size_t(pixels.extent.x) * prPixelFormatSize(format) <= pixels.stride);
    PrGpuDevice* device = renderer->device;
    PrGpuFormat textureFormat = prGpuFormatFromPixelFormat(format);
    constexpr PrGpuTextureUsageFlags kUsage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
    // A memory device draws into the pixels themselves, and any other device draws into a copy of them.
    bool inPlace = prGpuDeviceHasNative(device, PrGpuNativeKind_Pixels);
    PrGpuTexture texture{};
    PrStatus status = PrStatus_Ok;
    if (inPlace) {
        status = prGpuTextureCreateOverPixels(device, textureFormat, pixels, kUsage, &texture);
    } else {
        PrGpuTextureDesc desc = prGpuTextureDesc2d(textureFormat, pixels.extent, kUsage);
        status = prGpuTextureCreate(device, &desc, &texture);
        if (status == PrStatus_Ok)
            prGpuTextureWrite(device, texture, PrIVec2{0, 0}, prPixelsView(pixels));
    }
    PrGpuCommands* commands = nullptr;
    if (status == PrStatus_Ok && !prGpuCommandsBegin(device, nullptr, &commands))
        status = PrStatus_Lost;
    if (status != PrStatus_Ok) {
        prGpuTextureDestroy(device, texture);
        return status;
    }
    PrDrawTarget target{.extent = pixels.extent, .format = format, .commands = commands};
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    status = prDrawFramePrepare(frame, list, &target);
    if (status == PrStatus_Ok) {
        PrGpuSubresources all{};
        prGpuCommandsTextureBarrier(commands, texture, all, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc desc = prGpuPassDescColor(texture, PrGpuLoad_Keep, PrColor{});
        PrGpuPass* pass = prGpuPassBegin(commands, &desc);
        prDrawFrameRender(frame, pass, PrIVec2{0, 0});
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, texture, all, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
    }
    prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
    if (status == PrStatus_Ok && !inPlace)
        prGpuTextureRead(device, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, pixels);
    prDrawFrameDestroy(frame);
    prGpuTextureDestroy(device, texture);
    return status;
}
