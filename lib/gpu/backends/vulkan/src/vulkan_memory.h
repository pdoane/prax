// The Vulkan backend's device memory.
//
// A device limits the number of its VkDeviceMemory allocations, to a few thousand on many systems, and a scene has more
// buffers and textures than that. So the backend allocates memory in blocks, and places resources in them
// (gpu_shared_ranges.h). Each memory type has a pool of blocks for each VulkanMemoryContent, made when first used. A
// pool's blocks grow with the pool, from kVulkanMemoryBlockMinBytes to kVulkanMemoryBlockMaxBytes, so that a small
// application holds little and a large one makes few allocations. A pool keeps one block that holds nothing, the
// smallest, so that a resource made and destroyed again and again does not allocate each time. It frees every other.
//
// A resource has an allocation of its own if it is larger than a quarter of the largest block, if the device requires
// one for it, or if the device prefers one and the device's allocations are few. Memory that the CPU maps is placed in
// blocks too. Such a block is mapped whole when it is allocated, and stays mapped, because Vulkan maps an allocation
// only once.
//
// Allocating and freeing take one mutex and search bins, not resources (gpu_shared_ranges.h). Recording and submitting
// a frame allocate nothing once the stream and the staging have their memory.
//
// With the application's functions (PrGpuVulkanOptions::allocate), each buffer and texture in device memory is one call
// to them, and placing is the application's work. The memory that the CPU maps is still the backend's own.

#pragma once

#include "gpu/backends/vulkan/src/vulkan_internal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The sizes of a pool's first block and of its largest. A device with little memory has smaller blocks.
constexpr uint64_t kVulkanMemoryBlockMinBytes = uint64_t(8) << 20;
constexpr uint64_t kVulkanMemoryBlockMaxBytes = uint64_t(64) << 20;

// Where a buffer's memory is placed.
enum {
    VulkanMemoryPlace_Pool,     // in a block of a pool, unless the buffer is large or the device asks otherwise
    VulkanMemoryPlace_Alone,    // in an allocation of the buffer's own
};
typedef uint8_t VulkanMemoryPlace;

// Allocates memory for a buffer, with every property of `required` and preferring those of `preferred`, and binds it.
// Memory with VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT is mapped. Returns false if no memory is left. Safe from any thread.
//
// The memory that the device keeps and divides itself, a chunk of the stream or a block of staging, is placed alone. In
// a pool it would hold a block of an application's buffers long after they are gone.
bool vulkanMemoryBindBuffer(VulkanDevice* d, VkBuffer buffer, VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred, VulkanMemoryPlace place, VulkanMemory* out);

// Allocates device memory for an image and binds it. Returns false if no memory is left. Safe from any thread.
bool vulkanMemoryBindImage(VulkanDevice* d, VkImage image, VulkanMemory* out);

// Frees the memory of a buffer or an image that is destroyed, and clears `memory`. Safe from any thread.
void vulkanMemoryFree(VulkanDevice* d, VulkanMemory& memory);

// Frees the pools and their blocks, as the device is destroyed and after its resources are.
void vulkanMemoryReset(VulkanDevice* d);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
