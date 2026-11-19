// The Vulkan backend's device memory (vulkan_memory.h).

#include "gpu/backends/vulkan/src/vulkan_memory.h"

#include "pr/alloc.h"
#include "pr/assert.h"

#include "gpu/backends/vulkan/src/vulkan_resources.h"

namespace pr_impl {

namespace {

// What an allocation is for: the resource's requirements, the properties of its memory, and what the device says about
// an allocation of the resource's own. One of `buffer` and `image` is the resource.
struct Request {
    VkMemoryRequirements requirements;
    VkMemoryPropertyFlags required;
    VkMemoryPropertyFlags preferred;
    VulkanMemoryContent content;
    bool requiresDedicated;
    bool prefersDedicated;
    VkBuffer buffer;
    VkImage image;
};

// Returns the index of a memory type in `bits` with every property of `required`, preferring one that also has those of
// `preferred`, or UINT32_MAX if none has them.
uint32_t memoryType(const VulkanDevice* d, uint32_t bits, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred) {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < d->memoryProperties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0)
            continue;
        VkMemoryPropertyFlags flags = d->memoryProperties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required)
            continue;
        if ((flags & preferred) == preferred)
            return i;
        if (fallback == UINT32_MAX)
            fallback = i;
    }
    return fallback;
}

// Returns the size of the largest block of a memory type: kVulkanMemoryBlockMaxBytes, or an eighth of the type's heap
// if that is less.
uint64_t blockMaxBytes(const VulkanDevice* d, uint32_t type) {
    uint64_t heap = d->memoryProperties.memoryHeaps[d->memoryProperties.memoryTypes[type].heapIndex].size;
    return heap / 8 < kVulkanMemoryBlockMaxBytes ? heap / 8 : kVulkanMemoryBlockMaxBytes;
}

// Allocates `size` bytes of a memory type, for `request`'s resource alone if `request` is not null. Returns false if
// the device has no memory or no allocation left. Called with the memory mutex held.
bool deviceAllocate(VulkanDevice* d, uint32_t type, uint64_t size, const Request* PR_NULLABLE request, VkDeviceMemory* out) {
    if (d->memoryInfo.allocations >= d->limits.maxMemoryAllocationCount)
        return false;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = size;
    info.memoryTypeIndex = type;
    if (request != nullptr) {
        dedicated.buffer = request->buffer;
        dedicated.image = request->image;
        info.pNext = &dedicated;
    }
    if (!vulkanCheck(d, d->vk.AllocateMemory(d->device, &info, nullptr, out)))
        return false;
    ++d->memoryInfo.allocations;
    d->memoryInfo.allocatedBytes += size;
    return true;
}

void deviceFree(VulkanDevice* d, VkDeviceMemory memory, uint64_t size) {
    d->vk.FreeMemory(d->device, memory, nullptr);
    --d->memoryInfo.allocations;
    d->memoryInfo.allocatedBytes -= size;
}

