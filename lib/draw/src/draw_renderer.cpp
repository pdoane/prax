// Draw renderers: the interface functions, which enforce the contract around each backend's operations.

#include "pr/draw_renderer.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/draw_commands.h"
#include "pr/draw_renderer_backend.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// Returns the part that is the whole list, played from its root.
PrDrawPart wholeOf(const PrDrawList* list) {
    return PrDrawPart{
        .first = 0,
        .count = prDrawListCommandCount(list),
        .transform = prDrawTransformFromAffine2(prAffine2Identity()),
    };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawRendererOptions prDrawRendererOptionsDefault(void) {
    return PrDrawRendererOptions{};
}

void prDrawRendererDestroy(PrDrawRenderer* PR_NULLABLE renderer) {
    if (renderer != nullptr && renderer->destroyFn != nullptr)
        renderer->destroyFn(renderer);
}

PrStringView prDrawRendererName(const PrDrawRenderer* renderer) {
    return renderer->name;
}

PrGpuDevice* prDrawRendererDevice(const PrDrawRenderer* renderer) {
    return renderer->deviceFn(renderer);
}

void prDrawRendererDropResources(PrDrawRenderer* renderer) {
    if (renderer->dropResourcesFn != nullptr)
        renderer->dropResourcesFn(renderer);
}

PrDrawFrame* prDrawFrameCreate(PrDrawRenderer* renderer) {
    return renderer->frameCreateFn(renderer);
}

void prDrawFrameDestroy(PrDrawFrame* PR_NULLABLE frame) {
    if (frame != nullptr)
        frame->destroyFn(frame);
}

PrStatus prDrawFramePrepare(PrDrawFrame* frame, const PrDrawList* list, const PrDrawTarget* target) {
    PrDrawPart whole = wholeOf(list);
    return prDrawFramePreparePart(frame, list, &whole, target);
}

PrStatus prDrawFramePreparePart(PrDrawFrame* frame, const PrDrawList* list, const PrDrawPart* part, const PrDrawTarget* target) {
    PR_ASSERT(part->first <= prDrawListCommandCount(list) && part->count <= prDrawListCommandCount(list) - part->first);
    PR_ASSERT(prDrawListDepth(list) == 0);
    PR_ASSERT(target->commands != nullptr);
    PR_ASSERT(target->beneath == nullptr || target->beneath->kind != PrDrawBeneathKind_Texture ||
        target->beneath->texture != 0);
    PR_ASSERT(target->beneath == nullptr || target->beneath->kind != PrDrawBeneathKind_Draw || target->beneath->draw != nullptr);
    if (target->extent.x > kPrDrawRenderMaxSize || target->extent.y > kPrDrawRenderMaxSize)
        return PrStatus_Limit;
    if (target->format != PrPixelFormat_Rgba8 && target->format != PrPixelFormat_Bgra8 &&
            target->format != PrPixelFormat_RgbaF16)
        return PrStatus_NotSupported;
    return frame->prepareFn(frame, list, part, target);
}

PrDrawStats prDrawFrameStats(const PrDrawFrame* frame) {
    PrDrawStats stats{};
    if (frame->statsFn != nullptr)
        frame->statsFn(frame, &stats);
    return stats;
}

void prDrawFrameRender(PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin) {
    frame->renderFn(frame, pass, origin);
}

PrStatus prDrawRendererRenderPixels(PrDrawRenderer* renderer, const PrDrawList* list, PrPixels pixels, PrPixelFormat format) {
    PrDrawPart whole = wholeOf(list);
    return prDrawRendererRenderPartPixels(renderer, list, &whole, pixels, format);
}

PrStatus prDrawRendererRenderPartPixels(PrDrawRenderer* renderer, const PrDrawList* list, const PrDrawPart* part,
        PrPixels pixels, PrPixelFormat format) {
    PR_ASSERT(format == PrPixelFormat_Rgba8 || format == PrPixelFormat_Bgra8 || format == PrPixelFormat_RgbaF16);
    PR_ASSERT(size_t(pixels.extent.x) * prPixelFormatSize(format) <= pixels.stride);
    PrGpuDevice* device = prDrawRendererDevice(renderer);
    if (!prGpuDeviceHasNative(device, PrGpuNativeKind_Pixels))
        return PrStatus_NotSupported;
    constexpr PrGpuTextureUsageFlags kUsage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
    PrGpuTexture texture{};
    PrStatus status = prGpuTextureCreateOverPixels(device, prGpuFormatFromPixelFormat(format), pixels, kUsage, &texture);
    PrGpuCommands* commands = nullptr;
    if (status == PrStatus_Ok && !prGpuCommandsBegin(device, nullptr, &commands))
        status = PrStatus_Lost;
    if (status != PrStatus_Ok) {
        prGpuTextureRelease(device, texture);
        return status;
    }
    PrDrawTarget target{.extent = pixels.extent, .format = format, .commands = commands};
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    status = prDrawFramePreparePart(frame, list, part, &target);
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
    // A device that draws into pixels runs work when it is submitted, so the pixels hold the render after the
    // submission.
    prGpuCommandsSubmit(commands);
    prDrawFrameDestroy(frame);
    prGpuTextureRelease(device, texture);
    return status;
}
