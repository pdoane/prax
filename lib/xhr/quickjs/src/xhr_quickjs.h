// What the sources of XHR's JavaScript binding share.

#pragma once

#include "pr/html.h"
#include "pr/xhr.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the client of a window's requests and reads.
PrXhrClient jsXhrWindowClient(PrHtmlWindow* window);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
