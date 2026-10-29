// The collections of the drawings made in code. The suites and corpora that dev.sh fetches have their collections in
// suite_*.h. A suite lists documents: which files it includes, the references that each is compared against, and what
// its runner says of them.

#pragma once

#include "tools/viewer/documents.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the features and the workloads, drawn in code (suites.cpp).
std::vector<Collection> drawingCollections();

} // namespace viewer

PR_ASSUME_NONNULL_END
