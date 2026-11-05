// Icons: the images of the icons of the tabs' pages, by URL, which the chrome asks for each round.

#pragma once

#include "pr/base.h"
#include "pr/cache.h"
#include "pr/draw_image.h"
#include "pr/string.h"

#include "apps/browser/browser.h"

PR_ASSUME_NONNULL_BEGIN

namespace browser {

using namespace pr;

// Creates the cache of icons, whose owner is the window's thread. An icon is fetched as the user agent rather than as
// the page's document, so that an icon from another origin is not opaque to the browser.
PrCache* iconsCreate(Browser& b);

// Returns the image of the icon at `url`, or null while it loads, if it cannot be shown, or if `url` is empty. The
// image is valid until the next pump of the cache.
const PrDrawImage* PR_NULLABLE iconsGet(Browser& b, const String& url);

} // namespace browser

PR_ASSUME_NONNULL_END
