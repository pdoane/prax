// Page-margin boxes: the boxes that a page lays out in its margins once its content is laid out.
//
// css-page-3 - https://drafts.csswg.org/css-page-3/
//
// A page sizes and places each of its margin boxes by css-page-3/#margin-dimension. A corner box fills the corner where
// two margins meet. The three boxes along a side share the side's length between the corners: the middle box is
// centered, and the boxes take room in proportion to their content's min-content and max-content sizes. Across its
// margin, each box fills the margin less its own margins. Each box is then laid out at that size as a block container
// that establishes a block formatting context, through its cache like any box.

#pragma once

#include "pr/layout_page.h"
#include "pr/raw_array.h"

#include "layout/src/layout_fragment.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutState;

// Lays out the page-margin boxes `boxes` of a page whose box is `box`, by PrLayoutMarginBox, each kPrLayoutNodeNone for
// a box that the page does not generate. Appends their fragments to `out`, in painting order, at their offsets from the
// top left of the page box.
void layoutPageMargins(LayoutState& state, const PrLayoutPageBox& box, const LayoutNodeIndex* boxes,
    RawArray<LayoutChild>& out);

// Returns `box` with an extent of at least 1 px and margins that leave a page area of at least zero, as layout uses it.
PrLayoutPageBox layoutPageBoxFit(PrLayoutPageBox box);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
