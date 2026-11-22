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

PrGpuDeviceStage deviceStage(const PrGpuDevice* device) {
    return sharedOf(device)->stage.load();
}

PrStatus deviceLostStatus(const PrGpuDevice* base) {
    const GpuSharedDevice* device = sharedOf(base);
    switch (device->stage.load()) {
        case PrGpuDeviceStage_Ready:
            return device->removed(device) ? PrStatus_Lost : PrStatus_Ok;
        case PrGpuDeviceStage_Failed:
            return device->failure;
        default:
            return PrStatus_Ok;
    }
}

// Waits for the device's creation, including its ready callback, then frees it with the backend's `destroy`.
void deviceDestroy(PrGpuDevice* base) {
    GpuSharedDevice* device = sharedOf(base);
    // A creation that the system finishes later on this thread cannot be waited for, so it frees the device itself.
    if (device->initialize == nullptr && device->stage.load() == PrGpuDeviceStage_Creating) {
        device->abandoned = true;
        return;
    }
    prMutexLock(&device->stageMutex);
    while (!device->created)
        prConditionWait(&device->stageSignal, &device->stageMutex);
    prMutexUnlock(&device->stageMutex);
    prConditionReset(&device->stageSignal);
    prMutexReset(&device->stageMutex);
    device->destroy(device);
}

// Runs the backend's initialization, publishes the stage that it reached, and calls the ready callback.
void deviceInitialize(void* PR_NULLABLE context) {
    GpuSharedDevice* device = static_cast<GpuSharedDevice*>(context);
    gpuSharedDeviceFinish(device, device->initialize(device));
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
    PrStatus status = device->removed(device) ? PrStatus_Lost : device->samplerCreate(device, desc, out);
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
    prGpuTextureRelease(device, *texture);
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
            prGpuTextureRelease(device, *texture);
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

// A capture's function and context, and the format of the texels that the read gives it.
struct GpuSharedCapture {
    PrGpuCaptureFn capture;
    void* PR_NULLABLE context;
    PrGpuFormat format;
};

void captureRead(void* PR_NULLABLE context, PrPixelView texels) {
    GpuSharedCapture* c = static_cast<GpuSharedCapture*>(context);
    c->capture(c->context, texels, c->format);
    prMemoryFree(c);
}

} // namespace

void gpuSharedDeviceInit(GpuSharedDevice* device) {
    device->destroyFn = deviceDestroy;
    device->infoFn = deviceInfo;
    device->shaderTargetFn = deviceShaderTarget;
    device->nativeKindsFn = deviceNativeKinds;
    device->inPlaceImportsFn = deviceInPlaceImports;
    device->stageFn = deviceStage;
    device->lostStatusFn = deviceLostStatus;
    device->samplerInternFn = samplerIntern;
    device->textureImportFn = textureImport;
    device->stage.store(PrGpuDeviceStage_Creating);
    prMutexInit(&device->mutex);
    prMutexInit(&device->stageMutex);
    prConditionInit(&device->stageSignal);
}

void gpuSharedDeviceReset(GpuSharedDevice* device) {
    for (uint32_t i = 0; i < device->mipmapPipelines.count; ++i)
        device->pipelineReleaseFn(device, device->mipmapPipelines.data[i].pipeline);
    device->mipmapPipelines.reset();
    prGpuShaderLibraryDestroy(device->mipmapLibrary);
    device->mipmapLibrary = nullptr;
    device->samplers.reset();
    device->importCopies.reset();
    prMutexReset(&device->mutex);
}

PrGpuDeviceLimits gpuSharedDeviceLimits(PrGpuDeviceLimits asked) {
    const PrGpuDeviceLimits defaults = prGpuDeviceLimitsDefault();
    return PrGpuDeviceLimits{
        .resources = asked.resources != 0 ? asked.resources : defaults.resources,
        .samplers = asked.samplers != 0 ? asked.samplers : defaults.samplers,
        .pipelines = asked.pipelines != 0 ? asked.pipelines : defaults.pipelines,
    };
}

void gpuSharedDeviceStart(GpuSharedDevice* device, PrTaskPool* PR_NULLABLE pool, PrGpuDeviceReadyFn PR_NULLABLE ready,
        void* PR_NULLABLE readyContext) {
    PR_ASSERT(device->initialize != nullptr && device->removed != nullptr && device->destroy != nullptr);
    device->ready = ready;
    device->readyContext = readyContext;
    if (pool != nullptr)
        prTaskPoolPost(pool, deviceInitialize, device);
    else
        deviceInitialize(device);
}

void gpuSharedDeviceStartLater(GpuSharedDevice* device, PrGpuDeviceReadyFn PR_NULLABLE ready,
        void* PR_NULLABLE readyContext) {
    PR_ASSERT(device->removed != nullptr && device->destroy != nullptr);
    device->ready = ready;
    device->readyContext = readyContext;
}

