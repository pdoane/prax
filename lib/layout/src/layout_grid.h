// Grid layout: a grid container's items, placed in its grid and sized with its tracks (css-grid-1/#layout-algorithm).
//
// css-align-3 - https://drafts.csswg.org/css-align-3/
// css-grid-1 - https://drafts.csswg.org/css-grid-1/
//
// A grid container is a block-level or inline-level box. Block layout lays it out as a box that establishes an
// independent formatting context, and this file lays out its content (layout_content.h). Its items are placed in its
// grid, from their grid-placement properties and by auto-placement. Its columns are sized, then its rows at the
// columns' sizes, by the track sizing algorithm, which asks each item for its contributions. The tracks are aligned in
// the content box, and each item is laid out in its grid area and aligned there.
//
// An item is measured and laid out through its own cache, as a flex item is (layoutMeasure, layoutFinal). A measurement
// of a grid container solves it alone, from the measurements of its items, and lays out none of them finally.

#pragma once

#include "layout/src/layout_content.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutGridScratch;

// Creates empty scratch memory, and frees it.
LayoutGridScratch* layoutGridScratchCreate(void);
void layoutGridScratchDestroy(LayoutGridScratch* PR_NULLABLE scratch);

// The lines of a grid container's grid as its layout placed them, which the boxes out of flow whose containing block is
// the container are placed against (css-grid-1/#abspos-items). The lines are in the grid scratch memory, valid until
// layoutGridFinish.
struct LayoutGridLines {
    LayoutNodeIndex node;           // the grid container
    uint32_t mark;                  // the index of its lines in the grid scratch memory
};

// Solves the grid container `node` over its items in `input`, for the input's goal. If the goal is to build, also lays
// each item out at its final request in its grid area, and appends its fragment to the scratch children at its logical
// offset in the border box, in order-modified document order, which is the order the items paint in. A child that is
// out of flow has its static position where the container's sole item would be, in a grid area that is the content box.
// If the goal is to build, sets `lines` to where the container's lines are, until layoutGridFinish.
LayoutContentOutcome layoutGridContent(LayoutState& state, LayoutNodeIndex node, const LayoutContentInput& input,
                                       LayoutGridLines* PR_NULLABLE lines);

// Returns the containing block of the box out of flow `box`, whose containing block is the grid container that laid out
// `lines` (css-grid-1/#abspos-items). It is the grid area that the box's placement names, from the top left of the
// container's border box, whose extent is `extent`. An auto line, or a line that the grid does not have, is the padding
// edge of the container on that side, which `padding` gives, a rectangle from the same corner.
PrLayoutRect layoutGridAbsoluteArea(LayoutState& state, const LayoutGridLines& lines, LayoutNodeIndex box,
                                    PrLayoutRect padding, PrLayoutVec extent);

// Makes the grid container's fragment from `built`, which the function takes, with the used sizes of the tracks of the
// layout that kept `lines` (layoutFragmentGrid), and returns it. Frees the scratch memory that holds the lines.
LayoutFragment* layoutGridFinish(LayoutState& state, const LayoutGridLines& lines, LayoutFragment* built);

// Returns the intrinsic inline sizes of the content box of the grid container `node`, whose items are measured in
// `input` (css-grid-1/#intrinsic-sizes): the sums of its columns and gaps when its columns are sized under a
// min-content constraint and under a max-content constraint.
LayoutIntrinsicSizes layoutGridIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
