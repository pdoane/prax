// Taffy's adapter of the flex benchmark (flex_engine.h), over the C interface of its Rust library (taffy/src/lib.rs).
// Taffy rounds its results to whole pixels after a layout by default, and lib/layout does not snap its geometry, so the
// adapter turns rounding off.

#pragma once

#include "tools/bench/flex_bench/flex_engine.h"

#include <memory>

PR_ASSUME_NONNULL_BEGIN

namespace flex_bench {

// Returns the adapter of Taffy.
std::unique_ptr<Engine> makeTaffy();

} // namespace flex_bench

PR_ASSUME_NONNULL_END
