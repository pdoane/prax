// What the Metal backends share. The header describes the model that they share.

#include "gpu/backends/shared/src/gpu_shared_metal.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/search.h"
#include "pr/thread.h"

namespace pr_impl {

namespace {

MTLTextureType textureType(PrGpuTextureKind kind, uint8_t samples) {
    switch (kind) {
        case PrGpuTextureKind_2dArray: return samples > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
        case PrGpuTextureKind_Cube: return MTLTextureTypeCube;
        case PrGpuTextureKind_CubeArray: return MTLTextureTypeCubeArray;
        case PrGpuTextureKind_3d: return MTLTextureType3D;
        default: return samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    }
}

MTLStencilOperation stencilOp(PrGpuStencilOp op) {
    switch (op) {
        case PrGpuStencilOp_Zero: return MTLStencilOperationZero;
        case PrGpuStencilOp_Replace: return MTLStencilOperationReplace;
        case PrGpuStencilOp_IncrementClamp: return MTLStencilOperationIncrementClamp;
        case PrGpuStencilOp_DecrementClamp: return MTLStencilOperationDecrementClamp;
        case PrGpuStencilOp_Invert: return MTLStencilOperationInvert;
        case PrGpuStencilOp_IncrementWrap: return MTLStencilOperationIncrementWrap;
        case PrGpuStencilOp_DecrementWrap: return MTLStencilOperationDecrementWrap;
        default: return MTLStencilOperationKeep;
    }
}

MTLSamplerAddressMode addressMode(PrGpuAddress address) {
    switch (address) {
        case PrGpuAddress_Repeat: return MTLSamplerAddressModeRepeat;
        case PrGpuAddress_Mirror: return MTLSamplerAddressModeMirrorRepeat;
        case PrGpuAddress_Border: return MTLSamplerAddressModeClampToZero;
        default: return MTLSamplerAddressModeClampToEdge;
    }
}

void setHeapEntry(id<MTLBuffer> heap, uint32_t slot, uint64_t value) {
    static_cast<uint64_t*>(heap.contents)[slot] = value;
}

// Frees a resource slot and what it holds. The caller holds the mutex.
void freeResource(GpuMetalDevice* d, uint32_t slot) {
    GpuMetalResource& r = d->resources[slot];
    if (r.object != nil) {
        [d->residency removeAllocation:r.object];
        d->residencyDirty = true;
        [r.object release];
    }
    r = GpuMetalResource{};
    setHeapEntry(d->resourceHeap, slot, 0);
    d->freeResources.push(slot);
}

void freePipeline(GpuMetalDevice* d, uint32_t id) {
    GpuMetalPipeline& p = d->pipelines[id];
    [p.state release];
    [p.depthStencil release];
    p = GpuMetalPipeline{};
    d->freePipelines.push(id);
}

// Returns a block of shared memory of at least `size` bytes, from the free blocks or new, or one without a buffer if
// the device has no memory or slot for it. A block larger than `blockSize` is made for its size. The caller holds the
// mutex.
GpuMetalBlock blockTake(GpuMetalDevice* d, Array<GpuMetalBlock>& free, uint64_t blockSize, uint64_t size) {
    for (uint32_t i = 0; i < free.count; ++i) {
        if (free.data[i].buffer.length >= size) {
            GpuMetalBlock block = free.data[i];
            free.erase(i);
            return block;
        }
    }
    uint64_t length = size > blockSize ? alignUp(size, kGpuMetalStreamAlignment) : blockSize;
    id<MTLBuffer> buffer = [d->device newBufferWithLength:length options:MTLResourceStorageModeShared |
        MTLResourceCPUCacheModeWriteCombined | MTLResourceHazardTrackingModeUntracked];
    if (buffer == nil)
        return GpuMetalBlock{};
    uint32_t slot = gpuMetalResourceAddLocked(d, GpuMetalSlot_Buffer, buffer, buffer.gpuAddress);
    [buffer release];
    if (slot == 0)
        return GpuMetalBlock{};
    d->resources[slot].buffer = PrGpuBufferDesc{.size = length, .memory = PrGpuMemory_Upload};
    return GpuMetalBlock{buffer, slot, 0};
}

// Returns room for `size` bytes in the batch's staging, as the block's index in the batch and the offset in it. Returns
// false if the device has no memory for it. The caller holds the mutex.
bool stage(GpuMetalDevice* d, uint64_t size, uint32_t* block, uint64_t* offset) {
    GpuMetalBlock* last = d->uploadBlocks.count != 0 ? &d->uploadBlocks.last() : nullptr;
    uint64_t at = last != nullptr ? alignUp(last->used, 256) : 0;
    if (last == nullptr || at + size > last->buffer.length) {
        if (d->uploadBlocks.count >= kGpuMetalStagingFlushBlocks)
            d->uploadsSubmit(d);
        gpuMetalCollect(d);
        GpuMetalBlock added = blockTake(d, d->stagingBlocks, kGpuMetalStagingBlock, size);
        if (added.buffer == nil)
            return false;
        d->uploadBlocks.push(added);
        last = &d->uploadBlocks.last();
        at = 0;
    }
    d->uploadsOpen(d);
    last->used = at + size;
    *block = d->uploadBlocks.count - 1;
    *offset = at;
    return true;
}

void bufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        prMutexLock(&d->mutex);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        while (size != 0) {
            uint64_t n = size < kGpuMetalStagingBlock ? size : kGpuMetalStagingBlock;
            uint32_t block = 0;
            uint64_t at = 0;
            if (!stage(d, n, &block, &at))
                break;
            memCopy(static_cast<uint8_t*>(d->uploadBlocks.data[block].buffer.contents) + at, bytes, n);
            gpuSharedUploadAddCopy(&d->uploadBatch, GpuSharedUploadCopy{buffer.id, block, offset, at, n});
            bytes += n;
            offset += n;
            size -= n;
        }
        prMutexUnlock(&d->mutex);
    }
}

void textureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region, const void* data,
        uint32_t rowBytes, uint32_t layerBytes) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        id<MTLTexture> t = gpuMetalTexture(d, texture.id);
        const PrGpuTextureDesc& desc = d->resources[texture.id].texture;
        PrGpuFormatBlock b = prGpuFormatBlock(desc.format);
        uint32_t rows = (region->extent.height + b.height - 1) / b.height;
        uint32_t row = (region->extent.width + b.width - 1) / b.width * b.bytes;
        uint32_t slices = region->extent.depth;
        uint64_t sliceBytes = uint64_t(row) * rows;
        prMutexLock(&d->mutex);
        // Staging holds the texels with rows packed. An upload larger than a block is staged in a block of its size.
        uint32_t block = 0;
        uint64_t at = 0;
        if (!stage(d, sliceBytes * slices, &block, &at)) {
            prMutexUnlock(&d->mutex);
            return;
        }
        id<MTLBuffer> staging = d->uploadBlocks.data[block].buffer;
        uint8_t* to = static_cast<uint8_t*>(staging.contents) + at;
        const uint8_t* from = static_cast<const uint8_t*>(data);
        for (uint32_t s = 0; s < slices; ++s) {
            for (uint32_t y = 0; y < rows; ++y)
                memCopy(to + s * sliceBytes + uint64_t(y) * row,
                    from + uint64_t(s) * layerBytes + uint64_t(y) * rowBytes, row);
        }
        d->uploadsCopyTexture(d, GpuMetalTextureCopy{
            .staging = staging,
            .at = at,
            .rowBytes = row,
            .sliceBytes = sliceBytes,
            .slices = slices,
            .texture = t,
            .region = region,
            .is3d = desc.kind == PrGpuTextureKind_3d,
            .barrier = gpuSharedUploadNeedsTextureBarrier(&d->uploadBatch, texture.id),
        });
        prMutexUnlock(&d->mutex);
    }
}

