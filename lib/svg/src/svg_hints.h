// Presentation attributes: the attributes of SVG elements that map to CSS properties
//
// SVG2 - https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes

#pragma once

#include "css/src/css_hints.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the provider of the hints of SVG elements, which SVG gives each document's CSS.
const CssHintProvider* svgHints();

} // namespace pr_impl

PR_ASSUME_NONNULL_END
