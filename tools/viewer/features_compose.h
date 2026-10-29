// Feature drawings for clips, compositing, groups, masks and lists.

#pragma once

#include "tools/viewer/drawing.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the group's feature drawings, in the order that the viewer turns through them.
const std::vector<Drawing>& composeFeatures();

} // namespace viewer

PR_ASSUME_NONNULL_END
