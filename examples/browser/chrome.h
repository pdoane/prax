// The chrome: the browser's own interface around the page, in Studio Kit.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

#include "examples/browser/browser.h"

PR_ASSUME_NONNULL_BEGIN

namespace browser {

using namespace pr;

// Declares a round of the chrome: the tab strip, the toolbar with the address bar, the favorites bar, and the active
// tab's page or its start page.
void chromeBuild(Browser& b);

} // namespace browser

PR_ASSUME_NONNULL_END
