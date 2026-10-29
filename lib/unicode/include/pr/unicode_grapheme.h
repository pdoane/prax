// Grapheme clusters
//
// The extended grapheme clusters of Unicode Text Segmentation (https://www.unicode.org/reports/tr29/). A cluster is
// what a reader perceives as one character.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The byte offset where the grapheme cluster that starts at byte `at` of `text` ends, which is where the next cluster
// starts. `text.size` for the last cluster. `at` is a boundary: zero, or a value that an earlier call returned.
size_t prUnicodeGraphemeNext(PrStringView text, size_t at);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
