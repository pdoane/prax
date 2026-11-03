// JavaScript binding of CSS: the CSS Object Model, the Typed OM, and CSSOM View.

#pragma once

#include "pr/css.h"
#include "pr/css_view.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Binds CSS to the realm: the CSSOM, the Typed OM, and CSSOM View, whose members it adds to the DOM's interfaces. The
// DOM must be bound first (prJsDomBind).
void prJsCssBind(JSContext* ctx);

// The classes of CSSOM objects that the bindings of other modules return.
extern JSClassID prJsCssStylePropertiesClass;
extern JSClassID prJsCssStyleSheetClass;
extern JSClassID prJsCssStyleSheetListClass;
extern JSClassID prJsCssFontFaceSetClass;
extern JSClassID prJsCssMediaQueryListClass;

// Get the attributes of ElementCSSInlineStyle (cssom-1/#the-elementcssinlinestyle-mixin). `style` is a [SameObject,
// PutForwards=cssText] CSSStyleProperties. `attributeStyleMap` is a [SameObject] StylePropertyMap
// (css-typed-om-1/#dom-elementcssinlinestyle-attributestylemap).
JSValue prJsCssInlineStyle(JSContext* ctx, JSValueConst elementObject);
JSValue prJsCssSetInlineStyle(JSContext* ctx, JSValueConst object, JSValueConst input);
JSValue prJsCssAttributeStyleMap(JSContext* ctx, JSValueConst elementObject);

// Returns the FontFaceSet of a Document (css-font-loading-3/#dom-fontfacesource-fonts). The document's object holds the
// set's object. From the first call on, the binding is notified of the document's font loads.
JSValue prJsCssFonts(JSContext* ctx, JSValueConst documentObject);

// The style and attributeStyleMap properties, for the property list of a class that includes ElementCSSInlineStyle.
#define PR_JS_CSS_INLINE_STYLE                                                                        \
    PR_JS_GET_SET("style", prJsCssInlineStyle, prJsCssSetInlineStyle),                                 \
        PR_JS_GET("attributeStyleMap", prJsCssAttributeStyleMap)

// Returns the result of a scroll method: a promise resolved once its scroll completes, which every scroll does as it is
// made, or rejected with the pending exception if `scrolled` is false.
JSValue prJsCssScrollPromise(JSContext* ctx, bool scrolled);

// Reads the ScrollToOptions that a scroll method's arguments give (cssom-view-1/#dom-element-scroll): a dictionary, or
// an x and a y. Returns false if a conversion threw.
bool prJsCssScrollToOptionsRead(JSContext* ctx, int argc, JSValueConst* argv, PrCssScrollToOptions* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
