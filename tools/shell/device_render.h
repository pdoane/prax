// Draw lists rendered by a renderer of lib/draw on the platform's GPU device, and read back.

#pragma once

#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/draw_select.h"
#include "pr/texture_cache.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace shell {

// A device, a renderer on it, and the texture cache of the device.
struct DeviceRender;

// Creates the renderer of `backend`, which renders on a device, on the system's default device. Returns null if the
// platform has no device that it runs on.
DeviceRender* PR_NULLABLE deviceRenderCreate(PrDrawRendererBackend backend);

// Frees the renderer, the cache and the device. Every page that realizes its images in the cache must be gone.
void deviceRenderDestroy(DeviceRender* PR_NULLABLE render);

// Returns the texture cache on the renderer's device. A list that the renderer draws realizes its images in this cache.
// The cache realizes each image in the call that first asks for it.
PrTextureCache* deviceRenderTextures(DeviceRender* render);

// Returns the renderer. A page that the renderer draws makes its canvas bitmaps on the renderer's device, and draws
// into them with it.
PrDrawRenderer* deviceRenderRenderer(DeviceRender* render);

// Renders `list` into a texture of `extent` cleared to transparent, as the cpu renderer starts from transparent pixels,
// and reads it back into `rgba`, `extent` of 8-bit RGBA. Returns false if the render fails.
bool deviceRenderPixels(DeviceRender* render, const PrDrawList* list, PrUVec2 extent, uint8_t* rgba);

} // namespace shell

PR_ASSUME_NONNULL_END
