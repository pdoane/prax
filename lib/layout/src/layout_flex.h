// Flex layout: a flex container's items, sized and placed along its line (css-flexbox-2/#layout-algorithm).
//
// css-align-3 - https://drafts.csswg.org/css-align-3/
// css-flexbox-2 - https://drafts.csswg.org/css-flexbox-2/
//
// A flex container is a block-level or inline-level box. Block layout lays it out as a box that establishes an
// independent formatting context. Only the layout of its content differs. Its items are first solved, which finds their
// sizes, the cross size of the line, and their offsets. They are then built, each laid out at its final request. A
// measurement of a flex container solves it alone, from the measurements of its items, and does not lay out any of them
// finally.

#pragma once

#include "layout/src/layout_content.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutFlexItem;
struct LayoutFlexLine;

// Scratch memory that flex layout keeps between layouts: the items and lines of the containers being laid out,
// innermost last.
struct LayoutFlexScratch {
    RawArray<LayoutFlexItem> items;
    RawArray<LayoutFlexLine> lines;
};

// Creates empty scratch memory, and frees it.
LayoutFlexScratch* layoutFlexScratchCreate(void);
void layoutFlexScratchDestroy(LayoutFlexScratch* PR_NULLABLE scratch);

// Solves the flex container `node` over its items in `input`, for the input's goal. If the goal is to build, also lays
// each item out at its final request, and appends its fragment to the scratch children at its logical offset in the
// border box. A child that is out of flow has its static position where the container's sole item would be.
LayoutContentOutcome layoutFlexContent(LayoutState& state, LayoutNodeIndex node, const LayoutContentInput& input);

// Returns the intrinsic inline sizes of the content box of the flex container `node`, whose items are measured in
// `input` (css-flexbox-2/#intrinsic-sizes). Uses the Web-compatible algorithm, which works from the contributions of
// the items and, in a row, the gaps.
LayoutIntrinsicSizes layoutFlexIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
