// The Metal GPU backend, on Metal 3, which is the default Metal backend. pr/gpu_metal4.h is the same interface on Metal
// 4.
//
// Every encoder binds the heaps and the command buffer's latest root data, since Metal 3's bindings end with their
// encoder. Metal snapshots them at each draw and dispatch.
//
// Metal 3 orders nothing between encoders that use untracked resources, so the device orders them with a chain of
// fences on the queue. Metal evaluates a fence in the order that command buffers are committed, so a wait pairs with
// the last update before it in the queue, whichever thread recorded it and whenever. Each encoder waits on the order
// fence and updates it at its end, so that encoders run in the order of the queue, and a barrier only has to begin a
// new encoder. Render encoders wait on it only before their fragment stage, so that vertex work can overlap the
// fragment work before it, unless a barrier asked otherwise. A barrier that holds the vertex stage updates the vertex
// fence, which render encoders wait on before their vertex stage, and which an empty encoder in the chain updates.
// Dispatches between two barriers share an encoder, and run concurrently.
//
// Uploads are copied in a command buffer of the device's own, committed just before the next submission, or sooner once
// its staging fills a few blocks. Its encoders join the chain, and it updates the vertex fence as well.
//
// A swap chain that presents with the window's display transaction commits, waits for the command buffer to be
// scheduled, and presents on the thread that runs the window. A threaded one presents through the command buffer.

#include "pr/gpu_metal.h"

#include "pr/alloc.h"
#include "pr/assert.h"

#include "gpu/backends/shared/src/gpu_shared_metal.h"

namespace pr_impl {

namespace {

// What a binding holds for the next encoders.
struct RootBinding {
    id<MTLBuffer> PR_NULLABLE buffer;   // held by the command buffer's stream
    uint64_t offset;
};

struct Commands : GpuMetalCommands {
    id<MTLCommandBuffer> buffer;                       // retained
    id<MTLComputeCommandEncoder> PR_NULLABLE compute;  // the open encoder of dispatches, retained
    id<MTLBlitCommandEncoder> PR_NULLABLE blit;        // the open encoder of copies, retained
    RootBinding push;
    RootBinding uniforms;
    uint32_t passGroups;                               // debug groups pushed on the open pass's encoder
    uint32_t passPops;                                 // pops of groups outside the open pass, which wait for its end
};

struct Device : GpuMetalDevice {
    id<MTLFence> orderFence;                // retained
    id<MTLFence> vertexFence;               // retained

