// GPU devices: argument checks over the backend's functions, and what pr/gpu.h builds from them for every backend.

#include "pr/gpu.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/gpu_backend.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"

#if defined(PR_GPU_HAS_METAL)
#include "pr/gpu_metal.h"
#endif
#if defined(PR_GPU_HAS_METAL4)
#include "pr/gpu_metal4.h"
#endif
#if defined(PR_GPU_HAS_VULKAN)
#include "pr/gpu_vulkan.h"
#endif
#if defined(PR_GPU_HAS_D3D12)
#include "pr/gpu_d3d12.h"
#endif
#if defined(PR_GPU_HAS_CANVAS2D)
#include "pr/gpu_canvas2d.h"
#endif
#if defined(PR_GPU_HAS_MEMORY)
#include "pr/gpu_memory.h"
#endif

#if defined(PR_GPU_HAS_COREVIDEO)
#include "gpu/src/gpu_corevideo.h"
#endif

#include <stdlib.h>

namespace pr_impl {

namespace {

// The block of each format, by PrGpuFormat.
constexpr PrGpuFormatBlock kFormatBlocks[PrGpuFormat_Count] = {
    {0, 0, 0},      // None
    {1, 1, 1},      // R8
    {1, 1, 2},      // Rg8
    {1, 1, 4},      // Rgba8
    {1, 1, 4},      // Rgba8Srgb
    {1, 1, 4},      // Bgra8
    {1, 1, 4},      // Bgra8Srgb
    {1, 1, 4},      // Rgb10A2
    {1, 1, 4},      // Rg11B10F
    {1, 1, 2},      // R16F
    {1, 1, 4},      // Rg16F
    {1, 1, 8},      // Rgba16F
    {1, 1, 4},      // R32F
    {1, 1, 8},      // Rg32F
    {1, 1, 16},     // Rgba32F
    {1, 1, 1},      // R8Uint
    {1, 1, 2},      // R16Uint
    {1, 1, 4},      // R32Uint
    {1, 1, 8},      // Rg32Uint
    {1, 1, 16},     // Rgba32Uint
    {1, 1, 2},      // D16
    {1, 1, 4},      // D32F
    {1, 1, 8},      // D32FS8
    {4, 4, 8},      // Bc1
    {4, 4, 8},      // Bc1Srgb
    {4, 4, 16},     // Bc3
    {4, 4, 16},     // Bc3Srgb
    {4, 4, 8},      // Bc4
    {4, 4, 16},     // Bc5
    {4, 4, 16},     // Bc6HUfloat
    {4, 4, 16},     // Bc7
    {4, 4, 16},     // Bc7Srgb
    {4, 4, 16},     // Astc4x4
    {4, 4, 16},     // Astc4x4Srgb
};

// The alignment of the rows of a texture's texels in a buffer that a copy reads or writes, which every API accepts.
constexpr uint32_t kRowAlignment = 256;

// The part of a `size` rectangle at `origin` that lies inside `extent`, as its origin and size, and its start within
// the rectangle. Returns false if none of it does.
bool clip(PrIVec2 origin, PrUVec2 size, PrUVec2 extent, PrUVec2* at, PrUVec2* clipped, PrUVec2* from) {
    int64_t left = origin.x > 0 ? origin.x : 0;
    int64_t top = origin.y > 0 ? origin.y : 0;
    int64_t right = int64_t(origin.x) + size.x;
    int64_t bottom = int64_t(origin.y) + size.y;
    right = right < int64_t(extent.x) ? right : int64_t(extent.x);
    bottom = bottom < int64_t(extent.y) ? bottom : int64_t(extent.y);
    if (left >= right || top >= bottom)
        return false;
    *at = PrUVec2{uint32_t(left), uint32_t(top)};
    *clipped = PrUVec2{uint32_t(right - left), uint32_t(bottom - top)};
    *from = PrUVec2{uint32_t(left - origin.x), uint32_t(top - origin.y)};
    return true;
}

uint32_t readWord(const uint8_t* bytes, uint64_t at) {
    return prMemReadLeU32(bytes + at);
}

const PrGpuShaderEntry* PR_NULLABLE entryNamed(const PrGpuShaderLibrary* library, PrStringView name) {
    uint32_t count = 0;
    const PrGpuShaderEntry* entries = library->entriesFn(library, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].name == name)
            return &entries[i];
    }
    return nullptr;
}

// A read of a texture that a command buffer records: the buffer that the texels are copied into, and where the call
// goes once they are there.
struct GpuTextureRead {
    PrGpuDevice* device;
    PrGpuBuffer buffer;
    uint32_t rowBytes;
    PrUVec2 extent;
    PrGpuFormat format;
    PrGpuReadFn read;
    void* PR_NULLABLE context;
};

void textureReadDone(void* PR_NULLABLE context, double) {
    GpuTextureRead* r = static_cast<GpuTextureRead*>(context);
    PrPixelView texels{};
    if (!prGpuDeviceIsLost(r->device))
        texels = PrPixelView{static_cast<const uint8_t*>(prGpuBufferMap(r->device, r->buffer)), r->rowBytes, r->extent};
    r->read(r->context, texels);
    prGpuBufferDestroy(r->device, r->buffer);
    prMemoryFree(r);
}

