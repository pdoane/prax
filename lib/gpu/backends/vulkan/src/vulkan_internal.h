// The Vulkan GPU backend's state, shared by its sources.
//
// The backend loads the system's Vulkan loader when it creates a device, and calls Vulkan through a table of functions
// that it loads for that device, so that devices of different drivers coexist and nothing links to an SDK.
//
// A resource's id is its slot in the bindless array of mutable descriptors, and a sampler's id its slot in the array of
// samplers. The device's tables are indexed by slot, reserved as address space so that a slot's entry never moves while
// other threads read it. A slot is reused only once every command buffer that could have used it is done.
//
// Every submission goes to the device's one queue and signals its timeline semaphore with the next fence. A thread of
// the device's own waits for each command buffer in turn, then runs its callbacks, recycles what it held, and raises
// the count of fences reached, so that a fence is reached only after its callbacks have run.

#pragma once

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/gpu_backend.h"
#include "pr/gpu_vulkan.h"
#include "pr/mutex.h"
#include "pr/reserved_array.h"
#include "pr/thread.h"

// The backend calls Vulkan through the functions that it loads for each device, never through the loader's exports.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The functions that the backend calls, by their names without the vk prefix: those that the loader gives, then those
// of the instance and those of the device.
#define PR_VULKAN_GLOBAL_FUNCTIONS(X) \
    X(CreateInstance) \
    X(EnumerateInstanceExtensionProperties) \
    X(EnumerateInstanceLayerProperties) \
    X(EnumerateInstanceVersion)

// The functions that the instance gives.
#define PR_VULKAN_INSTANCE_FUNCTIONS(X) \
    X(CreateDebugUtilsMessengerEXT) \
    X(CreateDevice) \
    X(DestroyDebugUtilsMessengerEXT) \
    X(DestroyInstance) \
    X(DestroySurfaceKHR) \
    X(EnumerateDeviceExtensionProperties) \
    X(EnumeratePhysicalDevices) \
    X(GetDeviceProcAddr) \
    X(GetPhysicalDeviceFeatures2) \
    X(GetPhysicalDeviceFormatProperties) \
    X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR) \
    X(GetPhysicalDeviceSurfacePresentModesKHR) \
    X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(SetDebugUtilsObjectNameEXT)

// The functions that the device gives.
#define PR_VULKAN_DEVICE_FUNCTIONS(X) \
    X(AcquireNextImageKHR) \
    X(AllocateCommandBuffers) \
    X(AllocateDescriptorSets) \
    X(AllocateMemory) \
    X(BeginCommandBuffer) \
    X(BindBufferMemory) \
    X(BindImageMemory) \
    X(CmdBeginDebugUtilsLabelEXT) \
    X(CmdBeginRendering) \
    X(CmdBindDescriptorSets) \
    X(CmdBindIndexBuffer) \
    X(CmdBindPipeline) \
    X(CmdCopyBuffer) \
    X(CmdCopyBufferToImage) \
    X(CmdCopyImage) \
    X(CmdCopyImageToBuffer) \
    X(CmdDispatch) \
    X(CmdDispatchIndirect) \
    X(CmdDraw) \
    X(CmdDrawIndexed) \
    X(CmdDrawIndexedIndirect) \
    X(CmdDrawIndirect) \
    X(CmdEndDebugUtilsLabelEXT) \
    X(CmdEndRendering) \
    X(CmdFillBuffer) \
    X(CmdPipelineBarrier2) \
    X(CmdPushConstants) \
    X(CmdPushDescriptorSetKHR) \
    X(CmdResetQueryPool) \
    X(CmdSetBlendConstants) \
    X(CmdSetScissor) \
    X(CmdSetStencilReference) \
    X(CmdSetViewport) \
    X(CmdWriteTimestamp2) \
    X(CreateBuffer) \
    X(CreateCommandPool) \
    X(CreateComputePipelines) \
    X(CreateDescriptorPool) \
    X(CreateDescriptorSetLayout) \
    X(CreateGraphicsPipelines) \
    X(CreateImage) \
    X(CreateImageView) \
    X(CreatePipelineLayout) \
    X(CreateQueryPool) \
    X(CreateSampler) \
    X(CreateSemaphore) \
    X(CreateShaderModule) \
    X(CreateSwapchainKHR) \
    X(DestroyBuffer) \
    X(DestroyCommandPool) \
    X(DestroyDescriptorPool) \
    X(DestroyDescriptorSetLayout) \
    X(DestroyDevice) \
    X(DestroyImage) \
    X(DestroyImageView) \
    X(DestroyPipeline) \
    X(DestroyPipelineLayout) \
    X(DestroyQueryPool) \
    X(DestroySampler) \
    X(DestroySemaphore) \
    X(DestroyShaderModule) \
    X(DestroySwapchainKHR) \
    X(DeviceWaitIdle) \
    X(EndCommandBuffer) \
    X(FreeMemory) \
    X(GetBufferMemoryRequirements) \
    X(GetDeviceQueue) \
    X(GetImageMemoryRequirements) \
    X(GetQueryPoolResults) \
    X(GetSemaphoreCounterValue) \
    X(GetSwapchainImagesKHR) \
    X(MapMemory) \
    X(QueuePresentKHR) \
    X(QueueSubmit2) \
    X(QueueWaitIdle) \
    X(ResetCommandPool) \
    X(UpdateDescriptorSets) \
    X(WaitSemaphores)