bool isLost(const PrGpuDevice* device) {
    return gpuMetalDeviceFrom(device)->lost.load();
}

// Apple silicon's GPU is never removed, so the device only reports that it is lost and refuses new work.
void simulateLoss(PrGpuDevice* device) {
    gpuMetalDeviceFrom(device)->lost.store(true);
}

bool isComplete(const PrGpuDevice* device, PrGpuFence fence) {
    const GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    return d->lost.load() || d->done.load() >= fence;
}

bool supportsFormat(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    const GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    if (gpuMetalPixelFormat(format) == MTLPixelFormatInvalid)
        return false;
    bool bc = format >= PrGpuFormat_Bc1 && format <= PrGpuFormat_Bc7Srgb;
    bool compressed = format >= PrGpuFormat_Bc1;
    if (bc && !d->device.supportsBCTextureCompression)
        return false;
    if (compressed && (usage & ~PrGpuTextureUsageFlag_Sampled))
        return false;
    bool depth = prGpuFormatIsDepthStencil(format);
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) && !depth)
        return false;
    if ((usage & (PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Storage)) && depth)
        return false;
    return true;
}

PrStatus bufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        MTLResourceOptions options = MTLResourceHazardTrackingModeUntracked;
        options |= desc->memory == PrGpuMemory_Device ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared;
        if (desc->memory == PrGpuMemory_Upload)
            options |= MTLResourceCPUCacheModeWriteCombined;
        // Metal zeroes a new buffer's memory.
        id<MTLBuffer> buffer = [d->device newBufferWithLength:alignUp(desc->size, 16) options:options];
        if (buffer == nil)
            return PrStatus_Limit;
        if (desc->name.size != 0)
            buffer.label = gpuMetalString(desc->name);
        uint32_t slot = gpuMetalResourceAdd(d, GpuMetalSlot_Buffer, buffer, buffer.gpuAddress);
        [buffer release];
        if (slot == 0)
            return PrStatus_Limit;
        d->resources[slot].buffer = *desc;
        d->resources[slot].buffer.name = PrStringView{};
        out->id = slot;
        return PrStatus_Ok;
    }
}

void bufferDestroy(PrGpuDevice* device, PrGpuBuffer buffer) {
    gpuMetalRetire(gpuMetalDeviceFrom(device), GpuMetalRetiredKind_Resource, buffer.id);
}

PrGpuBufferDesc bufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return gpuMetalDeviceFrom(device)->resources[buffer.id].buffer;
}

// Metal reaches a buffer by its address, for reading and writing alike.
uint32_t bufferStorageHandle(const PrGpuDevice*, PrGpuBuffer buffer) {
    return buffer.id;
}

void* bufferMap(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return gpuMetalBuffer(gpuMetalDeviceFrom(device), buffer.id).contents;
}

MTLTextureUsage textureUsage(PrGpuTextureUsageFlags usage) {
    // No texture asks for MTLTextureUsagePixelFormatView, which would keep the GPU from compressing it. A view keeps
    // its texture's pixel format or swaps sRGB for linear, and neither needs it.
    MTLTextureUsage u = 0;
    if (usage & PrGpuTextureUsageFlag_Sampled)
        u |= MTLTextureUsageShaderRead;
    if (usage & PrGpuTextureUsageFlag_Storage)
        u |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (usage & (PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_DepthStencil))
        u |= MTLTextureUsageRenderTarget;
    return u;
}

PrStatus textureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        MTLPixelFormat format = gpuMetalPixelFormat(desc->format);
        if (format == MTLPixelFormatInvalid)
            return PrStatus_NotSupported;
        MTLTextureDescriptor* td = [[MTLTextureDescriptor new] autorelease];
        td.textureType = textureType(desc->kind, desc->samples);
        td.pixelFormat = format;
        td.width = desc->extent.width;
        td.height = desc->extent.height;
        bool cube = desc->kind == PrGpuTextureKind_Cube || desc->kind == PrGpuTextureKind_CubeArray;
        if (desc->kind == PrGpuTextureKind_3d)
            td.depth = desc->extent.depth;
        else
            td.arrayLength = cube ? desc->extent.depth / 6 : desc->extent.depth;
        td.mipmapLevelCount = desc->levels;
        td.sampleCount = desc->samples;
        td.storageMode = MTLStorageModePrivate;
        td.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        td.usage = textureUsage(desc->usage);
        id<MTLTexture> texture = [d->device newTextureWithDescriptor:td];
        if (texture == nil)
            return PrStatus_Limit;
        if (desc->name.size != 0)
            texture.label = gpuMetalString(desc->name);
        uint32_t slot = gpuMetalResourceAdd(d, GpuMetalSlot_Texture, texture, texture.gpuResourceID._impl);
        [texture release];
        if (slot == 0)
            return PrStatus_Limit;
        d->resources[slot].texture = *desc;
        d->resources[slot].texture.name = PrStringView{};
        out->id = slot;
        return PrStatus_Ok;
    }
}

PrStatus textureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        id<MTLTexture> t = gpuMetalTexture(d, texture.id);
        PrGpuTextureDesc desc = d->resources[texture.id].texture;
        uint8_t levels;
        if (subresources.levels != 0)
            levels = subresources.levels;
        else
            levels = uint8_t(desc.levels - subresources.firstLevel);
        uint32_t layerCount = desc.kind == PrGpuTextureKind_3d ? 1 : desc.extent.depth;
        uint16_t layers;
        if (subresources.layers != 0)
            layers = subresources.layers;
        else
            layers = uint16_t(layerCount - subresources.firstLayer);
        id<MTLTexture> view = [t newTextureViewWithPixelFormat:t.pixelFormat
            textureType:textureType(kind, desc.samples) levels:NSMakeRange(subresources.firstLevel, levels)
            slices:NSMakeRange(subresources.firstLayer, layers)];
        if (view == nil)
            return PrStatus_Limit;
        uint32_t slot = gpuMetalResourceAdd(d, GpuMetalSlot_Texture, view, view.gpuResourceID._impl);
        [view release];
        if (slot == 0)
            return PrStatus_Limit;
        PrGpuTextureDesc& v = d->resources[slot].texture;
        v = desc;
        v.name = PrStringView{};
        v.kind = kind;
        v.levels = levels;
        uint32_t width = desc.extent.width >> subresources.firstLevel;
        uint32_t height = desc.extent.height >> subresources.firstLevel;
        v.extent.width = width != 0 ? width : 1;
        v.extent.height = height != 0 ? height : 1;
        v.extent.depth = desc.kind == PrGpuTextureKind_3d ? desc.extent.depth : layers;
        out->id = slot;
        return PrStatus_Ok;
    }
}

void textureDestroy(PrGpuDevice* device, PrGpuTexture texture) {
    gpuMetalRetire(gpuMetalDeviceFrom(device), GpuMetalRetiredKind_Resource, texture.id);
}

PrGpuTextureDesc textureDesc(const PrGpuDevice* device, PrGpuTexture texture) {
    return gpuMetalDeviceFrom(device)->resources[texture.id].texture;
}

