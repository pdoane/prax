// The CPU draw backend: the reference renderer, which renders into pixels in memory.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the backend draws.
typedef struct PrDrawCpuOptions {
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;   // draws the glyphs that are pictures; without it, they draw nothing

    // Draws each glyph at the phase of a target pixel that an atlas holds it at (prDrawGlyphPlace), as a backend that
    // draws glyphs from an atlas does, instead of exactly where its run places it. A glyph then covers the same pixels
    // wherever its run puts it within the phase, so runs that a document splits differently draw alike.
    bool glyphPhases;
} PrDrawCpuOptions;

// Returns options with no glyph decoder, and glyphs drawn where their runs place them.
PrDrawCpuOptions prDrawCpuOptionsDefault(void);

// Creates a renderer that renders in memory, into pixels of either format, and blends sRGB-encoded values. A group's
// layer (PrDrawGroupLayer) is a texture in CPU memory (PrDrawCpuTexture), in Rgba8 or RgbaF16. Null options mean the
// defaults.
PrDrawRenderer* prDrawCpuCreate(const PrDrawCpuOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
