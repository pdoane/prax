// The triangles draw renderer, which turns a list into textured, colored triangles on a GPU device.
//
// The triangles follow the drawing model of Dear ImGui's renderers, with a choice of sampler for each draw. Colors and
// texels are sRGB-encoded 8-bit values with straight alpha, and blend as they are stored.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the backend draws.
typedef struct PrDrawTrianglesOptions {
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder; // draws the glyphs that are pictures; without it, they draw nothing
    uint32_t atlasSize;                           // the side of a glyph atlas texture, in texels
} PrDrawTrianglesOptions;

// Returns options with no glyph decoder, and atlas textures 1024 texels on a side.
PrDrawTrianglesOptions prDrawTrianglesOptionsDefault(void);

// Creates a renderer that renders on `device` into textures. Null options mean the defaults. Returns NotSupported if
// the device cannot run the renderer's shaders, and Lost if the device is lost. On failure, `*out` is null.
PrStatus prDrawTrianglesCreate(PrGpuDevice* device, const PrDrawTrianglesOptions* PR_NULLABLE options,
                               PrDrawRenderer* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
