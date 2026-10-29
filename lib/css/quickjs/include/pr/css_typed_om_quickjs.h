// JavaScript binding of the CSS Typed OM, which the CSSOM's binding defines.
//
// css-typed-om-1 - https://drafts.css-houdini.org/css-typed-om-1/

#pragma once

#include "pr/css.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines the Typed OM's classes, and its unit factories on `css`, the CSS namespace object.
void prJsCssTypedOmBind(JSContext* ctx, JSValueConst css);

// The class of StylePropertyMap, whose objects are views of a declaration block (css-typed-om-1/#stylepropertymap),
// such as an element's attributeStyleMap and a style rule's styleMap.
extern JSClassID prJsCssStylePropertyMapClass;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
