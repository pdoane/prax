// Dear ImGui's draw data drawn on a lib/gpu device, as Dear ImGui's own renderers draw it: the vertex color times the
// texture's straight color, blended over the target. Each call works in the current Dear ImGui context, and acts as the
// renderer of that context.
//
// The renderer creates and updates the textures that Dear ImGui asks for, such as its font atlas. A texture of the
// device's draws as the ImTextureID of imguiGpuTexture, and rests in the accesses of PrGpuAccessFlag_ShaderRead while
// draw data samples it.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

#include "imgui.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Starts the renderer on `device` in the current context, or shuts it down and frees its textures.
bool imguiGpuInit(PrGpuDevice* device);
void imguiGpuShutdown();

// How the alpha of a texture's texels relates to their color. Dear ImGui blends straight colors, so the renderer reads
// a straight color from each sample.
enum {
    ImGuiGpuAlpha_Straight,         // as Dear ImGui's own textures are
    ImGuiGpuAlpha_Premultiplied,    // the renderer divides the color by the alpha
    ImGuiGpuAlpha_Opaque,           // the alpha reads as one, whatever the texture stores
    ImGuiGpuAlpha_Count,
};
typedef uint8_t ImGuiGpuAlpha;

// Returns the texture id through which draw data samples a texture of the device whose texels have `alpha`.
ImTextureID imguiGpuTexture(PrGpuTexture texture, ImGuiGpuAlpha alpha);

// Draws the draw data into `pass`, whose attachments are the draw data's framebuffer. Leaves the pass's viewport and
// scissor over the whole attachments.
void imguiGpuRender(ImDrawData* data, PrGpuPass* pass);

} // namespace pr

PR_ASSUME_NONNULL_END
