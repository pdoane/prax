// The fragment draw renderer, which turns a list into instances whose coverage, clips and paint a fragment shader
// evaluates for each pixel, on a GPU device.

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

// How the coverage of a path is prepared when the path is antialiased, no analytic shape describes it, and its paint is
// not mapped along a stroke. Other paths are tessellated.
enum {
    PrDrawFragmentPaths_Tessellate,     // triangles with a fringe of vertex coverage
    PrDrawFragmentPaths_Strips,         // Bytes coverage where edges pass, and Solid between
    PrDrawFragmentPaths_Tiles,          // Tiles coverage where edges pass, and Solid between
};
typedef uint8_t PrDrawFragmentPaths;

// What the backend does where a list embeds another. A frozen list's records depend only on its commands and on the
// state where it is embedded: the transform, the clip, the opacity and how it composites.
enum {
    // The backend keeps the records that it made for a list, and copies them where a later frame embeds the list in the
    // same state, or in the same state moved by whole target pixels.
    PrDrawFragmentEmbeds_Keep,

    PrDrawFragmentEmbeds_Walk,          // the backend walks the list's commands at every embed
};
typedef uint8_t PrDrawFragmentEmbeds;

// The smallest side length of a glyph atlas page, in texels. A smaller `atlasSize` is raised to it.
enum { kPrDrawFragmentAtlasMinSize = 64 };

// How the backend draws.
typedef struct PrDrawFragmentOptions {
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;   // draws the glyphs that are pictures; without it, they draw nothing
    PrDrawFragmentPaths paths;
    uint32_t atlasSize;                     // the side length of the glyph atlas pages, in texels
    PrDrawFragmentEmbeds embeds;
} PrDrawFragmentOptions;

// Returns options with no glyph decoder, Tiles paths, atlas pages of 1024 texels on a side, and embeds kept.
PrDrawFragmentOptions prDrawFragmentOptionsDefault(void);

// Creates a renderer that renders on `device` into textures. Null options mean the defaults. Returns NotSupported if
// the device cannot run the renderer's shaders, and Lost if the device is lost. On failure, `*out` is null.
PrStatus prDrawFragmentCreate(PrGpuDevice* device, const PrDrawFragmentOptions* PR_NULLABLE options,
    PrDrawRenderer* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
