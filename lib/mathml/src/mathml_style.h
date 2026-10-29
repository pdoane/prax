// MathML styles: the user agent's sheet.
//
// MathML Core - https://w3c.github.io/mathml-core/

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The rules of the user agent's style sheet for MathML content (mathml-core/#user-agent-stylesheet) that apply to
// content laid out as CSS boxes: a lone identifier is drawn in its Mathematical Italic form.
constexpr char kMathMlStyle[] =
    "@namespace url(http://www.w3.org/1998/Math/MathML);\n"
    "mi { text-transform: math-auto; }\n";

} // namespace pr_impl

PR_ASSUME_NONNULL_END
