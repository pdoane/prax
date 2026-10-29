// The engine's own pages as a viewer collection (suites.h).

#pragma once

#include "tools/viewer/documents.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the pages of suites/prax: a directory for each chapter of CSS 2 and each module, whose pages turn the
// specification into figures, laid out for 600 by 800. Returns an empty list if there are none.
std::vector<Collection> praxCollections();

} // namespace viewer

PR_ASSUME_NONNULL_END