// Maps the whole of an allocation. Returns null if the device cannot.
uint8_t* PR_NULLABLE deviceMap(VulkanDevice* d, VkDeviceMemory memory) {
    void* mapped = nullptr;
    if (!vulkanCheck(d, d->vk.MapMemory(d->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
        return nullptr;
    return static_cast<uint8_t*>(mapped);
}

// Allocates memory for the request's resource alone.
bool allocateDedicated(VulkanDevice* d, uint32_t type, const Request& request, bool mapped, VulkanMemory* out) {
    uint64_t size = request.requirements.size;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!deviceAllocate(d, type, size, &request, &memory))
        return false;
    uint8_t* bytes = nullptr;
    if (mapped) {
        bytes = deviceMap(d, memory);
        if (bytes == nullptr) {
            deviceFree(d, memory, size);
            return false;
        }
    }
    *out = VulkanMemory{
        .memory = memory,
        .size = size,
        .mapped = bytes,
        .source = VulkanMemorySource_Dedicated,
        .type = uint8_t(type),
        .content = request.content,
    };
    return true;
}

// Adds a block that holds `size` bytes to a pool, mapped if `mapped`. A pool's blocks double its bytes until they reach
// the largest size, and a device that has no memory for a block is asked for a smaller one.
bool blockAdd(VulkanDevice* d, VulkanMemoryPool* pool, uint32_t type, uint64_t size, bool mapped) {
    uint64_t most = blockMaxBytes(d, type);
    uint64_t bytes = pool->bytes < kVulkanMemoryBlockMinBytes ? kVulkanMemoryBlockMinBytes : pool->bytes;
    if (bytes > most)
        bytes = most;
    while (bytes < size)
        bytes *= 2;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    while (!deviceAllocate(d, type, bytes, nullptr, &memory)) {
        if (bytes / 2 < size || d->memoryInfo.allocations >= d->limits.maxMemoryAllocationCount)
            return false;
        bytes /= 2;
    }
    uint8_t* bytesMapped = nullptr;
    if (mapped) {
        bytesMapped = deviceMap(d, memory);
        if (bytesMapped == nullptr) {
            deviceFree(d, memory, bytes);
            return false;
        }
    }
    uint32_t index = 0;
    while (index < pool->blocks.count && pool->blocks.data[index].memory != VK_NULL_HANDLE)
        ++index;
    if (index == pool->blocks.count)
        pool->blocks.emplace();
    pool->blocks.data[index] = VulkanMemoryBlock{.memory = memory, .size = bytes, .mapped = bytesMapped};
    pool->bytes += bytes;
    gpuSharedRangesAddBlock(&pool->ranges, index, bytes);
    return true;
}

// Frees a block that holds nothing. `range` is the free range of all its bytes.
void blockRemove(VulkanDevice* d, VulkanMemoryPool* pool, GpuSharedRangeIndex range) {
    VulkanMemoryBlock& block = pool->blocks.data[pool->ranges.ranges.data[range].block];
    gpuSharedRangesRemoveBlock(&pool->ranges, range);
    deviceFree(d, block.memory, block.size);
    pool->bytes -= block.size;
    block = VulkanMemoryBlock{};
}

// Gives a range of a block back to its pool. A block that now holds nothing is kept if the pool keeps none. Otherwise
// the smaller of the two is kept and the larger is freed, so that what the pool holds for nothing is small.
void rangeGive(VulkanDevice* d, VulkanMemoryPool* pool, GpuSharedRangeIndex range) {
    uint32_t index = pool->ranges.ranges.data[range].block;
    GpuSharedRangeIndex freed = gpuSharedRangesGive(&pool->ranges, range);
    if (--pool->blocks.data[index].resources != 0)
        return;
    PR_ASSERT(gpuSharedRangeIsBlock(&pool->ranges, freed));
    if (pool->emptyBlock == UINT32_MAX) {
        pool->emptyBlock = index;
        pool->emptyRange = freed;
        return;
    }
    if (pool->blocks.data[index].size >= pool->blocks.data[pool->emptyBlock].size) {
        blockRemove(d, pool, freed);
        return;
    }
    blockRemove(d, pool, pool->emptyRange);
    pool->emptyBlock = index;
    pool->emptyRange = freed;
}

// Places the request's resource in a block of the pool for its memory type and content.
bool allocateInPool(VulkanDevice* d, uint32_t type, const Request& request, bool mapped, VulkanMemory* out) {
    VulkanMemoryPool*& pool = d->pools[type][request.content];
    if (pool == nullptr) {
        pool = allocZeroed<VulkanMemoryPool>();
        pool->emptyBlock = UINT32_MAX;
        gpuSharedRangesInit(&pool->ranges);
    }
    uint64_t size = request.requirements.size;
    uint64_t alignment = request.requirements.alignment;
    GpuSharedRangeIndex range = gpuSharedRangesTake(&pool->ranges, size, alignment);
    if (range == kGpuSharedRangeNone) {
        if (!blockAdd(d, pool, type, size, mapped))
            return false;
        range = gpuSharedRangesTake(&pool->ranges, size, alignment);
        PR_ASSERT(range != kGpuSharedRangeNone);
    }
    GpuSharedRange placed = pool->ranges.ranges.data[range];
    VulkanMemoryBlock& block = pool->blocks.data[placed.block];
    ++block.resources;
    if (pool->emptyBlock == placed.block)
        pool->emptyBlock = UINT32_MAX;
    *out = VulkanMemory{
        .memory = block.memory,
        .offset = placed.offset,
        .size = size,
        .mapped = mapped ? block.mapped + placed.offset : nullptr,
        .source = VulkanMemorySource_Block,
        .type = uint8_t(type),
        .content = request.content,
        .range = range,
    };
    return true;
}

// Allocates memory for a request: from the application if it gives the device its memory and the CPU does not map this,
// and otherwise from the device, trying each memory type that fits until one has memory.
bool allocate(VulkanDevice* d, const Request& request, VulkanMemory* out) {
    *out = VulkanMemory{};
    const VkMemoryRequirements& requirements = request.requirements;
    bool mapped = request.content == VulkanMemoryContent_MappedBuffer;
    if (!mapped && d->allocate != nullptr) {
        uint64_t memory = 0;
        uint64_t offset = 0;
        if (!d->allocate(d->memoryContext, requirements.size, requirements.alignment, requirements.memoryTypeBits,
                &memory, &offset))
            return false;
        out->memory = reinterpret_cast<VkDeviceMemory>(memory);
        out->offset = offset;
        out->size = requirements.size;
        out->source = VulkanMemorySource_Application;
        return true;
    }
    prMutexLock(&d->memoryMutex);
    bool made = false;
    uint32_t bits = requirements.memoryTypeBits;
    while (!made) {
        uint32_t type = memoryType(d, bits, request.required, request.preferred);
        if (type == UINT32_MAX)
            break;
        bits &= ~(1u << type);
        // A preference is followed while allocations are plentiful, so that a device that prefers every resource alone
        // does not run out of them.
        bool dedicated = request.requiresDedicated || requirements.size > blockMaxBytes(d, type) / 4 ||
            (request.prefersDedicated && d->memoryInfo.allocations < d->limits.maxMemoryAllocationCount / 2);
        if (!dedicated)
            made = allocateInPool(d, type, request, mapped, out);
        // A device with no memory for another block may still have it for the resource.
        if (!made)
            made = allocateDedicated(d, type, request, mapped, out);
    }
    if (made)
        d->memoryInfo.usedBytes += out->size;
    prMutexUnlock(&d->memoryMutex);
    return made;
}

} // namespace

bool vulkanMemoryBindBuffer(VulkanDevice* d, VkBuffer buffer, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred, VulkanMemoryPlace place, VulkanMemory* out) {
    VkBufferMemoryRequirementsInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    info.buffer = buffer;
    VkMemoryDedicatedRequirements dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    d->vk.GetBufferMemoryRequirements2(d->device, &info, &requirements);
    bool mapped = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    Request request{
        .requirements = requirements.memoryRequirements,
        .required = required,
        .preferred = preferred,
        .content = VulkanMemoryContent(mapped ? VulkanMemoryContent_MappedBuffer : VulkanMemoryContent_Buffer),
        .requiresDedicated = dedicated.requiresDedicatedAllocation != VK_FALSE || place == VulkanMemoryPlace_Alone,
        .prefersDedicated = dedicated.prefersDedicatedAllocation != VK_FALSE,
        .buffer = buffer,
    };
    if (!allocate(d, request, out))
        return false;
    if (vulkanCheck(d, d->vk.BindBufferMemory(d->device, buffer, out->memory, out->offset)))
        return true;
    vulkanMemoryFree(d, *out);
    return false;
}

bool vulkanMemoryBindImage(VulkanDevice* d, VkImage image, VulkanMemory* out) {
    VkImageMemoryRequirementsInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    info.image = image;
    VkMemoryDedicatedRequirements dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    d->vk.GetImageMemoryRequirements2(d->device, &info, &requirements);
    Request request{
        .requirements = requirements.memoryRequirements,
        .required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .content = VulkanMemoryContent_Image,
        .requiresDedicated = dedicated.requiresDedicatedAllocation != VK_FALSE,
        .prefersDedicated = dedicated.prefersDedicatedAllocation != VK_FALSE,
        .image = image,
    };
    if (!allocate(d, request, out))
        return false;
    if (vulkanCheck(d, d->vk.BindImageMemory(d->device, image, out->memory, out->offset)))
        return true;
    vulkanMemoryFree(d, *out);
    return false;
}

void vulkanMemoryFree(VulkanDevice* d, VulkanMemory& memory) {
    switch (memory.source) {
        case VulkanMemorySource_Application:
            d->free(d->memoryContext, reinterpret_cast<uint64_t>(memory.memory), memory.offset);
            break;
        case VulkanMemorySource_Dedicated:
            prMutexLock(&d->memoryMutex);
            deviceFree(d, memory.memory, memory.size);
            d->memoryInfo.usedBytes -= memory.size;
            prMutexUnlock(&d->memoryMutex);
            break;
        case VulkanMemorySource_Block:
            prMutexLock(&d->memoryMutex);
            rangeGive(d, d->pools[memory.type][memory.content], memory.range);
            d->memoryInfo.usedBytes -= memory.size;
            prMutexUnlock(&d->memoryMutex);
            break;
        default:
            break;
    }
    memory = VulkanMemory{};
}

void vulkanMemoryReset(VulkanDevice* d) {
    for (uint32_t type = 0; type < VK_MAX_MEMORY_TYPES; ++type) {
        for (VulkanMemoryPool*& pool : d->pools[type]) {
            if (pool == nullptr)
                continue;
            for (uint32_t i = 0; i < pool->blocks.count; ++i) {
                const VulkanMemoryBlock& block = pool->blocks.data[i];
                PR_ASSERT(block.resources == 0);
                if (block.memory != VK_NULL_HANDLE)
                    deviceFree(d, block.memory, block.size);
            }
            pool->blocks.reset();
            gpuSharedRangesReset(&pool->ranges);
            prMemoryFree(pool);
            pool = nullptr;
        }
    }
}

} // namespace pr_impl
