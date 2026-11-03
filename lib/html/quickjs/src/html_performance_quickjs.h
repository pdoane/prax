// JavaScript binding of the performance timeline (pr/html_performance.h).

#pragma once

#include "pr/html.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Defines the timeline's interfaces, and adds its members to the prototype of Performance.
void jsHtmlPerformanceBind(JSContext* ctx);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
