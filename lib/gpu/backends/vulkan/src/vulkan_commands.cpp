// The Vulkan backend's command buffers: recording, the stream, barriers and passes, submission, and the thread that
// retires each command buffer once the device's timeline reaches its fence.
//
// Every command buffer is submitted to the device's one queue, in order, and signals the device's timeline semaphore
// with its fence. Uploads made before a submission are copied by a command buffer of the device's own, submitted just
// ahead of it. A command buffer that records into the application's is submitted with the application's native command
// buffer, which the application has ended, followed by a command buffer of the device's own that ends its work.
//
// Clip space has y up, as Direct3D and Metal have it, so every viewport is flipped: its origin is its bottom edge, and
// its height is negative.
//
// A pipeline that reads its target reads the pass's first color attachment. A pass begins with its attachments in the
// attachment layout. The fragments of a draw read what earlier draws wrote, never what the same draw writes.
//
// On a device with local read, the draw reads the attachment as an input attachment. The first draw of a pass that
// reads the target moves the attachment into the layout of local reads, and the pass's end moves it back. Moving it
// requires ending the rendering and beginning it again. Each draw that reads the target waits, by region, for the
// writes of the draws before it.
//
// On a device without local read, the draw samples a copy of the attachment instead. Before each draw that reads the
// target, the backend ends the rendering, copies the attachment over the draw's scissor into a texture that the command
// buffer owns, and begins the rendering again.

#include "gpu/backends/vulkan/src/vulkan_commands.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/mem.h"

#include "gpu/backends/vulkan/src/vulkan_resources.h"
#include "gpu/backends/vulkan/src/vulkan_swap_chain.h"

