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

// Returns the texture through which the renderer samples the image, and writes its extent to `extent`. The image is
// uploaded on first use, before the commands submitted after the call. Returns ImTextureID_Invalid if the device cannot
// create the texture.
ImTextureID imguiImageTexture(ImGuiBackend* imgui, const PrDrawImage* image, PrUVec2* extent);

// Returns the texture through which the renderer samples the host's texture.
ImTextureID imguiHostTexture(ImGuiBackend* imgui, PrDrawTextureId texture);

// Creates the renderer's state on `device`, with the backend's new context current. Returns null if the renderer cannot
// start on the device.
ImGuiBackend* PR_NULLABLE imguiDeviceCreate(PrGpuDevice* device);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
