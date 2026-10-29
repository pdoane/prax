// lib/layout's adapter of the flex benchmark (flex_engine.h).

#pragma once

#include "tools/bench/flex_bench/flex_engine.h"

#include <memory>

PR_ASSUME_NONNULL_BEGIN

namespace flex_bench {

// Returns the adapter of lib/layout.
std::unique_ptr<Engine> makePrax();

} // namespace flex_bench

PR_ASSUME_NONNULL_END
