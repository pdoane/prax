// The Metal GPU backend on Metal 4, which is kept for experiments. The default Metal backend is pr/gpu_metal.h.
//
// One argument table per command buffer serves every encoder. Metal 4 has no setBytes, so the table points at push data
// and uniforms in the stream. Metal snapshots the table at each draw and dispatch.
//
// Metal 4 orders nothing by itself. A barrier is a producer barrier on the command buffer's last encoder, which holds
// the stages that it names of every later encoder in the queue until those stages of every encoder before are done. So
// the last encoder stays open until the next one begins, and a barrier at the start of a command buffer goes on an
// empty encoder. Dispatches and copies between two barriers share an encoder, and run concurrently.
//
// The queue delivers feedback in order, on a serial queue.
//
// Uploads are copied in a command buffer of the device's own, committed just before the next submission, or sooner once
// its staging fills a few blocks. It waits for all the work before it, and all the work after it waits for it.

#include "pr/gpu_metal4.h"

#include "pr/alloc.h"
#include "pr/apple_string.h"
#include "pr/assert.h"
#include "pr/mem.h"

#include "gpu/backends/shared/src/gpu_shared_metal.h"

namespace pr_impl {

namespace {

// The functions of the device's objects, which their creation installs.
extern const PrGpuCommands kCommandsFunctions;
extern const PrGpuPass kPassFunctions;
extern const PrGpuSwapChain kSwapChainFunctions;

constexpr MTLStages kAllStages = MTLStageVertex | MTLStageFragment | MTLStageDispatch | MTLStageBlit;

// What a command buffer held of this backend's own objects.
enum {
    HeldKind_Allocator = GpuMetalHeldKind_Backend,
    HeldKind_Table,
};
typedef uint8_t HeldKind;

struct Commands : GpuMetalCommands {
    id<MTL4CommandBuffer> buffer;                      // retained
    id<MTL4CommandAllocator> PR_NULLABLE allocator;    // retained; null for the application's command buffer
    id<MTL4ArgumentTable> table;                       // retained
    id<MTL4ComputeCommandEncoder> PR_NULLABLE compute; // the encoder that takes dispatches and copies, retained
    id<MTL4CommandEncoder> PR_NULLABLE tail;           // the last encoder, open for barriers, retained
};

struct Device : GpuMetalDevice {
    id<MTL4Compiler> compiler;              // retained
    dispatch_queue_t feedback;              // the serial queue of the submissions' feedback, retained
    RawArray<id> allocators;                // free id<MTL4CommandAllocator>s, retained
    RawArray<id> tables;                    // free id<MTL4ArgumentTable>s, retained

    // The open batch of uploads: its command buffer and encoder.
    id<MTL4CommandBuffer> PR_NULLABLE uploads;               // retained
    id<MTL4CommandAllocator> PR_NULLABLE uploadAllocator;    // retained
    id<MTL4ComputeCommandEncoder> PR_NULLABLE uploadEncoder; // retained
};

Device* deviceFrom(PrGpuDevice* device) {
    return static_cast<Device*>(device);
}

Device* deviceFrom(GpuMetalDevice* device) {
    return static_cast<Device*>(device);
}

Commands* commandsFrom(PrGpuCommands* commands) {
    return static_cast<Commands*>(commands);
}

id<MTL4RenderCommandEncoder> passEncoder(GpuMetalPass* pass) {
    return pass->encoder;
}

void heldRecycle(GpuMetalDevice* device, GpuMetalHeld& held) {
    Device* d = deviceFrom(device);
    if (held.kind == HeldKind_Allocator) {
        [static_cast<id<MTL4CommandAllocator>>(held.object) reset];
        d->allocators.push(held.object);
    } else {
        d->tables.push(held.object);
    }
}

// Commits a command buffer, which reaches `fence` once it is done and the functions that wait for it have run. Takes
// the copied `dones`. The caller holds the mutex.
void commit(Device* d, id<MTL4CommandBuffer> buffer, PrGpuFence fence, GpuMetalDone* PR_NULLABLE dones,
        uint32_t doneCount) {
    MTL4CommitOptions* options = [[MTL4CommitOptions new] autorelease];
    Device* owner = d;
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
        double seconds = feedback.error == nil ? feedback.GPUEndTime - feedback.GPUStartTime : 0;
        gpuMetalSubmissionDone(owner, fence, dones, doneCount, feedback.error, seconds);
    }];
    id<MTL4CommandBuffer> list[1] = {buffer};
    [static_cast<id<MTL4CommandQueue>>(d->queue) commit:list count:1 options:options];
}

// ---------------------------------------------------------------------------------------------------------------------
// Uploads