// Metal reaches a texture by its resource id, for sampling and for reading and writing alike.
uint32_t textureStorageHandle(const PrGpuDevice*, PrGpuTexture texture) {
    return texture.id;
}

PrStatus samplerCreate(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor new] autorelease];
        sd.magFilter =
            desc->magnify == PrGpuFilter_Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.minFilter =
            desc->minify == PrGpuFilter_Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = desc->mip == PrGpuFilter_Linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sd.sAddressMode = addressMode(desc->address[0]);
        sd.tAddressMode = addressMode(desc->address[1]);
        sd.rAddressMode = addressMode(desc->address[2]);
        sd.borderColor = MTLSamplerBorderColorTransparentBlack;
        sd.maxAnisotropy = desc->maxAnisotropy > 1 ? desc->maxAnisotropy : 1;
        sd.lodMinClamp = desc->minLod;
        sd.lodMaxClamp = desc->maxLod;
        sd.lodBias = desc->lodBias;
        sd.compareFunction = gpuMetalCompare(desc->compare);
        sd.supportArgumentBuffers = YES;
        id<MTLSamplerState> sampler = [d->device newSamplerStateWithDescriptor:sd];
        if (sampler == nil)
            return PrStatus_Limit;
        prMutexLock(&d->mutex);
        uint32_t slot = d->nextSampler < kGpuMetalMaxSamplers ? d->nextSampler++ : 0;
        if (slot != 0) {
            d->samplers[slot] = sampler;
            setHeapEntry(d->samplerHeap, slot, sampler.gpuResourceID._impl);
        }
        prMutexUnlock(&d->mutex);
        if (slot == 0) {
            [sampler release];
            return PrStatus_Limit;
        }
        out->id = slot;
        return PrStatus_Ok;
    }
}

PrStatus shaderLibraryCreate(PrGpuDevice* device, PrGpuShaderEntry* entries, uint32_t entryCount,
        PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        if (entryCount == 0)
            return PrStatus_InvalidData;
        // Every entry point shares the one metallib, which the library's bytes hold until the library is destroyed.
        dispatch_data_t data = dispatch_data_create(entries[0].code, entries[0].codeSize, nullptr, ^{});
        NSError* error = nil;
        id<MTLLibrary> library = [d->device newLibraryWithData:data error:&error];
        dispatch_release(data);
        if (library == nil) {
            NSLog(@"lib/gpu: Metal took no shader library: %@", error);
            return PrStatus_InvalidData;
        }
        GpuMetalLibrary* m = allocZeroed<GpuMetalLibrary>();
        m->library = library;
        *out = m;
        return PrStatus_Ok;
    }
}

void shaderLibraryDestroy(PrGpuDevice*, PrGpuShaderLibrary* library) {
    GpuMetalLibrary* m = static_cast<GpuMetalLibrary*>(library);
    [m->library release];
    prMemoryFree(m);
}

void pipelineDestroy(PrGpuDevice* device, PrGpuPipeline pipeline) {
    gpuMetalRetire(gpuMetalDeviceFrom(device), GpuMetalRetiredKind_Pipeline, pipeline.id);
}

PrGpuExtent pipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline) {
    MTLSize size = gpuMetalDeviceFrom(device)->pipelines[pipeline.id].groupSize;
    return PrGpuExtent{uint32_t(size.width), uint32_t(size.height), uint32_t(size.depth)};
}

void* commandsStream(PrGpuDevice* device, PrGpuCommands* commands, uint64_t size, PrGpuBuffer* buffer, uint64_t* offset) {
    uint32_t slot = 0;
    void* bytes = gpuMetalStream(gpuMetalDeviceFrom(device), gpuMetalCommandsFrom(commands), size, &slot, offset);
    buffer->id = slot;
    return bytes;
}

void commandsOnDone(PrGpuDevice*, PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context) {
    gpuMetalCommandsFrom(commands)->dones.push(GpuMetalDone{done, context});
}

void passSetViewport(PrGpuDevice*, PrGpuPass* handle, PrRect viewport, float minDepth, float maxDepth) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    [pass->encoder setViewport:MTLViewport{double(viewport.left), double(viewport.top),
        double(viewport.right - viewport.left),
        double(viewport.bottom - viewport.top), double(minDepth), double(maxDepth)}];
    gpuMetalPassChanged(pass, PrGpuHostStateFlag_Viewport);
}

void passSetScissor(PrGpuDevice*, PrGpuPass* handle, PrIRect scissor) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    // Metal requires the scissor within the attachments.
    int32_t left = scissor.left > 0 ? scissor.left : 0;
    int32_t top = scissor.top > 0 ? scissor.top : 0;
    int32_t right = scissor.right < int32_t(pass->extent.x) ? scissor.right : int32_t(pass->extent.x);
    int32_t bottom = scissor.bottom < int32_t(pass->extent.y) ? scissor.bottom : int32_t(pass->extent.y);
    if (right < left)
        right = left;
    if (bottom < top)
        bottom = top;
    [pass->encoder setScissorRect:MTLScissorRect{NSUInteger(left), NSUInteger(top), NSUInteger(right - left),
        NSUInteger(bottom - top)}];
    gpuMetalPassChanged(pass, PrGpuHostStateFlag_Scissor);
}

void passSetBlendConstant(PrGpuDevice*, PrGpuPass* handle, PrColor constant) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    [pass->encoder setBlendColorRed:constant.r green:constant.g blue:constant.b alpha:constant.a];
    gpuMetalPassChanged(pass, PrGpuHostStateFlag_BlendConstant);
}

void passSetStencilReference(PrGpuDevice*, PrGpuPass* handle, uint8_t reference) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    [pass->encoder setStencilReferenceValue:reference];
    gpuMetalPassChanged(pass, PrGpuHostStateFlag_StencilReference);
}

void passSetIndexBuffer(PrGpuDevice*, PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, bool wide) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    pass->indexBuffer = buffer;
    pass->indexOffset = offset;
    pass->indexType = wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
}

