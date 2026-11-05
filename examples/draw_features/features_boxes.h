// Feature drawings for boxes: borders, shadows and decorations.

#pragma once

#include "examples/draw_features/drawing.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

// Returns the group's feature drawings, in the order that they are shown.
const Array<Drawing>& boxFeatures();

} // namespace drawings

PR_ASSUME_NONNULL_END
