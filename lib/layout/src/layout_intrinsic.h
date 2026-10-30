// Intrinsic sizes: the inline sizes that the content of a box asks for, and the contribution of a box to the intrinsic
// sizes of its parent.
//
// css-sizing-3 - https://drafts.csswg.org/css-sizing-3/
//
// The intrinsic sizes of a box are measured from its content, with no inline base, and are cached separately from the
// result of its layout (layout_cache.h). The cached sizes are reused while nothing in the subtree has changed and, if
// they read the block-axis percentage base, that base is the same.

#pragma once

#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the intrinsic sizes of the element or replaced element `node`, or of the anonymous flex item whose run starts
// at `node`. These are its border-box inline size with an auto inline size and no inline limits, shrunk as far as its
// content allows and grown as far as its content asks (css-sizing-3/#intrinsic-sizes). An element with a preferred
// aspect ratio and a definite block size instead has the inline size that the ratio gives, floored by its automatic
// minimum (css-sizing-4/#aspect-ratio-automatic). The tree caches the sizes if `state` reuses results.
LayoutIntrinsicSizes layoutIntrinsicSizes(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input);

// Returns the intrinsic sizes of the anonymous table cell that wraps the siblings from `first` through `last`, measured
// in `input`. The cell has no edges, and its sizes are not cached.
LayoutIntrinsicSizes layoutAnonymousIntrinsic(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last,
    const LayoutIntrinsicInput& input);

// Returns the contribution of the element or replaced element `node` to the intrinsic sizes of its parent
// (css-sizing-3/#contributions). This is its margin-box inline size from its sizing properties, falling back to its
// intrinsic sizes. Cyclic percentages resolve as css-sizing-3/#cyclic-percentage-contribution says, except that at
// min-content the whole size of a replaced box compresses, not only the percentage. If `content` is given, it is set to
// the intrinsic sizes of the box, which are measured once for both.
LayoutIntrinsicSizes layoutContribution(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input,
    LayoutIntrinsicSizes* PR_NULLABLE content = nullptr);

// Returns the first node whose cached intrinsic sizes differ from a fresh measurement with `fresh`, which bypasses the
// cache, or kPrLayoutNodeNone if none differs. A fresh measurement also bypasses the shapings of inline formatting
// contexts, so the check of a box covers the contexts in it.
uint32_t layoutIntrinsicVerify(LayoutState& fresh);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