PrStatus swapChainCreate(PrGpuDevice* device, const PrAppSurface* surface, const PrGpuSwapChainDesc* desc,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        if (surface->kind != PrAppSurfaceKind_NsView)
            return PrStatus_NotSupported;
        NSView* view = (NSView*)surface->handle;
        // A view that backs itself with a Metal layer keeps that layer, so that AppKit asks the view to display as it
        // resizes. Any other view hosts a new Metal layer.
        CAMetalLayer* layer = [view.layer isKindOfClass:CAMetalLayer.class] ? (CAMetalLayer*)view.layer : [CAMetalLayer layer];
        layer.device = d->device;
        // HDR presents extended sRGB, encoded, in half floats, which the layer shows past SDR white.
        bool hdr = desc->intent == PrGpuSwapChainIntent_Hdr;
        PrGpuSwapChainInfo info;
        if (hdr)
            info = PrGpuSwapChainInfo{PrGpuFormat_Rgba16F, PrColorSpace_Srgb};
        else
            info = PrGpuSwapChainInfo{PrGpuFormat_Bgra8, PrColorSpace_Srgb};
        layer.pixelFormat = gpuMetalPixelFormat(info.format);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(hdr ? kCGColorSpaceExtendedSRGB : kCGColorSpaceSRGB);
        layer.colorspace = space;
        CGColorSpaceRelease(space);
        layer.wantsExtendedDynamicRangeContent = hdr ? YES : NO;
        // The layer is readable, so that a frame can be copied or read back.
        layer.framebufferOnly = NO;
        layer.contentsScale = view.window != nil ? view.window.backingScaleFactor : 1;
        layer.presentsWithTransaction = (desc->flags & PrGpuSwapChainFlag_Threaded) == 0;
        if (view.layer != layer) {
            view.layer = layer;
            view.wantsLayer = YES;
        }
        // AppKit sets the gravity of a layer that backs a view from the view's placement, and that of a layer that a
        // view hosts is the swap chain's to set. A gravity's top is the layer's greatest y, which is the window's
        // bottom where the layer's geometry is flipped.
        bool stretch = desc->anchor == PrGpuSwapChainAnchor_Stretch;
        view.layerContentsPlacement =
            stretch ? NSViewLayerContentsPlacementScaleAxesIndependently : NSViewLayerContentsPlacementTopLeft;
        if (stretch)
            layer.contentsGravity = kCAGravityResize;
        else if ([layer contentsAreFlipped])
            layer.contentsGravity = kCAGravityBottomLeft;
        else
            layer.contentsGravity = kCAGravityTopLeft;
        [d->queue addResidencySet:layer.residencySet];
        GpuMetalSwapChain* sc = allocZeroed<GpuMetalSwapChain>();
        sc->device = device;
        sc->info = info;
        sc->layer = [layer retain];
        sc->view = view;
        sc->transactional = layer.presentsWithTransaction;
        *out = sc;
        return PrStatus_Ok;
    }
}

// Lets go of the acquired drawable's texture, whose slot stays until the work that drew into it is done.
void releaseDrawable(GpuMetalDevice* d, GpuMetalSwapChain* sc) {
    if (sc->texture.id != 0)
        gpuMetalRetire(d, GpuMetalRetiredKind_Resource, sc->texture.id);
    sc->texture = PrGpuTexture{};
    [sc->drawable release];
    sc->drawable = nil;
}

void swapChainDestroy(PrGpuDevice* device, PrGpuSwapChain* handle) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalSwapChain* sc = static_cast<GpuMetalSwapChain*>(handle);
    gpuMetalWaitIdle(device);
    releaseDrawable(d, sc);
    [d->queue removeResidencySet:sc->layer.residencySet];
    [sc->layer release];
    prMemoryFree(sc);
}

// A layer waits for a drawable only when asked for one, at its extent, which the frame gives.
void swapChainWait(PrGpuDevice*, PrGpuSwapChain*) {}

void swapChainPresent(PrGpuDevice* device, PrGpuSwapChain* handle, PrGpuCommands* commands) {
    GpuMetalSwapChain* sc = static_cast<GpuMetalSwapChain*>(handle);
    if (sc->drawable == nil)
        return;
    gpuMetalCommandsFrom(commands)->drawables.push(GpuMetalDrawable{[sc->drawable retain], sc->transactional});
    releaseDrawable(gpuMetalDeviceFrom(device), sc);
}

} // namespace

NSString* gpuMetalString(PrStringView view) {
    return [[[NSString alloc] initWithBytes:view.data length:view.size encoding:NSUTF8StringEncoding] autorelease];
}

MTLPixelFormat gpuMetalPixelFormat(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_R8: return MTLPixelFormatR8Unorm;
        case PrGpuFormat_Rg8: return MTLPixelFormatRG8Unorm;
        case PrGpuFormat_Rgba8: return MTLPixelFormatRGBA8Unorm;
        case PrGpuFormat_Rgba8Srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
        case PrGpuFormat_Bgra8: return MTLPixelFormatBGRA8Unorm;
        case PrGpuFormat_Bgra8Srgb: return MTLPixelFormatBGRA8Unorm_sRGB;
        case PrGpuFormat_Rgb10A2: return MTLPixelFormatRGB10A2Unorm;
        case PrGpuFormat_Rg11B10F: return MTLPixelFormatRG11B10Float;
        case PrGpuFormat_R16F: return MTLPixelFormatR16Float;
        case PrGpuFormat_Rg16F: return MTLPixelFormatRG16Float;
        case PrGpuFormat_Rgba16F: return MTLPixelFormatRGBA16Float;
        case PrGpuFormat_R32F: return MTLPixelFormatR32Float;
        case PrGpuFormat_Rg32F: return MTLPixelFormatRG32Float;
        case PrGpuFormat_Rgba32F: return MTLPixelFormatRGBA32Float;
        case PrGpuFormat_R8Uint: return MTLPixelFormatR8Uint;
        case PrGpuFormat_R16Uint: return MTLPixelFormatR16Uint;
        case PrGpuFormat_R32Uint: return MTLPixelFormatR32Uint;
        case PrGpuFormat_Rg32Uint: return MTLPixelFormatRG32Uint;
        case PrGpuFormat_Rgba32Uint: return MTLPixelFormatRGBA32Uint;
        case PrGpuFormat_D16: return MTLPixelFormatDepth16Unorm;
        case PrGpuFormat_D32F: return MTLPixelFormatDepth32Float;
        case PrGpuFormat_D32FS8: return MTLPixelFormatDepth32Float_Stencil8;
        case PrGpuFormat_Bc1: return MTLPixelFormatBC1_RGBA;
        case PrGpuFormat_Bc1Srgb: return MTLPixelFormatBC1_RGBA_sRGB;
        case PrGpuFormat_Bc3: return MTLPixelFormatBC3_RGBA;
        case PrGpuFormat_Bc3Srgb: return MTLPixelFormatBC3_RGBA_sRGB;
        case PrGpuFormat_Bc4: return MTLPixelFormatBC4_RUnorm;
        case PrGpuFormat_Bc5: return MTLPixelFormatBC5_RGUnorm;
        case PrGpuFormat_Bc6HUfloat: return MTLPixelFormatBC6H_RGBUfloat;
        case PrGpuFormat_Bc7: return MTLPixelFormatBC7_RGBAUnorm;
        case PrGpuFormat_Bc7Srgb: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        case PrGpuFormat_Astc4x4: return MTLPixelFormatASTC_4x4_LDR;
        case PrGpuFormat_Astc4x4Srgb: return MTLPixelFormatASTC_4x4_sRGB;
        default: return MTLPixelFormatInvalid;
    }
}

PrGpuFormat gpuMetalFormatFromPixelFormat(MTLPixelFormat format) {
    for (uint8_t f = 1; f < PrGpuFormat_Count; ++f) {
        if (gpuMetalPixelFormat(PrGpuFormat(f)) == format)
            return PrGpuFormat(f);
    }
    return PrGpuFormat_None;
}

MTLCompareFunction gpuMetalCompare(PrGpuCompare compare) {
    switch (compare) {
        case PrGpuCompare_Never: return MTLCompareFunctionNever;
        case PrGpuCompare_Less: return MTLCompareFunctionLess;
        case PrGpuCompare_Equal: return MTLCompareFunctionEqual;
        case PrGpuCompare_LessEqual: return MTLCompareFunctionLessEqual;
        case PrGpuCompare_Greater: return MTLCompareFunctionGreater;
        case PrGpuCompare_NotEqual: return MTLCompareFunctionNotEqual;
        case PrGpuCompare_GreaterEqual: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
    }
}

