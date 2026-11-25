// JavaScript binding of the reflecting IDL attributes that only HTML elements have: URLs and integers. Those of every
// element are pr/dom_reflect_quickjs.h's.
//
// html - https://html.spec.whatwg.org/multipage/common-dom-interfaces.html
//
// Each getter and setter reads and writes the content attribute `Name`, in no namespace, of the receiver, which is an
// HTML element.

#pragma once

#include "pr/dom_quickjs.h"
#include "pr/dom_reflect_quickjs.h"
#include "pr/html.h"
#include "pr/quickjs_binding.h"
#include "pr/string_view.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A USVString attribute that reflects a URL, which is the content attribute parsed against the document base URL. Its
// setter is a DOMString attribute's setter.
template <JsLiteral Name>
JSValue jsReflectUrl(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prHtmlElementReflectedUrl(element, Name.text));
}

// An integer attribute of `Kind` with the default `Fallback`. A Positive or NonNegative attribute's setter throws an
// "IndexSizeError" DOMException for a value outside its range.
template <JsLiteral Name, PrHtmlReflectedInteger Kind, int64_t Fallback>
JSValue jsReflectInteger(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, prHtmlElementReflectedInteger(element, Name.text, Kind, Fallback));
}
template <JsLiteral Name, PrHtmlReflectedInteger Kind, int64_t Fallback>
JSValue jsSetReflectInteger(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    int64_t value = 0;
    if (Kind == PrHtmlReflectedInteger_NonNegative || Kind == PrHtmlReflectedInteger_Long) {
        int32_t number = 0;
        if (JS_ToInt32(ctx, &number, input) < 0)
            return JS_EXCEPTION;
        value = number;
    } else {
        uint32_t number = 0;
        if (JS_ToUint32(ctx, &number, input) < 0)
            return JS_EXCEPTION;
        value = number;
    }
    PrStatus status = prHtmlElementSetReflectedInteger(element, Name.text, Kind, value, Fallback);
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "IndexSizeError");
}

// The members of a prototype that reflect the content attribute `attribute` in the IDL attribute `name`.
#define PR_REFLECT_URL(name, attribute) PR_JS_GET_SET(name, jsReflectUrl<attribute>, (jsSetReflectString<attribute, true>))
#define PR_REFLECT_INTEGER(name, attribute, kind, fallback)                                        \
    PR_JS_GET_SET(name, (jsReflectInteger<attribute, kind, fallback>),                             \
                  (jsSetReflectInteger<attribute, kind, fallback>))

} // namespace pr_impl

PR_ASSUME_NONNULL_END