// Declares a member of VulkanFunctions.
#define PR_VULKAN_MEMBER(name) PFN_vk##name name;

// The Vulkan functions of one device. A function of an extension that the device lacks is null.
struct VulkanFunctions {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PR_VULKAN_GLOBAL_FUNCTIONS(PR_VULKAN_MEMBER)
    PR_VULKAN_INSTANCE_FUNCTIONS(PR_VULKAN_MEMBER)
    PR_VULKAN_DEVICE_FUNCTIONS(PR_VULKAN_MEMBER)
};

// The member declarations end with VulkanFunctions.
#undef PR_VULKAN_MEMBER

// The sets of the pipeline layout, as pr/gpu.slang and cmake/shader_library.cmake place them. Set 0 is empty, because
// Slang keeps it for registers that no binding names.
enum {
    VulkanSet_Push = 1,             // the uniform buffer and the target that a draw reads, as push descriptors
    VulkanSet_Bindless = 2,         // the samplers at binding 0, and the mutable descriptors at binding 2
    VulkanSet_Count,
};
typedef uint32_t VulkanSet;

// The bindings of the bindless set.
enum {
    VulkanBinding_Samplers = 0,
    VulkanBinding_Resources = 2,
};
typedef uint32_t VulkanBinding;

// The size of the stream's chunks. A larger allocation takes a chunk of its own.
constexpr uint32_t kStreamChunkBytes = 4u << 20;

// The alignment of the stream's allocations, which pr/gpu.h promises.
constexpr uint64_t kStreamAlignment = 256;

// The size of a page of upload memory. A larger upload takes a page of its own.
constexpr uint64_t kStagingPageBytes = uint64_t(8) << 20;

// The frames that a swap chain has in flight.
constexpr uint32_t kSwapChainFrames = 2;

// Device memory: an allocation of the device's own, or a range of the host's.
struct VulkanMemory {
    VkDeviceMemory memory;
    uint64_t offset;
    uint8_t* mapped;            // the bytes, for memory that the host maps, or null
    bool host;                  // allocated through the host's functions
};

// An image and its memory, held by the texture that made it and by each view of it, and freed with the last of them.
// The image of a swap chain or of the host is not the device's to free.
struct VulkanImage {
    VkImage image;
    VulkanMemory memory;
    uint32_t references;
    bool owned;
};

