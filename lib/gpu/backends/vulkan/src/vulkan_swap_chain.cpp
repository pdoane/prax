// The Vulkan backend's swap chains.
//
// A swap chain presents in FIFO order. Its VkSwapchainKHR is made at the first acquire, at the extent that the acquire
// asks for, and made again whenever the extent changes or the surface reports it out of date. Each image is a texture
// of the device, with a slot, that the swap chain owns. Each frame in flight acquires with a semaphore of its own,
// which the submission that presents the frame waits for, and the submission signals a semaphore of the image's own
// that the present waits for.

#include "gpu/backends/vulkan/src/vulkan_swap_chain.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"

#include "gpu/backends/vulkan/src/vulkan_platform.h"
#include "gpu/backends/vulkan/src/vulkan_resources.h"

namespace pr_impl {

namespace {

// The functions of the device's objects, which their creation installs.
extern const PrGpuSwapChain kSwapChainFunctions;

VulkanSwapChain* swapChainOf(PrGpuSwapChain* swapChain) {
    return static_cast<VulkanSwapChain*>(swapChain);
}

// Waits for every frame in flight to finish with the swap chain's images and semaphores. A present's wait on its
// semaphore has no fence, so only an idle queue says that the presentation engine is done with it.
void waitFrames(VulkanDevice* d, VulkanSwapChain* s) {
    PrGpuFence latest = 0;
    for (PrGpuFence fence : s->frameFences) {
        if (fence > latest)
            latest = fence;
    }
    if (latest != 0)
        d->waitFn(d, latest);
    if (!d->lost.load()) {
        prMutexLock(&d->mutex);
        vulkanCheck(d, d->vk.QueueWaitIdle(d->queue));
        prMutexUnlock(&d->mutex);
    }
}

// Frees the textures of the swap chain's images, whose work is done, with their views, and destroys their semaphores.
// Called after waitFrames.
void releaseImages(VulkanDevice* d, VulkanSwapChain* s) {
    prMutexLock(&d->mutex);
    for (uint32_t i = 0; i < s->images.count; ++i) {
        if (s->images.data[i].slot != 0)
            vulkanSlotFree(d, s->images.data[i].slot);
        d->vk.DestroySemaphore(d->device, s->images.data[i].rendered, nullptr);
    }
    prMutexUnlock(&d->mutex);
    s->images.count = 0;
}

// Makes the swap chain's VkSwapchainKHR at `extent`, in place of the one that it had. Returns InvalidState if the
// surface cannot take images now, as while its window is minimized.
PrStatus rebuild(VulkanDevice* d, VulkanSwapChain* s, VkExtent2D requested) {
    waitFrames(d, s);
    VkExtent2D extent = requested;
    VkSurfaceCapabilitiesKHR capabilities;
    if (!vulkanCheck(d, d->vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(d->physical, s->surface, &capabilities)))
        return d->lost.load() ? PrStatus_Lost : PrStatus_InvalidState;
    if (capabilities.maxImageExtent.width == 0 || capabilities.maxImageExtent.height == 0)
        return PrStatus_InvalidState;
    if (extent.width < capabilities.minImageExtent.width)
        extent.width = capabilities.minImageExtent.width;
    if (extent.height < capabilities.minImageExtent.height)
        extent.height = capabilities.minImageExtent.height;
    if (extent.width > capabilities.maxImageExtent.width)
        extent.width = capabilities.maxImageExtent.width;
    if (extent.height > capabilities.maxImageExtent.height)
        extent.height = capabilities.maxImageExtent.height;
    uint32_t images = kSwapChainFrames + 1;
    if (images < capabilities.minImageCount)
        images = capabilities.minImageCount;
    if (capabilities.maxImageCount != 0 && images > capabilities.maxImageCount)
        images = capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = s->surface;
    info.minImageCount = images;
    info.imageFormat = s->format;
    info.imageColorSpace = s->space;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = s->usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = s->compositeAlpha;
    info.presentMode = s->presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = s->swapChain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult result = d->vk.CreateSwapchainKHR(d->device, &info, nullptr, &created);
    releaseImages(d, s);
    if (s->swapChain != VK_NULL_HANDLE)
        d->vk.DestroySwapchainKHR(d->device, s->swapChain, nullptr);
    s->swapChain = VK_NULL_HANDLE;
    if (!vulkanCheck(d, result))
        return d->lost.load() ? PrStatus_Lost : PrStatus_InvalidState;
    s->swapChain = created;
    s->extent = requested;

    uint32_t count = 0;
    d->vk.GetSwapchainImagesKHR(d->device, created, &count, nullptr);
    RawArray<VkImage> handles;
    handles.resize(count);
    d->vk.GetSwapchainImagesKHR(d->device, created, &count, handles.data);
    PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_RenderTarget;
    if ((s->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0)
        usage |= PrGpuTextureUsageFlag_Sampled;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(s->info.format, PrUVec2{extent.width, extent.height}, usage);
    PrStatus status = PrStatus_Ok;
    prMutexLock(&d->mutex);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t slot = vulkanTextureAdd(d, handles.data[i], &desc);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore rendered = VK_NULL_HANDLE;
        if (slot == 0 || !vulkanCheck(d, d->vk.CreateSemaphore(d->device, &semaphoreInfo, nullptr, &rendered))) {
            s->images.push(VulkanSwapImage{handles.data[i], slot, rendered});
            status = PrStatus_Limit;
            break;
        }
        s->images.push(VulkanSwapImage{handles.data[i], slot, rendered});
    }
    prMutexUnlock(&d->mutex);
    handles.reset();
    return status;
}

// Chooses the surface format for the intent, and the format and color space that the swap chain reports.
void chooseFormat(VulkanDevice* d, VulkanSwapChain* s, PrGpuSwapChainIntent intent) {
    uint32_t count = 0;
    d->vk.GetPhysicalDeviceSurfaceFormatsKHR(d->physical, s->surface, &count, nullptr);
    RawArray<VkSurfaceFormatKHR> formats;
    formats.resize(count);
    d->vk.GetPhysicalDeviceSurfaceFormatsKHR(d->physical, s->surface, &count, formats.data);
    struct Choice {
        VkFormat format;
        VkColorSpaceKHR space;
        PrColorSpace reported;
    };
    const Choice hdr[] = {
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, PrColorSpace_SrgbLinear},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT, PrColorSpace_Rec2100Pq},
    };
    const Choice sdr[] = {
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, PrColorSpace_Srgb},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, PrColorSpace_Srgb},
    };
    Choice chosen = {
        formats.count != 0 ? formats.data[0].format : VK_FORMAT_B8G8R8A8_UNORM,
        formats.count != 0 ? formats.data[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        PrColorSpace_Srgb,
    };
    bool found = false;
    if (intent == PrGpuSwapChainIntent_Hdr) {
        for (const Choice& choice : hdr) {
            for (uint32_t i = 0; i < formats.count && !found; ++i) {
                if (formats.data[i].format == choice.format && formats.data[i].colorSpace == choice.space) {
                    chosen = choice;
                    found = true;
                }
            }
        }
    }
    for (const Choice& choice : sdr) {
        for (uint32_t i = 0; i < formats.count && !found; ++i) {
            if (formats.data[i].format == choice.format && formats.data[i].colorSpace == choice.space) {
                chosen = choice;
                found = true;
            }
        }
    }
    formats.reset();
    s->format = chosen.format;
    s->space = chosen.space;
    s->info.format = vulkanGpuFormat(chosen.format);
    s->info.space = chosen.reported;
}

