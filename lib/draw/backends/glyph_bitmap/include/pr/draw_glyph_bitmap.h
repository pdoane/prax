// The bitmap glyph decoder, which draws the images of glyphs in bitmap strikes, decoded by an image codec.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a decoder that draws the images of glyphs with no SVG document (prDrawGlyphImage), decoded by `codec`. The
// codec outlives the decoder.
PrDrawGlyphDecoder* prDrawGlyphBitmapCreate(PrImageCodec* codec);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