MTLBlendFactor gpuMetalBlendFactor(PrGpuBlendFactor factor, bool alpha) {
    switch (factor) {
        case PrGpuBlendFactor_Zero: return MTLBlendFactorZero;
        case PrGpuBlendFactor_Source: return alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
        case PrGpuBlendFactor_OneMinusSource:
            return alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
        case PrGpuBlendFactor_SourceAlpha: return MTLBlendFactorSourceAlpha;
        case PrGpuBlendFactor_OneMinusSourceAlpha: return MTLBlendFactorOneMinusSourceAlpha;
        case PrGpuBlendFactor_Target: return alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
        case PrGpuBlendFactor_OneMinusTarget:
            return alpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOneMinusDestinationColor;
        case PrGpuBlendFactor_TargetAlpha: return MTLBlendFactorDestinationAlpha;
        case PrGpuBlendFactor_OneMinusTargetAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
        case PrGpuBlendFactor_SourceAlphaSaturated: return MTLBlendFactorSourceAlphaSaturated;
        case PrGpuBlendFactor_Constant: return alpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
        case PrGpuBlendFactor_OneMinusConstant:
            return alpha ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
        case PrGpuBlendFactor_Source1: return alpha ? MTLBlendFactorSource1Alpha : MTLBlendFactorSource1Color;
        case PrGpuBlendFactor_OneMinusSource1:
            return alpha ? MTLBlendFactorOneMinusSource1Alpha : MTLBlendFactorOneMinusSource1Color;
        default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation gpuMetalBlendOp(PrGpuBlendOp op) {
    switch (op) {
        case PrGpuBlendOp_Subtract: return MTLBlendOperationSubtract;
        case PrGpuBlendOp_ReverseSubtract: return MTLBlendOperationReverseSubtract;
        case PrGpuBlendOp_Min: return MTLBlendOperationMin;
        case PrGpuBlendOp_Max: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}

MTLPrimitiveType gpuMetalPrimitive(PrGpuTopology topology) {
    switch (topology) {
        case PrGpuTopology_TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
        case PrGpuTopology_LineList: return MTLPrimitiveTypeLine;
        case PrGpuTopology_LineStrip: return MTLPrimitiveTypeLineStrip;
        case PrGpuTopology_PointList: return MTLPrimitiveTypePoint;
        default: return MTLPrimitiveTypeTriangle;
    }
}

MTLPrimitiveTopologyClass gpuMetalTopologyClass(PrGpuTopology topology) {
    switch (topology) {
        case PrGpuTopology_LineList:
        case PrGpuTopology_LineStrip: return MTLPrimitiveTopologyClassLine;
        case PrGpuTopology_PointList: return MTLPrimitiveTopologyClassPoint;
        default: return MTLPrimitiveTopologyClassTriangle;
    }
}

// The stages of the work that makes accesses.
MTLStages gpuMetalStages(PrGpuAccessFlags access) {
    MTLStages stages = 0;
    if (access & (PrGpuAccessFlag_IndexRead | PrGpuAccessFlag_IndirectRead | PrGpuAccessFlag_VertexRead |
            PrGpuAccessFlag_VertexWrite))
        stages |= MTLStageVertex;
    if (access & (PrGpuAccessFlag_IndirectRead | PrGpuAccessFlag_ComputeRead | PrGpuAccessFlag_ComputeWrite))
        stages |= MTLStageDispatch;
    if (access & (PrGpuAccessFlag_FragmentRead | PrGpuAccessFlag_FragmentWrite | PrGpuAccessFlag_ColorAttachment |
            PrGpuAccessFlag_DepthStencilRead | PrGpuAccessFlag_DepthStencilWrite))
        stages |= MTLStageFragment;
    if (access & (PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite))
        stages |= MTLStageBlit;
    return stages;
}

// Puts an object into a new resource slot, adds it to the residency set and writes its heap entry. Returns zero if no
// slot is left. The caller holds the mutex.
uint32_t gpuMetalResourceAddLocked(GpuMetalDevice* d, GpuMetalSlot kind, id object, uint64_t entry) {
    uint32_t slot = 0;
    if (d->freeResources.count != 0) {
        slot = d->freeResources.last();
        d->freeResources.pop();
    } else if (d->nextResource < kGpuMetalMaxResources) {
        slot = d->nextResource++;
    }
    if (slot == 0)
        return 0;
    GpuMetalResource& r = d->resources[slot];
    r = GpuMetalResource{};
    r.kind = kind;
    r.object = [object retain];
    [d->residency addAllocation:object];
    d->residencyDirty = true;
    setHeapEntry(d->resourceHeap, slot, entry);
    return slot;
}

uint32_t gpuMetalResourceAdd(GpuMetalDevice* d, GpuMetalSlot kind, id object, uint64_t entry) {
    prMutexLock(&d->mutex);
    uint32_t slot = gpuMetalResourceAddLocked(d, kind, object, entry);
    prMutexUnlock(&d->mutex);
    return slot;
}

// Frees what the command buffers that are done held: retired objects, and the blocks, allocators and tables of their
// recording. The caller holds the mutex.
void gpuMetalCollect(GpuMetalDevice* d) {
    uint64_t done = d->done.load();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        const GpuMetalInFlight& f = d->inFlight.data[i];
        if (f.fence == 0 || f.fence > done)
            d->inFlight.data[kept++] = f;
    }
    d->inFlight.count = kept;
    uint64_t oldest = kept != 0 ? d->inFlight.data[0].serial : d->serial;
    kept = 0;
    for (uint32_t i = 0; i < d->retired.count; ++i) {
        const GpuMetalRetired& r = d->retired.data[i];
        if (r.serial > oldest) {
            d->retired.data[kept++] = r;
        } else if (r.kind == GpuMetalRetiredKind_Resource) {
            freeResource(d, r.slot);
        } else {
            freePipeline(d, r.slot);
        }
    }
    d->retired.count = kept;
    kept = 0;
    for (uint32_t i = 0; i < d->held.count; ++i) {
        GpuMetalHeld& h = d->held.data[i];
        if (h.fence > done) {
            d->held.data[kept++] = h;
            continue;
        }
        switch (h.kind) {
            case GpuMetalHeldKind_StreamBlock:
                h.block.used = 0;
                d->streamBlocks.push(h.block);
                break;
            case GpuMetalHeldKind_StagingBlock:
                h.block.used = 0;
                d->stagingBlocks.push(h.block);
                break;
            default:
                d->heldRecycle(d, h);
                break;
        }
    }
    d->held.count = kept;
}

// Retires an object, which is freed once the command buffers that began before now are done.
void gpuMetalRetire(GpuMetalDevice* d, GpuMetalRetiredKind kind, uint32_t slot) {
    prMutexLock(&d->mutex);
    d->retired.push(GpuMetalRetired{d->serial, kind, slot});
    gpuMetalCollect(d);
    prMutexUnlock(&d->mutex);
}

void gpuMetalCommitResidency(GpuMetalDevice* d) {
    if (d->residencyDirty) {
        [d->residency commit];
        d->residencyDirty = false;
    }
}

id<MTLBuffer> gpuMetalBuffer(const GpuMetalDevice* d, uint32_t slot) {
    PR_ASSERT(slot < kGpuMetalMaxResources && d->resources[slot].kind == GpuMetalSlot_Buffer);
    return d->resources[slot].object;
}

id<MTLTexture> gpuMetalTexture(const GpuMetalDevice* d, uint32_t slot) {
    PR_ASSERT(slot < kGpuMetalMaxResources && d->resources[slot].kind == GpuMetalSlot_Texture);
    return d->resources[slot].object;
}

void gpuMetalWait(PrGpuDevice* device, PrGpuFence fence) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    prMutexLock(&d->doneMutex);
    while (d->done.load() < fence)
        prConditionWait(&d->doneSignal, &d->doneMutex);
    prMutexUnlock(&d->doneMutex);
    prMutexLock(&d->mutex);
    gpuMetalCollect(d);
    prMutexUnlock(&d->mutex);
}

void gpuMetalWaitIdle(PrGpuDevice* device) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    prMutexLock(&d->mutex);
    d->uploadsSubmit(d);
    PrGpuFence fence = d->submitted;
    prMutexUnlock(&d->mutex);
    gpuMetalWait(device, fence);
}

