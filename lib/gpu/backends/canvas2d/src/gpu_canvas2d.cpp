// The Canvas 2D GPU backend: a memory device whose textures' copies are canvases of a web page, and whose swap chains
// present to a page's canvas.

#include "pr/gpu_canvas2d.h"

#include "pr/alloc.h"
#include "pr/memory.h"
#include "pr/raw_string.h"
#include "pr/surface.h"

#include "gpu/backends/shared/src/gpu_shared_memory.h"

// The page's side of the backend (gpu_canvas2d.js). Texels are premultiplied, with rows `stride` bytes apart, in RGBA
// order, or in BGRA order if `bgra` is set.
extern "C" bool prGpuCanvas2dAvailable(void);
extern "C" uint32_t prGpuCanvas2dCreateCanvas(uint32_t width, uint32_t height);
extern "C" uint32_t prGpuCanvas2dAttach(const char* PR_NULLABLE selector, size_t selectorSize, uint32_t width, uint32_t height,
    bool alpha);
extern "C" void prGpuCanvas2dRelease(uint32_t canvas);
extern "C" uint32_t prGpuCanvas2dImport(uint32_t image, uint32_t* width, uint32_t* height);
extern "C" void prGpuCanvas2dClear(uint32_t canvas, float r, float g, float b, float a);
extern "C" void prGpuCanvas2dWrite(uint32_t canvas, const uint8_t* texels, size_t stride, uint32_t width,
    uint32_t height, bool bgra);
extern "C" void prGpuCanvas2dRead(uint32_t canvas, uint8_t* texels, size_t stride, uint32_t width, uint32_t height, bool bgra);

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// The textures' canvases

bool holds(PrGpuFormat format) {
    return format == PrGpuFormat_Rgba8 || format == PrGpuFormat_Bgra8;
}

uint32_t create(PrUVec2 extent) {
    return prGpuCanvas2dCreateCanvas(extent.x, extent.y);
}

void clear(uint32_t canvas, PrColor color, PrGpuFormat format) {
    if (format == PrGpuFormat_Bgra8)
        prGpuCanvas2dClear(canvas, color.b, color.g, color.r, color.a);
    else
        prGpuCanvas2dClear(canvas, color.r, color.g, color.b, color.a);
}

void write(uint32_t canvas, PrPixelView texels, PrGpuFormat format) {
    prGpuCanvas2dWrite(canvas, texels.data, texels.stride, texels.extent.x, texels.extent.y,
        format == PrGpuFormat_Bgra8);
}

void read(uint32_t canvas, PrPixels texels, PrGpuFormat format) {
    prGpuCanvas2dRead(canvas, texels.data, texels.stride, texels.extent.x, texels.extent.y,
        format == PrGpuFormat_Bgra8);
}

constexpr GpuMemoryMirror kCanvases{
    .kind = PrGpuNativeKind_Canvas2d,
    .holds = holds,
    .create = create,
    .destroy = prGpuCanvas2dRelease,
    .clear = clear,
    .write = write,
    .read = read,
};

