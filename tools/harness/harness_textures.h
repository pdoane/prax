// The textures that tests draw images with. The tests that render in memory share one memory device. The caches hold
// lib/draw's colors (PrTextureValues_Encoded) and realize in the call that asks, so that the list of a test draws its
// images at once.

#pragma once

#include "pr/draw.h"
#include "pr/draw_cpu.h"
#include "pr/gpu_memory.h"
#include "pr/texture_cache.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Returns a cache on `device` of lib/draw's colors. The cache realizes in the call that asks. The caller destroys the
// cache before the device.
inline PrTextureCache* harnessTextureCacheCreate(PrGpuDevice* device) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    return prTextureCacheCreate(&desc);
}

// Returns the memory device that the tests that render in memory share. The first use makes the device, and it is kept
// for the process.
inline PrGpuDevice* harnessMemoryDevice() {
    static PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    return device;
}

// Returns the cache on the shared memory device.
inline PrTextureCache* harnessMemoryTextures() {
    static PrTextureCache* textures = harnessTextureCacheCreate(harnessMemoryDevice());
    return textures;
}

// Returns a Texture source of `rect` of the image's texels, realized in `textures`. The source has every mip level if
// `flags` has PrTextureCacheFlag_Levels. Returns a source that draws nothing if the image does not realize. The image
// must outlive the lists that draw the source.
inline PrDrawSource harnessImageSource(PrTextureCache* textures, const PrTextureSource* image, PrRect rect,
        PrTextureCacheFlags flags = 0) {
    PrTextureRegion region{};
    if (prTextureCacheWait(textures, image, prTextureCacheAskEncoded(flags), &region) != PrTextureCacheState_Ready)
        return PrDrawSource{};
    return prDrawSourceRegion(&region, rect, 0);
}

// Returns a cpu renderer on the shared memory device, without a glyph decoder. The caller destroys the renderer.
inline PrDrawRenderer* harnessMemoryRenderer() {
    PrDrawCpuOptions options = prDrawCpuOptionsDefault();
    options.device = harnessMemoryDevice();
    return prDrawCpuCreate(&options);
}

// Returns a source of the image's texels in the cache of the shared memory device. See harnessImageSource.
inline PrDrawSource harnessMemoryImage(const PrTextureSource* image, PrRect rect, PrTextureCacheFlags flags = 0) {
    return harnessImageSource(harnessMemoryTextures(), image, rect, flags);
}

} // namespace harness

PR_ASSUME_NONNULL_END
