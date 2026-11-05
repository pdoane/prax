// Feature drawings for paint: gradients, images and image brushes.

#pragma once

#include "examples/draw_features/drawing.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

// Returns the group's feature drawings, in the order that they are shown.
const Array<Drawing>& paintFeatures();

} // namespace drawings

PR_ASSUME_NONNULL_END
