// The glyph decoder of SVG, which draws the SVG documents of glyphs, and their images in bitmap strikes.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/draw_glyph.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a decoder that draws the SVG documents of glyphs. It decodes the images of glyphs without a document
// (prDrawGlyphImage) through `imageCodec`, and inflates gzip-compressed documents through `compressCodec`. Without a
// codec, the data that the codec would read draws nothing. Both codecs outlive the decoder. The decoder keeps the
// documents that it parsed most recently.
PrDrawGlyphDecoder* prSvgGlyphDecoderCreate(PrImageCodec* PR_NULLABLE imageCodec,
                                            PrCompressCodec* PR_NULLABLE compressCodec);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
