// The inline style of an element: the declarations of its style attribute
//
// cssom-1 - https://drafts.csswg.org/cssom-1/#the-elementcssinlinestyle-mixin

#pragma once

#include "pr/css.h"
#include "pr/dom.h"
#include "pr/string_view.h"

#include "css/src/css_object.h"
#include "css/src/css_rule.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An element's inline style: the declarations of its style attribute, and the declaration block through which script
// sees them.
struct CssInlineStyle {
    CssDeclarationBlock block;
    PrCssStyleDeclaration declaration;
};

// Returns the element's inline style (cssom-1/#the-elementcssinlinestyle-mixin), which holds the declarations of its
// style attribute, parsed on first use. A change made through CSSOM writes the attribute.
PrCssStyleDeclaration* cssElementInlineStyle(PrDomElement* element);

// Sets the inline style's declarations to the style attribute's new value (cssom-1/#the-elementcssinlinestyle-mixin).
// Does nothing if the element has no inline style yet, or if the inline style set the attribute.
void cssElementStyleAttributeChanged(PrDomElement* element, PrStringView value);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
