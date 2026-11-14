// The placement of a popup against its anchor, which every backend computes the same way in its own coordinates.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/geom.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the rectangle of a popup of `extent` that `placement` puts on a side of `anchor`. `anchor`, `work` and the
// result share one coordinate space whose y grows downward, and `placement.anchor` is not read. The popup flips to the
// anchor's other side, slides along its side, and shrinks to stay within `work`, in that order, as far as
// `placement.adjust` allows. It flips if the other side fits along the axis that leaves the anchor, whatever the other
// axis. `rightToLeft` swaps the start and the end of the horizontal sides and alignments.
PrRect appPopupPlace(const PrAppPopupPlacement& placement, PrRect anchor, PrVec2 extent, PrRect work, bool rightToLeft);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
