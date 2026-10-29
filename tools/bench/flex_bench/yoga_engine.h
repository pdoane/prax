// Yoga's adapter of the flex benchmark (flex_engine.h). Yoga rounds its results to whole pixels after a layout by
// default, and lib/layout does not snap its geometry, so the adapter turns rounding off.

#pragma once

#include "tools/bench/flex_bench/flex_engine.h"

#include <memory>

PR_ASSUME_NONNULL_BEGIN

namespace flex_bench {

// Returns the adapter of Yoga.
std::unique_ptr<Engine> makeYoga();

} // namespace flex_bench

PR_ASSUME_NONNULL_END
