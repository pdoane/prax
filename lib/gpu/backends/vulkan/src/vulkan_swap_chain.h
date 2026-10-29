// The Vulkan backend's swap chains.

#pragma once

#include "gpu/backends/vulkan/src/vulkan_internal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Submits for `c` the waits and signals of its presents, then presents them. Called after the queue submission.
void vulkanSwapChainPresentAll(VulkanDevice* d, VulkanCommands* c);

// Sets the device's backend functions that vulkan_swap_chain.cpp implements.
void vulkanSwapChainFunctions(VulkanDevice* d);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
