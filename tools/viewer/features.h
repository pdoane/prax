// Feature drawings: every group's drawings, and their release.

#pragma once

#include "tools/viewer/drawing.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns every feature drawing, in the order that the viewer turns through them.
const std::vector<Drawing>& allFeatures();

// Releases what the drawings created.
void releaseDrawings();

} // namespace viewer

PR_ASSUME_NONNULL_END
