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

// Frees the memory of comparisons.
void textDiffSequenceReset(TextDiffSequence* sequence);

// Replaces `changes` with the changes between the sequences of `desc`, in order. The keys are read only during the
// call.
void textDiffSequenceCompare(TextDiffSequence* sequence, const TextDiffSequenceDesc& desc, RawArray<PrTextDiffChange>& changes);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