// The kinds of a slot of the bindless array.
enum {
    VulkanSlotKind_Free,
    VulkanSlotKind_Buffer,
    VulkanSlotKind_Texture,
    VulkanSlotKind_StorageView,     // the storage view of the texture at `owner`
};
typedef uint8_t VulkanSlotKind;

// A view of a texture as an attachment, made when a pass first uses that level and layer.
struct VulkanAttachmentView {
    uint8_t level;
    uint16_t layer;
    VkImageView view;
};

// A slot of the bindless array, and what it holds.
struct VulkanResource {
    VulkanSlotKind kind;
    bool wrapped;               // the buffer is the host's
    uint32_t owner;             // for a storage view, the texture; otherwise zero
    uint32_t storage;           // a texture's storage slot, or zero
    VulkanMemory memory;        // the memory of a buffer that the device made
    VkBuffer buffer;
    PrGpuBufferDesc bufferDesc;
    VulkanImage* PR_NULLABLE held;  // a texture's image, which the texture holds a reference to
    VkImage image;
    VkImageView view;           // the sampled view of a texture, or the storage view of a storage slot
    VkImageAspectFlags aspect;
    PrGpuTextureDesc textureDesc;
    uint8_t firstLevel;         // the level and layer of the image that a view starts at
    uint16_t firstLayer;
    Array<VulkanAttachmentView> attachments;
};

// A sampler, by its slot.
struct VulkanSampler {
    VkSampler sampler;
};

// A pipeline, by its id less one.
struct VulkanPipeline {
    VkPipeline pipeline;
    VkPipelineBindPoint bindPoint;
    PrGpuExtent groupSize;
    bool readsTarget;
};

// A chunk of the stream: memory that the GPU reads and the host writes, with a slot of the bindless array.
struct VulkanStreamChunk {
    uint32_t slot;
    VkBuffer buffer;
    uint8_t* mapped;
    uint32_t size;
};

// A page of upload memory, which uploads fill in turn and which is freed once the work that copies from it is done.
struct VulkanStagingPage {
    VkBuffer buffer;
    VulkanMemory memory;
    uint64_t size;
    uint64_t used;
};

// What an upload does to its target.
enum {
    VulkanUploadKind_Copy,          // copies from a staging page into a buffer or a texture
    VulkanUploadKind_Fill,          // fills a new buffer with zeros
    VulkanUploadKind_Initialize,    // moves a new texture into the layout of shader reads
};
typedef uint8_t VulkanUploadKind;

// An upload that waits for the next command buffer to record it.
struct VulkanUpload {
    VulkanUploadKind kind;
    uint32_t page;              // an index into the device's pages
    uint64_t offset;
    uint32_t target;            // a buffer's or a texture's slot
    uint64_t targetOffset;      // in a buffer
    uint64_t size;              // the bytes, for a buffer
    PrGpuTextureRegion region;  // for a texture
    uint32_t rowTexels;         // the source's rows and images, in texels
    uint32_t imageRows;
};

// The kinds of what is retired.
enum {
    VulkanRetireKind_Slot,          // a buffer or texture, by its slot
    VulkanRetireKind_Pipeline,      // by its id
};
typedef uint8_t VulkanRetireKind;

// Something to free once every command buffer begun before it was retired is done, and the uploads that write into it
// are copied. A command buffer begun after it cannot use it, except to copy those uploads, which then hold it too.
struct VulkanRetired {
    VulkanRetireKind kind;
    uint32_t index;
    uint64_t serial;            // the serial of the first command buffer begun after it
};

// A callback to call once a command buffer is done.
struct VulkanDoneCall {
    PrGpuDoneFn done;
    void* PR_NULLABLE context;
};

// A command pool and its command buffer, with a query pool that times its work, reused once the work is done.
struct VulkanCommandSlot {
    VkCommandPool pool;
    VkCommandBuffer buffer;
    VkQueryPool queries;
};