// Opens the batch of uploads if it is not open. It waits for the work before it, which may read what it writes.
void uploadsOpen(GpuMetalDevice* device) {
    Device* d = deviceFrom(device);
    if (d->uploads != nil)
        return;
    if (d->allocators.count != 0) {
        d->uploadAllocator = d->allocators.last();
        d->allocators.pop();
    } else {
        d->uploadAllocator = [d->device newCommandAllocator];
    }
    d->uploads = [d->device newCommandBuffer];
    [d->uploads beginCommandBufferWithAllocator:d->uploadAllocator];
    d->uploads.label = @"uploads";
    d->uploadEncoder = [[d->uploads computeCommandEncoder] retain];
    [d->uploadEncoder barrierAfterQueueStages:kAllStages beforeStages:MTLStageBlit
        visibilityOptions:MTL4VisibilityOptionDevice];
    d->uploadSerial = d->serial++;
    d->inFlight.push(GpuMetalInFlight{d->uploadSerial, 0});
}

// Commits the open batch of uploads, ahead of whatever is submitted after it. Its copies into buffers are disjoint, so
// they go at once.
void uploadsSubmit(GpuMetalDevice* device) {
    Device* d = deviceFrom(device);
    if (d->uploads == nil)
        return;
    gpuMetalEncodeUploadCopies(d, d->uploadEncoder);
    [d->uploadEncoder barrierAfterStages:MTLStageBlit beforeQueueStages:kAllStages
        visibilityOptions:MTL4VisibilityOptionDevice];
    [d->uploadEncoder endEncoding];
    [d->uploadEncoder release];
    d->uploadEncoder = nil;
    [d->uploads endCommandBuffer];
    gpuMetalCommitResidency(d);
    PrGpuFence fence = ++d->submitted;
    uint32_t doneCount = 0;
    GpuMetalDone* dones = gpuMetalUploadsDones(d, &doneCount);
    commit(d, d->uploads, fence, dones, doneCount);
    gpuMetalUploadsRecord(d, fence);
    d->held.push(GpuMetalHeld{.fence = fence, .kind = HeldKind_Allocator, .object = d->uploadAllocator});
    [d->uploads release];
    d->uploads = nil;
    d->uploadAllocator = nil;
}

void uploadsCopyTexture(GpuMetalDevice* device, const GpuMetalTextureCopy& copy) {
    Device* d = deviceFrom(device);
    if (copy.barrier)
        [d->uploadEncoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit
            visibilityOptions:MTL4VisibilityOptionDevice];
    gpuMetalEncodeTextureCopy(d->uploadEncoder, copy);
}

// ---------------------------------------------------------------------------------------------------------------------
// The device

void destroyDevice(PrGpuDevice* device) {
    Device* d = deviceFrom(device);
    @autoreleasepool {
        prMutexLock(&d->mutex);
        gpuMetalCollect(d);
        for (uint32_t i = 0; i < d->allocators.count; ++i)
            [d->allocators.data[i] release];
        for (uint32_t i = 0; i < d->tables.count; ++i)
            [d->tables.data[i] release];
        prMutexUnlock(&d->mutex);
        d->allocators.reset();
        d->tables.reset();
        [d->compiler release];
        dispatch_release(d->feedback);
        gpuMetalDeviceReset(d);
    }
    prMemoryFree(d);
}

// ---------------------------------------------------------------------------------------------------------------------
// Pipelines

// Returns the descriptor of an entry point's function, specialized with the device's function constants.
MTL4FunctionDescriptor* functionDescriptor(const GpuMetalDevice* d, const PrGpuShaderLibrary* library,
        const PrGpuShaderEntry* entry) {
    MTL4LibraryFunctionDescriptor* f = [[MTL4LibraryFunctionDescriptor new] autorelease];
    f.name = prAppleNSString(entry->name);
    f.library = static_cast<const GpuMetalLibrary*>(library)->library;
    MTL4SpecializedFunctionDescriptor* specialized = [[MTL4SpecializedFunctionDescriptor new] autorelease];
    specialized.functionDescriptor = f;
    specialized.constantValues = gpuMetalFunctionConstants(d);
    return specialized;
}

