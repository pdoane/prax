// Feature Queries
//
// css-conditional-3 - https://drafts.csswg.org/css-conditional-3/
// css-conditional-4 - https://drafts.csswg.org/css-conditional-4/
// css-conditional-5 - https://drafts.csswg.org/css-conditional-5/

#pragma once

#include "pr/string.h"
#include "pr/string_view.h"

#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if the UA supports the font format that a <font-format> keyword names
// (css-conditional-5/#dfn-support-font-format). Every format is supported except embedded-opentype and svg.
bool cssFontFormatSupported(PrCssKeyword format);

// Returns true if the UA supports the font technology that a <font-tech> keyword names
// (css-conditional-5/#dfn-support-font-tech). Every technology is supported except features-graphite, color-SVG and
// incremental.
bool cssFontTechSupported(PrCssKeyword tech);

// Returns true if the UA supports what a match of <supports-condition> tests. The match's node is the first value of
// `condition` (css-conditional-3/#support-definition).
bool cssSupportsEvaluate(CssValueRun condition);

// Returns true if `text` is a supported <supports-condition>, or one wrapped in parentheses
// (css-conditional-3/#dom-css-supports-conditiontext).
bool cssSupportsText(PrStringView text);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
