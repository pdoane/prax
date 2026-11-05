// The user's profile: the files that keep the user's favorites and history between runs.

#pragma once

#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace browser {

using namespace pr;

// Reads the whole file at `path` into `out`. Returns false if the file cannot be read, and leaves `out` unchanged in
// that case.
bool profileRead(PrStringView path, String* out);

// Writes `text` as the whole file at `path`, creating its directory if it is missing. Returns false if the file cannot
// be written.
bool profileWrite(PrStringView path, PrStringView text);

} // namespace browser

PR_ASSUME_NONNULL_END
