// The Dear ImGui draw backend, which records a list through the stock ImDrawList and draws it as Dear ImGui's renderers
// draw its draw data, on a lib/gpu device.
//
// A command that ImDrawList has a call for is drawn with that call. A command without such a call is left out instead
// of being built from triangles.

#pragma once

#include "pr/base.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a renderer on `device`, which renders into textures, and has its own Dear ImGui context. Returns false if the
// renderer cannot start on the device, and `*out` is then null.
bool prDrawImGuiCreate(PrGpuDevice* device, PrDrawRenderer* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