// A pass that a command buffer records, the device's own or one that the host has open. A pass of the device's own
// keeps its attachments, so that it can end its rendering and begin it again when a draw reads its target.
struct VulkanPass : PrGpuPass {
    VkRenderingAttachmentInfo colors[kPrGpuMaxColorAttachments];
    VkRenderingAttachmentInfo depth;
    VkRenderingAttachmentInfo stencil;
    bool hasDepth;
    bool hasStencil;
    PrGpuAttachment target;         // the first color attachment, which a draw that reads the target reads
    VkImageView targetView;
    VkRect2D scissor;               // the scissor of the draws, which bounds the copy of the target
    bool localRead;                 // true once the first color attachment is in the layout of local reads
    bool copied;                    // true once a draw of the pass has copied the target
    bool named;                     // true if the pass opened a group of commands for debuggers
};

// The swap chain, which a present names before it is defined.
struct VulkanSwapChain;

// A present that a command buffer's submission waits and signals for.
struct VulkanPresent {
    VulkanSwapChain* swapChain;
    uint32_t image;
    VkSemaphore acquired;
};

// A command buffer: the state of its recording, and what it holds until it is done.
struct VulkanCommands : PrGpuCommands {
    VulkanCommandSlot slot;         // the device's own, which ends a command buffer that records into the host's
    VkCommandBuffer buffer;
    uint64_t serial;
    uint64_t fence;                 // the value on the command buffer's timeline, once submitted
    bool timed;

    // The state of draws and dispatches: the pipeline, and the push descriptors, pushed for each bind point when a draw
    // or dispatch first uses them.
    uint32_t pipeline;
    VkBuffer uniformBuffer;
    uint64_t uniformOffset;
    uint32_t uniformSize;
    bool pushDirty[2];              // for graphics and compute
    PrGpuHostStateFlags changed;    // the state that the device changed since the last sync

    // The stream: the chunks that the command buffer took, the last of which it fills.
    Array<VulkanStreamChunk> chunks;
    uint64_t streamUsed;

    Array<VulkanDoneCall> done;
    Array<VulkanPresent> presents;
    Array<uint32_t> pages;          // the staging pages of the uploads that the command buffer copies
    VulkanCommandSlot upload;       // the command buffer that copies the uploads ahead of this one, if any
    Array<uint8_t> scratch;         // the stream's bytes once the device is lost, which the GPU never reads
    VulkanPass pass;

    // The texture that holds the copy of the target, which a draw that reads its target samples on a device without
    // local read. The texture is kept after the command buffer is done, for the next command buffer that reuses this
    // recording. A pass that needs a larger texture or another format replaces it.
    PrGpuTexture targetCopy;
};

// A shader library: one SPIR-V module that holds every entry point.
struct VulkanLibrary : PrGpuShaderLibrary {
    VkShaderModule module;
};

// An image of a swap chain, the texture that wraps it, and the semaphore that its present waits for.
struct VulkanSwapImage {
    VkImage image;
    uint32_t slot;
    VkSemaphore rendered;           // signalled by the submission that presents the image
};

// A swap chain, and the frames in flight that use its images.
struct VulkanSwapChain : PrGpuSwapChain {
    VkSurfaceKHR surface;
    VkSwapchainKHR swapChain;
    VkFormat format;
    VkColorSpaceKHR space;
    VkPresentModeKHR presentMode;
    VkImageUsageFlags usage;
    VkExtent2D extent;              // the extent that the last acquire asked for, which the images may clamp
    Array<VulkanSwapImage> images;
    VkSemaphore PR_NULLABLE acquired[kSwapChainFrames]; // used in turn, one for each frame in flight
    PrGpuFence frameFences[kSwapChainFrames];           // the fence of the frame that last used each of `acquired`
    uint32_t frame;
    uint32_t current;               // the acquired image, or UINT32_MAX
    bool waited;
};