PrStatus graphicsPipelineCreate(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out) {
    Device* d = deviceFrom(device);
    @autoreleasepool {
        MTL4RenderPipelineDescriptor* pd = [[MTL4RenderPipelineDescriptor new] autorelease];
        pd.vertexFunctionDescriptor = functionDescriptor(d, desc->library, vertex);
        if (fragment != nullptr)
            pd.fragmentFunctionDescriptor = functionDescriptor(d, desc->library, fragment);
        pd.rasterSampleCount = desc->formats.samples;
        pd.inputPrimitiveTopology = gpuMetalTopologyClass(desc->topology);
        pd.maxVertexAmplificationCount = gpuMetalAmplification(d, desc->formats.viewCount);
        if (desc->name.size != 0)
            pd.label = prAppleNSString(desc->name);
        for (uint8_t i = 0; i < desc->formats.colorCount; ++i) {
            MTL4RenderPipelineColorAttachmentDescriptor* a = pd.colorAttachments[i];
            const PrGpuBlend& blend = desc->blend[i];
            a.blendingState = blend.enabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
            gpuMetalColorAttachmentFill(a, desc->formats.color[i], blend);
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> state =
            [d->compiler newRenderPipelineStateWithDescriptor:pd compilerTaskOptions:nil error:&error];
        if (state == nil) {
            NSLog(@"lib/gpu: Metal made no pipeline for %@: %@", pd.vertexFunctionDescriptor, error);
            return PrStatus_NotSupported;
        }
        GpuMetalPipeline p = gpuMetalPipelineFromDesc(d, desc);
        p.state = state;
        uint32_t id = gpuMetalPipelineAdd(d, p);
        if (id == 0) {
            [p.state release];
            [p.depthStencil release];
            return PrStatus_Limit;
        }
        out->id = id;
        return PrStatus_Ok;
    }
}

PrStatus computePipelineCreate(PrGpuDevice* device, const PrGpuShaderLibrary* library, const PrGpuShaderEntry* compute,
        PrStringView name, PrGpuPipeline* out) {
    Device* d = deviceFrom(device);
    @autoreleasepool {
        MTL4ComputePipelineDescriptor* pd = [[MTL4ComputePipelineDescriptor new] autorelease];
        pd.computeFunctionDescriptor = functionDescriptor(d, library, compute);
        if (name.size != 0)
            pd.label = prAppleNSString(name);
        NSError* error = nil;
        id<MTLComputePipelineState> state = [d->compiler newComputePipelineStateWithDescriptor:pd
            compilerTaskOptions:nil error:&error];
        if (state == nil) {
            NSLog(@"lib/gpu: Metal made no compute pipeline: %@", error);
            return PrStatus_NotSupported;
        }
        GpuMetalPipeline p{};
        p.compute = true;
        p.state = state;
        p.groupSize = MTLSizeMake(compute->groupSize.width, compute->groupSize.height, compute->groupSize.depth);
        uint32_t id = gpuMetalPipelineAdd(d, p);
        if (id == 0) {
            [state release];
            return PrStatus_Limit;
        }
        out->id = id;
        return PrStatus_Ok;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Command buffers

// Ends the last encoder, which stays open for the barriers after it until another encoder begins.
void endTail(Commands* c) {
    if (c->compute != nil) {
        [c->compute endEncoding];
        [c->compute release];
        c->compute = nil;
    }
    if (c->tail != nil) {
        [c->tail endEncoding];
        [c->tail release];
        c->tail = nil;
    }
}

// Returns the encoder of dispatches and copies, begun after the last barrier.
id<MTL4ComputeCommandEncoder> computeEncoder(Commands* c) {
    if (c->compute == nil) {
        endTail(c);
        c->compute = [[c->buffer computeCommandEncoder] retain];
        [c->compute setArgumentTable:c->table];
    }
    return c->compute;
}

// Unbinds every slot in the command buffer's argument table, which an earlier command buffer may have bound.
void clearSlots(Commands* c) {
    for (uint32_t i = GpuMetalBinding_Buffers; i < GpuMetalBinding_View; ++i)
        [c->table setAddress:0 atIndex:i];
    for (uint32_t i = 0; i < GpuMetalTextureBinding_Count; ++i)
        [c->table setTexture:MTLResourceID{} atIndex:i];
    for (uint32_t i = 0; i < kGpuMetalTextureSlots; ++i)
        [c->table setSamplerState:MTLResourceID{} atIndex:i];
}

bool commandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* options, PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    Device* d = deviceFrom(device);
    @autoreleasepool {
        Commands* c = allocZeroed<Commands>();
        prMutexLock(&d->mutex);
        gpuMetalCollect(d);
        gpuMetalCommandsInit(d, c);
        if (d->tables.count != 0) {
            c->table = d->tables.last();
            d->tables.pop();
        }
        if (options->native == nullptr && d->allocators.count != 0) {
            c->allocator = d->allocators.last();
            d->allocators.pop();
        }
        prMutexUnlock(&d->mutex);
        if (c->table == nil) {
            MTL4ArgumentTableDescriptor* td = [[MTL4ArgumentTableDescriptor new] autorelease];
            td.maxBufferBindCount = GpuMetalBinding_Count;
            td.maxTextureBindCount = GpuMetalTextureBinding_Count;
            td.maxSamplerStateBindCount = kGpuMetalTextureSlots;
            c->table = [d->device newArgumentTableWithDescriptor:td error:nil];
        }
        [c->table setAddress:d->resourceHeap.gpuAddress atIndex:GpuMetalBinding_Resources];
        [c->table setAddress:d->samplerHeap.gpuAddress atIndex:GpuMetalBinding_Samplers];
        clearSlots(c);
        if (options->native != nullptr) {
            c->native = true;
            c->buffer = [static_cast<id<MTL4CommandBuffer>>(options->native) retain];
        } else {
            if (c->allocator == nil)
                c->allocator = [d->device newCommandAllocator];
            c->buffer = [d->device newCommandBuffer];
            [c->buffer beginCommandBufferWithAllocator:c->allocator];
        }
        *out = c;
        return true;
    }
}

// Copies root data into the stream, and points the argument table's binding at it.
void setRoot(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size, uint32_t binding) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    Commands* c = commandsFrom(commands);
    uint32_t slot = 0;
    uint64_t offset = 0;
    void* bytes = gpuMetalStream(d, c, size != 0 ? size : 16, &slot, &offset);
    prMemCopy(bytes, data, size);
    if (slot != 0)
        [c->table setAddress:gpuMetalBuffer(d, slot).gpuAddress + offset atIndex:binding];
}

void commandsSetPush(PrGpuCommands* commands, const void* data, uint32_t size) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    setRoot(device, commands, data, size, GpuMetalBinding_Push);
}

