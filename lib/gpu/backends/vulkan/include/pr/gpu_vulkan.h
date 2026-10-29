// The Vulkan GPU backend.
//
// The backend needs Vulkan 1.3 with dynamic rendering, synchronization2, timeline semaphores and descriptor indexing,
// with mutable descriptors and push descriptors.
//
// A pipeline that reads its target reads it through dynamic rendering local read if the device has it. Otherwise the
// pipeline samples a copy of the target. The backend makes the copy before each draw that uses such a pipeline, so each
// of those draws ends the rendering and begins it again. The two kinds of device run different SPIR-V, and a shader
// library holds code for both.
//
// The native objects cross this header as untyped pointers or 64-bit integers, and native enumerations as integers. A
// caller casts them to their Vulkan types, such as `VkDevice` and `VkImageLayout`.
//
// Shaders reach resources through one pipeline layout, which the shader libraries are compiled for:
//
// - Set 0 is empty.
// - Set 1 holds push descriptors. Binding 0 is the uniform buffer of a draw or a dispatch. Binding 1 is the target that
//   a pipeline that reads its target reads, for the fragment stage. It is an input attachment on a device with local
//   read, and a sampled image of the copy otherwise.
// - Set 2 is the bindless set. Binding 0 is an array of samplers, and binding 2 an array of mutable descriptors that
//   holds every buffer and image, updated after it is bound. A handle is an index into one of the arrays.
// - Push constants are kPrGpuMaxPushBytes at offset zero, readable by every stage.
//
// A swap chain for SDR holds Bgra8 in sRGB. One for HDR holds scRGB, Rgba16F in SrgbLinear, where the surface takes it,
// and otherwise HDR10, Rgb10A2 in Rec2100Pq, where the surface takes that, and otherwise SDR.
//
// A host that brings its own device shares three things with the backend, so that the backend's work runs inside the
// host's frames without rebinding anything:
//
// - Descriptors. The host's bindless set holds the backend's descriptors, in ranges of its arrays that the host gives
//   the backend. Binding a second set in the middle of a pass would stall many GPUs, so the backend never binds a set
//   on a command buffer that it shares with the host.
// - The pipeline layout. Every pipeline of the backend is created with the host's layout, which matches the one above
//   in sets 1 and 2 and push constants, so that the host's set and push constants stay bound across the backend's
//   draws.
// - Memory. The backend allocates device memory through the host's functions, so that the host's budget and residency
//   cover it.
//
// A command buffer records into the host's when PrGpuCommandsOptions::native is the host's VkCommandBuffer, which is
// recording and outside any render pass. The host records its own work around the backend's, binds its descriptor set
// and pipeline layout for the backend's draws and dispatches, and ends the VkCommandBuffer. The host then submits it
// with prGpuCommandsSubmit, which submits it to the device's queue (prGpuVulkanQueue), never with vkQueueSubmit. A pass
// of the host's is the dynamic rendering instance that the host has begun in that command buffer.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Allocates and frees device memory for the backend's buffers and textures: `size` bytes aligned to `alignment`, of a
// device-local memory type in `memoryTypeBits`, as a VkMemoryRequirements gives them. The allocation returns its
// VkDeviceMemory and the offset of the bytes within it, and false if no memory is left. The functions are called from
// any thread. The memory that the host's processor maps, for uploads, the stream and readback, the backend allocates
// itself.
typedef bool (*PrGpuVulkanAllocateFn)(void* PR_NULLABLE context, uint64_t size, uint64_t alignment,
                                      uint32_t memoryTypeBits, uint64_t* memory, uint64_t* offset);
typedef void (*PrGpuVulkanFreeFn)(void* PR_NULLABLE context, uint64_t memory, uint64_t offset);

// The host's bindless set and pipeline layout, and the ranges of the set's arrays that the backend writes. Slots
// outside the ranges belong to the host. The first slot of each range is never a handle, so that zero stays no
// resource.
typedef struct PrGpuVulkanDescriptors {
    uint64_t set;                   // the host's VkDescriptorSet, bound at set 2 on every command buffer it records
    uint64_t pipelineLayout;        // the host's VkPipelineLayout
    uint32_t firstResource;
    uint32_t resourceCount;
    uint32_t firstSampler;
    uint32_t samplerCount;
} PrGpuVulkanDescriptors;

