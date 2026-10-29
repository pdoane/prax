// The Blend2D draw backend, which renders with Blend2D into pixels in memory.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the backend draws.
typedef struct PrDrawBlend2dOptions {
    // Zero renders synchronously on the calling thread. Any other count renders asynchronously on that many workers,
    // including the calling thread.
    uint32_t threads;

    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;   // draws the glyphs that are pictures; without it, they draw nothing
} PrDrawBlend2dOptions;

// Returns options for synchronous rendering, without a glyph decoder.
PrDrawBlend2dOptions prDrawBlend2dOptionsDefault(void);

// Creates a renderer that renders in memory, into pixels of either format. Null options mean the defaults.
PrDrawRenderer* prDrawBlend2dCreate(const PrDrawBlend2dOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