void commandsSetUniforms(PrGpuCommands* commands, const void* data, uint32_t size) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    setRoot(device, commands, data, size, GpuMetalBinding_Uniforms);
}

// The slots are entries of the argument table, which Metal snapshots at each draw and dispatch. An unbound entry is
// zero.
uint64_t bufferAddress(const GpuMetalDevice* d, PrGpuBuffer buffer, uint64_t offset) {
    return buffer.id != 0 ? gpuMetalBuffer(d, buffer.id).gpuAddress + offset : 0;
}

MTLResourceID textureID(const GpuMetalDevice* d, PrGpuTexture texture) {
    return texture.id != 0 ? gpuMetalTexture(d, texture.id).gpuResourceID : MTLResourceID{};
}

void commandsSetBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    Commands* c = commandsFrom(commands);
    const GpuMetalDevice* d = gpuMetalDeviceFrom(c->device);
    uint32_t index = gpuMetalSlotsSetBuffer(d, c, slot, buffer, offset);
    [c->table setAddress:bufferAddress(d, buffer, offset) atIndex:index];
}

void commandsSetRwBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    Commands* c = commandsFrom(commands);
    const GpuMetalDevice* d = gpuMetalDeviceFrom(c->device);
    uint32_t index = gpuMetalSlotsSetRwBuffer(d, c, slot, buffer, offset);
    [c->table setAddress:bufferAddress(d, buffer, offset) atIndex:index];
}

void commandsSetTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture, PrGpuSampler sampler) {
    Commands* c = commandsFrom(commands);
    const GpuMetalDevice* d = gpuMetalDeviceFrom(c->device);
    uint32_t index = gpuMetalSlotsSetTexture(d, c, slot, texture, sampler);
    [c->table setTexture:textureID(d, texture) atIndex:index];
    MTLResourceID samplerID = sampler.id != 0 ? d->samplers[sampler.id].gpuResourceID : MTLResourceID{};
    [c->table setSamplerState:samplerID atIndex:slot];
}

void commandsSetRwTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture) {
    Commands* c = commandsFrom(commands);
    const GpuMetalDevice* d = gpuMetalDeviceFrom(c->device);
    uint32_t index = gpuMetalSlotsSetRwTexture(d, c, slot, texture);
    [c->table setTexture:textureID(d, texture) atIndex:index];
}

void commandsDispatch(PrGpuCommands* commands, PrGpuExtent groups) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    Commands* c = commandsFrom(commands);
    const GpuMetalPipeline& p = gpuMetalDeviceFrom(device)->pipelines[c->pipeline];
    PR_ASSERT(p.compute);
    id<MTL4ComputeCommandEncoder> e = computeEncoder(c);
    [e setComputePipelineState:p.state];
    [e dispatchThreadgroups:MTLSizeMake(groups.width, groups.height, groups.depth) threadsPerThreadgroup:p.groupSize];
}

void commandsDispatchIndirect(PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    Commands* c = commandsFrom(commands);
    const GpuMetalPipeline& p = d->pipelines[c->pipeline];
    id<MTL4ComputeCommandEncoder> e = computeEncoder(c);
    [e setComputePipelineState:p.state];
    [e dispatchThreadgroupsWithIndirectBuffer:gpuMetalBuffer(d, buffer.id).gpuAddress + offset
        threadsPerThreadgroup:p.groupSize];
}

