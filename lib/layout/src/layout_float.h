// Floats: laying out a float, placing it among the floats of its block formatting context, and placing a box that
// avoids them.
//
// CSS2 - https://drafts.csswg.org/css2/
//
// A float is laid out through its own cache entry, independently of where it goes. It is placed by the rules of CSS2 at
// the point where layout reaches it: by block layout among its block-level siblings, and by inline layout on the line
// that holds it. A box that establishes a context, or a block-level replaced box, is placed beside the floats over its
// block size, and its margins may overlap them.

#pragma once

#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Floats

// Lays out the float `node` as a formatting context root in `containing`, the content box of its containing block. Its
// width shrinks to fit within that whole content box, whatever floats are beside it (CSS2/#float-width). Its result is
// cached in the same way as the result of a block-level box, and does not depend on where the float is placed.
LayoutAtomic layoutFloat(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& containing);

// Places a float that is styled by `box` and laid out as `f` in `mode` among the floats of `cb`. The float goes at or
// below `top`, and below the floats it clears (CSS2/#float-position). Returns its margin box.
LayoutExclusion layoutFloatPlace(const PrLayoutBoxStyle& box, const LayoutAtomic& f, const LayoutContainingBlock& cb,
    LayoutMode mode, PrLayoutUnit top);

// Returns the offset of the border box of a float, laid out as `f` and placed at `placed`, in the content box of its
// containing block `cb`. The block-start edge of that content box is at `contentTop` in the context. The offset
// includes the relative positioning of the float.
LayoutLogicalOffset layoutFloatOffset(const LayoutAtomic& f, const LayoutExclusion& placed,
    const LayoutContainingBlock& cb, LayoutMode mode, PrLayoutUnit contentTop);

// ---------------------------------------------------------------------------------------------------------------------
// Boxes that avoid floats

// A box that avoids floats, laid out at a position where it fits beside them.
struct LayoutAvoider {
    LayoutResult result;
    PrLayoutUnit top;               // block-start edge of the border box in the context
    PrLayoutUnit left;              // line-left edge of the border box in the context
};

// Lays out the block-level box `node`, which avoids floats, in `content`, the content box of its containing block, or
// the anonymous table that wraps the run of table parts from `node` through `runLast` if that is not kPrLayoutNodeNone.
// The box goes at the first offset at or below `top` at which its border box fits beside the floats of `cb` over its
// block size, or at which no float meets it (CSS2/#floats). Its inline size fills the space beside the floats at that
// offset. `margin` holds the margins of the box, indexed by LayoutSide.
LayoutAvoider layoutAvoiderPlace(LayoutState& state, LayoutNodeIndex node, LayoutNodeIndex runLast,
    const LayoutSpace& content, const LayoutContainingBlock& cb, PrLayoutUnit top, const PrLayoutUnit* margin);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
