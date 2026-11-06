// The vterm benchmark's adapter for lib/vterm (vterm_engine.h).

#pragma once

#include "tools/bench/vterm_bench/vterm_engine.h"

#include <memory>

PR_ASSUME_NONNULL_BEGIN

namespace vterm_bench {

// Returns the adapter for lib/vterm.
std::unique_ptr<Engine> makePrax();

} // namespace vterm_bench

PR_ASSUME_NONNULL_END