void commandsCopyBuffer(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        PrGpuBuffer target, uint64_t targetOffset, uint64_t size) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    [computeEncoder(commandsFrom(commands)) copyFromBuffer:gpuMetalBuffer(d, source.id) sourceOffset:sourceOffset
        toBuffer:gpuMetalBuffer(d, target.id) destinationOffset:targetOffset size:size];
}

void commandsCopyTexture(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    gpuMetalEncodeCopyTexture(gpuMetalDeviceFrom(device), computeEncoder(commandsFrom(commands)), source, sourceRegion,
        target, targetRegion);
}

void commandsCopyBufferToTexture(PrGpuCommands* commands, PrGpuBuffer source,
        uint64_t sourceOffset, uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target,
        const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    gpuMetalEncodeCopyBufferToTexture(gpuMetalDeviceFrom(device), computeEncoder(commandsFrom(commands)), source,
        sourceOffset, rowBytes, layerBytes, target, targetRegion);
}

void commandsCopyTextureToBuffer(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    gpuMetalEncodeCopyTextureToBuffer(gpuMetalDeviceFrom(device), computeEncoder(commandsFrom(commands)), source,
        sourceRegion, target, targetOffset, rowBytes, layerBytes);
}

void commandsPushGroup(PrGpuCommands* commands, PrStringView name) {
    @autoreleasepool {
        [commandsFrom(commands)->buffer pushDebugGroup:prAppleNSString(name)];
    }
}

void commandsPopGroup(PrGpuCommands* commands) {
    [commandsFrom(commands)->buffer popDebugGroup];
}

// Holds the stages of every later encoder that `after` names until the stages that `before` names of every earlier
// encoder are done.
void barrier(Commands* c, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    MTLStages wait = gpuMetalStages(before);
    MTLStages hold = gpuMetalStages(after);
    if (wait == 0 || hold == 0)
        return;
    if (c->compute != nil) {
        c->tail = c->compute;
        c->compute = nil;
    }
    if (c->tail == nil)
        c->tail = [[c->buffer computeCommandEncoder] retain];
    [c->tail barrierAfterStages:wait beforeQueueStages:hold visibilityOptions:MTL4VisibilityOptionDevice];
}

void commandsBufferBarrier(PrGpuCommands* commands, PrGpuBuffer, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    barrier(commandsFrom(commands), before, after);
}

void commandsTextureBarrier(PrGpuCommands* commands, PrGpuTexture, PrGpuSubresources,
        PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    barrier(commandsFrom(commands), before, after);
}

void commandsGlobalBarrier(PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    barrier(commandsFrom(commands), before, after);
}

void commandsGenerateMipmaps(PrGpuCommands* commands, PrGpuTexture texture, PrGpuAccessFlags before) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    Commands* c = commandsFrom(commands);
    barrier(c, before, PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite);
    [computeEncoder(c) generateMipmapsForTexture:gpuMetalTexture(gpuMetalDeviceFrom(device), texture.id)];
    barrier(c, PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_ShaderRead);
}

PrGpuNativeStateFlags nativeStateSync(PrGpuCommands* commands) {
    Commands* c = commandsFrom(commands);
    @autoreleasepool {
        if (c->pass == nullptr)
            endTail(c);
    }
    PrGpuNativeStateFlags changed = c->changed;
    c->changed = 0;
    return changed;
}

