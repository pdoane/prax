// Text internals: the glyph buffer that a shaping writes into.

#pragma once

#include "pr/raw_array.h"
#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A glyph buffer. It holds the scratch memory that shaping reuses.
struct TextGlyphBuffer {
    RawArray<PrTextGlyph> glyphs;
    RawArray<PrTextFeature> features;          // the merged features of a shaping
    RawArray<PrTextFeature> pieceFeatures;     // the features of a piece, moved into its text
    RawArray<char> piece;                      // the text of a piece
    RawArray<uint32_t> origin;                 // for each byte of the text of the piece, its byte in the run
    RawArray<uint32_t> probe;                  // glyph ids that a probe compares
    RawArray<uint32_t> boundaries;             // the offsets at which grapheme clusters start
    PrTextGlyphBuffer* PR_NULLABLE scratch;    // the buffer that pieces and probes shape into
};

} // namespace pr_impl

// C opaque types
struct PrTextGlyphBuffer : pr_impl::TextGlyphBuffer {};

PR_ASSUME_NONNULL_END
