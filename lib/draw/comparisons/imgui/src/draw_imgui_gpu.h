// The Dear ImGui draw backend on a lib/gpu device: the functions through which the shared part of the backend reaches
// the device.

#pragma once

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/gpu.h"
#include "pr/vec.h"

#include "draw/comparisons/imgui/src/draw_imgui.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the texture id through which the renderer samples the host's texture, whose texels have `alpha`.
ImTextureID imguiHostTexture(PrGpuTexture texture, PrTextureAlpha alpha);

// Creates the renderer's state on `device`, with the backend's new context current. Returns null if the renderer cannot
// start on the device.
ImGuiBackend* PR_NULLABLE imguiDeviceCreate(PrGpuDevice* device);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
