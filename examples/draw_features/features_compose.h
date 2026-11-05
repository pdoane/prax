// Feature drawings for clips, compositing, groups, masks and lists.

#pragma once

#include "examples/draw_features/drawing.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

// Returns the group's feature drawings, in the order that they are shown.
const Array<Drawing>& composeFeatures();

} // namespace drawings

PR_ASSUME_NONNULL_END
