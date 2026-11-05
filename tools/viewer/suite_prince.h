// Prince's samples as a viewer collection (suites.h).

#pragma once

#include "tools/viewer/documents.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the samples of yeslogic/prince-samples that dev.sh fetches: documents written for print, each compared with
// the PDF that Prince made of it and with the engine's own PDF and SVG. Returns an empty list if the samples are not
// fetched.
std::vector<Collection> princeCollections();

} // namespace viewer

PR_ASSUME_NONNULL_END
