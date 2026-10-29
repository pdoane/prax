// Transform Functions
//
// css-transforms-2 - https://drafts.csswg.org/css-transforms-2/

#pragma once

#include "pr/dom.h"
#include "pr/dom_geometry.h"

#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Writes the matrix of the <transform-function> `function`, which is a PrCssValueKind_Function with its arguments
// (css-transforms-2/#mathematical-description). Returns false if an argument is not absolute: a percentage, or a length
// that is not a fixed number of pixels.
bool cssTransformMatrix(CssValueRun function, PrDomMatrix* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
