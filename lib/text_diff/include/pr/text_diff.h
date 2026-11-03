// Text diffs: the lines that differ between two texts, and the characters that differ within the lines that changed.
//
// A diff compares two texts line by line, and finds a shortest edit script between them (Myers, "An O(ND) Difference
// Algorithm and Its Variations", 1986), in space that grows with the texts. Then it pairs the lines of each change in
// order, and compares the characters of each pair in the same way. The cost of a comparison grows with the length of
// what it compares times the size of the difference, so a difference past the limit of a diff's options is reported
// coarsely instead: as one change over what remains.
//
// One thread uses a diff at a time.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The results of comparing two texts, which a diff keeps until it compares again.
typedef struct PrTextDiff PrTextDiff;

// Returns line `index` of a text, without its line break. The bytes stay valid until the function is called again for
// the same text, or the comparison ends.
typedef PrStringView (*PrTextDiffLineFn)(void* PR_NULLABLE context, uint32_t index);

// A text that a diff compares, which the host gives a line at a time, so that it may keep the text in pieces.
typedef struct PrTextDiffText {
    uint32_t lineCount;
    PrTextDiffLineFn line;
    void* PR_NULLABLE context;
} PrTextDiffText;

// How hard a diff works.
typedef struct PrTextDiffOptions {
    // The most edits that one comparison of a part of the texts explores before it reports the part as one change.
    uint32_t maxCost;

    // The longest line, in bytes, whose characters are compared with its pair's. A pair with a longer line is reported
    // as one change of both lines whole. Zero compares lines alone, and reports no changes within them.
    uint32_t maxLineBytes;
} PrTextDiffOptions;

// A cost of 4096 edits, and lines of up to 4096 bytes.
PrTextDiffOptions prTextDiffOptionsDefault(void);

// A run of the old text that the new one replaces with a run of its own, in lines, or in bytes within a pair of lines.
// Either run may be empty, for what one side adds. Between changes, the texts are equal.
typedef struct PrTextDiffChange {
    uint32_t oldStart;
    uint32_t oldCount;
    uint32_t newStart;
    uint32_t newCount;
} PrTextDiffChange;

// A change within a pair of changed lines, whose runs are bytes from the start of each line and span whole characters.
typedef struct PrTextDiffLineChange {
    uint32_t oldLine;
    uint32_t newLine;
    PrTextDiffChange bytes;
} PrTextDiffLineChange;

// Creates a diff that holds no changes, or frees one. A null `options` means the defaults.
PrTextDiff* prTextDiffCreate(const PrTextDiffOptions* PR_NULLABLE options);
void prTextDiffDestroy(PrTextDiff* PR_NULLABLE diff);

// Compares `old` with `young`, and replaces the diff's changes with theirs. The texts are read only during the call.
void prTextDiffCompare(PrTextDiff* diff, const PrTextDiffText* old, const PrTextDiffText* young);

// The changes of lines of the last comparison, in order. The pointer is valid until the diff compares again.
uint32_t prTextDiffChangeCount(const PrTextDiff* diff);
const PrTextDiffChange* PR_NULLABLE prTextDiffChanges(const PrTextDiff* diff);

// The changes within the lines that the last comparison paired: the first old line of a change of lines with its first
// new line, and so on until one side runs out. They are in order of the lines and then of the bytes. Unless the options
// compare lines alone, a pair that differs has at least one. The pointer is valid until the diff compares again.
uint32_t prTextDiffLineChangeCount(const PrTextDiff* diff);
const PrTextDiffLineChange* PR_NULLABLE prTextDiffLineChanges(const PrTextDiff* diff);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