// The Vulkan objects that a device is created over. With every object null, the backend creates an instance and a
// device on the system's first discrete adapter, or its first adapter if none is discrete.
typedef struct PrGpuVulkanOptions {
    void* PR_NULLABLE instance;             // the host's VkInstance
    void* PR_NULLABLE physicalDevice;       // the host's VkPhysicalDevice
    void* PR_NULLABLE device;               // the host's VkDevice, created with prGpuVulkanDeviceFeatures
    void* PR_NULLABLE queue;                // a VkQueue that the backend submits its own command buffers to
    uint32_t queueFamily;

    // True if the host's device enabled VK_KHR_dynamic_rendering_local_read and its dynamicRenderingLocalRead feature.
    // A host enables them if the adapter has them.
    bool localRead;

    // The host's descriptors, required with the host's device. Null `allocate` makes the backend allocate memory
    // itself.
    const PrGpuVulkanDescriptors* PR_NULLABLE descriptors;
    PrGpuVulkanAllocateFn PR_NULLABLE allocate;
    PrGpuVulkanFreeFn PR_NULLABLE free;
    void* PR_NULLABLE memoryContext;

    // The number of handles of a device that the backend creates its own set for.
    uint32_t resourceHandles;
    uint32_t samplerHandles;

    // Picks an adapter for a device that the backend creates, by the order in which the system lists them. -1 picks the
    // first discrete adapter.
    int32_t adapter;

    // Enables the validation layer on an instance that the backend creates, and writes its messages to the standard
    // error. The layer must be installed, as the Vulkan SDK installs it. Without it, the instance is created without
    // the layer.
    bool validation;
} PrGpuVulkanOptions;

// Returns the default options: a device that the backend creates on the first discrete adapter, with 65536 resource
// handles and 4096 sampler handles, without validation. The environment variable PR_VULKAN_DEBUG overrides that: 1
// turns the layer on, and 0 turns it off. prGpuDeviceCreate turns the layer on where its options ask for validation.
// PR_VULKAN_ADAPTER picks an adapter by its index.
PrGpuVulkanOptions prGpuVulkanOptionsDefault(void);

// Creates a device over Vulkan. Null `options` means the defaults. Returns NotFound if the system has no Vulkan loader
// or no adapter, and NotSupported if the adapter lacks a feature that the backend needs. On failure, `*out` is null.
PrStatus prGpuVulkanCreate(const PrGpuVulkanOptions* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out);

// Returns the number of validation messages of error severity that the device's instance has reported. Always zero
// without the validation layer. Tests check it after each case.
uint32_t prGpuVulkanValidationErrors(const PrGpuDevice* device);

// Returns the device extensions that a host enables on a device that it creates for the backend, and a chain of Vulkan
// feature structures, starting with a VkPhysicalDeviceFeatures2, to pass to vkCreateDevice. The strings and the chain
// live as long as the process. A host that enables more features adds its own structures to a copy of the chain. Local
// read is one of those features (PrGpuVulkanOptions::localRead).
uint32_t prGpuVulkanDeviceExtensions(const char* PR_NONNULL const* PR_NULLABLE* PR_NONNULL out);
const void* prGpuVulkanDeviceFeatures(void);

// Return the native objects behind the device and its resources, valid while each lives: the VkInstance, the
// VkPhysicalDevice, the VkDevice, the VkQueue that the device submits to, a buffer's VkBuffer and a texture's VkImage.
void* prGpuVulkanInstance(const PrGpuDevice* device);
void* prGpuVulkanPhysicalDevice(const PrGpuDevice* device);
void* prGpuVulkanDevice(const PrGpuDevice* device);
void* prGpuVulkanQueue(const PrGpuDevice* device);
uint64_t prGpuVulkanBuffer(const PrGpuDevice* device, PrGpuBuffer buffer);
uint64_t prGpuVulkanImage(const PrGpuDevice* device, PrGpuTexture texture);

// Returns the loader's vkGetInstanceProcAddr that the device calls Vulkan through, as a PFN_vkGetInstanceProcAddr, for
// a host that has no loader of its own.
void* prGpuVulkanGetInstanceProcAddr(const PrGpuDevice* device);

// Wraps the host's VkImage or VkBuffer in a texture or buffer of the device, with handles, so that the backend's work
// reads or writes it. The host keeps ownership of the native object, which must outlive the wrapper. `desc` describes
// the native object as the host created it.
PrStatus prGpuVulkanTextureWrap(PrGpuDevice* device, uint64_t image, const PrGpuTextureDesc* desc, PrGpuTexture* out);
PrStatus prGpuVulkanBufferWrap(PrGpuDevice* device, uint64_t buffer, const PrGpuBufferDesc* desc, PrGpuBuffer* out);

// Returns the VkImageLayout that a texture has in `access`, for a host that moves its own images into the accesses that
// the backend's work expects.
uint32_t prGpuVulkanLayout(PrGpuAccessFlags access);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