uint32_t gpuMetalPipelineAdd(GpuMetalDevice* d, const GpuMetalPipeline& pipeline) {
    prMutexLock(&d->mutex);
    uint32_t id = 0;
    if (d->freePipelines.count != 0) {
        id = d->freePipelines.last();
        d->freePipelines.pop();
    } else if (d->nextPipeline < kGpuMetalMaxPipelines) {
        id = d->nextPipeline++;
    }
    if (id != 0)
        d->pipelines[id] = pipeline;
    prMutexUnlock(&d->mutex);
    return id;
}

void* gpuMetalStream(GpuMetalDevice* d, GpuMetalCommands* c, uint64_t size, uint32_t* slot, uint64_t* offset) {
    GpuMetalBlock* last = c->blocks.count != 0 ? &c->blocks.last() : nullptr;
    uint64_t at = last != nullptr ? alignUp(last->used, kGpuMetalStreamAlignment) : 0;
    if (last == nullptr || at + size > last->buffer.length) {
        prMutexLock(&d->mutex);
        GpuMetalBlock block = blockTake(d, d->streamBlocks, kGpuMetalStreamBlock, size);
        prMutexUnlock(&d->mutex);
        if (block.buffer == nil) {
            // Out of memory loses the device, and the caller writes into memory that the GPU never reads.
            d->lost.store(true);
            void* scratch = prMemoryAlloc(size);
            c->scratch.push(scratch);
            *slot = 0;
            *offset = 0;
            return scratch;
        }
        c->blocks.push(block);
        last = &c->blocks.last();
        at = 0;
    }
    last->used = at + size;
    *slot = last->slot;
    *offset = at;
    return static_cast<uint8_t*>(last->buffer.contents) + at;
}

void gpuMetalRenderPipelineSet(const GpuMetalPipeline& p, id encoder) {
    [encoder setRenderPipelineState:p.state];
    [encoder setDepthStencilState:p.depthStencil];
    [encoder setCullMode:p.cull];
    [encoder setFrontFacingWinding:p.winding];
    [encoder setDepthBias:p.depthBias slopeScale:p.depthBiasSlope clamp:p.depthBiasClamp];
}

PrStatus gpuMetalSwapChainAcquire(PrGpuDevice* device, PrGpuSwapChain* handle, PrUVec2 extent, PrGpuTexture* out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalSwapChain* sc = static_cast<GpuMetalSwapChain*>(handle);
    @autoreleasepool {
        releaseDrawable(d, sc);

        CGSize size = CGSizeMake(extent.x, extent.y);
        if (!CGSizeEqualToSize(sc->layer.drawableSize, size))
            sc->layer.drawableSize = size;
        id<CAMetalDrawable> drawable = [sc->layer nextDrawable];
        if (drawable == nil)
            return PrStatus_InvalidState;
        id<MTLTexture> texture = drawable.texture;
        uint32_t slot = gpuMetalResourceAdd(d, GpuMetalSlot_Texture, texture, texture.gpuResourceID._impl);
        if (slot == 0)
            return PrStatus_Limit;
        // The layer's residency set holds the drawable's texture, so the device's does not.
        prMutexLock(&d->mutex);
        [d->residency removeAllocation:texture];
        prMutexUnlock(&d->mutex);
        d->resources[slot].texture = PrGpuTextureDesc{
            .format = sc->info.format,
            .usage = PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled,
            .levels = 1,
            .samples = 1,
            .extent = {uint32_t(texture.width), uint32_t(texture.height), 1},
        };
        sc->drawable = [drawable retain];
        sc->texture = PrGpuTexture{slot};
        *out = sc->texture;
        return PrStatus_Ok;
    }
}

void gpuMetalSubmissionDone(GpuMetalDevice* d, PrGpuFence fence, GpuMetalDone* PR_NULLABLE dones, uint32_t doneCount,
        NSError* PR_NULLABLE error, double seconds) {
    if (error != nil) {
        NSLog(@"lib/gpu: Metal reported an error from a command buffer: %@", error);
        d->lost.store(true);
    }
    for (uint32_t i = 0; i < doneCount; ++i)
        dones[i].done(dones[i].context, seconds);
    prMemoryFree(dones);
    prMutexLock(&d->doneMutex);
    uint64_t done = d->done.load();
    if (fence != done + 1) {
        d->doneAhead.push(fence);
    } else {
        // Reaching a fence means that every submission before it is done, so one that completed early waits here.
        ++done;
        for (uint32_t i = 0; i < d->doneAhead.count;) {
            if (d->doneAhead.data[i] == done + 1) {
                ++done;
                d->doneAhead.data[i] = d->doneAhead.last();
                d->doneAhead.pop();
                i = 0;
            } else {
                ++i;
            }
        }
        d->done.store(done);
        prConditionBroadcast(&d->doneSignal);
    }
    prMutexUnlock(&d->doneMutex);
}

void gpuMetalSubmissionRecord(GpuMetalDevice* d, GpuMetalCommands* c, PrGpuFence fence) {
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        if (d->inFlight.data[i].serial == c->serial)
            d->inFlight.data[i].fence = fence;
    }
    for (uint32_t i = 0; i < c->blocks.count; ++i) {
        d->held.push(GpuMetalHeld{.fence = fence, .kind = GpuMetalHeldKind_StreamBlock, .block = c->blocks.data[i]});
    }
}

void gpuMetalUploadsRecord(GpuMetalDevice* d, PrGpuFence fence) {
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        if (d->inFlight.data[i].serial == d->uploadSerial)
            d->inFlight.data[i].fence = fence;
    }
    for (uint32_t i = 0; i < d->uploadBlocks.count; ++i) {
        d->held.push(GpuMetalHeld{
            .fence = fence,
            .kind = GpuMetalHeldKind_StagingBlock,
            .block = d->uploadBlocks.data[i],
        });
    }
    d->uploadBlocks.clear();
    gpuSharedUploadBatchClear(&d->uploadBatch);
}

GpuMetalDone* PR_NULLABLE gpuMetalDonesCopy(const GpuMetalCommands* c) {
    if (c->dones.count == 0)
        return nullptr;
    GpuMetalDone* dones = static_cast<GpuMetalDone*>(prMemoryAlloc(sizeof(GpuMetalDone) * c->dones.count));
    memCopy(dones, c->dones.data, sizeof(GpuMetalDone) * c->dones.count);
    return dones;
}

void gpuMetalCommandsInit(GpuMetalDevice* d, GpuMetalCommands* c) {
    c->device = d;
    c->serial = d->serial++;
    d->inFlight.push(GpuMetalInFlight{c->serial, 0});
}

