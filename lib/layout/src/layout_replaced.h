// Replaced boxes: sized from their style and their content's natural dimensions, and laid out as one box.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-images-3 - https://drafts.csswg.org/css-images-3/
// css-sizing-3 - https://drafts.csswg.org/css-sizing-3/

#pragma once

#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Lays out the replaced box of `node` in `space` (CSS2/#inline-replaced-width, CSS2/#inline-replaced-height,
// CSS2/#min-max-widths). Its size comes from its style's sizes, from its natural dimensions if those sizes are auto,
// and, if it is a flex item, from what its container `requests`. Auto margins share the space that its inline size
// leaves, or are zero if it floats or its container decides its inline size (CSS2/#block-replaced-width,
// CSS2/#float-replaced-width). Its result reads its containing block's block size if a size or limit does, or if it is
// out of flow (LayoutResultFlag_ReadsBlockBase).
LayoutResult layoutReplacedBlock(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests* PR_NULLABLE requests);

// Returns the intrinsic sizes of the replaced box of `node`: its size at min-content and at max-content, with no inline
// base (css-sizing-3/#intrinsic-sizes).
LayoutIntrinsicSizes layoutReplacedIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input);

// Returns true if the replaced box of `node` has a ratio and no natural size, and no block size in `input` for the
// ratio to size its inline axis from, so that its contributions are zero. Sets `reads` if it read the input's
// percentage base.
bool layoutReplacedHasUnsizedRatio(const LayoutState& state, LayoutNodeIndex node, const LayoutLogicalBox& logical,
    const LayoutIntrinsicInput& input, bool& reads);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
