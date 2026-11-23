// WPT's reftests and its import of the SVG 1.1 suite as viewer collections (suites.h).

#pragma once

#include "tools/viewer/documents.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns WPT's reftests, HTML and SVG alike, selected as tools/suite/wpt/wpt.py selects those that it renders, and the
// SVG 1.1 suite that WPT imports, against the browser. Returns an empty list if the corpus is not fetched.
std::vector<Collection> wptCollections();

} // namespace viewer

PR_ASSUME_NONNULL_END