// Records a copy of the first level of a 2D texture, from `origin` and `extent` in size, into a new readback buffer
// with rows `rowBytes` apart. The texture is in `access` before and after the copy.
PrGpuBuffer recordRead(PrGpuCommands* commands, PrGpuTexture texture, uint32_t access, PrUVec2 origin,
        PrUVec2 extent, uint32_t rowBytes) {
    PrGpuDevice* device = prGpuCommandsDevice(commands);
    PrGpuBufferDesc desc{.size = uint64_t(rowBytes) * extent.y, .memory = PrGpuMemory_Readback, .name = "texture read"};
    PrGpuBuffer buffer{};
    if (prGpuBufferCreate(device, &desc, &buffer) != PrStatus_Ok)
        return buffer;
    PrGpuSubresources first{.levels = 1, .layers = 1};
    if (access != PrGpuAccessFlag_CopyRead)
        prGpuCommandsTextureBarrier(commands, texture, first, access, PrGpuAccessFlag_CopyRead);
    PrGpuTextureRegion region = prGpuTextureRegion2d(origin, extent);
    prGpuCommandsCopyTextureToBuffer(commands, texture, &region, buffer, 0, rowBytes, 0);
    if (access != PrGpuAccessFlag_CopyRead)
        prGpuCommandsTextureBarrier(commands, texture, first, PrGpuAccessFlag_CopyRead, access);
    return buffer;
}

// What prGpuCommandsSubmitAndWait waits for: the seconds that the device spent.
void submittedDone(void* PR_NULLABLE context, double seconds) {
    *static_cast<double*>(context) = seconds;
}

constexpr PrStringView kBackendNames[PrGpuBackend_Count] = {
    "default", "metal", "metal4", "vulkan", "d3d12", "memory", "canvas2d",
};

// The backends in the build, the platform's own first and memory last, and then Default. Default creates the first of
// these that the system supports. Listing Default last means that a build without backends creates none.
constexpr PrGpuBackend kBuilt[] = {
#if defined(PR_GPU_HAS_METAL)
    PrGpuBackend_Metal,
#endif
#if defined(PR_GPU_HAS_METAL4)
    PrGpuBackend_Metal4,
#endif
#if defined(PR_GPU_HAS_D3D12)
    PrGpuBackend_D3d12,
#endif
#if defined(PR_GPU_HAS_VULKAN)
    PrGpuBackend_Vulkan,
#endif
#if defined(PR_GPU_HAS_CANVAS2D)
    PrGpuBackend_Canvas2d,
#endif
#if defined(PR_GPU_HAS_MEMORY)
    PrGpuBackend_Memory,
#endif
    PrGpuBackend_Default,
};

