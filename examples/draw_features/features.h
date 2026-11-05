// Feature drawings: every group's drawings, and their release.

#pragma once

#include "examples/draw_features/drawing.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

// Returns every feature drawing, in the order that they are shown.
const Array<Drawing>& allFeatures();

// Releases what the drawings created.
void releaseDrawings();

} // namespace drawings

PR_ASSUME_NONNULL_END
