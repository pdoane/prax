// Draw lists rendered by a renderer of lib/draw on the platform's GPU device, and read back.

#pragma once

#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace shell {

// A device and a renderer on it.
struct DeviceRender;

// Creates the renderer of `backend`, which renders on a device, on the system's default device. Returns null if the
// platform has no device that it runs on.
DeviceRender* PR_NULLABLE deviceRenderCreate(PrDrawRendererBackend backend);

// Frees the renderer and its device.
void deviceRenderDestroy(DeviceRender* PR_NULLABLE render);

// Renders `list` into a texture of `extent` cleared to transparent, as the cpu renderer starts from transparent pixels,
// and reads it back into `rgba`, `extent` of 8-bit RGBA. Returns false if the render fails.
bool deviceRenderPixels(DeviceRender* render, const PrDrawList* list, PrUVec2 extent, uint8_t* rgba);

} // namespace shell

PR_ASSUME_NONNULL_END