// Creates a device by `backend`, which is not Default.
PrStatus create(PrGpuBackend backend, const PrGpuDeviceOptions& o, PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    // A build without a backend has no use for the options.
    (void)o;
    switch (backend) {
#if defined(PR_GPU_HAS_METAL)
        case PrGpuBackend_Metal: {
            PrGpuMetalOptions metal = prGpuMetalOptionsDefault();
            metal.validation = o.validation;
            metal.slotsOnly = o.slotsOnly;
            return prGpuMetalCreate(&metal, out);
        }
#endif
#if defined(PR_GPU_HAS_METAL4)
        case PrGpuBackend_Metal4: {
            // Metal 4 always has bindless, and its slots are tested beside it.
            if (o.slotsOnly)
                return PrStatus_NotSupported;
            PrGpuMetal4Options metal = prGpuMetal4OptionsDefault();
            metal.validation = o.validation;
            return prGpuMetal4Create(&metal, out);
        }
#endif
#if defined(PR_GPU_HAS_VULKAN)
        case PrGpuBackend_Vulkan: {
            // Vulkan always has bindless, and its slots are tested beside it.
            if (o.slotsOnly)
                return PrStatus_NotSupported;
            PrGpuVulkanOptions vulkan = prGpuVulkanOptionsDefault();
            vulkan.validation = vulkan.validation || o.validation;
            return prGpuVulkanCreate(&vulkan, out);
        }
#endif
#if defined(PR_GPU_HAS_D3D12)
        case PrGpuBackend_D3d12: {
            // Direct3D 12 always has bindless, and its slots are tested beside it.
            if (o.slotsOnly)
                return PrStatus_NotSupported;
            PrGpuD3d12Options d3d12 = prGpuD3d12OptionsDefault();
            d3d12.validation = d3d12.validation || o.validation;
            return prGpuD3d12Create(&d3d12, out);
        }
#endif
#if defined(PR_GPU_HAS_CANVAS2D)
        case PrGpuBackend_Canvas2d:
            return prGpuCanvas2dCreate(out);
#endif
#if defined(PR_GPU_HAS_MEMORY)
        case PrGpuBackend_Memory:
            return prGpuMemoryCreate(out);
#endif
        default:
            return PrStatus_NotSupported;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Devices

PrGpuBackend prGpuBackendFromName(PrStringView name) {
    for (PrGpuBackend backend = 0; backend < PrGpuBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrGpuBackend_Count;
}

PrStringView prGpuBackendName(PrGpuBackend backend) {
    return backend < PrGpuBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prGpuBackendIsBuilt(PrGpuBackend backend) {
    for (PrGpuBackend built : kBuilt) {
        if (built != PrGpuBackend_Default && (built == backend || backend == PrGpuBackend_Default))
            return true;
    }
    return false;
}

PrGpuDeviceOptions prGpuDeviceOptionsDefault(void) {
    return PrGpuDeviceOptions{};
}

PrStatus prGpuDeviceCreate(PrGpuBackend backend, const PrGpuDeviceOptions* PR_NULLABLE options,
        PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuDeviceOptions o = options != nullptr ? *options : prGpuDeviceOptionsDefault();
    if (backend != PrGpuBackend_Default)
        return create(backend, o, out);
    const char* named = getenv("PR_GPU_BACKEND");
    backend = named != nullptr ? prGpuBackendFromName(named) : PrGpuBackend_Count;
    if (backend != PrGpuBackend_Default && prGpuBackendIsBuilt(backend))
        return create(backend, o, out);
    // Default takes the first backend that the system supports.
    PrStatus status = PrStatus_NotSupported;
    for (PrGpuBackend built : kBuilt) {
        if (built == PrGpuBackend_Default)
            break;
        status = create(built, o, out);
        if (status != PrStatus_NotSupported)
            return status;
    }
    return status;
}

void prGpuDeviceDestroy(PrGpuDevice* PR_NULLABLE device) {
    if (device == nullptr)
        return;
    device->waitIdleFn(device);
    device->destroyFn(device);
}

PrGpuDeviceInfo prGpuDeviceInfo(const PrGpuDevice* device) {
    return device->infoFn(device);
}

bool prGpuDeviceHasNative(const PrGpuDevice* device, PrGpuNativeKind kind) {
    return kind < PrGpuNativeKind_Count && (device->nativeKindsFn(device) & (1u << kind)) != 0;
}

bool prGpuDeviceImportsInPlace(const PrGpuDevice* device, PrGpuImportKind kind) {
    return kind < PrGpuImportKind_Count && (device->inPlaceImportsFn(device) & (1u << kind)) != 0;
}

bool prGpuDeviceIsLost(const PrGpuDevice* device) {
    return device->isLostFn(device);
}

void prGpuDeviceSimulateLoss(PrGpuDevice* device) {
    device->simulateLossFn(device);
}

bool prGpuDeviceIsComplete(const PrGpuDevice* device, PrGpuFence fence) {
    return fence == 0 || device->isCompleteFn(device, fence);
}

void prGpuDeviceWait(PrGpuDevice* device, PrGpuFence fence) {
    if (fence != 0)
        device->waitFn(device, fence);
}

void prGpuDeviceWaitIdle(PrGpuDevice* device) {
    device->waitIdleFn(device);
}

// ---------------------------------------------------------------------------------------------------------------------
// Formats

PrGpuFormatBlock prGpuFormatBlock(PrGpuFormat format) {
    PR_ASSERT(format != PrGpuFormat_None && format < PrGpuFormat_Count);
    return kFormatBlocks[format];
}

bool prGpuFormatIsDepthStencil(PrGpuFormat format) {
    return format == PrGpuFormat_D16 || format == PrGpuFormat_D32F || format == PrGpuFormat_D32FS8;
}

PrGpuFormat prGpuFormatFromPixelFormat(PrPixelFormat format) {
    switch (format) {
        case PrPixelFormat_Rgba8:
            return PrGpuFormat_Rgba8;
        case PrPixelFormat_Bgra8:
            return PrGpuFormat_Bgra8;
        case PrPixelFormat_Rgb10A2:
            return PrGpuFormat_Rgb10A2;
        case PrPixelFormat_RgbaF16:
            return PrGpuFormat_Rgba16F;
        case PrPixelFormat_RgbaF32:
            return PrGpuFormat_Rgba32F;
        case PrPixelFormat_R8:
            return PrGpuFormat_R8;
        case PrPixelFormat_RF32:
            return PrGpuFormat_R32F;
        default:
            return PrGpuFormat_None;
    }
}

bool prGpuFormatToPixelFormat(PrGpuFormat format, PrPixelFormat* out) {
    switch (format) {
        case PrGpuFormat_Rgba8:
        case PrGpuFormat_Rgba8Srgb:
            *out = PrPixelFormat_Rgba8;
            return true;
        case PrGpuFormat_Bgra8:
        case PrGpuFormat_Bgra8Srgb:
            *out = PrPixelFormat_Bgra8;
            return true;
        case PrGpuFormat_Rgb10A2:
            *out = PrPixelFormat_Rgb10A2;
            return true;
        case PrGpuFormat_Rgba16F:
            *out = PrPixelFormat_RgbaF16;
            return true;
        case PrGpuFormat_Rgba32F:
            *out = PrPixelFormat_RgbaF32;
            return true;
        case PrGpuFormat_R8:
            *out = PrPixelFormat_R8;
            return true;
        case PrGpuFormat_R32F:
            *out = PrPixelFormat_RF32;
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

PrStatus prGpuBufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    *out = PrGpuBuffer{};
    PR_ASSERT(desc->size != 0);
    if (device->isLostFn(device))
        return PrStatus_Lost;
    return device->bufferCreateFn(device, desc, out);
}

void prGpuBufferDestroy(PrGpuDevice* device, PrGpuBuffer buffer) {
    if (buffer.id != 0)
        device->bufferDestroyFn(device, buffer);
}

PrGpuBufferDesc prGpuBufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer) {
    PR_ASSERT(buffer.id != 0);
    return device->bufferDescFn(device, buffer);
}

uint32_t prGpuBufferStorageHandle(const PrGpuDevice* device, PrGpuBuffer buffer) {
    PR_ASSERT(buffer.id != 0);
    PR_ASSERT(device->bufferDescFn(device, buffer).usage & PrGpuBufferUsageFlag_Storage);
    return device->bufferStorageHandleFn(device, buffer);
}

void* prGpuBufferMap(const PrGpuDevice* device, PrGpuBuffer buffer) {
    PR_ASSERT(buffer.id != 0);
    PR_ASSERT(device->bufferDescFn(device, buffer).memory != PrGpuMemory_Device);
    return device->bufferMapFn(device, buffer);
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

PrGpuTextureDesc prGpuTextureDesc2d(PrGpuFormat format, PrUVec2 extent, PrGpuTextureUsageFlags usage) {
    return PrGpuTextureDesc{
        .format = format,
        .usage = usage,
        .levels = 1,
        .samples = 1,
        .extent = {extent.x, extent.y, 1},
    };
}

uint8_t prGpuMipLevels(PrUVec2 extent) {
    uint32_t side = extent.x > extent.y ? extent.x : extent.y;
    uint8_t levels = 1;
    while (side > 1) {
        side >>= 1;
        ++levels;
    }
    return levels;
}

bool prGpuDeviceSupportsFormat(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    return format != PrGpuFormat_None && format < PrGpuFormat_Count && device->supportsFormatFn(device, format, usage);
}

PrStatus prGpuTextureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    *out = PrGpuTexture{};
    PR_ASSERT(desc->format != PrGpuFormat_None && desc->format < PrGpuFormat_Count);
    PR_ASSERT(desc->levels >= 1 && desc->samples >= 1);
    PR_ASSERT(desc->extent.width != 0 && desc->extent.height != 0 && desc->extent.depth != 0);
    if (device->isLostFn(device))
        return PrStatus_Lost;
    if (desc->kind != PrGpuTextureKind_3d && (desc->extent.width > device->infoFn(device).maxTextureSize ||
            desc->extent.height > device->infoFn(device).maxTextureSize))
        return PrStatus_Limit;
    return device->textureCreateFn(device, desc, out);
}

PrStatus prGpuTextureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out) {
    *out = PrGpuTexture{};
    PR_ASSERT(texture.id != 0);
    if (device->isLostFn(device))
        return PrStatus_Lost;
    return device->textureCreateViewFn(device, texture, kind, subresources, out);
}

void prGpuTextureDestroy(PrGpuDevice* device, PrGpuTexture texture) {
    if (texture.id == 0)
        return;
    device->textureDestroyFn(device, texture);
}

PrGpuTextureDesc prGpuTextureDesc(const PrGpuDevice* device, PrGpuTexture texture) {
    PR_ASSERT(texture.id != 0);
    return device->textureDescFn(device, texture);
}

PrUVec2 prGpuTextureExtent(const PrGpuDevice* device, PrGpuTexture texture) {
    PrGpuTextureDesc desc = prGpuTextureDesc(device, texture);
    return PrUVec2{desc.extent.width, desc.extent.height};
}

uint32_t prGpuTextureStorageHandle(const PrGpuDevice* device, PrGpuTexture texture) {
    PR_ASSERT(texture.id != 0);
    PR_ASSERT(device->textureDescFn(device, texture).usage & PrGpuTextureUsageFlag_Storage);
    return device->textureStorageHandleFn(device, texture);
}

PrStatus prGpuTextureCreateOverPixels(PrGpuDevice* device, PrGpuFormat format, PrPixels pixels,
        PrGpuTextureUsageFlags usage, PrGpuTexture* out) {
    *out = PrGpuTexture{};
    PR_ASSERT(prGpuDeviceHasNative(device, PrGpuNativeKind_Pixels));
    PR_ASSERT(pixels.extent.x != 0 && pixels.extent.y != 0);
    if (device->isLostFn(device))
        return PrStatus_Lost;
    return device->textureCreateOverPixelsFn(device, format, pixels, usage, out);
}

PrStatus prGpuTextureImport(PrGpuDevice* device, const PrGpuImport* image, PrGpuReleaseFn PR_NULLABLE release,
        void* PR_NULLABLE context, PrGpuTexture* texture) {
    PR_ASSERT(image->handle != nullptr);
    if (device->isLostFn(device))
        return PrStatus_Lost;
    return device->textureImportFn(device, image, release, context, texture);
}

bool prGpuImportLock(const PrGpuImport* image, PrGpuImportPixels* out) {
    *out = PrGpuImportPixels{};
    switch (image->kind) {
        case PrGpuImportKind_Pixels:
            *out = *static_cast<const PrGpuImportPixels*>(image->handle);
            return true;
#if defined(PR_GPU_HAS_COREVIDEO)
        case PrGpuImportKind_CoreVideo:
            return gpuCoreVideoLock(image->handle, out);
#endif
        default:
            return false;
    }
}

void prGpuImportUnlock(const PrGpuImport* image) {
#if defined(PR_GPU_HAS_COREVIDEO)
    if (image->kind == PrGpuImportKind_CoreVideo)
        gpuCoreVideoUnlock(image->handle);
#else
    (void)image;
#endif
}

bool prGpuTextureNative(PrGpuDevice* device, PrGpuTexture texture, PrGpuNativeKind kind, void* out) {
    PR_ASSERT(prGpuDeviceHasNative(device, kind));
    PR_ASSERT(texture.id != 0);
    return device->textureNativeFn(device, texture, kind, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Samplers

PrGpuSamplerDesc prGpuSamplerDescDefault(PrGpuFilter filter, PrGpuAddress address) {
    return PrGpuSamplerDesc{
        .magnify = filter,
        .minify = filter,
        .mip = filter,
        .address = {address, address, address},
        .maxAnisotropy = 1,
        .maxLod = 1000,
    };
}

PrStatus prGpuSamplerIntern(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out) {
    *out = PrGpuSampler{};
    return device->samplerInternFn(device, desc, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Uploads

void prGpuBufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size) {
    PR_ASSERT(buffer.id != 0);
    PR_ASSERT(offset + size <= device->bufferDescFn(device, buffer).size);
    if (size != 0 && !device->isLostFn(device))
        device->bufferUploadFn(device, buffer, offset, data, size);
}

PrGpuTextureRegion prGpuTextureRegion2d(PrUVec2 origin, PrUVec2 extent) {
    return PrGpuTextureRegion{.x = origin.x, .y = origin.y, .extent = {extent.x, extent.y, 1}};
}

void prGpuTextureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region,
        const void* data, uint32_t rowBytes, uint32_t layerBytes) {
    PR_ASSERT(texture.id != 0);
    if (region->extent.width == 0 || region->extent.height == 0 || region->extent.depth == 0)
        return;
    if (!device->isLostFn(device))
        device->textureUploadFn(device, texture, region, data, rowBytes, layerBytes);
}

void prGpuTextureWrite(PrGpuDevice* device, PrGpuTexture texture, PrIVec2 origin, PrPixelView texels) {
    PrGpuTextureDesc desc = prGpuTextureDesc(device, texture);
    PrUVec2 at{};
    PrUVec2 size{};
    PrUVec2 from{};
    if (!clip(origin, texels.extent, PrUVec2{desc.extent.width, desc.extent.height}, &at, &size, &from))
        return;
    uint32_t bytes = kFormatBlocks[desc.format].bytes;
    PrGpuTextureRegion region = prGpuTextureRegion2d(at, size);
    const uint8_t* first = texels.data + from.y * texels.stride + size_t(from.x) * bytes;
    prGpuTextureUpload(device, texture, &region, first, uint32_t(texels.stride), uint32_t(texels.stride * size.y));
}

void prGpuTextureRead(PrGpuDevice* device, PrGpuTexture texture, uint32_t access, PrIVec2 origin, PrPixels out) {
    PrGpuTextureDesc desc = prGpuTextureDesc(device, texture);
    uint32_t bytes = kFormatBlocks[desc.format].bytes;
    for (uint32_t y = 0; y < out.extent.y; ++y)
        prMemZero(out.data + y * out.stride, size_t(out.extent.x) * bytes);
    PrUVec2 at{};
    PrUVec2 size{};
    PrUVec2 to{};
    if (!clip(origin, out.extent, PrUVec2{desc.extent.width, desc.extent.height}, &at, &size, &to))
        return;
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(device, nullptr, &commands))
        return;
    uint32_t rowBytes = alignUp(size.x * bytes, kRowAlignment);
    PrGpuBuffer buffer = recordRead(commands, texture, access, at, size, rowBytes);
    prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
    if (buffer.id == 0)
        return;
    if (!device->isLostFn(device)) {
        const uint8_t* from = static_cast<const uint8_t*>(prGpuBufferMap(device, buffer));
        for (uint32_t y = 0; y < size.y; ++y)
            prMemCopy(out.data + (to.y + y) * out.stride + size_t(to.x) * bytes, from + size_t(y) * rowBytes,
                size_t(size.x) * bytes);
    }
    prGpuBufferDestroy(device, buffer);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shaders and pipelines

PrStatus prGpuShaderLibraryCreate(PrGpuDevice* device, const void* bytes, uint64_t size,
        PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const uint8_t* b = static_cast<const uint8_t*>(bytes);
    constexpr uint32_t kMagic = 0x4C535250;
    constexpr uint32_t kVersion = 2;
    if (size < 16 || readWord(b, 0) != kMagic || readWord(b, 4) != kVersion)
        return PrStatus_InvalidData;
    uint32_t entryCount = readWord(b, 8);
    uint32_t targetCount = readWord(b, 12);
    uint64_t targetsAt = 16 + uint64_t(entryCount) * 24;
    uint64_t targetSize = 4 + uint64_t(entryCount) * 8;
    if (targetsAt + targetSize * targetCount > size)
        return PrStatus_InvalidData;
    uint64_t codesAt = 0;
    for (uint32_t t = 0; t < targetCount && codesAt == 0; ++t) {
        if (readWord(b, targetsAt + targetSize * t) == device->shaderTargetFn(device))
            codesAt = targetsAt + targetSize * t + 4;
    }
    if (codesAt == 0)
        return PrStatus_NotSupported;
    if (device->isLostFn(device))
        return PrStatus_Lost;
    PrGpuShaderEntry* entries = static_cast<PrGpuShaderEntry*>(prMemoryAlloc(sizeof(PrGpuShaderEntry) * entryCount));
    for (uint32_t i = 0; i < entryCount; ++i) {
        uint64_t at = 16 + uint64_t(i) * 24;
        uint64_t nameAt = readWord(b, at);
        uint64_t nameSize = readWord(b, at + 4);
        uint64_t codeAt = readWord(b, codesAt + uint64_t(i) * 8);
        uint64_t codeSize = readWord(b, codesAt + uint64_t(i) * 8 + 4);
        uint32_t stage = readWord(b, at + 8);
        if (nameAt + nameSize > size || codeAt + codeSize > size || stage >= PrGpuShaderStage_Count) {
            prMemoryFree(entries);
            return PrStatus_InvalidData;
        }
        entries[i] = PrGpuShaderEntry{
            .name = {reinterpret_cast<const char*>(b + nameAt), size_t(nameSize)},
            .stage = PrGpuShaderStage(stage),
            .groupSize = {readWord(b, at + 12), readWord(b, at + 16), readWord(b, at + 20)},
            .code = b + codeAt,
            .codeSize = codeSize,
        };
    }
    PrGpuShaderLibrary* library = nullptr;
    PrStatus status = device->shaderLibraryCreateFn(device, entries, entryCount, &library);
    if (status != PrStatus_Ok) {
        prMemoryFree(entries);
        return status;
    }
    *out = library;
    return PrStatus_Ok;
}

void prGpuShaderLibraryDestroy(PrGpuShaderLibrary* PR_NULLABLE library) {
    if (library == nullptr)
        return;
    library->destroyFn(library);
}

PrGpuBlend prGpuBlendPremultiplied(void) {
    return PrGpuBlend{
        .enabled = true,
        .colorSource = PrGpuBlendFactor_One,
        .colorTarget = PrGpuBlendFactor_OneMinusSourceAlpha,
        .colorOp = PrGpuBlendOp_Add,
        .alphaSource = PrGpuBlendFactor_One,
        .alphaTarget = PrGpuBlendFactor_OneMinusSourceAlpha,
        .alphaOp = PrGpuBlendOp_Add,
        .write = PrGpuColorWriteFlag_All,
    };
}

PrGpuGraphicsPipelineDesc prGpuGraphicsPipelineDescDefault(const PrGpuShaderLibrary* library) {
    PrGpuGraphicsPipelineDesc desc{};
    desc.library = library;
    desc.depthCompare = PrGpuCompare_Always;
    desc.stencilReadMask = 0xff;
    desc.stencilWriteMask = 0xff;
    desc.stencilFront.compare = PrGpuCompare_Always;
    desc.stencilBack.compare = PrGpuCompare_Always;
    desc.formats.samples = 1;
    for (PrGpuBlend& blend : desc.blend)
        blend.write = PrGpuColorWriteFlag_All;
    return desc;
}

PrStatus prGpuPipelineCreateGraphics(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc, PrGpuPipeline* out) {
    *out = PrGpuPipeline{};
    PR_ASSERT(desc->formats.colorCount <= kPrGpuMaxColorAttachments && desc->formats.samples >= 1);
    const PrGpuShaderEntry* vertex = entryNamed(desc->library, desc->vertex);
    const PrGpuShaderEntry* fragment = desc->fragment.size != 0 ? entryNamed(desc->library, desc->fragment) : nullptr;
    if (vertex == nullptr || (desc->fragment.size != 0 && fragment == nullptr))
        return PrStatus_NotFound;
    if (vertex->stage != PrGpuShaderStage_Vertex ||
            (fragment != nullptr && fragment->stage != PrGpuShaderStage_Fragment))
        return PrStatus_WrongType;
    if (desc->readsTarget && (device->infoFn(device).features & PrGpuFeatureFlag_TargetReads) == 0)
        return PrStatus_NotSupported;
    if (device->isLostFn(device))
        return PrStatus_Lost;
    return device->graphicsPipelineCreateFn(device, desc, vertex, fragment, out);
}

PrStatus prGpuPipelineCreateCompute(PrGpuDevice* device, const PrGpuShaderLibrary* library, PrStringView compute,
        PrStringView name, PrGpuPipeline* out) {
    *out = PrGpuPipeline{};
    const PrGpuShaderEntry* entry = entryNamed(library, compute);
    if (entry == nullptr)
        return PrStatus_NotFound;
    if (entry->stage != PrGpuShaderStage_Compute)
        return PrStatus_WrongType;
    if (device->isLostFn(device))
        return PrStatus_Lost;
    return device->computePipelineCreateFn(device, library, entry, name, out);
}

void prGpuPipelineDestroy(PrGpuDevice* device, PrGpuPipeline pipeline) {
    if (pipeline.id != 0)
        device->pipelineDestroyFn(device, pipeline);
}

PrGpuExtent prGpuPipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline) {
    PR_ASSERT(pipeline.id != 0);
    return device->pipelineGroupSizeFn(device, pipeline);
}

// ---------------------------------------------------------------------------------------------------------------------
// Command buffers

PrGpuCommandsOptions prGpuCommandsOptionsDefault(void) {
    return PrGpuCommandsOptions{};
}

bool prGpuCommandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* PR_NULLABLE options,
        PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (device->isLostFn(device))
        return false;
    PrGpuCommandsOptions o = options != nullptr ? *options : prGpuCommandsOptionsDefault();
    return device->commandsBeginFn(device, &o, out);
}

PrGpuDevice* prGpuCommandsDevice(const PrGpuCommands* commands) {
    return commands->deviceFn(commands);
}

PrGpuNativeStateFlags prGpuNativeStateSync(PrGpuCommands* commands) {
    return commands->nativeStateSyncFn(commands);
}

PrGpuFence prGpuCommandsSubmit(PrGpuCommands* commands) {
    return commands->submitFn(commands);
}

double prGpuCommandsSubmitAndWait(PrGpuCommands* commands) {
    PrGpuDevice* device = prGpuCommandsDevice(commands);
    double seconds = 0;
    prGpuCommandsOnDone(commands, submittedDone, &seconds);
    prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
    return seconds;
}

void* prGpuCommandsStream(PrGpuCommands* commands, uint64_t size, PrGpuBuffer* buffer, uint64_t* offset) {
    return commands->streamFn(commands, size, buffer, offset);
}

void prGpuCommandsSetPush(PrGpuCommands* commands, const void* data, uint32_t size) {
    PR_ASSERT(size <= kPrGpuMaxPushBytes);
    commands->setPushFn(commands, data, size);
}

void prGpuCommandsSetUniforms(PrGpuCommands* commands, const void* data, uint32_t size) {
    PR_ASSERT(size <= kPrGpuMaxUniformBytes);
    commands->setUniformsFn(commands, data, size);
}

void prGpuCommandsSetPipeline(PrGpuCommands* commands, PrGpuPipeline pipeline) {
    PR_ASSERT(pipeline.id != 0);
    commands->setPipelineFn(commands, pipeline);
}

void prGpuCommandsSetBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    PR_ASSERT(slot < prGpuDeviceInfo(prGpuCommandsDevice(commands)).maxBufferSlots);
    PR_ASSERT(offset % kPrGpuSlotOffsetAlignment == 0);
    PR_ASSERT(buffer.id == 0 || offset < prGpuBufferDesc(prGpuCommandsDevice(commands), buffer).size);
    commands->setBufferFn(commands, slot, buffer, offset);
}

void prGpuCommandsSetRwBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    PR_ASSERT(slot < prGpuDeviceInfo(prGpuCommandsDevice(commands)).maxRwBufferSlots);
    PR_ASSERT(offset % kPrGpuSlotOffsetAlignment == 0);
    PR_ASSERT(buffer.id == 0 ||
        (prGpuBufferDesc(prGpuCommandsDevice(commands), buffer).usage & PrGpuBufferUsageFlag_Storage) != 0);
    PR_ASSERT(buffer.id == 0 || offset < prGpuBufferDesc(prGpuCommandsDevice(commands), buffer).size);
    commands->setRwBufferFn(commands, slot, buffer, offset);
}

void prGpuCommandsSetTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture, PrGpuSampler sampler) {
    PR_ASSERT(slot < prGpuDeviceInfo(prGpuCommandsDevice(commands)).maxTextureSlots);
    PR_ASSERT(texture.id == 0 ||
        (prGpuTextureDesc(prGpuCommandsDevice(commands), texture).usage & PrGpuTextureUsageFlag_Sampled) != 0);
    commands->setTextureFn(commands, slot, texture, sampler);
}

void prGpuCommandsSetRwTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture) {
    PR_ASSERT(slot < prGpuDeviceInfo(prGpuCommandsDevice(commands)).maxRwTextureSlots);
    PR_ASSERT(texture.id == 0 ||
        (prGpuTextureDesc(prGpuCommandsDevice(commands), texture).usage & PrGpuTextureUsageFlag_Storage) != 0);
    commands->setRwTextureFn(commands, slot, texture);
}

void prGpuCommandsDispatch(PrGpuCommands* commands, PrGpuExtent groups) {
    if (groups.width != 0 && groups.height != 0 && groups.depth != 0)
        commands->dispatchFn(commands, groups);
}

void prGpuCommandsDispatchIndirect(PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    PR_ASSERT(buffer.id != 0);
    commands->dispatchIndirectFn(commands, buffer, offset);
}

void prGpuCommandsCopyBuffer(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset, PrGpuBuffer target,
        uint64_t targetOffset, uint64_t size) {
    PR_ASSERT(source.id != 0 && target.id != 0);
    if (size != 0)
        commands->copyBufferFn(commands, source, sourceOffset, target, targetOffset, size);
}

void prGpuCommandsCopyTexture(PrGpuCommands* commands, PrGpuTexture source, const PrGpuTextureRegion* sourceRegion,
        PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    PR_ASSERT(source.id != 0 && target.id != 0);
    commands->copyTextureFn(commands, source, sourceRegion, target, targetRegion);
}

