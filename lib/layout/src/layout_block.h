// Block layout: the entry points that lay out, measure and finally lay out a block-level box, through its cache, and
// the initial containing block.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-display-4 - https://drafts.csswg.org/css-display-4/
// css-overflow-3 - https://drafts.csswg.org/css-overflow-3/
//
// Every block-level box is laid out by its shell, which sizes it, hands its content to the algorithm that its children
// name (layout_content.h), and finishes it around what the content gave.

#pragma once

#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Lays out the block-level box `node`, which is a block container or a replaced box, in `space`, or returns its cached
// result if that result is still valid. A box that is in flow in a context records what it reads of the context in the
// reads of the context of `space`.
LayoutResult layoutBlock(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space);

// Lays out the block-level box `node` in the fragmentainer of `space`, from the token that `space` resumes from, if
// any, and keeps nothing in its cache (layout_fragmentation.h). A box that may not break is laid out as layoutBlock
// lays it out.
LayoutResult layoutBlockFragmented(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests* PR_NULLABLE requests = nullptr);

// Measures what `kind` asks for of the flex item `node` in `space`, at the `requests` of its container. The inline
// request must be Fixed. Returns the answer from the item's measurement records if one is valid. Otherwise lays the
// item out and keeps the answer from that layout in its records.
//
// If that layout is not the one held in the item's layout entry, it is handed to the item's final layout in the running
// layout (layoutFinal) instead of being kept in the entry, so that a measurement never evicts a final result.
LayoutMeasurement layoutMeasure(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests& requests, LayoutMeasureKind kind);

// Lays out the flex item `node` in `space` at its final `requests`, as layoutBlock does. If a measurement in the
// running layout already laid the item out at the same space and requests, takes that layout instead and keeps it in
// the item's layout entry.
LayoutResult layoutFinal(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space, const LayoutRequests& requests);

// Lays out the anonymous table cell that wraps the siblings from `first` through `last`
// (css-tables-3/#fixup-algorithm), in `space` at `requests`. The cell's style is the initial style of a table cell, and
// it inherits from the parent of its content. Its result is not cached.
LayoutResult layoutAnonymousBlock(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last,
    const LayoutSpace& space, const LayoutRequests& requests);

// Lays out the anonymous table that wraps the run of table parts from `first` through `last` in a block container's
// flow (css-tables-3/#fixup-algorithm), in `space`. Its result is not cached.
LayoutResult layoutAnonymousTable(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last, const LayoutSpace& space);

// Releases the layouts made by measurements that no final layout took, and the measurements remembered by a layout that
// keeps no records, and clears the marks of the boxes that the layout laid out across fragmentainers. Called when a
// layout ends.
void layoutEndLayout(LayoutState& state);

// Lays out the root in a viewport of `extent`, and returns a fragment of the initial containing block that holds it.
const LayoutFragment* layoutViewport(LayoutState& state, uint32_t root, PrLayoutVec extent);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
