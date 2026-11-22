// The written part of the webp suite: files that libwebp's encoders write for each feature, which webp_compare writes
// (tools/suite/webp/webp_compare.cpp) and the suite decodes beside libwebp (tools/suite/webp.py).

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace webp {

// Writes the files under `directory`, a directory for each part of the features, each file named for what it was
// written with. Returns false if a file cannot be written.
bool writeAll(const char* directory);

} // namespace webp

PR_ASSUME_NONNULL_END
