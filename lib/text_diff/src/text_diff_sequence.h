// The comparison under a text diff: two sequences of elements, each named by a key that equal elements share, and a
// shortest edit script between them (Myers, "An O(ND) Difference Algorithm and Its Variations", 1986). A text diff
// compares lines by their hashes, and the characters of a pair of lines by their bytes.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/text_diff.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if element `a` of the old sequence equals element `b` of the new one, which share a key.
using TextDiffEqualFn = bool (*)(void* PR_NULLABLE context, uint32_t a, uint32_t b);

// What two sequences are compared by. A null `equal` takes equal keys as equal elements.
struct TextDiffSequenceDesc {
    const uint64_t* PR_NULLABLE oldKeys;
    uint32_t oldCount;
    const uint64_t* PR_NULLABLE newKeys;
    uint32_t newCount;
    TextDiffEqualFn PR_NULLABLE equal;
    void* PR_NULLABLE equalContext;
};

// A part of a comparison: the old elements from `oldStart` to `oldEnd` against the new from `newStart` to `newEnd`.
struct TextDiffPart {
    uint32_t oldStart;
    uint32_t oldEnd;
    uint32_t newStart;
    uint32_t newEnd;
};

// The memory of comparisons, which a diff keeps between them.
struct TextDiffSequence {
    // The most edits that one comparison of a part of the sequences explores before it reports the part as one change,
    // at least one.
    uint32_t maxCost;
    RawArray<TextDiffPart> parts;

    // By index of a diagonal past the most edits of the search: the furthest that the search forward reached on each,
    // and the search back from the ends.
    RawArray<int64_t> forward;
    RawArray<int64_t> backward;
};

// Writes a key of each line of `text` to `keys`: a hash of its bytes, FNV-1a.
void textDiffLineKeys(const PrTextDiffText* text, RawArray<uint64_t>& keys);

// Two texts whose lines a comparison reads, as the context of textDiffSameLines.
struct TextDiffLines {
    const PrTextDiffText* old;
    const PrTextDiffText* young;
};

// Returns true if line `a` of the old text has the bytes of line `b` of the new one. The context is a TextDiffLines.
bool textDiffSameLines(void* PR_NULLABLE context, uint32_t a, uint32_t b);

// Frees the memory of comparisons.
void textDiffSequenceReset(TextDiffSequence* sequence);

// Appends the changes between the elements of `whole` of the sequences of `desc`, in order. A change that this call
// appends joins another that it appended where they meet, and never a change that `changes` held before. The keys are
// read only during the call.
void textDiffSequenceCompare(TextDiffSequence* sequence, const TextDiffSequenceDesc& desc, TextDiffPart whole,
    RawArray<PrTextDiffChange>& changes);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