PrGpuFence commandsSubmit(PrGpuCommands* commands) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    gpuSharedSubmitted(c);
    @autoreleasepool {
        endTail(c);
        if (!c->native)
            [c->buffer endCommandBuffer];
        uint32_t doneCount = c->dones.count;
        GpuMetalDone* dones = gpuMetalDonesCopy(c);
        id<MTL4CommandQueue> queue = d->queue;
        prMutexLock(&d->mutex);
        uploadsSubmit(d);
        gpuMetalCommitResidency(d);
        for (uint32_t i = 0; i < c->drawables.count; ++i)
            [queue waitForDrawable:c->drawables.data[i].drawable];
        PrGpuFence fence = ++d->submitted;
        commit(d, c->buffer, fence, dones, doneCount);
        for (uint32_t i = 0; i < c->drawables.count; ++i) {
            id<CAMetalDrawable> drawable = c->drawables.data[i].drawable;
            [queue signalDrawable:drawable];
            [drawable present];
            [drawable release];
        }
        gpuMetalSubmissionRecord(d, c, fence);
        if (c->allocator != nil)
            d->held.push(GpuMetalHeld{.fence = fence, .kind = HeldKind_Allocator, .object = c->allocator});
        d->held.push(GpuMetalHeld{.fence = fence, .kind = HeldKind_Table, .object = c->table});
        prMutexUnlock(&d->mutex);
        [c->buffer release];
        gpuMetalCommandsDestroy(c);
        return fence;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes

// A texture's native object is its id<MTLTexture>.
bool textureNative(PrGpuDevice* device, PrGpuTexture texture, PrGpuNativeKind, void* out) {
    *static_cast<void**>(out) = gpuMetalTexture(gpuMetalDeviceFrom(device), texture.id);
    return true;
}

// Points the argument table at the view base of the draws after the call (GpuMetalView).
void setView(Commands* c, GpuMetalPass* pass, uint32_t view) {
    [c->table setAddress:pass->viewBases + view * kGpuMetalStreamAlignment atIndex:GpuMetalBinding_View];
}

// Writes the view bases of a pass into the stream: one for each view in a pass that draws each view on its own, and
// otherwise one of zero.
void writeViews(GpuMetalDevice* d, Commands* c, GpuMetalPass* pass) {
    uint32_t count = pass->eachView ? pass->formats.viewCount : 1;
    uint32_t slot = 0;
    uint64_t offset = 0;
    uint8_t* bytes = static_cast<uint8_t*>(gpuMetalStream(d, c, count * kGpuMetalStreamAlignment, &slot, &offset));
    for (uint32_t i = 0; i < count; ++i) {
        GpuMetalView base{i, i};
        prMemCopy(bytes + i * kGpuMetalStreamAlignment, &base, sizeof(base));
    }
    pass->viewBases = slot != 0 ? gpuMetalBuffer(d, slot).gpuAddress + offset : 0;
}

// Records a draw once, or, in a pass that draws each view on its own, once for each view.
template <typename Draw>
void eachView(GpuMetalPass* pass, Draw draw) {
    if (!pass->eachView) {
        draw();
        return;
    }
    Commands* c = commandsFrom(pass->commands);
    for (uint8_t i = 0; i < pass->formats.viewCount; ++i) {
        setView(c, pass, i);
        draw();
    }
    setView(c, pass, 0);
}

// Binds the device's argument table in the pass's encoder, with the pass's view base, and, in a pass of the device's
// own, the viewport and scissor over the whole attachments.
void passBind(Commands* c, GpuMetalPass* pass) {
    id<MTL4RenderCommandEncoder> encoder = passEncoder(pass);
    gpuMetalPassViewsBegin(gpuMetalDeviceFrom(c->device), encoder, pass);
    setView(c, pass, 0);
    [encoder setArgumentTable:c->table atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    if (!pass->native) {
        [encoder setViewport:MTLViewport{0, 0, double(pass->extent.x), double(pass->extent.y), 0, 1}];
        [encoder setScissorRect:MTLScissorRect{0, 0, pass->extent.x, pass->extent.y}];
    }
}

PrGpuPass* passBegin(PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    Commands* c = commandsFrom(commands);
    gpuSharedPassBegan(c);
    @autoreleasepool {
        GpuMetalPass* pass = allocZeroed<GpuMetalPass>();
        static_cast<PrGpuPass&>(*pass) = kPassFunctions;
        pass->commands = c;
        if (desc->native != nullptr) {
            pass->native = true;
            pass->formats = desc->nativeFormats;
            pass->extent = desc->nativeExtent;
            pass->encoder = [static_cast<id<MTL4RenderCommandEncoder>>(desc->native) retain];
            c->changed |= PrGpuNativeStateFlag_Bindings;
        } else {
            MTL4RenderPassDescriptor* rp = [[MTL4RenderPassDescriptor new] autorelease];
            gpuMetalPassAttachmentsFill(d, rp, desc, pass);
            endTail(c);
            pass->encoder = [[c->buffer renderCommandEncoderWithDescriptor:rp] retain];
            if (desc->name.size != 0)
                [pass->encoder setLabel:prAppleNSString(desc->name)];
        }
        pass->eachView = pass->formats.viewCount > 1 && !gpuMetalAmplifies(d, pass->formats.viewCount);
        writeViews(d, c, pass);
        passBind(c, pass);
        c->pass = pass;
        c->pipeline = 0;
        return pass;
    }
}

// Hands the pass's encoder to a native draw, then binds the device's argument table again, and leaves the pass's
// pipeline and slots unset. An application's pass reports every state as changed, since the native draw may have
// changed any of it.
void passNativeDraw(PrGpuPass* handle, PrGpuNativeKind, PrGpuNativeDrawFn draw, void* PR_NULLABLE context) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    Commands* c = commandsFrom(pass->commands);
    @autoreleasepool {
        PrGpuMetal4NativePass native{.encoder = pass->encoder, .commandBuffer = c->buffer};
        draw(context, &native);
        clearSlots(c);
        passBind(c, pass);
        c->pipeline = 0;
        gpuMetalPassChanged(pass, PrGpuNativeStateFlags(~0u));
    }
}

void passEnd(PrGpuPass* handle) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    Commands* c = static_cast<Commands*>(pass->commands);
    gpuSharedPassEnded(c);
    c->pass = nullptr;
    if (pass->native) {
        [pass->encoder release];
    } else {
        // The pass's encoder stays open for the barriers after it.
        c->tail = pass->encoder;
    }
    prMemoryFree(pass);
}

