// Presentation attributes: the attributes of SVG elements that map to CSS properties
//
// SVG2 - https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes

#pragma once

#include "pr/text_buffer.h"

#include "css/src/css_hints.h"
#include "css/src/css_property.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the provider of the hints of SVG elements, which SVG gives each document's CSS.
const CssHintProvider* svgHints();

// Returns the property that the attribute `name` of `element` is a presentation attribute of, or PrCssProperty_Count if
// it is none (SVG2/styling.html#PresentationAttributes).
PrCssProperty svgPresentationOf(const PrDomElement* element, PrStringView name);

// Appends the declaration that the presentation attribute of `property` makes with the value `text`, as a hint makes
// it, or nothing if the value is not one of the property.
void svgPresentationDeclare(PrTextBuffer* out, PrCssProperty property, PrStringView text);

// Appends the CSS transform list of an SVG transform attribute's value (css-transforms-1/#svg-syntax). Returns false,
// appending nothing that the caller keeps, if the text is not in SVG's syntax.
bool svgTransformCss(PrTextBuffer* out, PrStringView text);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
