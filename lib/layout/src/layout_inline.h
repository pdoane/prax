// Inline layout: the lines of an inline formatting context beside the floats of its block formatting context, and its
// intrinsic sizes.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-inline-3 - https://drafts.csswg.org/css-inline-3/
// css-text-3 - https://drafts.csswg.org/css-text-3/
//
// A block container opens each run of its inline-level children, and lays out or measures the run one segment at a
// time. Between two segments, the container lays out the block-level box that separates them as a block-level child of
// its own (CSS2/#anonymous-block-level). A run holds a frame of scratch memory while it is open. An atomic inline, or a
// block between segments, may hold runs of its own, so runs nest, and each depth of nesting has a frame of its own.

#pragma once

#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The place of an inline formatting context in its block formatting context.
struct LayoutInlineBfc {
    LayoutContainingBlock containing; // the content box of its container among the floats of the context
    PrLayoutUnit top;                 // its block-start edge: where its first line goes, or where its floats go if it
                                      // has no line

    // `top` holds even if the context has no line. Otherwise its floats wait for the margins before it to resolve.
    bool topFinal;

    // Its first line may resolve the margins before it. Otherwise a context with a line stops before laying the line
    // out.
    bool mayResolve;

    // Its first line is the first formatted line of its container, which text-indent indents
    // (css-text-3/#text-indent-property). No block-level box in flow comes before it in the container.
    bool firstFormatted;

    // The outside markers that its first line that is not phantom holds (LayoutSpace::marker).
    LayoutLineExtent marker;

    // Its container establishes the formatting context and holds nothing else, so nothing outside the context reads its
    // lines or places anything beside them. A layout of the container at another block size, such as the stretched
    // layout of a flex item after its measurement, may then take the lines that its last layout made
    // (LayoutLineRecord). So may a layout at another available size that leaves the same inline size.
    bool reuses;
};

// What laying out an inline formatting context gives its block container.
struct LayoutInlineResult {
    PrLayoutUnit blockSize;         // of its lines, from its block-start edge
    PrLayoutUnit firstBaseline;     // baseline of its first line from the block-start edge of the context, if any
    PrLayoutUnit lastBaseline;      // baseline of its last line from the block-start edge of the context, if any
    bool hasBaselines;
    bool hasLines;                  // true if it has a line box, which resolves the margins before it
    bool stopped;                   // true if it has a line and stopped before laying it out
    LayoutLineSides placed;         // the sides of the floats it placed, if it has no line
    LayoutLineSides unplaced;       // the sides of the floats waiting for the margins before it, if `top` may move
};

struct LayoutInlineFrame;

// A run of inline-level siblings that is open for layout or measurement: its frame of scratch memory, and its shaping.
struct LayoutInlineRun {
    LayoutInlineFrame* frame;
    const LayoutInlineShaping* shaping;
    uint32_t container;             // the block container whose children are the nodes of the run
    LayoutSpace space;              // the content box of the container, for layout
};

// Opens the inline-level siblings from `first` through `last`, which are children of the block container `container`,
// for layout in `space`, the content box of the container. Takes their shaping, and lays out each atomic inline and
// float in the run through its own cache.
LayoutInlineRun layoutInlineBegin(LayoutState& state, uint32_t container, uint32_t first, uint32_t last,
    const LayoutSpace& space);

// Opens the inline-level siblings from `first` through `last` for measurement: takes their shaping.
LayoutInlineRun layoutInlineMeasureBegin(LayoutState& state, uint32_t first, uint32_t last);

// Releases what the run holds: the fragments of the atomic inlines and floats that no line took, and its frame. Runs
// must end in the reverse of the order in which they began.
void layoutInlineEnd(LayoutState& state, LayoutInlineRun& run);

// Returns the number of segments in the run, which is one more than the number of block-level boxes inside its inline
// boxes.
uint32_t layoutInlineSegmentCount(const LayoutInlineRun& run);

// Returns the block-level box that ends segment `segment`, which is not the last.
uint32_t layoutInlineBlockAfter(const LayoutInlineRun& run, uint32_t segment);

// Lays out segment `segment` of the run as one inline formatting context beside the floats of `bfc`. Appends the line
// fragments, and the fragments of the floats it places, to the scratch children, each offset from the inline-start,
// block-start corner of the context.
//
// Inline boxes that are open across a block-level box before the segment continue into it, and those open across one
// after it break there. Each of them has no edge on the side of the block. If `bfc` reuses, the lines are the ones its
// last layout made, if they are still valid. They are kept for the next layout if nothing on them read the block size
// of the container: no float, and no relative offset that is a percentage of the block size.
LayoutInlineResult layoutInlineSegment(LayoutState& state, LayoutInlineRun& run, uint32_t segment,
    const LayoutInlineBfc& bfc);

// Returns the intrinsic sizes of segment `segment` of the run (css-sizing-3/#intrinsic-sizes): its widest piece between
// break opportunities, and its widest line between forced breaks. Each piece or line is widened by the indent of the
// line it starts. Atomic inlines count as their contributions in `input`, and floats count as beside the line they are
// on. `firstFormatted` says whether the first line of the segment is the first formatted line of its container.
LayoutIntrinsicSizes layoutInlineSegmentIntrinsic(LayoutState& state, LayoutInlineRun& run, uint32_t segment,
    const LayoutIntrinsicInput& input, bool firstFormatted);

// Frees the scratch memory of inline layout.
void layoutInlineScratchDestroy(LayoutInlineScratch* PR_NULLABLE scratch);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
