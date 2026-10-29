// The Vulkan backend's shader libraries and pipelines.

#pragma once

#include "gpu/backends/vulkan/src/vulkan_internal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Sets the device's backend functions that vulkan_pipeline.cpp implements.
void vulkanPipelineFunctions(VulkanDevice* d);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