void prGpuCommandsCopyBufferToTexture(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    PR_ASSERT(source.id != 0 && target.id != 0);
    commands->copyBufferToTextureFn(commands, source, sourceOffset, rowBytes, layerBytes, target, targetRegion);
}

void prGpuCommandsCopyTextureToBuffer(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes) {
    PR_ASSERT(source.id != 0 && target.id != 0);
    commands->copyTextureToBufferFn(commands, source, sourceRegion, target, targetOffset, rowBytes, layerBytes);
}

void prGpuCommandsGenerateMipmaps(PrGpuCommands* commands, PrGpuTexture texture, uint32_t before) {
    PrGpuTextureDesc desc = prGpuTextureDesc(prGpuCommandsDevice(commands), texture);
    PR_ASSERT(desc.kind == PrGpuTextureKind_2d);
    PR_ASSERT((desc.usage & PrGpuTextureUsageFlag_Sampled) && (desc.usage & PrGpuTextureUsageFlag_RenderTarget));
    if (desc.levels > 1) {
        commands->generateMipmapsFn(commands, texture, before);
    } else if (before != PrGpuAccessFlag_ShaderRead) {
        prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{.levels = 1, .layers = 1}, before,
            PrGpuAccessFlag_ShaderRead);
    }
}

void prGpuCommandsOnDone(PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context) {
    commands->onDoneFn(commands, done, context);
}