void gpuMetalCommandsDestroy(GpuMetalCommands* c) {
    for (uint32_t i = 0; i < c->scratch.count; ++i)
        prMemoryFree(c->scratch.data[i]);
    c->scratch.reset();
    c->blocks.reset();
    c->drawables.reset();
    c->dones.reset();
    prMemoryFree(c);
}

GpuMetalPipeline gpuMetalPipelineFromDesc(GpuMetalDevice* d, const PrGpuGraphicsPipelineDesc* desc) {
    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor new] autorelease];
    dd.depthCompareFunction = desc->depthTest ? gpuMetalCompare(desc->depthCompare) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = desc->depthWrite;
    if (desc->stencilTest) {
        MTLStencilDescriptor* faces[2] = {
            [[MTLStencilDescriptor new] autorelease],
            [[MTLStencilDescriptor new] autorelease],
        };
        const PrGpuStencilFace* from[2] = {&desc->stencilFront, &desc->stencilBack};
        for (int f = 0; f < 2; ++f) {
            faces[f].stencilFailureOperation = stencilOp(from[f]->fail);
            faces[f].depthFailureOperation = stencilOp(from[f]->depthFail);
            faces[f].depthStencilPassOperation = stencilOp(from[f]->pass);
            faces[f].stencilCompareFunction = gpuMetalCompare(from[f]->compare);
            faces[f].readMask = desc->stencilReadMask;
            faces[f].writeMask = desc->stencilWriteMask;
        }
        dd.frontFaceStencil = faces[0];
        dd.backFaceStencil = faces[1];
    }
    GpuMetalPipeline p{};
    p.depthStencil = [d->device newDepthStencilStateWithDescriptor:dd];
    p.primitive = gpuMetalPrimitive(desc->topology);
    if (desc->cull == PrGpuCull_Front)
        p.cull = MTLCullModeFront;
    else if (desc->cull == PrGpuCull_Back)
        p.cull = MTLCullModeBack;
    else
        p.cull = MTLCullModeNone;
    p.winding = desc->frontClockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
    p.depthBias = desc->depthBias;
    p.depthBiasSlope = desc->depthBiasSlope;
    p.depthBiasClamp = desc->depthBiasClamp;
    return p;
}

void gpuMetalCommandsSetPipeline(PrGpuDevice* device, PrGpuCommands* commands, PrGpuPipeline pipeline) {
    GpuMetalCommands* c = gpuMetalCommandsFrom(commands);
    c->pipeline = pipeline.id;
    const GpuMetalPipeline& p = gpuMetalDeviceFrom(device)->pipelines[pipeline.id];
    if (GpuMetalPass* pass = c->pass) {
        PR_ASSERT(!p.compute);
        gpuMetalRenderPipelineSet(p, pass->encoder);
        gpuMetalPassChanged(pass, PrGpuHostStateFlag_Pipeline);
    }
}

void gpuMetalEncodeCopyTexture(const GpuMetalDevice* d, id encoder, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    bool is3d = d->resources[source.id].texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : sourceRegion->extent.depth;
    for (uint32_t i = 0; i < images; ++i) {
        NSUInteger depth = is3d ? sourceRegion->extent.depth : 1;
        NSUInteger sourceSlice = is3d ? 0 : sourceRegion->firstLayer + i;
        NSUInteger targetSlice = is3d ? 0 : targetRegion->firstLayer + i;
        [encoder copyFromTexture:gpuMetalTexture(d, source.id) sourceSlice:sourceSlice sourceLevel:sourceRegion->level
            sourceOrigin:MTLOriginMake(sourceRegion->x, sourceRegion->y, is3d ? sourceRegion->z : 0)
            sourceSize:MTLSizeMake(sourceRegion->extent.width, sourceRegion->extent.height, depth)
            toTexture:gpuMetalTexture(d, target.id) destinationSlice:targetSlice destinationLevel:targetRegion->level
            destinationOrigin:MTLOriginMake(targetRegion->x, targetRegion->y, is3d ? targetRegion->z : 0)];
    }
}

void gpuMetalEncodeCopyBufferToTexture(const GpuMetalDevice* d, id encoder, PrGpuBuffer source, uint64_t sourceOffset,
        uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    bool is3d = d->resources[target.id].texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : targetRegion->extent.depth;
    uint64_t imageBytes = layerBytes != 0 ? layerBytes : uint64_t(rowBytes) * targetRegion->extent.height;
    for (uint32_t i = 0; i < images; ++i) {
        NSUInteger depth = is3d ? targetRegion->extent.depth : 1;
        NSUInteger targetSlice = is3d ? 0 : targetRegion->firstLayer + i;
        [encoder copyFromBuffer:gpuMetalBuffer(d, source.id) sourceOffset:sourceOffset + i * imageBytes
            sourceBytesPerRow:rowBytes sourceBytesPerImage:imageBytes
            sourceSize:MTLSizeMake(targetRegion->extent.width, targetRegion->extent.height, depth)
            toTexture:gpuMetalTexture(d, target.id) destinationSlice:targetSlice destinationLevel:targetRegion->level
            destinationOrigin:MTLOriginMake(targetRegion->x, targetRegion->y, is3d ? targetRegion->z : 0)];
    }
}

void gpuMetalEncodeCopyTextureToBuffer(const GpuMetalDevice* d, id encoder, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes) {
    bool is3d = d->resources[source.id].texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : sourceRegion->extent.depth;
    uint64_t imageBytes = layerBytes != 0 ? layerBytes : uint64_t(rowBytes) * sourceRegion->extent.height;
    for (uint32_t i = 0; i < images; ++i) {
        NSUInteger depth = is3d ? sourceRegion->extent.depth : 1;
        NSUInteger sourceSlice = is3d ? 0 : sourceRegion->firstLayer + i;
        [encoder copyFromTexture:gpuMetalTexture(d, source.id) sourceSlice:sourceSlice sourceLevel:sourceRegion->level
            sourceOrigin:MTLOriginMake(sourceRegion->x, sourceRegion->y, is3d ? sourceRegion->z : 0)
            sourceSize:MTLSizeMake(sourceRegion->extent.width, sourceRegion->extent.height, depth)
            toBuffer:gpuMetalBuffer(d, target.id) destinationOffset:targetOffset + i * imageBytes
            destinationBytesPerRow:rowBytes destinationBytesPerImage:imageBytes];
    }
}

void gpuMetalEncodeUploadCopies(const GpuMetalDevice* d, id encoder) {
    const Array<GpuSharedUploadCopy>& copies = d->uploadBatch.copies;
    for (uint32_t i = 0; i < copies.count; ++i) {
        const GpuSharedUploadCopy& c = copies.data[i];
        if (c.size != 0)
            [encoder copyFromBuffer:d->uploadBlocks.data[c.block].buffer sourceOffset:c.source
                toBuffer:gpuMetalBuffer(d, c.buffer) destinationOffset:c.target size:c.size];
    }
}

void gpuMetalEncodeTextureCopy(id encoder, const GpuMetalTextureCopy& copy) {
    const PrGpuTextureRegion* region = copy.region;
    uint32_t images = copy.is3d ? 1 : copy.slices;
    for (uint32_t i = 0; i < images; ++i) {
        NSUInteger depth = copy.is3d ? copy.slices : 1;
        NSUInteger slice = copy.is3d ? 0 : region->firstLayer + i;
        [encoder copyFromBuffer:copy.staging sourceOffset:copy.at + i * copy.sliceBytes
            sourceBytesPerRow:copy.rowBytes sourceBytesPerImage:copy.sliceBytes
            sourceSize:MTLSizeMake(region->extent.width, region->extent.height, depth) toTexture:copy.texture
            destinationSlice:slice destinationLevel:region->level
            destinationOrigin:MTLOriginMake(region->x, region->y, copy.is3d ? region->z : 0)];
    }
}