// Imports an image of the page as a texture. The texture's copy is a reference of its own to the image.
PrStatus textureImport(PrGpuDevice* device, const PrGpuImport* image, PrGpuReleaseFn PR_NULLABLE release,
        void* PR_NULLABLE context, PrGpuTexture* out) {
    *out = PrGpuTexture{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t canvas = prGpuCanvas2dImport(uint32_t(uintptr_t(image->handle)), &width, &height);
    if (canvas == 0)
        return PrStatus_NotSupported;
    PrStatus status = gpuMemoryImport(device, PrUVec2{width, height}, PrGpuFormat_Rgba8, canvas, release, context, out);
    if (status != PrStatus_Ok)
        prGpuCanvas2dRelease(canvas);
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------
// Swap chains

// The functions of the device's swap chains, which their creation installs.
extern const PrGpuSwapChain kSwapChainFunctions;

// A swap chain: the page's canvas, by its selector, and the texture that the canvas is. The texture is made again when
// the frame's extent changes. A transparent swap chain's canvas shows the page behind it.
struct SwapChain : GpuSharedSwapChain {
    PrRawString selector;
    PrGpuTexture texture;
    bool transparent;
};

// Brings the canvas up to date with what the frame drew, whether through the canvas or into the texels.
void present(void* PR_NULLABLE context) {
    SwapChain* s = static_cast<SwapChain*>(context);
    PrGpuCanvas canvas = 0;
    prGpuTextureNative(s->device, s->texture, PrGpuNativeKind_Canvas2d, &canvas);
}

PrStatus swapChainCreate(PrGpuDevice* device, const PrSurface* surface, const PrGpuSwapChainDesc* desc,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (surface->kind != PrSurfaceKind_HtmlCanvas)
        return PrStatus_NotSupported;
    SwapChain* s = allocZeroed<SwapChain>();
    static_cast<PrGpuSwapChain&>(*s) = kSwapChainFunctions;
    s->device = device;
    s->info = PrGpuSwapChainInfo{.format = PrGpuFormat_Rgba8, .space = PrColorSpace_Srgb};
    s->transparent = (desc->flags & PrGpuSwapChainFlag_Transparent) != 0;
    s->selector = prRawStringCopy(static_cast<const char*>(surface->handle));
    *out = s;
    return PrStatus_Ok;
}

void swapChainDestroy(PrGpuSwapChain* swapChain) {
    SwapChain* s = static_cast<SwapChain*>(swapChain);
    prGpuTextureDestroy(s->device, s->texture);
    prRawStringReset(&s->selector);
    prMemoryFree(s);
}

// The page paces its frames with its animation frames, so the swap chain has nothing to wait for.
void swapChainWait(PrGpuSwapChain*) {}

PrStatus swapChainAcquire(PrGpuSwapChain* swapChain, PrUVec2 extent, PrGpuTexture* out) {
    SwapChain* s = static_cast<SwapChain*>(swapChain);
    PrGpuDevice* device = s->device;
    if (s->texture.id != 0) {
        PrUVec2 held = prGpuTextureExtent(device, s->texture);
        if (held.x != extent.x || held.y != extent.y) {
            prGpuTextureDestroy(device, s->texture);
            s->texture = PrGpuTexture{};
        }
    }
    if (s->texture.id == 0) {
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, extent,
            PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        PrStatus status = prGpuTextureCreate(device, &desc, &s->texture);
        if (status != PrStatus_Ok)
            return status;
        // Sizing the page's canvas to the extent clears it, as the new texture's texels are clear.
        PrStringView selector = prRawStringView(&s->selector);
        uint32_t canvas = prGpuCanvas2dAttach(selector.data, selector.size, extent.x, extent.y, s->transparent);
        if (canvas == 0) {
            prGpuTextureDestroy(device, s->texture);
            s->texture = PrGpuTexture{};
            return PrStatus_Lost;
        }
        gpuMemoryAdoptMirror(device, s->texture, canvas);
    }
    *out = s->texture;
    return PrStatus_Ok;
}

void swapChainPresent(PrGpuSwapChain* swapChain, PrGpuCommands* commands) {
    gpuMemoryRecordCall(commands, present, swapChain);
}

// The functions of the device's swap chains.
constexpr PrGpuSwapChain kSwapChainFunctions = {
    .deviceFn = gpuSharedSwapChainDevice,
    .infoFn = gpuSharedSwapChainInfo,
    .destroyFn = swapChainDestroy,
    .waitFn = swapChainWait,
    .acquireFn = swapChainAcquire,
    .presentFn = swapChainPresent,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prGpuCanvas2dCreate(PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (!prGpuCanvas2dAvailable())
        return PrStatus_NotSupported;
    GpuSharedDevice* device = static_cast<GpuSharedDevice*>(gpuMemoryCreate("canvas2d", &kCanvases));
    device->inPlaceImports = 1u << PrGpuImportKind_WebImage;
    device->textureImportInPlace = textureImport;
    device->swapChainCreateFn = swapChainCreate;
    *out = device;
    return PrStatus_Ok;
}