namespace pr_impl {

namespace {

// The functions of the device's objects, which their creation installs.
extern const PrGpuCommands kCommandsFunctions;
extern const PrGpuPass kPassFunctions;

// The bind points by the index that VulkanCommands keeps their push descriptors' state at.
constexpr VkPipelineBindPoint kBindPoints[2] = {VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE};

VulkanPass* passOf(PrGpuPass* pass) {
    return static_cast<VulkanPass*>(pass);
}

VulkanCommands* commandsOfPass(PrGpuPass* pass) {
    return static_cast<VulkanCommands*>(static_cast<GpuSharedPass*>(pass)->commands);
}

// Marks the state of the command buffer that the device changed, which lasts past its passes into the application's
// work.
void changed(VulkanCommands* c, PrGpuNativeStateFlags flags) {
    c->changed |= flags;
}

// Returns a command slot from the device's free list, or a new one. Called with the device's mutex held.
bool slotTake(VulkanDevice* d, VulkanCommandSlot* out) {
    if (d->freeCommandSlots.count != 0) {
        *out = d->freeCommandSlots.last();
        d->freeCommandSlots.pop();
        return true;
    }
    VulkanCommandSlot slot{};
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = d->queueFamily;
    if (!vulkanCheck(d, d->vk.CreateCommandPool(d->device, &poolInfo, nullptr, &slot.pool)))
        return false;
    VkCommandBufferAllocateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bufferInfo.commandPool = slot.pool;
    bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferInfo.commandBufferCount = 1;
    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;
    if (!vulkanCheck(d, d->vk.AllocateCommandBuffers(d->device, &bufferInfo, &slot.buffer)) ||
            !vulkanCheck(d, d->vk.CreateQueryPool(d->device, &queryInfo, nullptr, &slot.queries))) {
        d->vk.DestroyCommandPool(d->device, slot.pool, nullptr);
        return false;
    }
    *out = slot;
    return true;
}

// Returns a recording of the device's, ready for a command buffer. Called with the device's mutex held.
VulkanCommands* commandsTake(VulkanDevice* d) {
    VulkanCommands* c = nullptr;
    if (d->freeCommands.count != 0) {
        c = d->freeCommands.last();
        d->freeCommands.pop();
    } else {
        c = allocZeroed<VulkanCommands>();
        static_cast<PrGpuCommands&>(*c) = kCommandsFunctions;
        static_cast<PrGpuPass&>(c->pass) = kPassFunctions;
    }
    c->device = d;
    c->native = false;
    c->inPass = false;
    c->submitted = false;
    c->serial = d->nextSerial++;
    d->openSerials.push(c->serial);
    c->fence = 0;
    c->pipeline = 0;
    c->uniformBuffer = VK_NULL_HANDLE;
    c->pushDirty[0] = c->pushDirty[1] = false;
    c->changed = 0;
    c->streamUsed = 0;
    c->upload = VulkanCommandSlot{};
    c->pass.commands = c;
    c->pass.native = false;
    c->pass.localRead = false;
    c->pass.copied = false;
    return c;
}

// Begins recording into a command buffer of the device's own.
void beginBuffer(VulkanDevice* d, VkCommandBuffer buffer) {
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->vk.BeginCommandBuffer(buffer, &info);
}

// Begins the timing of a command buffer, and binds the device's own set if the device has one. The application binds
// its own set in the command buffers that it records.
void beginWork(VulkanDevice* d, VulkanCommands* c) {
    c->timed = (d->info.features & PrGpuFeatureFlag_Timestamps) != 0;
    if (c->timed) {
        d->vk.CmdResetQueryPool(c->buffer, c->slot.queries, 0, 2);
        d->vk.CmdWriteTimestamp2(c->buffer, VK_PIPELINE_STAGE_2_NONE, c->slot.queries, 0);
    }
    if (d->ownsDescriptors) {
        for (VkPipelineBindPoint point : kBindPoints)
            d->vk.CmdBindDescriptorSets(c->buffer, point, d->layout, VulkanSet_Bindless, 1, &d->set, 0, nullptr);
    }
}

// Ends a command buffer's work, and ends its timing. Makes the work's writes visible to the CPU, which reads what the
// work copied into readback memory once the fence is reached.
void endWork(VulkanDevice* d, VulkanCommands* c) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    d->vk.CmdPipelineBarrier2(c->buffer, &dependency);
    if (c->timed)
        d->vk.CmdWriteTimestamp2(c->buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, c->slot.queries, 1);
}

// Pushes the descriptors of a draw or a dispatch at `point`, if they changed since the last one there: the uniform
// buffer, and, for a draw in a pass that reads its target, the target.
void flushPush(VulkanDevice* d, VulkanCommands* c, uint32_t point) {
    if (!c->pushDirty[point])
        return;
    c->pushDirty[point] = false;
    VkWriteDescriptorSet writes[2] = {};
    uint32_t count = 0;
    VkDescriptorBufferInfo uniform{c->uniformBuffer, c->uniformOffset, c->uniformSize};
    if (c->uniformBuffer != VK_NULL_HANDLE) {
        VkWriteDescriptorSet& write = writes[count++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &uniform;
    }
    VkDescriptorImageInfo target{};
    if (point == 0 && c->inPass && (c->pass.localRead || c->pass.copied)) {
        if (c->pass.localRead)
            target =
                VkDescriptorImageInfo{VK_NULL_HANDLE, c->pass.targetView, VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR};
        else
            target = VkDescriptorImageInfo{
                VK_NULL_HANDLE, vulkanResourceAt(d, c->targetCopy.id).view,
                VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
            };
        VkWriteDescriptorSet& write = writes[count++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = vulkanTargetDescriptor(d);
        write.pImageInfo = &target;
    }
    if (count != 0)
        d->vk.CmdPushDescriptorSetKHR(c->buffer, kBindPoints[point], d->layout, VulkanSet_Push, count, writes);
}

// The stages and accesses of a set of PrGpuAccessFlags.
struct Scope {
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 accesses;
};

Scope scopeOf(PrGpuAccessFlags access) {
    constexpr VkAccessFlags2 shaderRead = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    Scope scope{0, 0};
    if ((access & PrGpuAccessFlag_IndexRead) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        scope.accesses |= VK_ACCESS_2_INDEX_READ_BIT;
    }
    if ((access & PrGpuAccessFlag_IndirectRead) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        scope.accesses |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    }
    if ((access & PrGpuAccessFlag_VertexRead) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        scope.accesses |= shaderRead;
    }
    if ((access & PrGpuAccessFlag_VertexWrite) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        scope.accesses |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if ((access & PrGpuAccessFlag_FragmentRead) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        scope.accesses |= shaderRead;
    }
    if ((access & PrGpuAccessFlag_FragmentWrite) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        scope.accesses |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if ((access & PrGpuAccessFlag_ComputeRead) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        scope.accesses |= shaderRead;
    }
    if ((access & PrGpuAccessFlag_ComputeWrite) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        scope.accesses |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if ((access & PrGpuAccessFlag_CopyRead) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_COPY_BIT;
        scope.accesses |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if ((access & PrGpuAccessFlag_CopyWrite) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_COPY_BIT;
        scope.accesses |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if ((access & PrGpuAccessFlag_ColorAttachment) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        scope.accesses |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if ((access & (PrGpuAccessFlag_DepthStencilRead | PrGpuAccessFlag_DepthStencilWrite)) != 0) {
        scope.stages |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        scope.accesses |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if ((access & PrGpuAccessFlag_DepthStencilWrite) != 0)
        scope.accesses |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // A presented image waits for the acquire semaphore at the color output stage, which the barrier after it chains
    // on.
    if ((access & PrGpuAccessFlag_Present) != 0)
        scope.stages |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    // Discarded contents that name no earlier access make the barrier wait for every earlier command before its layout
    // transition writes the resource. That includes the wait for an acquired image's semaphore.
    if (access == PrGpuAccessFlag_Discard)
        scope.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return scope;
}

// Returns the accesses of a barrier's source scope, which are writes only: a barrier makes earlier writes visible, and
// waits for earlier reads by its stages alone.
VkAccessFlags2 writesOf(VkAccessFlags2 accesses) {
    constexpr VkAccessFlags2 writes = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    return accesses & writes;
}

// Takes `size` bytes of a command buffer's stream, aligned for any use.
uint8_t* PR_NULLABLE streamTake(VulkanDevice* d, VulkanCommands* c, uint64_t size, uint32_t* slot, uint64_t* offset) {
    uint64_t alignment = kStreamAlignment;
    if (d->limits.minUniformBufferOffsetAlignment > alignment)
        alignment = d->limits.minUniformBufferOffsetAlignment;
    if (d->limits.minStorageBufferOffsetAlignment > alignment)
        alignment = d->limits.minStorageBufferOffsetAlignment;
    if (c->chunks.count != 0) {
        VulkanStreamChunk& chunk = c->chunks.last();
        uint64_t at = alignUp(c->streamUsed, alignment);
        if (at + size <= chunk.size) {
            c->streamUsed = at + size;
            *slot = chunk.slot;
            *offset = at;
            return chunk.mapped + at;
        }
    }
    uint32_t chunkSize = size > kStreamChunkBytes ? uint32_t(alignUp(size, alignment)) : kStreamChunkBytes;
    prMutexLock(&d->mutex);
    VulkanStreamChunk chunk{};
    if (chunkSize == kStreamChunkBytes && d->freeChunks.count != 0) {
        chunk = d->freeChunks.last();
        d->freeChunks.pop();
    } else {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = chunkSize;
        // The stream holds any data of a frame: uniforms, what shaders read by handle, indices and indirect arguments.
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        VulkanMemory memory{};
        uint32_t taken = 0;
        bool made = vulkanCheck(d, d->vk.CreateBuffer(d->device, &info, nullptr, &buffer));
        if (made) {
            VkMemoryRequirements requirements;
            d->vk.GetBufferMemoryRequirements(d->device, buffer, &requirements);
            made = vulkanMemoryAllocate(d, requirements,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory);
        }
        if (made) {
            d->vk.BindBufferMemory(d->device, buffer, memory.memory, memory.offset);
            taken = vulkanSlotTake(d);
            made = taken != 0;
        }
        if (!made) {
            if (buffer != VK_NULL_HANDLE)
                d->vk.DestroyBuffer(d->device, buffer, nullptr);
            vulkanMemoryFree(d, memory);
            prMutexUnlock(&d->mutex);
            return nullptr;
        }
        VulkanResource& r = d->resources.data[taken];
        r.kind = VulkanSlotKind_Buffer;
        r.buffer = buffer;
        r.memory = memory;
        r.bufferDesc.size = chunkSize;
        r.bufferDesc.memory = PrGpuMemory_Upload;
        vulkanSlotWriteBuffer(d, taken, buffer);
        chunk = VulkanStreamChunk{taken, buffer, memory.mapped, chunkSize};
    }
    prMutexUnlock(&d->mutex);
    c->chunks.push(chunk);
    c->streamUsed = size;
    *slot = chunk.slot;
    *offset = 0;
    return chunk.mapped;
}

// ---------------------------------------------------------------------------------------------------------------------
// Fences

bool isComplete(const PrGpuDevice* device, PrGpuFence fence) {
    const VulkanDevice* d = vulkanDevice(device);
    return d->lost.load() || d->reached.load() >= fence;
}

void wait(PrGpuDevice* device, PrGpuFence fence) {
    VulkanDevice* d = vulkanDevice(device);
    prMutexLock(&d->doneMutex);
    while (d->reached.load() < fence && !d->lost.load())
        prConditionWait(&d->reachedSignal, &d->doneMutex);
    prMutexUnlock(&d->doneMutex);
}

void waitIdle(PrGpuDevice* device) {
    VulkanDevice* d = vulkanDevice(device);
    prMutexLock(&d->mutex);
    uint64_t issued = d->issued;
    prMutexUnlock(&d->mutex);
    wait(device, issued);
}

// ---------------------------------------------------------------------------------------------------------------------
// Command buffers

bool commandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* options, PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    VulkanDevice* d = vulkanDevice(device);
    prMutexLock(&d->mutex);
    VulkanCommandSlot slot{};
    if (!slotTake(d, &slot)) {
        prMutexUnlock(&d->mutex);
        return false;
    }
    VulkanCommands* c = commandsTake(d);
    prMutexUnlock(&d->mutex);
    c->slot = slot;
    c->native = options->native != nullptr;
    c->buffer = c->native ? static_cast<VkCommandBuffer>(options->native) : slot.buffer;
    if (!c->native)
        beginBuffer(d, c->buffer);
    beginWork(d, c);
    *out = c;
    return true;
}

PrGpuFence commandsSubmit(PrGpuCommands* commands) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = vulkanCommands(commands);
    gpuSharedSubmitted(c);
    // The application has ended its native command buffer, so the end of the work goes in a command buffer of the
    // device's own, which is submitted after it.
    VkCommandBuffer native = c->buffer;
    if (c->native) {
        c->buffer = c->slot.buffer;
        beginBuffer(d, c->buffer);
    }
    endWork(d, c);
    d->vk.EndCommandBuffer(c->buffer);

    prMutexLock(&d->mutex);
    VkCommandBufferSubmitInfo buffers[3] = {};
    uint32_t bufferCount = 0;
    if (d->uploads.count != 0 && slotTake(d, &c->upload)) {
        beginBuffer(d, c->upload.buffer);
        vulkanUploadsRecord(d, c, c->upload.buffer);
        d->vk.EndCommandBuffer(c->upload.buffer);
        buffers[bufferCount].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        buffers[bufferCount++].commandBuffer = c->upload.buffer;
    }
    if (c->native) {
        buffers[bufferCount].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        buffers[bufferCount++].commandBuffer = native;
    }
    buffers[bufferCount].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    buffers[bufferCount++].commandBuffer = c->buffer;

    c->fence = ++d->issued;
    RawArray<VkSemaphoreSubmitInfo> waits;
    RawArray<VkSemaphoreSubmitInfo> signals;
    VkSemaphoreSubmitInfo timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    timeline.semaphore = d->timeline;
    timeline.value = c->fence;
    timeline.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals.push(timeline);
    for (uint32_t i = 0; i < c->presents.count; ++i) {
        const VulkanPresent& present = c->presents.data[i];
        VkSemaphoreSubmitInfo acquired{};
        acquired.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        acquired.semaphore = present.acquired;
        acquired.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waits.push(acquired);
        VkSemaphoreSubmitInfo rendered{};
        rendered.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        rendered.semaphore = present.swapChain->images.data[present.image].rendered;
        rendered.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push(rendered);
    }
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = waits.count;
    submit.pWaitSemaphoreInfos = waits.data;
    submit.commandBufferInfoCount = bufferCount;
    submit.pCommandBufferInfos = buffers;
    submit.signalSemaphoreInfoCount = signals.count;
    submit.pSignalSemaphoreInfos = signals.data;
    if (!d->lost.load())
        vulkanCheck(d, d->vk.QueueSubmit2(d->queue, 1, &submit, VK_NULL_HANDLE));
    waits.reset();
    signals.reset();
    vulkanSwapChainPresentAll(d, c);
    PrGpuFence fence = c->fence;
    vulkanPend(d, c);
    prMutexUnlock(&d->mutex);
    return fence;
}

void* commandsStream(PrGpuCommands* commands, uint64_t size, PrGpuBuffer* buffer, uint64_t* offset) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = vulkanCommands(commands);
    uint32_t slot = 0;
    uint8_t* bytes = streamTake(d, c, size, &slot, offset);
    buffer->id = slot;
    if (bytes != nullptr)
        return bytes;
    // The device has no memory left, so it is lost, and the bytes go to scratch memory that the GPU never reads.
    d->lost.store(true);
    buffer->id = 0;
    *offset = 0;
    if (c->scratch.count < size)
        c->scratch.resize(uint32_t(size));
    return c->scratch.data;
}

void commandsSetPush(PrGpuCommands* commands, const void* data, uint32_t size) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = vulkanCommands(commands);
    if (size != 0)
        d->vk.CmdPushConstants(c->buffer, d->layout, VK_SHADER_STAGE_ALL, 0, alignUp(size, 4u), data);
    changed(c, PrGpuNativeStateFlag_Push);
}

void commandsSetUniforms(PrGpuCommands* commands, const void* data, uint32_t size) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = vulkanCommands(commands);
    uint32_t slot = 0;
    uint64_t offset = 0;
    uint32_t bytesSize = size != 0 ? size : 16;
    uint8_t* bytes = streamTake(d, c, bytesSize, &slot, &offset);
    if (bytes == nullptr) {
        d->lost.store(true);
        return;
    }
    prMemCopy(bytes, data, size);
    c->uniformBuffer = d->resources.data[slot].buffer;
    c->uniformOffset = offset;
    c->uniformSize = bytesSize;
    c->pushDirty[0] = c->pushDirty[1] = true;
}

void commandsSetPipeline(PrGpuCommands* commands, PrGpuPipeline pipeline) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = vulkanCommands(commands);
    const VulkanPipeline& p = d->pipelines.data[pipeline.id - 1];
    PR_ASSERT((p.bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) == c->inPass);
    d->vk.CmdBindPipeline(c->buffer, p.bindPoint, p.pipeline);
    c->pipeline = pipeline.id;
    changed(c, PrGpuNativeStateFlag_Pipeline);
}

void commandsDispatch(PrGpuCommands* commands, PrGpuExtent groups) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = vulkanCommands(commands);
    flushPush(d, c, 1);
    d->vk.CmdDispatch(c->buffer, groups.width, groups.height, groups.depth);
}

void commandsDispatchIndirect(PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = vulkanCommands(commands);
    flushPush(d, c, 1);
    d->vk.CmdDispatchIndirect(c->buffer, vulkanResourceAt(d, buffer.id).buffer, offset);
}

void commandsCopyBuffer(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        PrGpuBuffer target, uint64_t targetOffset, uint64_t size) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VkBufferCopy copy{sourceOffset, targetOffset, size};
    d->vk.CmdCopyBuffer(vulkanCommands(commands)->buffer, vulkanResourceAt(d, source.id).buffer,
        vulkanResourceAt(d, target.id).buffer, 1, &copy);
}

// The subresource of a copy into or out of `r`, and the offset and extent of its texels.
VkImageSubresourceLayers layersOf(const VulkanResource& r, const PrGpuTextureRegion& region) {
    bool is3d = r.textureDesc.kind == PrGpuTextureKind_3d;
    VkImageSubresourceLayers layers{};
    layers.aspectMask = (r.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : r.aspect;
    layers.mipLevel = r.firstLevel + region.level;
    layers.baseArrayLayer = r.firstLayer + (is3d ? 0 : region.firstLayer);
    layers.layerCount = is3d ? 1 : region.extent.depth;
    return layers;
}

VkOffset3D offsetOf(const VulkanResource& r, const PrGpuTextureRegion& region) {
    bool is3d = r.textureDesc.kind == PrGpuTextureKind_3d;
    return VkOffset3D{int32_t(region.x), int32_t(region.y), is3d ? int32_t(region.z) : 0};
}

VkExtent3D extentOf(const VulkanResource& r, const PrGpuTextureRegion& region) {
    bool is3d = r.textureDesc.kind == PrGpuTextureKind_3d;
    return VkExtent3D{region.extent.width, region.extent.height, is3d ? region.extent.depth : 1};
}

void commandsCopyTexture(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    const VulkanResource& from = vulkanResourceAt(d, source.id);
    const VulkanResource& to = vulkanResourceAt(d, target.id);
    VkImageCopy copy{};
    copy.srcSubresource = layersOf(from, *sourceRegion);
    copy.srcOffset = offsetOf(from, *sourceRegion);
    copy.dstSubresource = layersOf(to, *targetRegion);
    copy.dstOffset = offsetOf(to, *targetRegion);
    copy.extent = extentOf(from, *sourceRegion);
    d->vk.CmdCopyImage(vulkanCommands(commands)->buffer, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

VkBufferImageCopy bufferCopyOf(const VulkanResource& texture, const PrGpuTextureRegion& region, uint64_t offset,
        uint32_t rowBytes, uint32_t layerBytes) {
    PrGpuFormatBlock block = prGpuFormatBlock(texture.textureDesc.format);
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.bufferRowLength = rowBytes / block.bytes * block.width;
    copy.bufferImageHeight = layerBytes != 0 ? layerBytes / rowBytes * block.height : 0;
    copy.imageSubresource = layersOf(texture, region);
    copy.imageOffset = offsetOf(texture, region);
    copy.imageExtent = extentOf(texture, region);
    return copy;
}

void commandsCopyBufferToTexture(PrGpuCommands* commands, PrGpuBuffer source,
        uint64_t sourceOffset, uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target,
        const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    const VulkanResource& to = vulkanResourceAt(d, target.id);
    VkBufferImageCopy copy = bufferCopyOf(to, *targetRegion, sourceOffset, rowBytes, layerBytes);
    d->vk.CmdCopyBufferToImage(vulkanCommands(commands)->buffer, vulkanResourceAt(d, source.id).buffer, to.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

void commandsCopyTextureToBuffer(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    const VulkanResource& from = vulkanResourceAt(d, source.id);
    VkBufferImageCopy copy = bufferCopyOf(from, *sourceRegion, targetOffset, rowBytes, layerBytes);
    d->vk.CmdCopyImageToBuffer(vulkanCommands(commands)->buffer, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        vulkanResourceAt(d, target.id).buffer, 1, &copy);
}

void commandsOnDone(PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context) {
    vulkanCommands(commands)->done.push(VulkanDoneCall{done, context});
}

void commandsPushGroup(PrGpuCommands* commands, PrStringView name) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    if (d->vk.CmdBeginDebugUtilsLabelEXT == nullptr || d->messenger == VK_NULL_HANDLE)
        return;
    char text[128];
    size_t size = name.size < sizeof(text) - 1 ? name.size : sizeof(text) - 1;
    prMemCopy(text, name.data, size);
    text[size] = '\0';
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = text;
    d->vk.CmdBeginDebugUtilsLabelEXT(vulkanCommands(commands)->buffer, &label);
}

void commandsPopGroup(PrGpuCommands* commands) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    if (d->vk.CmdEndDebugUtilsLabelEXT != nullptr && d->messenger != VK_NULL_HANDLE)
        d->vk.CmdEndDebugUtilsLabelEXT(vulkanCommands(commands)->buffer);
}

// ---------------------------------------------------------------------------------------------------------------------
// Barriers

void commandsBufferBarrier(PrGpuCommands* commands, PrGpuBuffer buffer, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    Scope source = scopeOf(before);
    Scope target = scopeOf(after);
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stages;
    barrier.srcAccessMask = writesOf(source.accesses);
    barrier.dstStageMask = target.stages;
    barrier.dstAccessMask = target.accesses;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vulkanResourceAt(d, buffer.id).buffer;
    barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    d->vk.CmdPipelineBarrier2(vulkanCommands(commands)->buffer, &dependency);
}

// The image subresources of a texture or view that `s` names, where zero levels or layers mean every one after the
// first.
VkImageSubresourceRange rangeOf(const VulkanResource& r, PrGpuSubresources s) {
    bool is3d = r.textureDesc.kind == PrGpuTextureKind_3d;
    VkImageSubresourceRange range{};
    range.aspectMask = r.aspect;
    range.baseMipLevel = r.firstLevel + s.firstLevel;
    range.levelCount = s.levels != 0 ? s.levels : r.textureDesc.levels - s.firstLevel;
    range.baseArrayLayer = is3d ? 0 : r.firstLayer + s.firstLayer;
    range.layerCount = is3d ? 1 : (s.layers != 0 ? s.layers : r.textureDesc.extent.depth - s.firstLayer);
    return range;
}

void commandsTextureBarrier(PrGpuCommands* commands, PrGpuTexture texture,
        PrGpuSubresources subresources, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    const VulkanResource& r = vulkanResourceAt(d, texture.id);
    Scope source = scopeOf(before);
    Scope target = scopeOf(after);
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stages;
    barrier.srcAccessMask = writesOf(source.accesses);
    barrier.dstStageMask = target.stages;
    barrier.dstAccessMask = target.accesses;
    if ((before & PrGpuAccessFlag_Discard) != 0)
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    else
        barrier.oldLayout = vulkanLayoutFromAccess(before);
    barrier.newLayout = vulkanLayoutFromAccess(after);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r.image;
    barrier.subresourceRange = rangeOf(r, subresources);
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    d->vk.CmdPipelineBarrier2(vulkanCommands(commands)->buffer, &dependency);
}

void commandsGlobalBarrier(PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    Scope source = scopeOf(before);
    Scope target = scopeOf(after);
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stages;
    barrier.srcAccessMask = writesOf(source.accesses);
    barrier.dstStageMask = target.stages;
    barrier.dstAccessMask = target.accesses;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    d->vk.CmdPipelineBarrier2(vulkanCommands(commands)->buffer, &dependency);
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes

VkAttachmentLoadOp loadOf(PrGpuLoad load) {
    switch (load) {
        case PrGpuLoad_Clear:
            return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case PrGpuLoad_Discard:
            return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        default:
            return VK_ATTACHMENT_LOAD_OP_LOAD;
    }
}

VkAttachmentStoreOp storeOf(PrGpuStore store) {
    return store == PrGpuStore_Keep ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

// Sets a pass's viewport to `viewport`, flipped so that clip space has y up.
void setViewport(VulkanDevice* d, VulkanCommands* c, PrRect viewport, float minDepth, float maxDepth) {
    VkViewport v{};
    v.x = viewport.left;
    v.y = viewport.bottom;
    v.width = viewport.right - viewport.left;
    v.height = viewport.top - viewport.bottom;
    v.minDepth = minDepth;
    v.maxDepth = maxDepth;
    d->vk.CmdSetViewport(c->buffer, 0, 1, &v);
}

// Sets the viewport and the scissor to cover a pass of `extent`.
void coverPass(VulkanDevice* d, VulkanCommands* c, PrUVec2 extent) {
    setViewport(d, c, PrRect{0, 0, float(extent.x), float(extent.y)}, 0, 1);
    c->pass.scissor = VkRect2D{{0, 0}, {extent.x, extent.y}};
    d->vk.CmdSetScissor(c->buffer, 0, 1, &c->pass.scissor);
    changed(c, PrGpuNativeStateFlag_Viewport | PrGpuNativeStateFlag_Scissor);
}

VkRenderingAttachmentInfo attachmentOf(VulkanDevice* d, const PrGpuAttachment& attachment, PrGpuLoad load, PrGpuStore store) {
    VkRenderingAttachmentInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    info.imageView = vulkanAttachmentView(d, attachment.texture.id, attachment.level, attachment.layer);
    info.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    info.loadOp = loadOf(load);
    info.storeOp = storeOf(store);
    if (store == PrGpuStore_Resolve && attachment.resolve.id != 0) {
        info.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        info.resolveImageView = vulkanAttachmentView(d, attachment.resolve.id, attachment.level, attachment.layer);
        info.resolveImageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    }
    return info;
}

// Begins the rendering of a pass of the device's own over the attachments that it keeps.
void beginRendering(VulkanDevice* d, VulkanCommands* c) {
    VulkanPass& pass = c->pass;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = VkRect2D{{0, 0}, {pass.extent.x, pass.extent.y}};
    info.layerCount = 1;
    info.colorAttachmentCount = pass.formats.colorCount;
    info.pColorAttachments = pass.colors;
    info.pDepthAttachment = pass.hasDepth ? &pass.depth : nullptr;
    info.pStencilAttachment = pass.hasStencil ? &pass.stencil : nullptr;
    d->vk.CmdBeginRendering(c->buffer, &info);
}

PrGpuPass* passBegin(PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = vulkanCommands(commands);
    gpuSharedPassBegan(c);
    VulkanPass& pass = c->pass;
    pass.localRead = false;
    pass.copied = false;
    pass.named = false;
    if (desc->native) {
        // The device sets the viewport and the scissor of the application's pass, so the application sets them again
        // after the device's work.
        pass.formats = desc->nativeFormats;
        pass.extent = desc->nativeExtent;
        pass.native = true;
        coverPass(d, c, pass.extent);
        return &pass;
    }
    pass.native = false;
    pass.formats = PrGpuAttachmentFormats{};
    pass.formats.colorCount = desc->colorCount;
    pass.formats.samples = 1;
    PrUVec2 extent{0, 0};
    for (uint32_t i = 0; i < desc->colorCount; ++i) {
        const PrGpuAttachment& attachment = desc->color[i];
        const VulkanResource& r = vulkanResourceAt(d, attachment.texture.id);
        pass.colors[i] = attachmentOf(d, attachment, attachment.load, attachment.store);
        PrColor clear = desc->clearColor[i];
        pass.colors[i].clearValue.color = VkClearColorValue{{clear.r, clear.g, clear.b, clear.a}};
        pass.formats.color[i] = r.textureDesc.format;
        pass.formats.samples = r.textureDesc.samples;
        extent = PrUVec2{
            r.textureDesc.extent.width >> attachment.level,
            r.textureDesc.extent.height >> attachment.level,
        };
    }
    pass.target = desc->colorCount != 0 ? desc->color[0] : PrGpuAttachment{};
    pass.targetView = desc->colorCount != 0 ? pass.colors[0].imageView : VK_NULL_HANDLE;
    pass.hasDepth = false;
    pass.hasStencil = false;
    if (desc->depthStencil.texture.id != 0) {
        const PrGpuAttachment& attachment = desc->depthStencil;
        const VulkanResource& r = vulkanResourceAt(d, attachment.texture.id);
        pass.depth = attachmentOf(d, attachment, attachment.load, attachment.store);
        pass.depth.clearValue.depthStencil = VkClearDepthStencilValue{desc->clearDepth, desc->clearStencil};
        pass.hasDepth = true;
        if ((r.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
            pass.stencil = attachmentOf(d, attachment, desc->stencilLoad, desc->stencilStore);
            pass.stencil.clearValue.depthStencil = pass.depth.clearValue.depthStencil;
            pass.hasStencil = true;
        }
        pass.formats.depthStencil = r.textureDesc.format;
        pass.formats.samples = r.textureDesc.samples;
        if (desc->colorCount == 0)
            extent = PrUVec2{
                r.textureDesc.extent.width >> attachment.level,
                r.textureDesc.extent.height >> attachment.level,
            };
    }
    pass.extent = PrUVec2{extent.x != 0 ? extent.x : 1, extent.y != 0 ? extent.y : 1};
    beginRendering(d, c);
    coverPass(d, c, pass.extent);
    pass.named = desc->name.size != 0;
    if (pass.named)
        commandsPushGroup(commands, desc->name);
    return &pass;
}

// Moves the pass's first color attachment between the attachment layout and the layout of local reads, outside the
// rendering. `toLocalRead` gives the direction.
void moveTarget(VulkanDevice* d, VulkanCommands* c, bool toLocalRead) {
    const VulkanResource& r = vulkanResourceAt(d, c->pass.target.texture.id);
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
    barrier.oldLayout = toLocalRead ? VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR;
    barrier.newLayout = toLocalRead ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR : VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r.image;
    barrier.subresourceRange = rangeOf(r, PrGpuSubresources{c->pass.target.level, 1, c->pass.target.layer, 1});
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    d->vk.CmdPipelineBarrier2(c->buffer, &dependency);
}

// Begins the rendering again after a target read ended it. Every attachment loads what the pass drew before.
void resumeRendering(VulkanDevice* d, VulkanCommands* c) {
    VulkanPass& pass = c->pass;
    for (uint32_t i = 0; i < pass.formats.colorCount; ++i)
        pass.colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    pass.depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    pass.stencil.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    beginRendering(d, c);
}

// Prepares a draw that reads its target as an input attachment. The first such draw of the pass moves the target into
// the layout of local reads. Every such draw waits for the writes of the draws before it.
void prepareLocalRead(VulkanDevice* d, VulkanCommands* c) {
    VulkanPass& pass = c->pass;
    if (!pass.localRead) {
        d->vk.CmdEndRendering(c->buffer);
        moveTarget(d, c, true);
        pass.colors[0].imageLayout = VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR;
        resumeRendering(d, c);
        pass.localRead = true;
        c->pushDirty[0] = true;
    }
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    d->vk.CmdPipelineBarrier2(c->buffer, &dependency);
}

// Returns a barrier that moves `range` of `image` from one layout, stage and access to another.
VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageSubresourceRange range, VkImageLayout oldLayout,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkImageLayout newLayout,
        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    return barrier;
}

void imageBarriers(VulkanDevice* d, VulkanCommands* c, const VkImageMemoryBarrier2* barriers, uint32_t count) {
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = barriers;
    d->vk.CmdPipelineBarrier2(c->buffer, &dependency);
}

// Ensures that the command buffer's copy texture has the format of the pass's first color attachment and covers the
// pass. A texture that does not fit is replaced, and the device frees it once the work that used it is done. Returns
// false if the device cannot create the texture.
bool takeTargetCopy(VulkanDevice* d, VulkanCommands* c) {
    const VulkanPass& pass = c->pass;
    PrGpuFormat format = pass.formats.color[0];
    PrUVec2 extent = pass.extent;
    if (c->targetCopy.id != 0) {
        const PrGpuTextureDesc& held = vulkanResourceAt(d, c->targetCopy.id).textureDesc;
        if (held.format == format && held.extent.width >= extent.x && held.extent.height >= extent.y)
            return true;
        // A texture of another format is replaced by one of the pass's extent. A texture that is too small is replaced
        // by one that covers both extents, so that passes of different sizes do not replace it in turn.
        if (held.format == format) {
            extent.x = held.extent.width > extent.x ? held.extent.width : extent.x;
            extent.y = held.extent.height > extent.y ? held.extent.height : extent.y;
        }
        d->textureDestroyFn(d, c->targetCopy);
        c->targetCopy = PrGpuTexture{};
    }
    PrGpuTextureDesc desc = prGpuTextureDesc2d(format, extent, PrGpuTextureUsageFlag_Sampled);
    desc.name = "target copy";
    return d->textureCreateFn(d, &desc, &c->targetCopy) == PrStatus_Ok;
}

// Prepares a draw that samples a copy of its target. The draw samples the copy of the pass's first color attachment at
// its pixel, so the copy must hold what the earlier draws wrote over the scissor.
void copyTarget(VulkanDevice* d, VulkanCommands* c) {
    VulkanPass& pass = c->pass;
    PR_ASSERT(pass.formats.samples == 1);
    uint32_t held = c->targetCopy.id;
    if (!takeTargetCopy(d, c)) {
        d->lost.store(true);
        return;
    }
    if (!pass.copied || c->targetCopy.id != held) {
        pass.copied = true;
        c->pushDirty[0] = true;
    }
    const VkRect2D& s = pass.scissor;
    if (s.extent.width == 0 || s.extent.height == 0)
        return;
    const VulkanResource& target = vulkanResourceAt(d, pass.target.texture.id);
    const VulkanResource& copy = vulkanResourceAt(d, c->targetCopy.id);
    VkImageSubresourceRange targetRange = rangeOf(target, PrGpuSubresources{pass.target.level, 1, pass.target.layer, 1});
    VkImageSubresourceRange copyRange = rangeOf(copy, PrGpuSubresources{0, 1, 0, 1});
    d->vk.CmdEndRendering(c->buffer);
    const VkImageMemoryBarrier2 before[2] = {
        imageBarrier(target.image, targetRange, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
        imageBarrier(copy.image, copyRange, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT),
    };
    imageBarriers(d, c, before, 2);
    VkImageCopy region{};
    region.srcSubresource = VkImageSubresourceLayers{
        targetRange.aspectMask, targetRange.baseMipLevel,
        targetRange.baseArrayLayer, 1,
    };
    region.srcOffset = VkOffset3D{s.offset.x, s.offset.y, 0};
    region.dstSubresource = VkImageSubresourceLayers{
        copyRange.aspectMask, copyRange.baseMipLevel,
        copyRange.baseArrayLayer, 1,
    };
    region.dstOffset = region.srcOffset;
    region.extent = VkExtent3D{s.extent.width, s.extent.height, 1};
    d->vk.CmdCopyImage(c->buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    const VkImageMemoryBarrier2 after[2] = {
        imageBarrier(target.image, targetRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        imageBarrier(copy.image, copyRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
    };
    imageBarriers(d, c, after, 2);
    resumeRendering(d, c);
}

// Prepares a draw that reads its target, by the device's kind of target read.
void prepareTargetRead(VulkanDevice* d, VulkanCommands* c) {
    PR_ASSERT(!c->pass.native && c->pass.formats.colorCount != 0);
    if (d->localRead)
        prepareLocalRead(d, c);
    else
        copyTarget(d, c);
}

void passEnd(PrGpuPass* pass) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanPass* p = passOf(pass);
    VulkanCommands* c = commandsOfPass(pass);
    if (p->native) {
        p->native = false;
        gpuSharedPassEnded(c);
        return;
    }
    d->vk.CmdEndRendering(c->buffer);
    if (p->localRead)
        moveTarget(d, c, false);
    p->localRead = false;
    p->copied = false;
    if (p->named)
        commandsPopGroup(c);
    gpuSharedPassEnded(c);
}

// Hands the pass's command buffer to a native draw, within the pass's rendering, then binds the device's set again if
// it has one of its own, covers the pass with the viewport and the scissor, and leaves the pass's pipeline and root
// data unset. An application's pass reports every state as changed, since the native draw may have changed any of it.
void passNativeDraw(PrGpuPass* pass, PrGpuNativeKind, PrGpuNativeDrawFn draw, void* PR_NULLABLE context) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = commandsOfPass(pass);
    PrGpuVulkanNativePass native{
        .commandBuffer = reinterpret_cast<uint64_t>(c->buffer),
        .pipelineLayout = reinterpret_cast<uint64_t>(d->layout),
    };
    draw(context, &native);
    if (d->ownsDescriptors)
        d->vk.CmdBindDescriptorSets(c->buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, d->layout, VulkanSet_Bindless, 1, &d->set,
            0, nullptr);
    coverPass(d, c, c->pass.extent);
    c->pipeline = 0;
    c->pushDirty[0] = c->pushDirty[1] = true;
    changed(c, PrGpuNativeStateFlags(~0u));
}

PrGpuNativeStateFlags nativeStateSync(PrGpuCommands* commands) {
    VulkanCommands* c = vulkanCommands(commands);
    PrGpuNativeStateFlags flags = c->changed;
    c->changed = 0;
    return flags;
}

void passSetViewport(PrGpuPass* pass, PrRect viewport, float minDepth, float maxDepth) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanCommands* c = commandsOfPass(pass);
    setViewport(vulkanDevice(device), c, viewport, minDepth, maxDepth);
    changed(c, PrGpuNativeStateFlag_Viewport);
}

void passSetScissor(PrGpuPass* pass, PrIRect scissor) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanCommands* c = commandsOfPass(pass);
    PrUVec2 extent = c->pass.extent;
    int32_t left = scissor.left < 0 ? 0 : scissor.left;
    int32_t top = scissor.top < 0 ? 0 : scissor.top;
    int32_t right = scissor.right > int32_t(extent.x) ? int32_t(extent.x) : scissor.right;
    int32_t bottom = scissor.bottom > int32_t(extent.y) ? int32_t(extent.y) : scissor.bottom;
    VkRect2D rect{{left, top}, {right > left ? uint32_t(right - left) : 0, bottom > top ? uint32_t(bottom - top) : 0}};
    c->pass.scissor = rect;
    vulkanDevice(device)->vk.CmdSetScissor(c->buffer, 0, 1, &rect);
    changed(c, PrGpuNativeStateFlag_Scissor);
}

void passSetBlendConstant(PrGpuPass* pass, PrColor constant) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanCommands* c = commandsOfPass(pass);
    float values[4] = {constant.r, constant.g, constant.b, constant.a};
    vulkanDevice(device)->vk.CmdSetBlendConstants(c->buffer, values);
    changed(c, PrGpuNativeStateFlag_BlendConstant);
}

void passSetStencilReference(PrGpuPass* pass, uint8_t reference) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanCommands* c = commandsOfPass(pass);
    vulkanDevice(device)->vk.CmdSetStencilReference(c->buffer, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
    changed(c, PrGpuNativeStateFlag_StencilReference);
}

void passSetIndexBuffer(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, bool wide) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = commandsOfPass(pass);
    d->vk.CmdBindIndexBuffer(c->buffer, vulkanResourceAt(d, buffer.id).buffer, offset,
        wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    changed(c, PrGpuNativeStateFlag_IndexBuffer);
}

// Prepares a draw: the target read of a pipeline that reads its target, and the push descriptors.
void prepareDraw(VulkanDevice* d, VulkanCommands* c) {
    if (c->pipeline != 0 && d->pipelines.data[c->pipeline - 1].readsTarget)
        prepareTargetRead(d, c);
    flushPush(d, c, 0);
}

void passDraw(PrGpuPass* pass, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
        uint32_t firstInstance) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = commandsOfPass(pass);
    prepareDraw(d, c);
    d->vk.CmdDraw(c->buffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void passDrawIndexed(PrGpuPass* pass, uint32_t indexCount, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = commandsOfPass(pass);
    prepareDraw(d, c);
    d->vk.CmdDrawIndexed(c->buffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void passDrawIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = commandsOfPass(pass);
    prepareDraw(d, c);
    d->vk.CmdDrawIndirect(c->buffer, vulkanResourceAt(d, buffer.id).buffer, offset, count, stride);
}

void passDrawIndexedIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(pass)->commands->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanCommands* c = commandsOfPass(pass);
    prepareDraw(d, c);
    d->vk.CmdDrawIndexedIndirect(c->buffer, vulkanResourceAt(d, buffer.id).buffer, offset, count, stride);
}

// Retires a command buffer that its timeline has reached: reports its time, then recycles what it held.
void retire(VulkanDevice* d, VulkanCommands* c, bool lost) {
    double seconds = 0;
    if (c->timed && !lost) {
        uint64_t ticks[2] = {};
        VkResult result = d->vk.GetQueryPoolResults(d->device, c->slot.queries, 0, 2, sizeof(ticks), ticks,
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS && ticks[1] >= ticks[0])
            seconds = double(ticks[1] - ticks[0]) * double(d->limits.timestampPeriod) * 1e-9;
    }
    for (uint32_t i = 0; i < c->done.count; ++i)
        c->done.data[i].done(c->done.data[i].context, seconds);

    prMutexLock(&d->mutex);
    d->vk.ResetCommandPool(d->device, c->slot.pool, 0);
    d->freeCommandSlots.push(c->slot);
    if (c->upload.pool != VK_NULL_HANDLE) {
        d->vk.ResetCommandPool(d->device, c->upload.pool, 0);
        d->freeCommandSlots.push(c->upload);
    }
    for (uint32_t i = 0; i < c->chunks.count; ++i) {
        VulkanStreamChunk chunk = c->chunks.data[i];
        if (chunk.size == kStreamChunkBytes)
            d->freeChunks.push(chunk);
        else
            vulkanRetire(d, VulkanRetired{VulkanRetireKind_Slot, chunk.slot, 0});
    }
    for (uint32_t i = 0; i < c->pages.count; ++i) {
        VulkanStagingPage& page = d->pages.data[c->pages.data[i]];
        d->vk.DestroyBuffer(d->device, page.buffer, nullptr);
        vulkanMemoryFree(d, page.memory);
        page = VulkanStagingPage{};
    }
    d->openSerials.removeFirst(c->serial);
    c->chunks.count = 0;
    c->done.count = 0;
    c->presents.count = 0;
    c->pages.count = 0;
    d->freeCommands.push(c);
    vulkanRetireDone(d);
    prMutexUnlock(&d->mutex);
}

// The functions of the device's objects.
constexpr PrGpuCommands kCommandsFunctions = {
    .deviceFn = gpuSharedCommandsDevice,
    .submitFn = commandsSubmit,
    .streamFn = commandsStream,
    .setPushFn = commandsSetPush,
    .setUniformsFn = commandsSetUniforms,
    .setPipelineFn = commandsSetPipeline,
    .dispatchFn = commandsDispatch,
    .dispatchIndirectFn = commandsDispatchIndirect,
    .copyBufferFn = commandsCopyBuffer,
    .copyTextureFn = commandsCopyTexture,
    .copyBufferToTextureFn = commandsCopyBufferToTexture,
    .copyTextureToBufferFn = commandsCopyTextureToBuffer,
    .generateMipmapsFn = gpuSharedGenerateMipmaps,
    .onDoneFn = commandsOnDone,
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
    .setViewportFn = passSetViewport,
    .setScissorFn = passSetScissor,
    .setBlendConstantFn = passSetBlendConstant,
    .setStencilReferenceFn = passSetStencilReference,
    .setIndexBufferFn = passSetIndexBuffer,
    .drawFn = passDraw,
    .drawIndexedFn = passDrawIndexed,
    .drawIndirectFn = passDrawIndirect,
    .drawIndexedIndirectFn = passDrawIndexedIndirect,
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Shared

void vulkanPend(VulkanDevice* d, VulkanCommands* c) {
    prMutexLock(&d->doneMutex);
    d->pending.push(c);
    prConditionSignal(&d->pendingSignal);
    prMutexUnlock(&d->doneMutex);
}

void vulkanCompletionRun(void* PR_NULLABLE context) {
    VulkanDevice* d = static_cast<VulkanDevice*>(context);
    prMutexLock(&d->doneMutex);
    for (;;) {
        while (d->pending.count == 0 && !d->stopping)
            prConditionWait(&d->pendingSignal, &d->doneMutex);
        if (d->pending.count == 0)
            break;
        VulkanCommands* c = d->pending.data[0];
        d->pending.erase(0);
        prMutexUnlock(&d->doneMutex);
        if (!d->lost.load()) {
            VkSemaphoreWaitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            info.semaphoreCount = 1;
            info.pSemaphores = &d->timeline;
            info.pValues = &c->fence;
            vulkanCheck(d, d->vk.WaitSemaphores(d->device, &info, UINT64_MAX));
        }
        PrGpuFence fence = c->fence;
        retire(d, c, d->lost.load());
        prMutexLock(&d->doneMutex);
        if (d->reached.load() < fence)
            d->reached.store(fence);
        prConditionBroadcast(&d->reachedSignal);
    }
    prMutexUnlock(&d->doneMutex);
}

void vulkanCommandsReset(VulkanDevice* d) {
    for (uint32_t i = 0; i < d->freeCommands.count; ++i) {
        VulkanCommands* c = d->freeCommands.data[i];
        c->chunks.reset();
        c->done.reset();
        c->presents.reset();
        c->pages.reset();
        c->scratch.reset();
        prMemoryFree(c);
    }
    d->freeCommands.reset();
    for (uint32_t i = 0; i < d->freeCommandSlots.count; ++i) {
        VulkanCommandSlot& slot = d->freeCommandSlots.data[i];
        if (slot.pool != VK_NULL_HANDLE)
            d->vk.DestroyCommandPool(d->device, slot.pool, nullptr);
        d->vk.DestroyQueryPool(d->device, slot.queries, nullptr);
    }
    d->freeCommandSlots.reset();
    d->freeChunks.reset();
}

void vulkanCommandsFunctions(VulkanDevice* d) {
    d->isCompleteFn = isComplete;
    d->waitFn = wait;
    d->waitIdleFn = waitIdle;
    d->commandsBeginFn = commandsBegin;
}

} // namespace pr_impl
