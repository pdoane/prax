// The Vulkan backend's memory, resources and uploads, and the retirement of what the device destroys.

#pragma once

#include "gpu/backends/vulkan/src/vulkan_internal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if `result` is success, and marks the device lost if it says so.
bool vulkanCheck(VulkanDevice* d, VkResult result);

// Names a Vulkan object for debuggers and captures, where the device's instance validates.
void vulkanName(VulkanDevice* d, VkObjectType type, uint64_t handle, PrStringView name);

// Return the Vulkan format of a format, the format of a Vulkan format, or None, and the aspects of its images.
VkFormat vulkanFormat(PrGpuFormat format);
PrGpuFormat vulkanGpuFormat(VkFormat format);
VkImageAspectFlags vulkanAspect(PrGpuFormat format);

// Returns the layout of a texture in `access`.
VkImageLayout vulkanLayoutFromAccess(PrGpuAccessFlags access);

// Allocates memory for `requirements` with every property of `required`, preferring those of `preferred`, and maps
// memory that the host's processor sees. Returns false if no memory is left.
bool vulkanMemoryAllocate(VulkanDevice* d, VkMemoryRequirements requirements, VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred, VulkanMemory* out);
void vulkanMemoryFree(VulkanDevice* d, VulkanMemory& memory);

// Returns a free slot of the bindless array, or zero if none is left. Called with the device's mutex held.
uint32_t vulkanSlotTake(VulkanDevice* d);

// Writes a buffer's descriptor, a storage buffer, into `slot`. Called with the device's mutex held.
void vulkanSlotWriteBuffer(VulkanDevice* d, uint32_t slot, VkBuffer buffer);

// Queues `retired` to be freed once every command buffer that might use it is done. Called with the device's mutex
// held.
void vulkanRetire(VulkanDevice* d, VulkanRetired retired);

// Frees what is retired and done. Called with the device's mutex held.
void vulkanRetireDone(VulkanDevice* d);

// Records the copies of the waiting uploads into `buffer`, a command buffer of the device's own that is submitted ahead
// of `c`, and gives their pages to `c`. Called with the device's mutex held.
void vulkanUploadsRecord(VulkanDevice* d, VulkanCommands* c, VkCommandBuffer buffer);

// Returns the view of a texture's `level` and `layer` as an attachment, made the first time that a pass asks for it.
VkImageView vulkanAttachmentView(VulkanDevice* d, uint32_t slot, uint8_t level, uint16_t layer);

// Puts an image that the device does not own, a swap chain's or the host's, into a new slot as a texture that `desc`
// describes, with its views. Returns the slot, or zero if no slot is left. Called with the device's mutex held.
uint32_t vulkanTextureAdd(VulkanDevice* d, VkImage image, const PrGpuTextureDesc* desc);

// Frees a slot and what it holds at once, for work that is done with it. Called with the device's mutex held.
void vulkanSlotFree(VulkanDevice* d, uint32_t slot);

// Implement the wrapping of the host's images and buffers (pr/gpu_vulkan.h).
PrStatus vulkanTextureWrap(VulkanDevice* d, VkImage image, const PrGpuTextureDesc* desc, PrGpuTexture* out);
PrStatus vulkanBufferWrap(VulkanDevice* d, VkBuffer buffer, const PrGpuBufferDesc* desc, PrGpuBuffer* out);

// Frees every slot, sampler and pipeline that remains, as the device is destroyed.
void vulkanResourcesReset(VulkanDevice* d);

// Sets the device's backend functions that vulkan_resources.cpp implements.
void vulkanResourceFunctions(VulkanDevice* d);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
