// Limits: drawings that reach past what a renderer holds at full resolution, which it draws at a lower one: a blur
// wider than a layer reaches, and an image of a draw list larger than the largest texture.

#pragma once

#include "examples/draw_features/drawing.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

// Returns the limits, in the order that they are shown.
const Array<Drawing>& limitDrawings();

} // namespace drawings

PR_ASSUME_NONNULL_END