const GpuMetalPipeline& passPipeline(PrGpuDevice* device, GpuMetalPass* pass) {
    return gpuMetalDeviceFrom(device)->pipelines[gpuMetalCommandsFrom(pass->commands)->pipeline];
}

void passDraw(PrGpuPass* handle, uint32_t vertexCount, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    eachView(pass, [&] {
        [passEncoder(pass) drawPrimitives:passPipeline(device, pass).primitive vertexStart:firstVertex
            vertexCount:vertexCount instanceCount:instanceCount baseInstance:firstInstance];
    });
}

void passDrawIndexed(PrGpuPass* handle, uint32_t indexCount, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    id<MTLBuffer> indices = gpuMetalBuffer(d, pass->indexBuffer.id);
    uint64_t at = pass->indexOffset + uint64_t(firstIndex) * (pass->indexType == MTLIndexTypeUInt32 ? 4 : 2);
    eachView(pass, [&] {
        [passEncoder(pass) drawIndexedPrimitives:passPipeline(device, pass).primitive indexCount:indexCount
            indexType:pass->indexType indexBuffer:indices.gpuAddress + at indexBufferLength:indices.length - at
            instanceCount:instanceCount baseVertex:vertexOffset baseInstance:firstInstance];
    });
}

void passDrawIndirect(PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    uint64_t address = gpuMetalBuffer(d, buffer.id).gpuAddress + offset;
    eachView(pass, [&] {
        for (uint32_t i = 0; i < count; ++i)
            [passEncoder(pass) drawPrimitives:passPipeline(device, pass).primitive indirectBuffer:address + i * stride];
    });
}

void passDrawIndexedIndirect(PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    id<MTLBuffer> indices = gpuMetalBuffer(d, pass->indexBuffer.id);
    uint64_t address = gpuMetalBuffer(d, buffer.id).gpuAddress + offset;
    eachView(pass, [&] {
        for (uint32_t i = 0; i < count; ++i)
            [passEncoder(pass) drawIndexedPrimitives:passPipeline(device, pass).primitive indexType:pass->indexType
                indexBuffer:indices.gpuAddress + pass->indexOffset indexBufferLength:indices.length - pass->indexOffset
                indirectBuffer:address + i * stride];
    });
}

// On macOS 27, the compositor skips each present with the transaction of a drawable that an MTL4 queue signalled,
// except during a live resize. A frame presents with the transaction only there, which is where it keeps the drawable
// in step with the window's size.
PrStatus swapChainAcquire(PrGpuSwapChain* handle, PrUVec2 extent, PrGpuSwapChainFrame* out) {
    GpuMetalSwapChain* sc = static_cast<GpuMetalSwapChain*>(handle);
    if (sc->transactional && sc->layer.presentsWithTransaction != sc->view.inLiveResize)
        sc->layer.presentsWithTransaction = sc->view.inLiveResize;
    return gpuMetalSwapChainAcquire(handle, extent, out);
}

// The functions of the device's objects.
constexpr PrGpuCommands kCommandsFunctions = {
    .deviceFn = gpuSharedCommandsDevice,
    .submitFn = commandsSubmit,
    .streamFn = gpuMetalCommandsStream,
    .setPushFn = commandsSetPush,
    .setUniformsFn = commandsSetUniforms,
    .setPipelineFn = gpuMetalCommandsSetPipeline,
    .setBufferFn = commandsSetBuffer,
    .setRwBufferFn = commandsSetRwBuffer,
    .setTextureFn = commandsSetTexture,
    .setRwTextureFn = commandsSetRwTexture,
    .dispatchFn = commandsDispatch,
    .dispatchIndirectFn = commandsDispatchIndirect,
    .copyBufferFn = commandsCopyBuffer,
    .copyTextureFn = commandsCopyTexture,
    .copyBufferToTextureFn = commandsCopyBufferToTexture,
    .copyTextureToBufferFn = commandsCopyTextureToBuffer,
    .generateMipmapsFn = commandsGenerateMipmaps,
    .onDoneFn = gpuMetalCommandsOnDone,
    .pushGroupFn = commandsPushGroup,
    .popGroupFn = commandsPopGroup,
    .bufferBarrierFn = commandsBufferBarrier,
    .textureBarrierFn = commandsTextureBarrier,
    .globalBarrierFn = commandsGlobalBarrier,
    .nativeStateSyncFn = nativeStateSync,
    .passBeginFn = passBegin,
};

