// Feature drawings for shapes and strokes.

#pragma once

#include "tools/viewer/drawing.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the group's feature drawings, in the order that the viewer turns through them.
const std::vector<Drawing>& shapeFeatures();

} // namespace viewer

PR_ASSUME_NONNULL_END
