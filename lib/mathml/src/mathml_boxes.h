// What MathML's elements give their boxes beyond their computed style
//
// mathml-core - https://w3c.github.io/mathml-core/
// mathml4 - https://w3c.github.io/mathml/
//
// An mtd spans columns and rows, and an mglyph is replaced by its image.
//
// The layout record of a MathML element's box holds a math group. The group holds the layout algorithm that the
// element's markup and in-flow children give it, and the values of its attributes resolved against its style
// (mathml-core/#layout-algorithms). The in-flow children are those that the presentation's computed styles put in flow.
// A change to an element's children or attributes can change the math groups of the elements around it, and the
// provider names those elements to the presentation (CssBoxProvider::reach).

#pragma once

#include "css/src/css_boxes.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns MathML's box provider, which prMathMlEnable gives each document's CSS.
const CssBoxProvider* mathMlBoxes();

} // namespace pr_impl

PR_ASSUME_NONNULL_END
