// The examples of the CommonMark and GFM specifications as viewer collections (suites.h).

#pragma once

#include "tools/viewer/documents.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the examples of the Markdown suites that dev.sh has fetched: CommonMark's spec.txt, and GFM's spec.txt,
// extensions.txt and regression.txt. Returns an empty list for a suite whose corpus is not fetched.
std::vector<Collection> markdownCollections();

} // namespace viewer

PR_ASSUME_NONNULL_END
