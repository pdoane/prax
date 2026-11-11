// The Vulkan backend's use of the operating system.

#pragma once

#include "gpu/backends/vulkan/src/vulkan_internal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Loads the system's Vulkan loader, and returns its module and vkGetInstanceProcAddr, or null if it has none.
void* PR_NULLABLE vulkanLoaderOpen(PFN_vkGetInstanceProcAddr PR_NULLABLE* PR_NONNULL getInstanceProcAddr);
void vulkanLoaderClose(void* PR_NULLABLE loader);

// Returns the integer value of an environment variable, or `fallback` if it is not set or not an integer.
int32_t vulkanEnvironment(const char* name, int32_t fallback);

// Writes a line to the standard error.
void vulkanLog(const char* prefix, const char* text);

// The instance extension that creates surfaces on this system, or null.
const char* PR_NULLABLE vulkanSurfaceExtension(void);

// Creates a surface over a native one, for a swap chain that `desc` describes.
VkResult vulkanSurfaceCreate(VulkanDevice* d, const PrSurface* surface, const PrGpuSwapChainDesc* desc,
    VkSurfaceKHR PR_NULLABLE* PR_NONNULL out);

#if defined(__APPLE__)
// Returns the CAMetalLayer that an NSView presents. The layer is the view's backing layer if that is a CAMetalLayer,
// and otherwise a new layer that the view hosts. `anchor` places the layer's contents in the view. The view retains the
// layer.
void* vulkanMetalLayer(void* view, PrGpuSwapChainAnchor anchor);
#endif

} // namespace pr_impl

PR_ASSUME_NONNULL_END