    // The open batch of uploads: its command buffer and encoder.
    id<MTLCommandBuffer> PR_NULLABLE uploads;            // retained
    id<MTLBlitCommandEncoder> PR_NULLABLE uploadEncoder; // retained
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

id<MTLRenderCommandEncoder> passEncoder(GpuMetalPass* pass) {
    return pass->encoder;
}

id<MTLCommandBuffer> newCommandBuffer(Device* d) {
    // The device keeps what work uses until the work is done, so the command buffer retains nothing.
    return [[static_cast<id<MTLCommandQueue>>(d->queue) commandBufferWithUnretainedReferences] retain];
}

// Commits a command buffer, which reaches `fence` once it is done and the functions that wait for it have run. Takes
// the copied `dones`. The caller holds the mutex.
void commit(Device* d, id<MTLCommandBuffer> buffer, PrGpuFence fence, GpuMetalDone* PR_NULLABLE dones,
        uint32_t doneCount) {
    Device* owner = d;
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        double seconds = done.error == nil ? done.GPUEndTime - done.GPUStartTime : 0;
        gpuMetalSubmissionDone(owner, fence, dones, doneCount, done.error, seconds);
    }];
    [buffer commit];
}

// ---------------------------------------------------------------------------------------------------------------------
// Uploads

id<MTLBlitCommandEncoder> newUploadEncoder(Device* d) {
    id<MTLBlitCommandEncoder> encoder = [[d->uploads blitCommandEncoder] retain];
    [encoder waitForFence:d->orderFence];
    return encoder;
}

// Opens the batch of uploads if it is not open. It waits for the work before it, which may read what it writes.
void uploadsOpen(GpuMetalDevice* device) {
    Device* d = deviceFrom(device);
    if (d->uploads != nil)
        return;
    d->uploads = newCommandBuffer(d);
    d->uploads.label = @"uploads";
    d->uploadEncoder = newUploadEncoder(d);
    d->uploadSerial = d->serial++;
    d->inFlight.push(GpuMetalInFlight{d->uploadSerial, 0});
}

// Commits the open batch of uploads, ahead of whatever is submitted after it. Its copies into buffers are disjoint, so
// they go at once. Every stage of the work after it waits for it.
void uploadsSubmit(GpuMetalDevice* device) {
    Device* d = deviceFrom(device);
    if (d->uploads == nil)
        return;
    gpuMetalEncodeUploadCopies(d, d->uploadEncoder);
    [d->uploadEncoder updateFence:d->vertexFence];
    [d->uploadEncoder updateFence:d->orderFence];
    [d->uploadEncoder endEncoding];
    [d->uploadEncoder release];
    d->uploadEncoder = nil;
    gpuMetalCommitResidency(d);
    PrGpuFence fence = ++d->submitted;
    commit(d, d->uploads, fence, nullptr, 0);
    gpuMetalUploadsRecord(d, fence);
    [d->uploads release];
    d->uploads = nil;
}

void uploadsCopyTexture(GpuMetalDevice* device, const GpuMetalTextureCopy& copy) {
    Device* d = deviceFrom(device);
    if (copy.barrier) {
        [d->uploadEncoder updateFence:d->orderFence];
        [d->uploadEncoder endEncoding];
        [d->uploadEncoder release];
        d->uploadEncoder = newUploadEncoder(d);
    }
    gpuMetalEncodeTextureCopy(d->uploadEncoder, copy);
}

// ---------------------------------------------------------------------------------------------------------------------
// The device

void destroyDevice(PrGpuDevice* device) {
    Device* d = deviceFrom(device);
    @autoreleasepool {
        [d->orderFence release];
        [d->vertexFence release];
        gpuMetalDeviceReset(d);
    }
    prMemoryFree(d);
}

// ---------------------------------------------------------------------------------------------------------------------
// Pipelines

PrStatus graphicsPipelineCreate(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        id<MTLLibrary> library = static_cast<const GpuMetalLibrary*>(desc->library)->library;
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
        pd.vertexFunction = [[library newFunctionWithName:gpuMetalString(vertex->name)] autorelease];
        if (fragment != nullptr)
            pd.fragmentFunction = [[library newFunctionWithName:gpuMetalString(fragment->name)] autorelease];
        pd.rasterSampleCount = desc->formats.samples;
        pd.inputPrimitiveTopology = gpuMetalTopologyClass(desc->topology);
        if (desc->name.size != 0)
            pd.label = gpuMetalString(desc->name);
        for (uint8_t i = 0; i < desc->formats.colorCount; ++i) {
            MTLRenderPipelineColorAttachmentDescriptor* a = pd.colorAttachments[i];
            const PrGpuBlend& blend = desc->blend[i];
            a.blendingEnabled = blend.enabled;
            gpuMetalColorAttachmentFill(a, desc->formats.color[i], blend);
        }
        if (prGpuFormatIsDepthStencil(desc->formats.depthStencil)) {
            pd.depthAttachmentPixelFormat = gpuMetalPixelFormat(desc->formats.depthStencil);
            if (desc->formats.depthStencil == PrGpuFormat_D32FS8)
                pd.stencilAttachmentPixelFormat = pd.depthAttachmentPixelFormat;
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [d->device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (state == nil) {
            NSLog(@"lib/gpu: Metal made no pipeline for %@: %@", pd.vertexFunction, error);
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
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    @autoreleasepool {
        MTLComputePipelineDescriptor* pd = [[MTLComputePipelineDescriptor new] autorelease];
        id<MTLLibrary> shaders = static_cast<const GpuMetalLibrary*>(library)->library;
        pd.computeFunction = [[shaders newFunctionWithName:gpuMetalString(compute->name)] autorelease];
        if (name.size != 0)
            pd.label = gpuMetalString(name);
        NSError* error = nil;
        id<MTLComputePipelineState> state = [d->device newComputePipelineStateWithDescriptor:pd
            options:MTLPipelineOptionNone reflection:nil error:&error];
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

// Ends the open encoder of dispatches or of copies, joining the chain of fences.
void endEncoders(Device* d, Commands* c) {
    if (c->compute != nil) {
        [c->compute updateFence:d->orderFence];
        [c->compute endEncoding];
        [c->compute release];
        c->compute = nil;
    }
    if (c->blit != nil) {
        [c->blit updateFence:d->orderFence];
        [c->blit endEncoding];
        [c->blit release];
        c->blit = nil;
    }
}

// Returns the encoder of dispatches, begun after the last barrier.
id<MTLComputeCommandEncoder> computeEncoder(Device* d, Commands* c) {
    if (c->compute == nil) {
        endEncoders(d, c);
        c->compute = [[c->buffer computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent] retain];
        [c->compute waitForFence:d->orderFence];
        [c->compute setBuffer:c->push.buffer offset:c->push.offset atIndex:GpuMetalBinding_Push];
        [c->compute setBuffer:c->uniforms.buffer offset:c->uniforms.offset atIndex:GpuMetalBinding_Uniforms];
        [c->compute setBuffer:d->resourceHeap offset:0 atIndex:GpuMetalBinding_Resources];
        [c->compute setBuffer:d->samplerHeap offset:0 atIndex:GpuMetalBinding_Samplers];
    }
    return c->compute;
}

// Returns the encoder of copies, begun after the last barrier.
id<MTLBlitCommandEncoder> blitEncoder(Device* d, Commands* c) {
    if (c->blit == nil) {
        endEncoders(d, c);
        c->blit = [[c->buffer blitCommandEncoder] retain];
        [c->blit waitForFence:d->orderFence];
    }
    return c->blit;
}

bool commandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* options, PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    Device* d = deviceFrom(device);
    @autoreleasepool {
        Commands* c = allocZeroed<Commands>();
        prMutexLock(&d->mutex);
        gpuMetalCollect(d);
        gpuMetalCommandsInit(d, c);
        prMutexUnlock(&d->mutex);
        if (options->native != nullptr) {
            c->native = true;
            c->buffer = [static_cast<id<MTLCommandBuffer>>(options->native) retain];
        } else {
            c->buffer = newCommandBuffer(d);
        }
        *out = c;
        return true;
    }
}

// Copies root data into the stream, and binds it on the encoder that is open.
void setRoot(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size, uint32_t binding,
        RootBinding* root) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    Commands* c = commandsFrom(commands);
    uint32_t slot = 0;
    uint64_t offset = 0;
    void* bytes = gpuMetalStream(d, c, size != 0 ? size : 16, &slot, &offset);
    prMemCopy(bytes, data, size);
    if (slot == 0)
        return;
    *root = RootBinding{gpuMetalBuffer(d, slot), offset};
    if (c->pass != nullptr) {
        id<MTLRenderCommandEncoder> encoder = passEncoder(c->pass);
        [encoder setVertexBuffer:root->buffer offset:offset atIndex:binding];
        [encoder setFragmentBuffer:root->buffer offset:offset atIndex:binding];
    } else if (c->compute != nil) {
        [c->compute setBuffer:root->buffer offset:offset atIndex:binding];
    }
}

void commandsSetPush(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size) {
    setRoot(device, commands, data, size, GpuMetalBinding_Push, &commandsFrom(commands)->push);
}

void commandsSetUniforms(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size) {
    setRoot(device, commands, data, size, GpuMetalBinding_Uniforms, &commandsFrom(commands)->uniforms);
}

void commandsDispatch(PrGpuDevice* device, PrGpuCommands* commands, PrGpuExtent groups) {
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    const GpuMetalPipeline& p = d->pipelines[c->pipeline];
    PR_ASSERT(p.compute);
    id<MTLComputeCommandEncoder> e = computeEncoder(d, c);
    [e setComputePipelineState:p.state];
    [e dispatchThreadgroups:MTLSizeMake(groups.width, groups.height, groups.depth) threadsPerThreadgroup:p.groupSize];
}

void commandsDispatchIndirect(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    const GpuMetalPipeline& p = d->pipelines[c->pipeline];
    id<MTLComputeCommandEncoder> e = computeEncoder(d, c);
    [e setComputePipelineState:p.state];
    [e dispatchThreadgroupsWithIndirectBuffer:gpuMetalBuffer(d, buffer.id) indirectBufferOffset:offset
        threadsPerThreadgroup:p.groupSize];
}

void commandsCopyBuffer(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        PrGpuBuffer target, uint64_t targetOffset, uint64_t size) {
    Device* d = deviceFrom(device);
    [blitEncoder(d, commandsFrom(commands)) copyFromBuffer:gpuMetalBuffer(d, source.id) sourceOffset:sourceOffset
        toBuffer:gpuMetalBuffer(d, target.id) destinationOffset:targetOffset size:size];
}

void commandsCopyTexture(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    Device* d = deviceFrom(device);
    gpuMetalEncodeCopyTexture(d, blitEncoder(d, commandsFrom(commands)), source, sourceRegion, target, targetRegion);
}

void commandsCopyBufferToTexture(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer source,
        uint64_t sourceOffset, uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target,
        const PrGpuTextureRegion* targetRegion) {
    Device* d = deviceFrom(device);
    gpuMetalEncodeCopyBufferToTexture(d, blitEncoder(d, commandsFrom(commands)), source, sourceOffset, rowBytes,
        layerBytes, target, targetRegion);
}

void commandsCopyTextureToBuffer(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes) {
    Device* d = deviceFrom(device);
    gpuMetalEncodeCopyTextureToBuffer(d, blitEncoder(d, commandsFrom(commands)), source, sourceRegion, target,
        targetOffset, rowBytes, layerBytes);
}

// A command buffer takes debug groups only between encoders, so a group in a pass goes on the pass's encoder, and a
// group begun outside a pass that ends in one ends with the pass. A group begun in a pass ends in it.
void commandsPushGroup(PrGpuDevice* device, PrGpuCommands* commands, PrStringView name) {
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    @autoreleasepool {
        if (c->pass != nullptr) {
            [passEncoder(c->pass) pushDebugGroup:gpuMetalString(name)];
            ++c->passGroups;
        } else {
            endEncoders(d, c);
            [c->buffer pushDebugGroup:gpuMetalString(name)];
        }
    }
}

void commandsPopGroup(PrGpuDevice* device, PrGpuCommands* commands) {
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    if (c->pass != nullptr) {
        if (c->passGroups != 0) {
            [passEncoder(c->pass) popDebugGroup];
            --c->passGroups;
        } else {
            ++c->passPops;
        }
    } else {
        endEncoders(d, c);
        [c->buffer popDebugGroup];
    }
}

// Begins a new encoder for the work after the barrier, which the chain of fences then orders. A barrier that holds the
// vertex stage brings the vertex fence up to date on an empty encoder of the chain.
void barrier(Device* d, Commands* c, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    MTLStages wait = gpuMetalStages(before);
    MTLStages hold = gpuMetalStages(after);
    if (wait == 0 || hold == 0)
        return;
    PR_ASSERT(c->pass == nullptr);
    endEncoders(d, c);
    if ((hold & MTLStageVertex) != 0) {
        @autoreleasepool {
            id<MTLBlitCommandEncoder> sync = [c->buffer blitCommandEncoder];
            [sync waitForFence:d->orderFence];
            [sync updateFence:d->vertexFence];
            [sync updateFence:d->orderFence];
            [sync endEncoding];
        }
    }
}

void commandsBufferBarrier(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer, PrGpuAccessFlags before,
        PrGpuAccessFlags after) {
    barrier(deviceFrom(device), commandsFrom(commands), before, after);
}

void commandsTextureBarrier(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture, PrGpuSubresources,
        PrGpuAccessFlags before, PrGpuAccessFlags after) {
    barrier(deviceFrom(device), commandsFrom(commands), before, after);
}

void commandsGlobalBarrier(PrGpuDevice* device, PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    barrier(deviceFrom(device), commandsFrom(commands), before, after);
}

void commandsGenerateMipmaps(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture texture, PrGpuAccessFlags before) {
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    barrier(d, c, before, PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite);
    [blitEncoder(d, c) generateMipmapsForTexture:gpuMetalTexture(d, texture.id)];
    barrier(d, c, PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_ShaderRead);
}

PrGpuNativeStateFlags nativeStateSync(PrGpuDevice* device, PrGpuCommands* commands) {
    Commands* c = commandsFrom(commands);
    @autoreleasepool {
        if (c->pass == nullptr)
            endEncoders(deviceFrom(device), c);
    }
    PrGpuNativeStateFlags changed = c->changed;
    c->changed = 0;
    return changed;
}

PrGpuFence commandsSubmit(PrGpuDevice* device, PrGpuCommands* commands) {
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    @autoreleasepool {
        endEncoders(d, c);
        uint32_t doneCount = c->dones.count;
        GpuMetalDone* dones = gpuMetalDonesCopy(c);
        bool transactional = false;
        for (uint32_t i = 0; i < c->drawables.count; ++i) {
            if (c->drawables.data[i].transactional)
                transactional = true;
            else
                [c->buffer presentDrawable:c->drawables.data[i].drawable];
        }
        prMutexLock(&d->mutex);
        uploadsSubmit(d);
        gpuMetalCommitResidency(d);
        PrGpuFence fence = ++d->submitted;
        commit(d, c->buffer, fence, dones, doneCount);
        gpuMetalSubmissionRecord(d, c, fence);
        prMutexUnlock(&d->mutex);
        // A present in the window's display transaction takes effect with it, so the drawable must be scheduled by
        // then. The wait holds no lock, since it can last until the work before this command buffer is scheduled.
        if (transactional)
            [c->buffer waitUntilScheduled];
        for (uint32_t i = 0; i < c->drawables.count; ++i) {
            id<CAMetalDrawable> drawable = c->drawables.data[i].drawable;
            if (c->drawables.data[i].transactional)
                [drawable present];
            [drawable release];
        }
        [c->buffer release];
        gpuMetalCommandsDestroy(c);
        return fence;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes

// Joins a render encoder to the chain of fences. An application's pass joins the chain too, so that its draws see the
// work before it.
void renderEncoderWait(Device* d, id<MTLRenderCommandEncoder> encoder) {
    [encoder waitForFence:d->vertexFence beforeStages:MTLRenderStageVertex];
    [encoder waitForFence:d->orderFence beforeStages:MTLRenderStageFragment];
}

// A texture's native object is its id<MTLTexture>.
bool textureNative(PrGpuDevice* device, PrGpuTexture texture, PrGpuNativeKind, void* out) {
    *static_cast<void**>(out) = gpuMetalTexture(gpuMetalDeviceFrom(device), texture.id);
    return true;
}

// Binds the device's buffers in the pass's encoder: the root data and the heaps that its shaders read, and, in a pass
// of the device's own, the viewport and scissor over the whole attachments.
void passBind(Device* d, Commands* c, GpuMetalPass* pass) {
    id<MTLRenderCommandEncoder> encoder = passEncoder(pass);
    [encoder setVertexBuffer:c->push.buffer offset:c->push.offset atIndex:GpuMetalBinding_Push];
    [encoder setFragmentBuffer:c->push.buffer offset:c->push.offset atIndex:GpuMetalBinding_Push];
    [encoder setVertexBuffer:c->uniforms.buffer offset:c->uniforms.offset atIndex:GpuMetalBinding_Uniforms];
    [encoder setFragmentBuffer:c->uniforms.buffer offset:c->uniforms.offset atIndex:GpuMetalBinding_Uniforms];
    [encoder setVertexBuffer:d->resourceHeap offset:0 atIndex:GpuMetalBinding_Resources];
    [encoder setFragmentBuffer:d->resourceHeap offset:0 atIndex:GpuMetalBinding_Resources];
    [encoder setVertexBuffer:d->samplerHeap offset:0 atIndex:GpuMetalBinding_Samplers];
    [encoder setFragmentBuffer:d->samplerHeap offset:0 atIndex:GpuMetalBinding_Samplers];
    if (!pass->native) {
        [encoder setViewport:MTLViewport{0, 0, double(pass->extent.x), double(pass->extent.y), 0, 1}];
        [encoder setScissorRect:MTLScissorRect{0, 0, pass->extent.x, pass->extent.y}];
    }
}

PrGpuPass* passBegin(PrGpuDevice* device, PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    Device* d = deviceFrom(device);
    Commands* c = commandsFrom(commands);
    @autoreleasepool {
        GpuMetalPass* pass = allocZeroed<GpuMetalPass>();
        pass->commands = commands;
        if (desc->native != nullptr) {
            pass->native = true;
            pass->formats = desc->nativeFormats;
            pass->extent = desc->nativeExtent;
            pass->encoder = [static_cast<id<MTLRenderCommandEncoder>>(desc->native) retain];
            c->changed |= PrGpuNativeStateFlag_Bindings;
        } else {
            MTLRenderPassDescriptor* rp = [[MTLRenderPassDescriptor new] autorelease];
            gpuMetalPassAttachmentsFill(d, rp, desc, pass);
            endEncoders(d, c);
            pass->encoder = [[c->buffer renderCommandEncoderWithDescriptor:rp] retain];
            if (desc->name.size != 0)
                [pass->encoder setLabel:gpuMetalString(desc->name)];
        }
        id<MTLRenderCommandEncoder> encoder = passEncoder(pass);
        renderEncoderWait(d, encoder);
        passBind(d, c, pass);
        c->pass = pass;
        c->pipeline = 0;
        return pass;
    }
}

// Hands the pass's encoder to a native draw, then binds the device's buffers again, and leaves the pass's pipeline
// unset. An application's pass reports every state as changed, since the native draw may have changed any of it.
void passNativeDraw(PrGpuDevice* device, PrGpuPass* handle, PrGpuNativeKind, PrGpuNativeDrawFn draw, void* PR_NULLABLE context) {
    Device* d = deviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    Commands* c = commandsFrom(pass->commands);
    @autoreleasepool {
        PrGpuMetalNativePass native{.encoder = pass->encoder, .commandBuffer = c->buffer};
        draw(context, &native);
        passBind(d, c, pass);
        c->pipeline = 0;
        gpuMetalPassChanged(pass, PrGpuNativeStateFlags(~0u));
    }
}

void passEnd(PrGpuDevice* device, PrGpuPass* handle) {
    Device* d = deviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    Commands* c = commandsFrom(pass->commands);
    id<MTLRenderCommandEncoder> encoder = passEncoder(pass);
    PR_ASSERT(c->passGroups == 0);
    c->pass = nullptr;
    // An application's pass stays open for the application to end. The work after it still waits for it through the
    // fence.
    [encoder updateFence:d->orderFence afterStages:MTLRenderStageFragment];
    if (!pass->native)
        [encoder endEncoding];
    [encoder release];
    for (; c->passPops != 0; --c->passPops)
        [c->buffer popDebugGroup];
    prMemoryFree(pass);
}

const GpuMetalPipeline& passPipeline(PrGpuDevice* device, GpuMetalPass* pass) {
    return gpuMetalDeviceFrom(device)->pipelines[gpuMetalCommandsFrom(pass->commands)->pipeline];
}

void passDraw(PrGpuDevice* device, PrGpuPass* handle, uint32_t vertexCount, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance) {
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    [passEncoder(pass) drawPrimitives:passPipeline(device, pass).primitive vertexStart:firstVertex
        vertexCount:vertexCount instanceCount:instanceCount baseInstance:firstInstance];
}

void passDrawIndexed(PrGpuDevice* device, PrGpuPass* handle, uint32_t indexCount, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    uint64_t at = pass->indexOffset + uint64_t(firstIndex) * (pass->indexType == MTLIndexTypeUInt32 ? 4 : 2);
    [passEncoder(pass) drawIndexedPrimitives:passPipeline(device, pass).primitive indexCount:indexCount
        indexType:pass->indexType indexBuffer:gpuMetalBuffer(d, pass->indexBuffer.id) indexBufferOffset:at
        instanceCount:instanceCount baseVertex:vertexOffset baseInstance:firstInstance];
}

void passDrawIndirect(PrGpuDevice* device, PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, uint32_t count,
        uint32_t stride) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    id<MTLBuffer> arguments = gpuMetalBuffer(d, buffer.id);
    for (uint32_t i = 0; i < count; ++i)
        [passEncoder(pass) drawPrimitives:passPipeline(device, pass).primitive indirectBuffer:arguments
            indirectBufferOffset:offset + uint64_t(i) * stride];
}

void passDrawIndexedIndirect(PrGpuDevice* device, PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset,
        uint32_t count, uint32_t stride) {
    GpuMetalDevice* d = gpuMetalDeviceFrom(device);
    GpuMetalPass* pass = static_cast<GpuMetalPass*>(handle);
    id<MTLBuffer> arguments = gpuMetalBuffer(d, buffer.id);
    for (uint32_t i = 0; i < count; ++i)
        [passEncoder(pass) drawIndexedPrimitives:passPipeline(device, pass).primitive indexType:pass->indexType
            indexBuffer:gpuMetalBuffer(d, pass->indexBuffer.id) indexBufferOffset:pass->indexOffset
            indirectBuffer:arguments indirectBufferOffset:offset + uint64_t(i) * stride];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrGpuMetalOptions prGpuMetalOptionsDefault(void) {
    return PrGpuMetalOptions{};
}

PrStatus prGpuMetalCreate(const PrGpuMetalOptions* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuMetalOptions o = options != nullptr ? *options : prGpuMetalOptionsDefault();
    @autoreleasepool {
        id<MTLDevice> device;
        if (o.device != nullptr)
            device = [static_cast<id<MTLDevice>>(o.device) retain];
        else
            device = MTLCreateSystemDefaultDevice();
        if (device == nil)
            return PrStatus_NotSupported;
        if (![device supportsFamily:MTLGPUFamilyMetal3]) {
            [device release];
            return PrStatus_NotSupported;
        }
        Device* d = allocZeroed<Device>();
        gpuMetalDeviceInit(d, device);
        id<MTLCommandQueue> queue;
        if (o.queue != nullptr)
            queue = [static_cast<id<MTLCommandQueue>>(o.queue) retain];
        else
            queue = [device newCommandQueue];
        gpuMetalDeviceAddQueue(d, queue);
        d->orderFence = [device newFence];
        d->vertexFence = [device newFence];
        // The first wait on each fence pairs with this update.
        id<MTLCommandBuffer> prime = [queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [prime blitCommandEncoder];
        [encoder updateFence:d->orderFence];
        [encoder updateFence:d->vertexFence];
        [encoder endEncoding];
        [prime commit];

        d->uploadsOpen = uploadsOpen;
        d->uploadsSubmit = uploadsSubmit;
        d->uploadsCopyTexture = uploadsCopyTexture;
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
        d->nativeStateSync = nativeStateSync;
        d->passBegin = passBegin;
        d->passEnd = passEnd;
        d->passNativeDraw = passNativeDraw;
        d->textureNative = textureNative;
        d->passDraw = passDraw;
        d->passDrawIndexed = passDrawIndexed;
        d->passDrawIndirect = passDrawIndirect;
        d->passDrawIndexedIndirect = passDrawIndexedIndirect;
        d->info.backend = "Metal";
        d->nativeKinds = 1u << PrGpuNativeKind_Metal;
        *out = d;
        return PrStatus_Ok;
    }
}

void* prGpuMetalDevice(const PrGpuDevice* device) {
    return gpuMetalDeviceFrom(device)->device;
}

void* prGpuMetalQueue(const PrGpuDevice* device) {
    return gpuMetalDeviceFrom(device)->queue;
}

void* prGpuMetalResidencySet(const PrGpuDevice* device) {
    return gpuMetalDeviceFrom(device)->residency;
}

void* prGpuMetalOrderFence(const PrGpuDevice* device) {
    return static_cast<const Device*>(gpuMetalDeviceFrom(device))->orderFence;
}

PrStatus prGpuMetalTextureWrap(PrGpuDevice* device, void* texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out) {
    return gpuMetalTextureWrap(gpuMetalDeviceFrom(device), static_cast<id<MTLTexture>>(texture), usage, out);
}
