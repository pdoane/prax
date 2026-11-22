// The renderers of lib/draw and its comparisons, in the configurations worth comparing, which the viewer and draw_bench
// render with.

#pragma once

#include "pr/draw_renderer.h"
#include "pr/gpu.h"
#include "pr/texture_cache.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// A renderer in one configuration. The configurations of one renderer form a family, adjacent in the list. The first
// configuration of a family is the renderer's defaults.
struct RendererConfig {
    const char* PR_NONNULL name;
    PrDrawRenderer* PR_NONNULL renderer;
    const char* PR_NONNULL familyName;
    uint32_t family;                     // the family's position among the families, from 0
    PrTextureCache* PR_NONNULL textures; // the cache on the renderer's device, where its lists realize images
};

// Creates each renderer in each configuration worth comparing, and the image codec that they read bitmap glyphs with.
// Renderers that render on a device render on `device`, a device of prGpuDeviceCreate that outlives them. Without a
// device, those renderers are left out. The renderers that render in memory share a memory device. Each device has a
// texture cache. The cache realizes in the call that asks (PrTextureCacheDesc), so that a list recorded for a renderer
// draws its images at once. The cache packs the images as `pack` says.
std::vector<RendererConfig> createConfigs(PrGpuDevice* PR_NULLABLE device,
    PrTextureCachePack pack = PrTextureCachePack_Auto);

// Marks the end of a frame in the texture caches that createConfigs made (prTextureCacheTrim). The renderers share the
// caches, so a program calls this once for each frame or each drawing of its own, not once for each renderer. Any
// thread may call it.
void trimConfigs();

// Destroys the renderers of the configurations, then the codecs, the glyph decoders, the caches and the memory device
// that createConfigs made.
void destroyConfigs(std::vector<RendererConfig>& configs);

// Returns the configuration that the viewer opens on: the fragment renderer's default configuration, or the first
// configuration if there is no fragment renderer.
uint32_t defaultConfig(const std::vector<RendererConfig>& configs);

// Returns the number of families among the configurations.
uint32_t familyCount(const std::vector<RendererConfig>& configs);

// Returns the index of the configuration after `index` in its family. The family's last configuration is followed by
// its first.
uint32_t nextInFamily(const std::vector<RendererConfig>& configs, uint32_t index);

// Returns the frame to render with `renderer`. That is `frame` if `madeBy`, the renderer that made it, is `renderer`.
// Otherwise, `frame` is destroyed and replaced with a new frame, and `madeBy` with `renderer`.
PrDrawFrame* PR_NONNULL frameFor(PrDrawFrame* PR_NULLABLE& frame, PrDrawRenderer* PR_NULLABLE& madeBy,
    PrDrawRenderer* PR_NONNULL renderer);

} // namespace harness

PR_ASSUME_NONNULL_END
