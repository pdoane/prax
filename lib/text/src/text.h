// Text internals: the glyph buffer that a shaping writes into.

#pragma once

#include "pr/array.h"
#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A glyph buffer. It holds the scratch memory that shaping reuses.
struct TextGlyphBuffer {
    Array<PrTextGlyph> glyphs;
    Array<PrTextFeature> features;          // the merged features of a shaping
    Array<PrTextFeature> pieceFeatures;     // the features of a piece, moved into its text
    Array<char> piece;                      // the text of a piece
    Array<uint32_t> origin;                 // for each byte of the text of the piece, its byte in the run
    Array<uint32_t> probe;                  // glyph ids that a probe compares
    Array<uint32_t> boundaries;             // the offsets at which grapheme clusters start
    PrTextGlyphBuffer* PR_NULLABLE scratch; // the buffer that pieces and probes shape into
};

} // namespace pr_impl

// C opaque types
struct PrTextGlyphBuffer : pr_impl::TextGlyphBuffer {};

PR_ASSUME_NONNULL_END
