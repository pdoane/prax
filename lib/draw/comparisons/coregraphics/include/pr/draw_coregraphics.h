// The CoreGraphics draw backend, which renders with Quartz 2D into pixels in memory.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the backend draws.
typedef struct PrDrawCoreGraphicsOptions {
    bool fontSmoothing;                           // Quartz's font smoothing, which thickens glyphs
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder; // draws the glyphs that are pictures; without it, they draw nothing
} PrDrawCoreGraphicsOptions;

// Returns options with font smoothing, without a glyph decoder.
PrDrawCoreGraphicsOptions prDrawCoreGraphicsOptionsDefault(void);

// Creates a renderer that renders in memory, into pixels of either format, through a bitmap context in sRGB. Null
// options mean the defaults.
PrDrawRenderer* prDrawCoreGraphicsCreate(const PrDrawCoreGraphicsOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
