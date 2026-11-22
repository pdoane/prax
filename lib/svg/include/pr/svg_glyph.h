// The glyph decoder of SVG, which draws the SVG documents of glyphs, and their images in bitmap strikes.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/draw_glyph.h"
#include "pr/image.h"
#include "pr/texture_cache.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a decoder for glyphs that are pictures. The decoder draws the SVG document of a glyph that has one. For a
// glyph without a document, it draws the image from the glyph's bitmap strikes (prFontGlyphImage).
//
// The decoder decodes images with `imageCodec` and inflates gzip-compressed documents with `compressCodec`. It draws
// images and the rasters of documents from `textures`. `textures` must be on the device of the renderer that the
// decoder serves. All three must outlive the decoder. If a codec or the cache is null, the content that needs it draws
// nothing.
//
// The decoder keeps the documents that it parsed most recently and the images that it opened. A glyph's image draws
// once its texture arrives. Until then, the glyph cache asks the decoder again at each use. A picture that the glyph
// cache keeps draws textures of the device that `textures` was on when the picture was drawn.
PrDrawGlyphDecoder* prSvgGlyphDecoderCreate(const PrImageCodecSet* PR_NULLABLE imageCodec,
    const PrCompressCodecSet* PR_NULLABLE compressCodec, PrTextureCache* PR_NULLABLE textures);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
