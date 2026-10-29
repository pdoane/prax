// What HTML's elements give their boxes beyond their computed style
//
// html - https://html.spec.whatwg.org/multipage/rendering.html

#pragma once

#include "pr/dom.h"

#include "css/src/css_boxes.h"
#include "css/src/css_style.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the provider of what HTML elements give their boxes, which HTML gives each document's CSS.
const CssBoxProvider* htmlBoxes();

// Returns the page's supported color schemes (html/#meta-color-scheme): those named by the content of the first meta
// element in the document tree that is named color-scheme and whose content is a valid color-scheme value, or none.
// Normal, and a value that names neither light nor dark, support none.
CssColorSchemeFlags htmlPageSchemes(PrDomDocument* document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
