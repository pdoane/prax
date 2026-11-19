// A Metal swap chain over a CompositorServices session's surface (PrSurfaceKind_CompositorServices), on visionOS.
//
// The session begins each frame and holds its drawable, which the surface points to. Acquiring wraps the drawable's
// textures and reads its views from their texture maps. Presenting hands the drawable to the command buffer, which
// encodes its present just before it is committed. The session ends the frame after that.

#include "gpu/backends/shared/src/gpu_shared_metal_compositor.h"

#include "pr/alloc.h"

#import <CompositorServices/CompositorServices.h>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

namespace {

extern const PrGpuSwapChain kCompositorFunctions;

struct CompositorSwapChain : GpuSharedSwapChain {
    cp_drawable_t PR_NULLABLE const* PR_NONNULL drawable;   // the session's slot of the begun frame's drawable
    cp_drawable_t PR_NULLABLE acquired;                     // the drawable of the acquired frame
    PrGpuTexture color;
    PrGpuTexture depth;
};

// Lets go of the acquired drawable's textures, whose slots stay until the work that drew into them is done.
void release(GpuMetalDevice* d, CompositorSwapChain* sc) {
    if (sc->color.id != 0)
        gpuMetalRetire(d, GpuMetalRetiredKind_Resource, sc->color.id);
    if (sc->depth.id != 0)
        gpuMetalRetire(d, GpuMetalRetiredKind_Resource, sc->depth.id);
    sc->color = PrGpuTexture{};
    sc->depth = PrGpuTexture{};
    sc->acquired = nullptr;
}

void encodePresent(void* drawable, id commandBuffer) {
    cp_drawable_encode_present(static_cast<cp_drawable_t>(drawable), static_cast<id<MTLCommandBuffer>>(commandBuffer));
}

void destroy(PrGpuSwapChain* handle) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(handle)->device;
    CompositorSwapChain* sc = static_cast<CompositorSwapChain*>(handle);
    gpuMetalWaitIdle(device);
    release(gpuMetalDeviceFrom(device), sc);
    prMemoryFree(sc);
}

// The session waits for the compositor as it begins the frame.
void wait(PrGpuSwapChain*) {}

PrStatus acquire(PrGpuSwapChain* handle, PrUVec2, PrGpuSwapChainFrame* out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(static_cast<GpuSharedSwapChain*>(handle)->device);
    CompositorSwapChain* sc = static_cast<CompositorSwapChain*>(handle);
    @autoreleasepool {
        release(d, sc);
        cp_drawable_t drawable = *sc->drawable;
        // A dedicated layout has a texture for each view, which a frame of one texture does not hold.
        if (drawable == nullptr || cp_drawable_get_texture_count(drawable) != 1)
            return PrStatus_InvalidState;
        PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled;
        PrStatus status = gpuMetalTextureWrap(d, cp_drawable_get_color_texture(drawable, 0), usage, &sc->color);
        if (status == PrStatus_Ok) {
            status = gpuMetalTextureWrap(d, cp_drawable_get_depth_texture(drawable, 0),
                PrGpuTextureUsageFlag_DepthStencil, &sc->depth);
        }
        if (status != PrStatus_Ok) {
            release(d, sc);
            return status;
        }
        sc->acquired = drawable;
        sc->info = PrGpuSwapChainInfo{d->resources[sc->color.id].texture.format, PrColorSpace_Srgb};
        out->color = sc->color;
        out->depth = sc->depth;
        size_t views = cp_drawable_get_view_count(drawable);
        out->viewCount = uint8_t(views < kPrGpuMaxViews ? views : kPrGpuMaxViews);
        for (uint8_t i = 0; i < out->viewCount; ++i) {
            cp_view_texture_map_t map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, i));
            MTLViewport viewport = cp_view_texture_map_get_viewport(map);
            out->views[i] = PrGpuView{
                .viewport = PrIRect{
                    int32_t(viewport.originX), int32_t(viewport.originY),
                    int32_t(viewport.originX + viewport.width), int32_t(viewport.originY + viewport.height),
                },
                .layer = uint16_t(cp_view_texture_map_get_slice_index(map)),
            };
        }
        return PrStatus_Ok;
    }
}

void present(PrGpuSwapChain* handle, PrGpuCommands* commands) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(handle)->device;
    CompositorSwapChain* sc = static_cast<CompositorSwapChain*>(handle);
    if (sc->acquired == nullptr)
        return;
    GpuMetalDrawable drawable{.compositorDrawable = sc->acquired, .encodePresent = encodePresent};
    gpuMetalCommandsFrom(commands)->drawables.push(drawable);
    release(gpuMetalDeviceFrom(device), sc);
}

constexpr PrGpuSwapChain kCompositorFunctions = {
    .deviceFn = gpuSharedSwapChainDevice,
    .infoFn = gpuSharedSwapChainInfo,
    .destroyFn = destroy,
    .waitFn = wait,
    .acquireFn = acquire,
    .presentFn = present,
    .compositorExtent = true,
};

} // namespace

PrStatus gpuMetalCompositorSwapChainCreate(PrGpuDevice* device, const PrSurface* surface,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    if (!gpuMetalDeviceFrom(device)->compositorPresents)
        return PrStatus_NotSupported;
    CompositorSwapChain* sc = allocZeroed<CompositorSwapChain>();
    static_cast<PrGpuSwapChain&>(*sc) = kCompositorFunctions;
    sc->device = device;
    sc->info = PrGpuSwapChainInfo{PrGpuFormat_Bgra8Srgb, PrColorSpace_Srgb};
    sc->drawable = static_cast<cp_drawable_t PR_NULLABLE const*>(surface->handle);
    *out = sc;
    return PrStatus_Ok;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