void gpuMetalPassChanged(GpuMetalPass* pass, PrGpuHostStateFlags flags) {
    if (pass->host)
        gpuMetalCommandsFrom(pass->commands)->changed |= flags;
}

void gpuMetalDeviceInit(GpuMetalDevice* d, id<MTLDevice> device) {
    d->device = device;
    prMutexInit(&d->doneMutex);
    prConditionInit(&d->doneSignal);
    MTLResidencySetDescriptor* rd = [[MTLResidencySetDescriptor new] autorelease];
    rd.initialCapacity = 256;
    d->residency = [device newResidencySetWithDescriptor:rd error:nil];
    d->resourceHeap = [device newBufferWithLength:kGpuMetalMaxResources * sizeof(uint64_t)
        options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
    d->samplerHeap = [device newBufferWithLength:kGpuMetalMaxSamplers * sizeof(uint64_t)
        options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
    [d->residency addAllocation:d->resourceHeap];
    [d->residency addAllocation:d->samplerHeap];
    [d->residency commit];
    prMutexInit(&d->mutex);
    d->resources =
        static_cast<GpuMetalResource*>(prMemoryAllocZeroed(sizeof(GpuMetalResource) * kGpuMetalMaxResources));
    d->samplers = static_cast<id<MTLSamplerState>*>(prMemoryAllocZeroed(sizeof(id) * kGpuMetalMaxSamplers));
    d->pipelines =
        static_cast<GpuMetalPipeline*>(prMemoryAllocZeroed(sizeof(GpuMetalPipeline) * kGpuMetalMaxPipelines));
    d->nextResource = 1;
    d->nextSampler = 1;
    d->nextPipeline = 1;
    d->serial = 1;

    d->isLost = isLost;
    d->simulateLoss = simulateLoss;
    d->isComplete = isComplete;
    d->wait = gpuMetalWait;
    d->waitIdle = gpuMetalWaitIdle;
    d->supportsFormat = supportsFormat;
    d->bufferCreate = bufferCreate;
    d->bufferDestroy = bufferDestroy;
    d->bufferDesc = bufferDesc;
    d->bufferStorageHandle = bufferStorageHandle;
    d->bufferMap = bufferMap;
    d->bufferUpload = bufferUpload;
    d->textureCreate = textureCreate;
    d->textureCreateView = textureCreateView;
    d->textureDestroy = textureDestroy;
    d->textureDesc = textureDesc;
    d->textureStorageHandle = textureStorageHandle;
    d->textureUpload = textureUpload;
    d->samplerCreate = samplerCreate;
    d->shaderLibraryCreate = shaderLibraryCreate;
    d->shaderLibraryDestroy = shaderLibraryDestroy;
    d->pipelineDestroy = pipelineDestroy;
    d->pipelineGroupSize = pipelineGroupSize;
    d->commandsStream = commandsStream;
    d->commandsSetPipeline = gpuMetalCommandsSetPipeline;
    d->commandsOnDone = commandsOnDone;
    d->passSetViewport = passSetViewport;
    d->passSetScissor = passSetScissor;
    d->passSetBlendConstant = passSetBlendConstant;
    d->passSetStencilReference = passSetStencilReference;
    d->passSetIndexBuffer = passSetIndexBuffer;
    d->swapChainCreate = swapChainCreate;
    d->swapChainDestroy = swapChainDestroy;
    d->swapChainWait = swapChainWait;
    d->swapChainAcquire = gpuMetalSwapChainAcquire;
    d->swapChainPresent = swapChainPresent;
    d->shaderTarget = PrGpuShaderTarget_Metal;
    // The name's UTF-8 lives no longer than the autorelease pool, so the device keeps a copy.
    if (![device.name getCString:d->adapter maxLength:sizeof(d->adapter) encoding:NSUTF8StringEncoding])
        memCopy(d->adapter, "Metal", sizeof("Metal"));
    d->info.adapter = PrStringView{d->adapter, strlen(d->adapter)};
    // Framebuffer fetch on Apple's GPUs reads in raster order.
    d->info.features = PrGpuFeatureFlag_Timestamps | PrGpuFeatureFlag_TextureCompressionAstc |
        PrGpuFeatureFlag_OrderedTargetReads |
        (device.supportsBCTextureCompression ? PrGpuFeatureFlag_TextureCompressionBc : 0);
    d->info.maxTextureSize = 16384;
    d->info.maxResourceHandles = kGpuMetalMaxResources - 1;
    d->info.maxSamplerHandles = kGpuMetalMaxSamplers - 1;
}

void gpuMetalDeviceAddQueue(GpuMetalDevice* d, id queue) {
    d->queue = queue;
    [d->queue addResidencySet:d->residency];
}

void gpuMetalDeviceReset(GpuMetalDevice* d) {
    prMutexLock(&d->mutex);
    gpuMetalCollect(d);
    for (uint32_t i = 0; i < d->held.count; ++i)
        [d->held.data[i].object release];
    for (uint32_t i = 1; i < d->nextResource; ++i)
        [d->resources[i].object release];
    for (uint32_t i = 1; i < d->nextSampler; ++i)
        [d->samplers[i] release];
    for (uint32_t i = 1; i < d->nextPipeline; ++i) {
        [d->pipelines[i].state release];
        [d->pipelines[i].depthStencil release];
    }
    prMutexUnlock(&d->mutex);
    [d->queue removeResidencySet:d->residency];
    prMemoryFree(d->resources);
    prMemoryFree(d->samplers);
    prMemoryFree(d->pipelines);
    d->freeResources.reset();
    d->freePipelines.reset();
    d->inFlight.reset();
    d->retired.reset();
    d->held.reset();
    d->streamBlocks.reset();
    d->stagingBlocks.reset();
    d->uploadBlocks.reset();
    d->doneAhead.reset();
    gpuSharedUploadBatchReset(&d->uploadBatch);
    [d->resourceHeap release];
    [d->samplerHeap release];
    [d->residency release];
    [d->queue release];
    [d->device release];
    prMutexReset(&d->mutex);
    prConditionReset(&d->doneSignal);
    prMutexReset(&d->doneMutex);
}

PrStatus gpuMetalTextureWrap(GpuMetalDevice* d, id<MTLTexture> texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out) {
    id<MTLTexture> t = texture;
    *out = PrGpuTexture{};
    uint32_t slot = gpuMetalResourceAdd(d, GpuMetalSlot_Texture, t, t.gpuResourceID._impl);
    if (slot == 0)
        return PrStatus_Limit;
    d->resources[slot].texture = PrGpuTextureDesc{
        .format = gpuMetalFormatFromPixelFormat(t.pixelFormat),
        .usage = usage,
        .levels = uint8_t(t.mipmapLevelCount),
        .samples = uint8_t(t.sampleCount),
        .extent = {uint32_t(t.width), uint32_t(t.height), uint32_t(t.arrayLength)},
    };
    out->id = slot;
    return PrStatus_Ok;
}

} // namespace pr_impl