PrStatus swapChainCreate(PrGpuDevice* device, const PrSurface* surface, const PrGpuSwapChainDesc* desc,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    VulkanDevice* d = vulkanDevice(device);
    if (d->vk.CreateSwapchainKHR == nullptr || d->vk.GetPhysicalDeviceSurfaceSupportKHR == nullptr)
        return PrStatus_NotSupported;
    VkSurfaceKHR native = VK_NULL_HANDLE;
    if (vulkanSurfaceCreate(d, surface, desc, &native) != VK_SUCCESS)
        return PrStatus_NotSupported;
    VkBool32 supported = VK_FALSE;
    d->vk.GetPhysicalDeviceSurfaceSupportKHR(d->physical, d->queueFamily, native, &supported);
    if (!supported) {
        d->vk.DestroySurfaceKHR(d->instance, native, nullptr);
        return PrStatus_NotSupported;
    }
    VkSurfaceCapabilitiesKHR capabilities{};
    d->vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(d->physical, native, &capabilities);
    VulkanSwapChain* s = allocZeroed<VulkanSwapChain>();
    static_cast<PrGpuSwapChain&>(*s) = kSwapChainFunctions;
    s->device = device;
    s->surface = native;
    s->presentMode = VK_PRESENT_MODE_FIFO_KHR;
    // A transparent swap chain presents premultiplied alpha where the surface composes it, and opaque elsewhere.
    s->compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((desc->flags & PrGpuSwapChainFlag_Transparent) != 0 &&
            (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0)
        s->compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    s->current = UINT32_MAX;
    // The images may be copied from and into, because a draw that reads its target may copy it. They are also sampled
    // and read as input attachments if the surface allows it.
    constexpr VkImageUsageFlags kOptional = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    s->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        (capabilities.supportedUsageFlags & kOptional);
    chooseFormat(d, s, desc->intent);
    for (VkSemaphore& acquired : s->acquired) {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vulkanCheck(d, d->vk.CreateSemaphore(d->device, &info, nullptr, &acquired));
    }
    *out = s;
    return PrStatus_Ok;
}

void swapChainDestroy(PrGpuSwapChain* swapChain) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(swapChain)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanSwapChain* s = swapChainOf(swapChain);
    waitFrames(d, s);
    releaseImages(d, s);
    if (s->swapChain != VK_NULL_HANDLE)
        d->vk.DestroySwapchainKHR(d->device, s->swapChain, nullptr);
    for (VkSemaphore acquired : s->acquired)
        d->vk.DestroySemaphore(d->device, acquired, nullptr);
    d->vk.DestroySurfaceKHR(d->instance, s->surface, nullptr);
    s->images.reset();
    prMemoryFree(s);
}

