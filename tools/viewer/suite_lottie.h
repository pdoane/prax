// The Lottie corpora as viewer collections (suites.h).

#pragma once

#include "tools/viewer/documents.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the Lottie corpora, beside the other players playing each animation. Returns an empty list if the corpus is
// not fetched.
std::vector<Collection> lottieCollections();

} // namespace viewer

PR_ASSUME_NONNULL_END
