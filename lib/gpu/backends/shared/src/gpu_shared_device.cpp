// What every GPU backend's objects hold after their functions, and the functions that the backends share.

#include "gpu/backends/shared/src/gpu_shared_device.h"

#include "pr/alloc.h"

#include "gpu/backends/shared/src/kGpuSharedMipmapShaders.h"

namespace pr_impl {

namespace {

GpuSharedDevice* sharedOf(PrGpuDevice* device) {
    return static_cast<GpuSharedDevice*>(device);
}

const GpuSharedDevice* sharedOf(const PrGpuDevice* device) {
    return static_cast<const GpuSharedDevice*>(device);
}

PrGpuDeviceInfo deviceInfo(const PrGpuDevice* device) {
    return sharedOf(device)->info;
}

PrGpuShaderTarget deviceShaderTarget(const PrGpuDevice* device) {
    return sharedOf(device)->shaderTarget;
}

uint32_t deviceNativeKinds(const PrGpuDevice* device) {
    return sharedOf(device)->nativeKinds;
}

uint32_t deviceInPlaceImports(const PrGpuDevice* device) {
    return sharedOf(device)->inPlaceImports;
}

bool sameSampler(const PrGpuSamplerDesc& a, const PrGpuSamplerDesc& b) {
    return a.magnify == b.magnify && a.minify == b.minify && a.mip == b.mip && a.address[0] == b.address[0] &&
        a.address[1] == b.address[1] && a.address[2] == b.address[2] && a.maxAnisotropy == b.maxAnisotropy &&
        a.compare == b.compare && a.minLod == b.minLod && a.maxLod == b.maxLod && a.lodBias == b.lodBias;
}

// Returns the sampler for the description, made on its first use.
PrStatus samplerIntern(PrGpuDevice* base, const PrGpuSamplerDesc* desc, PrGpuSampler* out) {
    GpuSharedDevice* device = sharedOf(base);
    prMutexLock(&device->mutex);
    for (uint32_t i = 0; i < device->samplers.count; ++i) {
        if (sameSampler(device->samplers.data[i].desc, *desc)) {
            *out = device->samplers.data[i].sampler;
            prMutexUnlock(&device->mutex);
            return PrStatus_Ok;
        }
    }
    PrStatus status = device->isLostFn(device) ? PrStatus_Lost : device->samplerCreate(device, desc, out);
    if (status == PrStatus_Ok)
        device->samplers.push(GpuSharedSampler{*desc, *out});
    prMutexUnlock(&device->mutex);
    return status;
}

// Returns true if an import made `texture` by copying, so that a later import may write into it.
bool isImportCopy(GpuSharedDevice* device, PrGpuTexture texture) {
    if (texture.id == 0)
        return false;
    prMutexLock(&device->mutex);
    bool found = false;
    for (uint32_t id : device->importCopies.span())
        found |= id == texture.id;
    prMutexUnlock(&device->mutex);
    return found;
}

// Makes `*texture` a texture that shaders sample, holding a copy of texels in memory. The texture that `*texture`
// names, if any, is written into if an import made it by copying and it has the texels' extent and format, and is
// replaced otherwise.
PrStatus importCopy(GpuSharedDevice* device, const PrGpuImportPixels& pixels, PrGpuTexture* texture) {
    if (isImportCopy(device, *texture)) {
        PrGpuTextureDesc held = device->textureDescFn(device, *texture);
        if (held.format == pixels.format && held.extent.width == pixels.pixels.extent.x &&
                held.extent.height == pixels.pixels.extent.y) {
            prGpuTextureWrite(device, *texture, PrIVec2{0, 0}, pixels.pixels);
            return PrStatus_Ok;
        }
    }
    PrGpuTextureDesc desc = prGpuTextureDesc2d(pixels.format, pixels.pixels.extent, PrGpuTextureUsageFlag_Sampled);
    PrGpuTexture made{};
    PrStatus status = device->textureCreateFn(device, &desc, &made);
    if (status != PrStatus_Ok)
        return status;
    prGpuTextureWrite(device, made, PrIVec2{0, 0}, pixels.pixels);
    prMutexLock(&device->mutex);
    device->importCopies.push(made.id);
    prMutexUnlock(&device->mutex);
    prGpuTextureDestroy(device, *texture);
    *texture = made;
    return PrStatus_Ok;
}

// Imports an image in place if the device imports its kind in place, and otherwise copies its texels.
PrStatus textureImport(PrGpuDevice* base, const PrGpuImport* image, PrGpuReleaseFn PR_NULLABLE release,
        void* PR_NULLABLE context, PrGpuTexture* texture) {
    GpuSharedDevice* device = sharedOf(base);
    if (image->kind < PrGpuImportKind_Count && (device->inPlaceImports & (1u << image->kind)) != 0) {
        PrGpuTexture made{};
        PrStatus status = device->textureImportInPlace(device, image, release, context, &made);
        if (status == PrStatus_Ok) {
            prGpuTextureDestroy(device, *texture);
            *texture = made;
        }
        return status;
    }
    PrGpuImportPixels pixels{};
    if (!prGpuImportLock(image, &pixels))
        return PrStatus_NotSupported;
    PrStatus status = importCopy(device, pixels, texture);
    prGpuImportUnlock(image);
    if (status == PrStatus_Ok && release != nullptr)
        release(context);
    return status;
}

// Returns the pipeline that draws mip levels of `format`, made on first use, or zero if the device cannot make it.
PrGpuPipeline mipmapPipeline(GpuSharedDevice* device, PrGpuFormat format) {
    prMutexLock(&device->mutex);
    PrGpuPipeline pipeline{};
    for (uint32_t i = 0; i < device->mipmapPipelines.count; ++i) {
        if (device->mipmapPipelines.data[i].format == format)
            pipeline = device->mipmapPipelines.data[i].pipeline;
    }
    if (pipeline.id == 0) {
        if (device->mipmapLibrary == nullptr) {
            prGpuShaderLibraryCreate(device, kGpuSharedMipmapShaders, sizeof(kGpuSharedMipmapShaders),
                &device->mipmapLibrary);
        }
        if (device->mipmapLibrary != nullptr) {
            PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(device->mipmapLibrary);
            desc.vertex = "mipVertex";
            desc.fragment = "mipFragment";
            desc.formats.colorCount = 1;
            desc.formats.color[0] = format;
            desc.name = "mipmaps";
            if (prGpuPipelineCreateGraphics(device, &desc, &pipeline) == PrStatus_Ok)
                device->mipmapPipelines.push(GpuSharedMipmapPipeline{format, pipeline});
        }
    }
    prMutexUnlock(&device->mutex);
    return pipeline;
}

} // namespace

void gpuSharedDeviceInit(GpuSharedDevice* device) {
    device->infoFn = deviceInfo;
    device->shaderTargetFn = deviceShaderTarget;
    device->nativeKindsFn = deviceNativeKinds;
    device->inPlaceImportsFn = deviceInPlaceImports;
    device->samplerInternFn = samplerIntern;
    device->textureImportFn = textureImport;
    prMutexInit(&device->mutex);
}

void gpuSharedDeviceReset(GpuSharedDevice* device) {
    for (uint32_t i = 0; i < device->mipmapPipelines.count; ++i)
        device->pipelineDestroyFn(device, device->mipmapPipelines.data[i].pipeline);
    device->mipmapPipelines.reset();
    prGpuShaderLibraryDestroy(device->mipmapLibrary);
    device->mipmapLibrary = nullptr;
    device->samplers.reset();
    device->importCopies.reset();
    prMutexReset(&device->mutex);
}

void gpuSharedTextureForget(GpuSharedDevice* device, PrGpuTexture texture) {
    prMutexLock(&device->mutex);
    device->importCopies.removeFirst(texture.id);
    prMutexUnlock(&device->mutex);
}

PrGpuDevice* gpuSharedCommandsDevice(const PrGpuCommands* commands) {
    return static_cast<const GpuSharedCommands*>(commands)->device;
}

PrGpuCommands* gpuSharedPassCommands(const PrGpuPass* pass) {
    return static_cast<const GpuSharedPass*>(pass)->commands;
}

PrGpuAttachmentFormats gpuSharedPassFormats(const PrGpuPass* pass) {
    return static_cast<const GpuSharedPass*>(pass)->formats;
}

PrUVec2 gpuSharedPassExtent(const PrGpuPass* pass) {
    return static_cast<const GpuSharedPass*>(pass)->extent;
}

PrGpuDevice* gpuSharedSwapChainDevice(const PrGpuSwapChain* swapChain) {
    return static_cast<const GpuSharedSwapChain*>(swapChain)->device;
}

PrGpuSwapChainInfo gpuSharedSwapChainInfo(const PrGpuSwapChain* swapChain) {
    return static_cast<const GpuSharedSwapChain*>(swapChain)->info;
}

const PrGpuShaderEntry* gpuSharedLibraryEntries(const PrGpuShaderLibrary* library, uint32_t* count) {
    const GpuSharedLibrary* shared = static_cast<const GpuSharedLibrary*>(library);
    *count = shared->entryCount;
    return shared->entries;
}

void gpuSharedGenerateMipmaps(PrGpuCommands* commands, PrGpuTexture texture, PrGpuAccessFlags before) {
    GpuSharedDevice* device = sharedOf(static_cast<GpuSharedCommands*>(commands)->device);
    PrGpuTextureDesc desc = prGpuTextureDesc(device, texture);
    PrGpuPipeline pipeline = mipmapPipeline(device, desc.format);
    PrGpuSampler sampler{};
    PrGpuSamplerDesc linear = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Clamp);
    linear.mip = PrGpuFilter_Nearest;
    if (pipeline.id == 0 || prGpuSamplerIntern(device, &linear, &sampler) != PrStatus_Ok)
        return;
    PrGpuSubresources first{.levels = 1, .layers = 1};
    prGpuCommandsPushGroup(commands, "mipmaps");
    prGpuCommandsTextureBarrier(commands, texture, first, before, PrGpuAccessFlag_FragmentRead);
    prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{.firstLevel = 1, .layers = 1},
        PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    for (uint8_t level = 1; level < desc.levels; ++level) {
        PrGpuTexture from{};
        PrGpuTexture into{};
        prGpuTextureCreateView(device, texture, PrGpuTextureKind_2d,
            PrGpuSubresources{.firstLevel = uint8_t(level - 1), .levels = 1, .layers = 1}, &from);
        prGpuTextureCreateView(device, texture, PrGpuTextureKind_2d,
            PrGpuSubresources{.firstLevel = level, .levels = 1, .layers = 1}, &into);
        PrGpuPassDesc pass = prGpuPassDescColor(into, PrGpuLoad_Discard, PrColor{});
        pass.name = "mip level";
        PrGpuPass* p = prGpuPassBegin(commands, &pass);
        prGpuCommandsSetPipeline(commands, pipeline);
        uint32_t push[2] = {from.id, sampler.id};
        prGpuCommandsSetPush(commands, push, sizeof(push));
        prGpuPassDraw(p, 3, 1, 0, 0);
        prGpuPassEnd(p);
        prGpuTextureDestroy(device, from);
        prGpuTextureDestroy(device, into);
        prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{.firstLevel = level, .levels = 1, .layers = 1},
            PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_FragmentRead);
    }
    prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{.layers = 1}, PrGpuAccessFlag_FragmentRead,
        PrGpuAccessFlag_ShaderRead);
    prGpuCommandsPopGroup(commands);
}

} // namespace pr_impl
