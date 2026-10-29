// Workloads: dense drawings for measurement, each stressing one route through a renderer. The last one mixes them as a
// user interface does, interleaving work that batches separately.

#pragma once

#include "tools/viewer/drawing.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the workloads, in the order that the viewer turns through them.
const std::vector<Drawing>& workloadDrawings();

} // namespace viewer

PR_ASSUME_NONNULL_END