constexpr PrGpuPass kPassFunctions = {
    .commandsFn = gpuSharedPassCommands,
    .formatsFn = gpuSharedPassFormats,
    .extentFn = gpuSharedPassExtent,
    .endFn = passEnd,
    .nativeDrawFn = passNativeDraw,
    .setViewportFn = gpuMetalPassSetViewport,
    .setScissorFn = gpuMetalPassSetScissor,
    .setBlendConstantFn = gpuMetalPassSetBlendConstant,
    .setStencilReferenceFn = gpuMetalPassSetStencilReference,
    .setIndexBufferFn = gpuMetalPassSetIndexBuffer,
    .drawFn = passDraw,
    .drawIndexedFn = passDrawIndexed,
    .drawIndirectFn = passDrawIndirect,
    .drawIndexedIndirectFn = passDrawIndexedIndirect,
};

constexpr PrGpuSwapChain kSwapChainFunctions = {
    .deviceFn = gpuSharedSwapChainDevice,
    .infoFn = gpuSharedSwapChainInfo,
    .destroyFn = gpuMetalSwapChainDestroy,
    .waitFn = gpuMetalSwapChainWait,
    .acquireFn = swapChainAcquire,
    .presentFn = gpuMetalSwapChainPresent,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrGpuMetal4Options prGpuMetal4OptionsDefault(void) {
    return PrGpuMetal4Options{};
}

PrStatus prGpuMetal4Create(const PrGpuMetal4Options* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuMetal4Options o = options != nullptr ? *options : prGpuMetal4OptionsDefault();
    @autoreleasepool {
        id<MTLDevice> device;
        if (o.device != nullptr)
            device = [static_cast<id<MTLDevice>>(o.device) retain];
        else
            device = MTLCreateSystemDefaultDevice();
        if (device == nil)
            return PrStatus_NotSupported;
        if (![device supportsFamily:MTLGPUFamilyMetal4]) {
            [device release];
            return PrStatus_NotSupported;
        }
        Device* d = allocZeroed<Device>();
        gpuMetalDeviceInit(d, device, true);
        d->feedback = dispatch_queue_create("lib/gpu feedback", DISPATCH_QUEUE_SERIAL);
        if (o.queue != nullptr) {
            gpuMetalDeviceAddQueue(d, [static_cast<id<MTL4CommandQueue>>(o.queue) retain]);
        } else {
            // The descriptor releases its queue, though its property does not say that it retains it.
            MTL4CommandQueueDescriptor* qd = [[MTL4CommandQueueDescriptor new] autorelease];
            dispatch_retain(d->feedback);
            qd.feedbackQueue = d->feedback;
            gpuMetalDeviceAddQueue(d, [device newMTL4CommandQueueWithDescriptor:qd error:nil]);
        }
        d->compiler = [device newCompilerWithDescriptor:[[MTL4CompilerDescriptor new] autorelease] error:nil];
        d->uploadsOpen = uploadsOpen;
        d->uploadsSubmit = uploadsSubmit;
        d->uploadsCopyTexture = uploadsCopyTexture;
        d->heldRecycle = heldRecycle;
        d->destroyFn = destroyDevice;
        d->graphicsPipelineCreateFn = graphicsPipelineCreate;
        d->computePipelineCreateFn = computePipelineCreate;
        d->commandsBeginFn = commandsBegin;
        d->commandsFunctions = &kCommandsFunctions;
        d->swapChainFunctions = &kSwapChainFunctions;
        d->textureNativeFn = textureNative;
        d->info.backend = "Metal 4";
        d->nativeKinds = 1u << PrGpuNativeKind_Metal4;
        *out = d;
        return PrStatus_Ok;
    }
}

void* prGpuMetal4Device(const PrGpuDevice* device) {
    return gpuMetalDeviceFrom(device)->device;
}

void* prGpuMetal4Queue(const PrGpuDevice* device) {
    return gpuMetalDeviceFrom(device)->queue;
}

void* prGpuMetal4ResidencySet(const PrGpuDevice* device) {
    return gpuMetalDeviceFrom(device)->residency;
}

PrStatus prGpuMetal4TextureWrap(PrGpuDevice* device, void* texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out) {
    return gpuMetalTextureWrap(gpuMetalDeviceFrom(device), static_cast<id<MTLTexture>>(texture), usage, out);
}