void prGpuCommandsReadTexture(PrGpuCommands* commands, PrGpuTexture texture, uint32_t access, PrGpuReadFn read,
        void* PR_NULLABLE context) {
    PrGpuDevice* device = prGpuCommandsDevice(commands);
    PrGpuTextureDesc desc = prGpuTextureDesc(device, texture);
    PrUVec2 extent{desc.extent.width, desc.extent.height};
    uint32_t rowBytes = alignUp(extent.x * kFormatBlocks[desc.format].bytes, kRowAlignment);
    PrGpuBuffer buffer = recordRead(commands, texture, access, PrUVec2{0, 0}, extent, rowBytes);
    if (buffer.id == 0) {
        read(context, PrPixelView{});
        return;
    }
    GpuTextureRead* r = alloc<GpuTextureRead>();
    *r = GpuTextureRead{device, buffer, rowBytes, extent, desc.format, read, context};
    prGpuCommandsOnDone(commands, textureReadDone, r);
}

void prGpuCommandsPushGroup(PrGpuCommands* commands, PrStringView name) {
    commands->pushGroupFn(commands, name);
}

void prGpuCommandsPopGroup(PrGpuCommands* commands) {
    commands->popGroupFn(commands);
}

// ---------------------------------------------------------------------------------------------------------------------
// Barriers