void swapChainWait(PrGpuSwapChain* swapChain) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(swapChain)->device;
    VulkanSwapChain* s = swapChainOf(swapChain);
    PrGpuFence fence = s->frameFences[s->frame];
    if (fence != 0)
        device->waitFn(device, fence);
    s->waited = true;
}

PrStatus swapChainAcquire(PrGpuSwapChain* swapChain, PrUVec2 extent, PrGpuTexture* out) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(swapChain)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanSwapChain* s = swapChainOf(swapChain);
    PR_ASSERT(s->current == UINT32_MAX);
    if (!s->waited)
        swapChainWait(swapChain);
    s->waited = false;
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        if (s->swapChain == VK_NULL_HANDLE || s->extent.width != extent.x || s->extent.height != extent.y) {
            PrStatus status = rebuild(d, s, VkExtent2D{extent.x, extent.y});
            if (status != PrStatus_Ok)
                return status;
        }
        uint32_t image = 0;
        VkResult result = d->vk.AcquireNextImageKHR(d->device, s->swapChain, UINT64_MAX, s->acquired[s->frame],
            VK_NULL_HANDLE, &image);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            s->extent = VkExtent2D{0, 0};
            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            vulkanCheck(d, result);
            return d->lost.load() ? PrStatus_Lost : PrStatus_InvalidState;
        }
        s->current = image;
        *out = PrGpuTexture{s->images.data[image].slot};
        return PrStatus_Ok;
    }
    return PrStatus_InvalidState;
}

void swapChainPresent(PrGpuSwapChain* swapChain, PrGpuCommands* commands) {
    VulkanSwapChain* s = swapChainOf(swapChain);
    if (s->current == UINT32_MAX)
        return;
    vulkanCommands(commands)->presents.push(VulkanPresent{s, s->current, s->acquired[s->frame]});
}

// The functions of the device's objects.
constexpr PrGpuSwapChain kSwapChainFunctions = {
    .deviceFn = gpuSharedSwapChainDevice,
    .infoFn = gpuSharedSwapChainInfo,
    .destroyFn = swapChainDestroy,
    .waitFn = swapChainWait,
    .acquireFn = swapChainAcquire,
    .presentFn = swapChainPresent,
};

} // namespace

void vulkanSwapChainPresentAll(VulkanDevice* d, VulkanCommands* c) {
    for (uint32_t i = 0; i < c->presents.count; ++i) {
        const VulkanPresent& present = c->presents.data[i];
        VulkanSwapChain* s = present.swapChain;
        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &s->images.data[present.image].rendered;
        info.swapchainCount = 1;
        info.pSwapchains = &s->swapChain;
        info.pImageIndices = &present.image;
        VkResult result = d->lost.load() ? VK_ERROR_DEVICE_LOST : d->vk.QueuePresentKHR(d->queue, &info);
        // An out-of-date swap chain is made again at the next acquire.
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            s->extent = VkExtent2D{0, 0};
        else
            vulkanCheck(d, result);
        s->frameFences[s->frame] = c->fence;
        s->frame = (s->frame + 1) % kSwapChainFrames;
        s->current = UINT32_MAX;
    }
}

void vulkanSwapChainFunctions(VulkanDevice* d) {
    d->swapChainCreateFn = swapChainCreate;
}

} // namespace pr_impl
