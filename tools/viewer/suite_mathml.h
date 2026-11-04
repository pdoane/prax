// The visual tests of MathML as a viewer collection (suites.h).

#pragma once

#include "tools/viewer/documents.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the MathML collection: Mozilla's MathML torture test once for each math font of MathFonts, eyeasme.com's
// MathML browser test, and lib/tex's sample of real mathematics in Markdown. A test whose corpus is not fetched is left
// out, and so is Mozilla's without the third-party server (serveThirdPartyFrom).
std::vector<Collection> mathmlCollections();

} // namespace viewer

PR_ASSUME_NONNULL_END