// A device: its Vulkan objects, its tables of resources and pipelines, and its timelines.
struct VulkanDevice : PrGpuDevice {
    VulkanFunctions vk;
    void* PR_NULLABLE loader;
    bool ownsInstance;
    bool ownsDevice;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamily;
    VkDebugUtilsMessengerEXT messenger;
    Atomic<uint32_t> validationErrors;
    Atomic<bool> lost;
    char adapter[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    VkPhysicalDeviceLimits limits;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    bool localRead;                 // true if a draw reads its target as an input attachment, not as a copy

    // Memory from the host, or the device's own.
    PrGpuVulkanAllocateFn PR_NULLABLE allocate;
    PrGpuVulkanFreeFn PR_NULLABLE free;
    void* PR_NULLABLE memoryContext;

    // The bindless set and the pipeline layout, the host's or the device's own.
    bool ownsDescriptors;
    VkDescriptorPool descriptorPool;
    VkDescriptorSetLayout emptyLayout;
    VkDescriptorSetLayout pushLayout;
    VkDescriptorSetLayout setLayout;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    uint32_t firstResource;
    uint32_t resourceCount;
    uint32_t firstSampler;
    uint32_t samplerCount;

    // Guards everything below.
    PrMutex mutex;
    ReservedArray<VulkanResource> resources;    // by slot
    Array<uint32_t> freeSlots;
    uint32_t nextSlot;
    Array<VulkanSampler> samplers;              // by slot less firstSampler
    ReservedArray<VulkanPipeline> pipelines;    // by id less one
    Array<uint32_t> freePipelines;
    Array<VulkanStreamChunk> freeChunks;
    Array<VulkanCommandSlot> freeCommandSlots;
    Array<VulkanCommands*> freeCommands;
    Array<VulkanStagingPage> pages;             // by index; a page with no buffer is free
    uint32_t openPage;                          // the page that uploads fill, or UINT32_MAX
    Array<VulkanUpload> uploads;                // waiting for a command buffer
    Array<VulkanRetired> retired;
    Array<uint64_t> openSerials;                // of the command buffers begun and not yet done
    uint64_t nextSerial;
    uint64_t issued;                            // the fence of the last submission

    // The timeline that submissions signal.
    VkSemaphore timeline;

    // The command buffers that wait for their timeline, in the order of their fences, and the thread that retires them.
    // `doneMutex` guards them, `stopping` and raising `reached`. `pendingSignal` wakes the thread for a new command
    // buffer or to stop, and `reachedSignal` wakes the threads that wait for a fence.
    PrMutex doneMutex;
    Array<VulkanCommands*> pending;
    PrCondition pendingSignal;
    PrCondition reachedSignal;
    Atomic<uint64_t> reached;                   // the fence of the last command buffer that is retired
    PrThread completion;
    bool running;                   // the completion thread has started
    bool stopping;
};

// Returns the resource in `slot`, which must hold one.
inline VulkanResource& vulkanResourceAt(VulkanDevice* d, uint32_t slot) {
    PR_ASSERT(slot != 0 && slot < d->resources.count);
    return d->resources.data[slot];
}
inline const VulkanResource& vulkanResourceAt(const VulkanDevice* d, uint32_t slot) {
    PR_ASSERT(slot != 0 && slot < d->resources.count);
    return d->resources.data[slot];
}

// Returns the type of the push descriptor through which a pipeline that reads its target reads it: an input attachment
// on a device with local read, and a sampled image of the copy otherwise.
inline VkDescriptorType vulkanTargetDescriptor(const VulkanDevice* d) {
    return d->localRead ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}

// Return the backend's state behind the interface's objects.
inline VulkanDevice* vulkanDevice(PrGpuDevice* device) {
    return static_cast<VulkanDevice*>(device);
}
inline const VulkanDevice* vulkanDevice(const PrGpuDevice* device) {
    return static_cast<const VulkanDevice*>(device);
}
inline VulkanCommands* vulkanCommands(PrGpuCommands* commands) {
    return static_cast<VulkanCommands*>(commands);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
