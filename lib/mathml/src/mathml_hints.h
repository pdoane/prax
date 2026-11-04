// MathML's presentational hints: the attributes of MathML elements that map to CSS properties
//
// mathml-core - https://w3c.github.io/mathml-core/
// mathml4 - https://w3c.github.io/mathml/
//
// MathML Core makes some attributes presentational hints (mathml-core/#global-attributes), such as dir, mathcolor and
// mathsize on every MathML element and the sizes of mspace and mpadded. MathML 4's table attributes map to the cells'
// alignment, padding and borders and to the table's frame, so that CSS's table layout lays out an mtable as MathML 4
// describes it (mathml4/#presm_mtable).

#pragma once

#include "css/src/css_hints.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns MathML's provider of presentational hints, which prMathMlEnable gives each document's CSS.
const CssHintProvider* mathMlHints();

} // namespace pr_impl

PR_ASSUME_NONNULL_END
