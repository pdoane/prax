// Unicode normalization over code points, and the tables that tools/gen/gen_unicode.py generates for it.

#pragma once

#include "pr/raw_array.h"
#include "pr/unicode_normalization.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Writes to `*out` the full canonical decomposition of `codePoint` and returns its length, or returns zero if the code
// point has no canonical decomposition. The Hangul syllables decompose by algorithm and are not in the table.
uint32_t unicodeDecomposition(uint32_t codePoint, const uint32_t* PR_NULLABLE* PR_NONNULL out);

// Returns true if `first` followed by `second` composes to a primary composite other than a Hangul syllable, and writes
// the composite to `*composite`.
bool unicodeComposition(uint32_t first, uint32_t second, uint32_t* composite);

// Puts the code points in `form`, in place.
void unicodeNormalize(RawArray<uint32_t>* codePoints, PrUnicodeNormalForm form);

// Returns true if the `count` code points at `codePoints` are in `form`.
bool unicodeIsNormalized(const uint32_t* PR_NULLABLE codePoints, uint32_t count, PrUnicodeNormalForm form);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
