// Studio Kit's icons, as the kit's other widgets draw them: in a color of their look rather than a tone.

#pragma once

#include "pr/kit_icon.h"

#include "studio/kit/src/kit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Makes an icon of the set under `parent`: a square of `size` px, or 16 px for zero, that draws `icon` in `color`.
PrLayoutNode kitIcon(PrKit& kit, PrLayoutNode parent, uint64_t key, PrKitIconKind icon, float size, PrColor color);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