void prGpuCommandsBufferBarrier(PrGpuCommands* commands, PrGpuBuffer buffer, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    PR_ASSERT(buffer.id != 0);
    commands->bufferBarrierFn(commands, buffer, before, after);
}

void prGpuCommandsTextureBarrier(PrGpuCommands* commands, PrGpuTexture texture, PrGpuSubresources subresources,
        PrGpuAccessFlags before, PrGpuAccessFlags after) {
    PR_ASSERT(texture.id != 0);
    commands->textureBarrierFn(commands, texture, subresources, before, after);
}

void prGpuCommandsGlobalBarrier(PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    commands->globalBarrierFn(commands, before, after);
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes

PrGpuPassDesc prGpuPassDescColor(PrGpuTexture texture, PrGpuLoad load, PrColor clear) {
    PrGpuPassDesc desc{};
    desc.colorCount = 1;
    desc.color[0] = PrGpuAttachment{.texture = texture, .load = load, .store = PrGpuStore_Keep};
    desc.clearColor[0] = clear;
    return desc;
}

PrGpuPass* prGpuPassBegin(PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    PR_ASSERT(desc->native != nullptr || desc->colorCount != 0 || desc->depthStencil.texture.id != 0);
    return commands->passBeginFn(commands, desc);
}

void prGpuPassEnd(PrGpuPass* pass) {
    pass->endFn(pass);
}

void prGpuPassNativeDraw(PrGpuPass* pass, PrGpuNativeKind kind, PrGpuNativeDrawFn draw, void* PR_NULLABLE context) {
    PR_ASSERT(prGpuDeviceHasNative(prGpuCommandsDevice(prGpuPassCommands(pass)), kind));
    PR_ASSERT(pass->formatsFn(pass).colorCount != 0);
    pass->nativeDrawFn(pass, kind, draw, context);
}

PrGpuCommands* prGpuPassCommands(const PrGpuPass* pass) {
    return pass->commandsFn(pass);
}

PrGpuAttachmentFormats prGpuPassFormats(const PrGpuPass* pass) {
    return pass->formatsFn(pass);
}

PrUVec2 prGpuPassExtent(const PrGpuPass* pass) {
    return pass->extentFn(pass);
}

void prGpuPassSetViewport(PrGpuPass* pass, PrRect viewport, float minDepth, float maxDepth) {
    pass->setViewportFn(pass, viewport, minDepth, maxDepth);
}

void prGpuPassSetScissor(PrGpuPass* pass, PrIRect scissor) {
    pass->setScissorFn(pass, scissor);
}

void prGpuPassSetBlendConstant(PrGpuPass* pass, PrColor constant) {
    pass->setBlendConstantFn(pass, constant);
}

void prGpuPassSetStencilReference(PrGpuPass* pass, uint8_t reference) {
    pass->setStencilReferenceFn(pass, reference);
}

void prGpuPassSetIndexBuffer(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, bool wide) {
    PR_ASSERT(buffer.id != 0);
    pass->setIndexBufferFn(pass, buffer, offset, wide);
}

void prGpuPassDraw(PrGpuPass* pass, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
        uint32_t firstInstance) {
    if (vertexCount != 0 && instanceCount != 0)
        pass->drawFn(pass, vertexCount, instanceCount, firstVertex, firstInstance);
}

void prGpuPassDrawIndexed(PrGpuPass* pass, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
        int32_t vertexOffset, uint32_t firstInstance) {
    if (indexCount != 0 && instanceCount != 0)
        pass->drawIndexedFn(pass, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void prGpuPassDrawIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    PR_ASSERT(buffer.id != 0);
    if (count != 0)
        pass->drawIndirectFn(pass, buffer, offset, count, stride);
}

void prGpuPassDrawIndexedIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    PR_ASSERT(buffer.id != 0);
    if (count != 0)
        pass->drawIndexedIndirectFn(pass, buffer, offset, count, stride);
}

// ---------------------------------------------------------------------------------------------------------------------
// Swap chains

PrStatus prGpuSwapChainCreate(PrGpuDevice* device, const PrAppSurface* surface, const PrGpuSwapChainDesc* desc,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (device->isLostFn(device))
        return PrStatus_Lost;
    return device->swapChainCreateFn(device, surface, desc, out);
}

void prGpuSwapChainDestroy(PrGpuSwapChain* PR_NULLABLE swapChain) {
    if (swapChain != nullptr)
        swapChain->destroyFn(swapChain);
}

PrGpuSwapChainInfo prGpuSwapChainInfo(const PrGpuSwapChain* swapChain) {
    return swapChain->infoFn(swapChain);
}

void prGpuSwapChainWait(PrGpuSwapChain* swapChain) {
    swapChain->waitFn(swapChain);
}

PrStatus prGpuSwapChainAcquire(PrGpuSwapChain* swapChain, PrUVec2 extent, PrGpuTexture* out) {
    *out = PrGpuTexture{};
    PrGpuDevice* device = swapChain->deviceFn(swapChain);
    if (device->isLostFn(device))
        return PrStatus_Lost;
    if (extent.x == 0 || extent.y == 0)
        return PrStatus_InvalidState;
    if (extent.x > device->infoFn(device).maxTextureSize || extent.y > device->infoFn(device).maxTextureSize)
        return PrStatus_Limit;
    return swapChain->acquireFn(swapChain, extent, out);
}

void prGpuSwapChainPresent(PrGpuSwapChain* swapChain, PrGpuCommands* commands) {
    PR_ASSERT(prGpuCommandsDevice(commands) == swapChain->deviceFn(swapChain));
    swapChain->presentFn(swapChain, commands);
}
