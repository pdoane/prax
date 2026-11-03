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
#include "pr/assert.h"
#include "pr/mem.h"

#include "gpu/backends/shared/src/gpu_shared_metal.h"

namespace pr_impl {

namespace {

constexpr MTLStages kAllStages = MTLStageVertex | MTLStageFragment | MTLStageDispatch | MTLStageBlit;

// What a command buffer held of this backend's own objects.
enum {
    HeldKind_Allocator = GpuMetalHeldKind_Backend,
    HeldKind_Table,
};
typedef uint8_t HeldKind;

struct Commands : GpuMetalCommands {
    id<MTL4CommandBuffer> buffer;                      // retained
    id<MTL4CommandAllocator> PR_NULLABLE allocator;    // retained; null for the host's command buffer
    id<MTL4ArgumentTable> table;                       // retained
    id<MTL4ComputeCommandEncoder> PR_NULLABLE compute; // the encoder that takes dispatches and copies, retained
    id<MTL4CommandEncoder> PR_NULLABLE tail;           // the last encoder, open for barriers, retained
};

struct Device : GpuMetalDevice {
    id<MTL4Compiler> compiler;              // retained
    dispatch_queue_t feedback;              // the serial queue of the submissions' feedback, retained
    Array<id> allocators;                   // free id<MTL4CommandAllocator>s, retained
    Array<id> tables;                       // free id<MTL4ArgumentTable>s, retained

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
    commit(d, d->uploads, fence, nullptr, 0);
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

MTL4LibraryFunctionDescriptor* functionDescriptor(const PrGpuShaderLibrary* library, const PrGpuShaderEntry* entry) {
    MTL4LibraryFunctionDescriptor* f = [[MTL4LibraryFunctionDescriptor new] autorelease];
    f.name = gpuMetalString(entry->name);
    f.library = static_cast<const GpuMetalLibrary*>(library)->library;
    return f;
}

PrStatus graphicsPipelineCreate(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out) {
    Device* d = deviceFrom(device);
    @autoreleasepool {
        MTL4RenderPipelineDescriptor* pd = [[MTL4RenderPipelineDescriptor new] autorelease];
        pd.vertexFunctionDescriptor = functionDescriptor(desc->library, vertex);
        if (fragment != nullptr)
            pd.fragmentFunctionDescriptor = functionDescriptor(desc->library, fragment);
        pd.rasterSampleCount = desc->formats.samples;
        pd.inputPrimitiveTopology = gpuMetalTopologyClass(desc->topology);
        if (desc->name.size != 0)
            pd.label = gpuMetalString(desc->name);
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
        pd.computeFunctionDescriptor = functionDescriptor(library, compute);
        if (name.size != 0)
            pd.label = gpuMetalString(name);
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
            c->table = [d->device newArgumentTableWithDescriptor:td error:nil];
        }
        [c->table setAddress:d->resourceHeap.gpuAddress atIndex:GpuMetalBinding_Resources];
        [c->table setAddress:d->samplerHeap.gpuAddress atIndex:GpuMetalBinding_Samplers];
        if (options->native != nullptr) {
            c->host = true;
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
    memCopy(bytes, data, size);
    if (slot != 0)
        [c->table setAddress:gpuMetalBuffer(d, slot).gpuAddress + offset atIndex:binding];
}

void commandsSetPush(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size) {
    setRoot(device, commands, data, size, GpuMetalBinding_Push);
}

void commandsSetUniforms(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size) {
    setRoot(device, commands, data, size, GpuMetalBinding_Uniforms);
}

void commandsDispatch(PrGpuDevice* device, PrGpuCommands* commands, PrGpuExtent groups) {
    Commands* c = commandsFrom(commands);
    const GpuMetalPipeline& p = gpuMetalDeviceFrom(device)->pipelines[c->pipeline];
    PR_ASSERT(p.compute);
    id<MTL4ComputeCommandEncoder> e = computeEncoder(c);
    [e setComputePipelineState:p.state];
    [e dispatchThreadgroups:MTLSizeMake(groups.width, groups.height, groups.depth) threadsPerThreadgroup:p.groupSize];
}

void commandsDispatchIndirect(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    Commands* c = commandsFrom(commands);
    const GpuMetalPipeline& p = d->pipelines[c->pipeline];
    id<MTL4ComputeCommandEncoder> e = computeEncoder(c);
    [e setComputePipelineState:p.state];
    [e dispatchThreadgroupsWithIndirectBuffer:gpuMetalBuffer(d, buffer.id).gpuAddress + offset
        threadsPerThreadgroup:p.groupSize];
}

void commandsCopyBuffer(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        PrGpuBuffer target, uint64_t targetOffset, uint64_t size) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    [computeEncoder(commandsFrom(commands)) copyFromBuffer:gpuMetalBuffer(d, source.id) sourceOffset:sourceOffset
        toBuffer:gpuMetalBuffer(d, target.id) destinationOffset:targetOffset size:size];
}

void commandsCopyTexture(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuMetalEncodeCopyTexture(gpuMetalDeviceFrom(device), computeEncoder(commandsFrom(commands)), source, sourceRegion,
        target, targetRegion);
}

void commandsCopyBufferToTexture(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer source,
        uint64_t sourceOffset, uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target,
        const PrGpuTextureRegion* targetRegion) {
    gpuMetalEncodeCopyBufferToTexture(gpuMetalDeviceFrom(device), computeEncoder(commandsFrom(commands)), source,
        sourceOffset, rowBytes, layerBytes, target, targetRegion);
}

void commandsCopyTextureToBuffer(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes) {
    gpuMetalEncodeCopyTextureToBuffer(gpuMetalDeviceFrom(device), computeEncoder(commandsFrom(commands)), source,
        sourceRegion, target, targetOffset, rowBytes, layerBytes);
}

void commandsPushGroup(PrGpuDevice*, PrGpuCommands* commands, PrStringView name) {
    @autoreleasepool {
        [commandsFrom(commands)->buffer pushDebugGroup:gpuMetalString(name)];
    }
}

void commandsPopGroup(PrGpuDevice*, PrGpuCommands* commands) {
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

void commandsBufferBarrier(PrGpuDevice*, PrGpuCommands* commands, PrGpuBuffer, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    barrier(commandsFrom(commands), before, after);
}

void commandsTextureBarrier(PrGpuDevice*, PrGpuCommands* commands, PrGpuTexture, PrGpuSubresources,
        PrGpuAccessFlags before, PrGpuAccessFlags after) {
    barrier(commandsFrom(commands), before, after);
}

void commandsGlobalBarrier(PrGpuDevice*, PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    barrier(commandsFrom(commands), before, after);
}

void commandsGenerateMipmaps(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture texture, PrGpuAccessFlags before) {
    Commands* c = commandsFrom(commands);
    barrier(c, before, PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite);
    [computeEncoder(c) generateMipmapsForTexture:gpuMetalTexture(gpuMetalDeviceFrom(device), texture.id)];
    barrier(c, PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_ShaderRead);
}

PrGpuHostStateFlags hostStateSync(PrGpuDevice*, PrGpuCommands* commands) {
    Commands* c = commandsFrom(commands);
    @autoreleasepool {
        if (c->pass == nullptr)
            endTail(c);
    }
    PrGpuHostStateFlags changed = c->changed;
    c->changed = 0;
    return changed;
}

PrGpuFence commandsSubmit(PrGpuDevice* device, PrGpuCommands* commands) {
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    @autoreleasepool {
        endTail(c);
        if (!c->host)
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

PrGpuPass* passBegin(PrGpuDevice* device, PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    Commands* c = commandsFrom(commands);
    @autoreleasepool {
        GpuMetalPass* pass = allocZeroed<GpuMetalPass>();
        pass->commands = commands;
        if (desc->host != nullptr) {
            pass->host = true;
            pass->formats = desc->hostFormats;
            pass->extent = desc->hostExtent;
            pass->encoder = [static_cast<id<MTL4RenderCommandEncoder>>(desc->host) retain];
            c->changed |= PrGpuHostStateFlag_Bindings;
        } else {
            MTL4RenderPassDescriptor* rp = [[MTL4RenderPassDescriptor new] autorelease];
            gpuMetalPassAttachmentsFill(d, rp, desc, pass);
            endTail(c);
            pass->encoder = [[c->buffer renderCommandEncoderWithDescriptor:rp] retain];
            if (desc->name.size != 0)
                [pass->encoder setLabel:gpuMetalString(desc->name)];
        }
        id<MTL4RenderCommandEncoder> encoder = passEncoder(pass);
        [encoder setArgumentTable:c->table atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        if (!pass->host) {
            [encoder setViewport:MTLViewport{0, 0, double(pass->extent.x), double(pass->extent.y), 0, 1}];
        }
        c->pass = pass;
        c->pipeline = 0;
        return pass;
    }
}

void passEnd(PrGpuDevice*, PrGpuPass* handle) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    Commands* c = static_cast<Commands*>(pass->commands);
    c->pass = nullptr;
    if (pass->host) {
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

void passDraw(PrGpuDevice* device, PrGpuPass* handle, uint32_t vertexCount, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    [passEncoder(pass) drawPrimitives:passPipeline(device,
        pass).primitive vertexStart:firstVertex vertexCount:vertexCount
        instanceCount:instanceCount baseInstance:firstInstance];
}

void passDrawIndexed(PrGpuDevice* device, PrGpuPass* handle, uint32_t indexCount, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    id<MTLBuffer> indices = gpuMetalBuffer(d, pass->indexBuffer.id);
    uint64_t at = pass->indexOffset + uint64_t(firstIndex) * (pass->indexType == MTLIndexTypeUInt32 ? 4 : 2);
    [passEncoder(pass) drawIndexedPrimitives:passPipeline(device, pass).primitive indexCount:indexCount
        indexType:pass->indexType indexBuffer:indices.gpuAddress + at indexBufferLength:indices.length - at
        instanceCount:instanceCount baseVertex:vertexOffset baseInstance:firstInstance];
}

void passDrawIndirect(PrGpuDevice* device, PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, uint32_t count,
        uint32_t stride) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    uint64_t address = gpuMetalBuffer(d, buffer.id).gpuAddress + offset;
    for (uint32_t i = 0; i < count; ++i)
        [passEncoder(pass) drawPrimitives:passPipeline(device, pass).primitive indirectBuffer:address + i * stride];
}

void passDrawIndexedIndirect(PrGpuDevice* device, PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset,
        uint32_t count, uint32_t stride) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    id<MTLBuffer> indices = gpuMetalBuffer(d, pass->indexBuffer.id);
    uint64_t address = gpuMetalBuffer(d, buffer.id).gpuAddress + offset;
    for (uint32_t i = 0; i < count; ++i)
        [passEncoder(pass) drawIndexedPrimitives:passPipeline(device, pass).primitive indexType:pass->indexType
            indexBuffer:indices.gpuAddress + pass->indexOffset indexBufferLength:indices.length - pass->indexOffset
            indirectBuffer:address + i * stride];
}

// On macOS 27, the compositor skips each present with the transaction of a drawable that an MTL4 queue signalled,
// except during a live resize. A frame presents with the transaction only there, which is where it keeps the drawable
// in step with the window's size.
PrStatus swapChainAcquire(PrGpuDevice* device, PrGpuSwapChain* handle, PrUVec2 extent, PrGpuTexture* out) {
    GpuMetalSwapChain* sc = static_cast<GpuMetalSwapChain*>(handle);
    if (sc->transactional && sc->layer.presentsWithTransaction != sc->view.inLiveResize)
        sc->layer.presentsWithTransaction = sc->view.inLiveResize;
    return gpuMetalSwapChainAcquire(device, handle, extent, out);
}

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
        gpuMetalDeviceInit(d, device);
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
        d->destroy = destroyDevice;
        d->graphicsPipelineCreate = graphicsPipelineCreate;
        d->computePipelineCreate = computePipelineCreate;
        d->commandsBegin = commandsBegin;
        d->commandsSubmit = commandsSubmit;
        d->commandsSetPush = commandsSetPush;
        d->commandsSetUniforms = commandsSetUniforms;
        d->commandsDispatch = commandsDispatch;
        d->commandsDispatchIndirect = commandsDispatchIndirect;
        d->commandsCopyBuffer = commandsCopyBuffer;
        d->commandsCopyTexture = commandsCopyTexture;
        d->commandsGenerateMipmaps = commandsGenerateMipmaps;
        d->commandsCopyBufferToTexture = commandsCopyBufferToTexture;
        d->commandsCopyTextureToBuffer = commandsCopyTextureToBuffer;
        d->commandsPushGroup = commandsPushGroup;
        d->commandsPopGroup = commandsPopGroup;
        d->commandsBufferBarrier = commandsBufferBarrier;
        d->commandsTextureBarrier = commandsTextureBarrier;
        d->commandsGlobalBarrier = commandsGlobalBarrier;
        d->hostStateSync = hostStateSync;
        d->passBegin = passBegin;
        d->passEnd = passEnd;
        d->passDraw = passDraw;
        d->passDrawIndexed = passDrawIndexed;
        d->passDrawIndirect = passDrawIndirect;
        d->passDrawIndexedIndirect = passDrawIndexedIndirect;
        d->swapChainAcquire = swapChainAcquire;
        d->info.backend = "Metal 4";
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

void* prGpuMetal4Texture(const PrGpuDevice* device, PrGpuTexture texture) {
    return gpuMetalTexture(gpuMetalDeviceFrom(device), texture.id);
}

PrStatus prGpuMetal4TextureWrap(PrGpuDevice* device, void* texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out) {
    return gpuMetalTextureWrap(gpuMetalDeviceFrom(device), static_cast<id<MTLTexture>>(texture), usage, out);
}
