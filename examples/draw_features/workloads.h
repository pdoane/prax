// Workloads: dense drawings for measurement, each stressing one route through a renderer. The last one mixes them as a
// user interface does, interleaving work that batches separately.

#pragma once

#include "examples/draw_features/drawing.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

// Returns the workloads, in the order that they are shown.
const Array<Drawing>& workloadDrawings();

} // namespace drawings

PR_ASSUME_NONNULL_END