void gpuSharedDeviceFinish(GpuSharedDevice* device, PrStatus status) {
    device->failure = status;
    device->stage.store(status == PrStatus_Ok ? PrGpuDeviceStage_Ready : PrGpuDeviceStage_Failed);
    if (device->abandoned) {
        device->created = true;
        if (status == PrStatus_Ok)
            device->waitIdleFn(device);
        deviceDestroy(device);
        return;
    }
    if (device->ready != nullptr)
        device->ready(device->readyContext, device);
    prMutexLock(&device->stageMutex);
    device->created = true;
    prConditionBroadcast(&device->stageSignal);
    prMutexUnlock(&device->stageMutex);
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
    const GpuSharedSwapChain* sc = static_cast<const GpuSharedSwapChain*>(swapChain);
    PrGpuSwapChainInfo info = sc->info;
    info.sync = sc->sync;
    return info;
}

void gpuSharedSwapChainRequestCapture(PrGpuSwapChain* swapChain, PrGpuCaptureFn capture, void* PR_NULLABLE context) {
    GpuSharedSwapChain* s = static_cast<GpuSharedSwapChain*>(swapChain);
    s->capture = capture;
    s->captureContext = context;
    s->captureRequested = true;
}

void gpuSharedSwapChainCancelCapture(PrGpuSwapChain* swapChain) {
    GpuSharedSwapChain* s = static_cast<GpuSharedSwapChain*>(swapChain);
    s->captureRequested = false;
    if (s->captureTexture.id == 0) {
        s->capture = nullptr;
        s->captureContext = nullptr;
    }
}

bool gpuSharedSwapChainAcquireCapture(GpuSharedSwapChain* swapChain, PrUVec2 extent, PrGpuTexture* out) {
    if (!swapChain->captureRequested)
        return false;
    swapChain->captureRequested = false;
    // The texture has the surface's format, so that every pipeline made for the swap chain draws into it.
    PrGpuTextureDesc desc = prGpuTextureDesc2d(swapChain->info.format, extent,
        PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
    desc.name = "swap chain capture";
    PrGpuTexture texture{};
    if (prGpuTextureCreate(swapChain->device, &desc, &texture) != PrStatus_Ok)
        return false;
    swapChain->captureTexture = texture;
    *out = texture;
    return true;
}

bool gpuSharedSwapChainPresentCapture(GpuSharedSwapChain* swapChain, PrGpuCommands* commands) {
    PrGpuTexture texture = swapChain->captureTexture;
    if (texture.id == 0)
        return false;
    GpuSharedCapture* c = allocZeroed<GpuSharedCapture>();
    *c = GpuSharedCapture{swapChain->capture, swapChain->captureContext, swapChain->info.format};
    // The frame leaves the texture as it leaves a texture of the surface, in the accesses of Present.
    prGpuCommandsReadTexture(commands, texture, PrGpuAccessFlag_Present, captureRead, c);
    prGpuTextureRelease(swapChain->device, texture);
    swapChain->captureTexture = PrGpuTexture{};
    if (!swapChain->captureRequested) {
        swapChain->capture = nullptr;
        swapChain->captureContext = nullptr;
    }
    return true;
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
        prGpuTextureCreateView(device, texture, PrGpuTextureViewDesc{
            .kind = PrGpuTextureKind_2d,
            .subresources = PrGpuSubresources{.firstLevel = uint8_t(level - 1), .levels = 1, .layers = 1},
        }, &from);
        prGpuTextureCreateView(device, texture, PrGpuTextureViewDesc{
            .kind = PrGpuTextureKind_2d,
            .subresources = PrGpuSubresources{.firstLevel = level, .levels = 1, .layers = 1},
        }, &into);
        PrGpuPassDesc pass = prGpuPassDescColor(into, PrGpuLoad_Discard, PrColor{});
        pass.name = "mip level";
        prGpuCommandsSetTexture(commands, 0, from, sampler);
        PrGpuPass* p = prGpuPassBegin(commands, &pass);
        prGpuCommandsSetPipeline(commands, pipeline);
        prGpuPassDraw(p, 3, 1, 0, 0);
        prGpuPassEnd(p);
        prGpuTextureRelease(device, from);
        prGpuTextureRelease(device, into);
        prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{.firstLevel = level, .levels = 1, .layers = 1},
            PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_FragmentRead);
    }
    prGpuCommandsSetTexture(commands, 0, PrGpuTexture{}, PrGpuSampler{});
    prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{.layers = 1}, PrGpuAccessFlag_FragmentRead,
        PrGpuAccessFlag_ShaderRead);
    prGpuCommandsPopGroup(commands);
}

} // namespace pr_impl
