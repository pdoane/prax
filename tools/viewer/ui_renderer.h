// The renderer that draws the viewer's controls on the device, through the Dear ImGui renderer on lib/gpu
// (imgui_gpu.h). Each call works in the current Dear ImGui context, which is the controls' own.

#pragma once

#include "pr/gpu.h"

#include "imgui.h"

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

struct UiRenderer;

// Starts the renderer on `device`, a device of prGpuDeviceCreate, or stops it and frees it.
UiRenderer* uiRendererCreate(PrGpuDevice* device);
void uiRendererDestroy(UiRenderer* PR_NULLABLE renderer);

// Returns the texture id through which the frame's draw data samples `texture`, which is in the accesses of shader
// reads while the frame draws it.
ImTextureID uiRendererTexture(UiRenderer* renderer, PrGpuTexture texture);

// Draws the frame's draw data into `pass`, which is open over the frame.
void uiRendererRender(UiRenderer* renderer, ImDrawData* data, PrGpuPass* pass);

} // namespace viewer

PR_ASSUME_NONNULL_END
