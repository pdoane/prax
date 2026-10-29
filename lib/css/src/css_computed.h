// The computed style of an element as declarations, which the presentation of its document fills
//
// cssom-1 - https://drafts.csswg.org/cssom-1/#dom-window-getcomputedstyle

#pragma once

#include "pr/css.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/string_view.h"

#include "css/src/css_object.h"
#include "css/src/css_pseudo.h"
#include "css/src/css_rule.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the computed style of the element, or of its pseudo-element that `pseudoElement` names, as read-only
// declarations (cssom-1/#dom-window-getcomputedstyle), creating them on first use. Each read fills them from the
// presentation of the element's document, and leaves them empty if it has no presentation. `pseudoElement` names a
// pseudo-element if it starts with a colon, and the declarations are always empty if it is then not a pseudo-element
// selector, or is ::slotted() or ::part(). A pseudo-element whose boxes the engine does not generate has the element's
// style.
PrCssStyleDeclaration* cssElementComputedStyle(PrDomElement* element, PrStringView pseudoElement);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
