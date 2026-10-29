// resvg's tests as a viewer collection (suites.h).

#pragma once

#include "tools/viewer/documents.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns resvg's tests against their PNGs. Returns an empty list if the corpus is not fetched.
std::vector<Collection> svgCollections();

} // namespace viewer

PR_ASSUME_NONNULL_END
