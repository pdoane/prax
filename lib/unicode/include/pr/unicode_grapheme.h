// Grapheme clusters
//
// The extended grapheme clusters of Unicode Text Segmentation (https://www.unicode.org/reports/tr29/). A cluster is
// what a reader perceives as one character.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/unicode.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The state of segmentation at the end of a grapheme cluster, which decides whether the next code point continues the
// cluster. Some rules look back further than one code point, and the state holds what they read. Text can therefore be
// segmented one code point at a time, as a terminal receives it.
typedef struct PrUnicodeGraphemeState {
    uint32_t regional;                  // the number of regional indicators that end the cluster
    PrUnicodeGraphemeBreak before;      // the Grapheme_Cluster_Break of the cluster's last code point
    bool pictographic;                  // true if the cluster ends in Extended_Pictographic Extend*
    bool pictographicZwj;               // true if the cluster ends in Extended_Pictographic Extend* ZWJ

    // The progress of an Indic conjunct at the cluster's end: 1 after a Consonant and the Extend code points after it,
    // 2 once a Linker follows them, and 0 otherwise.
    uint8_t conjunct;
} PrUnicodeGraphemeState;

// The state of a cluster that begins with `codePoint`.
PrUnicodeGraphemeState prUnicodeGraphemeStateBegin(uint32_t codePoint);

// Returns true if `codePoint` continues the cluster that `state` ends. Updates `state` to the state after `codePoint`,
// whether it is the last code point of that cluster or the first of the next.
bool prUnicodeGraphemeStatePush(PrUnicodeGraphemeState* state, uint32_t codePoint);

// The byte offset where the grapheme cluster that starts at byte `at` of `text` ends, which is where the next cluster
// starts. `text.size` for the last cluster. `at` is a boundary: zero, or a value that an earlier call returned.
size_t prUnicodeGraphemeNext(PrStringView text, size_t at);

// The byte offset where the word segment that starts at byte `at` of `text` ends, by the word boundaries of the same
// report, which is where the next segment starts. A segment is a word, a run of spaces, or a single other character.
// `text.size` for the last. `at` is a boundary: zero, or a value that an earlier call returned.
size_t prUnicodeWordNext(PrStringView text, size_t at);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
