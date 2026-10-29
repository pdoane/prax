// The Vulkan backend's command buffers, passes and barriers, the stream, and completion.

#pragma once

#include "gpu/backends/vulkan/src/vulkan_internal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Gives a command buffer to the completion thread, which waits for its fence. Called with the device's mutex held, in
// the order of the fences.
void vulkanPend(VulkanDevice* d, VulkanCommands* c);

// Runs the completion thread, which waits for each command buffer's fence in turn, retires it, and raises the fence
// that the device has reached.
void vulkanCompletionRun(void* PR_NULLABLE context);

// Frees the recordings and command pools that the device keeps for reuse, as the device is destroyed.
void vulkanCommandsReset(VulkanDevice* d);

// Sets the device's backend functions that vulkan_commands.cpp implements.
void vulkanCommandsFunctions(VulkanDevice* d);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
