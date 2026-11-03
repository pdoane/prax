// JavaScript binding of reflecting IDL attributes.
//
// html - https://html.spec.whatwg.org/multipage/common-dom-interfaces.html
//
// Each getter and setter reads and writes the content attribute `Name`, in no namespace, of the receiver, which is an
// element.

#pragma once

#include "pr/ascii.h"
#include "pr/dom_quickjs.h"
#include "pr/html.h"
#include "pr/quickjs_binding.h"
#include "pr/string_view.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A DOMString attribute, which is the content attribute's value, or the empty string if there is no attribute.
template <JsLiteral Name>
JSValue jsReflectString(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrStringView value = prDomElementGetAttribute(element, Name.text);
    return value.data != nullptr ? jsNewStringView(ctx, value) : jsFixedString(ctx, "");
}
template <JsLiteral Name>
JSValue jsSetReflectString(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument value = {ctx};
    if (element == nullptr || !value.read(input))
        return JS_EXCEPTION;
    PrStatus status = prDomElementSetAttribute(element, Name.text, value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// A USVString attribute that reflects a URL, which is the content attribute parsed against the document base URL. Its
// setter is a DOMString attribute's setter.
template <JsLiteral Name>
JSValue jsReflectUrl(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prHtmlElementReflectedUrl(element, Name.text));
}

// A boolean attribute, which is true if the content attribute is present.
template <JsLiteral Name>
JSValue jsReflectBool(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    return element != nullptr ? JS_NewBool(ctx, prDomElementHasAttribute(element, Name.text)) : JS_EXCEPTION;
}
template <JsLiteral Name>
JSValue jsSetReflectBool(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    if (!JS_ToBool(ctx, input)) {
        prDomElementRemoveAttribute(element, Name.text);
        return JS_UNDEFINED;
    }
    PrStatus status = prDomElementSetAttribute(element, Name.text, "");
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
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

// The keywords of an enumerated attribute that reflects limited to only known values: the canonical keyword of each
// state, and the keywords that a missing or an invalid value reflects as, the empty string where the attribute has no
// such default.
struct JsReflectedKeywords {
    const PrStringView* keywords;
    uint32_t count;
    PrStringView missing;
    PrStringView invalid;
};

// A DOMString attribute limited to only known values. The value is the keyword that the content attribute matches ASCII
// case-insensitively, or the default for a missing or an invalid value. Its setter is a DOMString attribute's setter.
template <JsLiteral Name, const JsReflectedKeywords* PR_NONNULL Keywords>
JSValue jsReflectEnumerated(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrStringView value = prDomElementGetAttribute(element, Name.text);
    PrStringView result = value.data == nullptr ? Keywords->missing : Keywords->invalid;
    for (uint32_t i = 0; value.data != nullptr && i < Keywords->count; ++i) {
        if (asciiEqualsFold(value, Keywords->keywords[i]))
            result = Keywords->keywords[i];
    }
    return jsFixedString(ctx, result);
}

// The members of a prototype that reflect the content attribute `attribute` in the IDL attribute `name`.
#define PR_REFLECT_STRING(name, attribute)                                                         \
    PR_JS_GET_SET(name, jsReflectString<attribute>, jsSetReflectString<attribute>)
#define PR_REFLECT_BOOL(name, attribute) PR_JS_GET_SET(name, jsReflectBool<attribute>, jsSetReflectBool<attribute>)
#define PR_REFLECT_URL(name, attribute) PR_JS_GET_SET(name, jsReflectUrl<attribute>, jsSetReflectString<attribute>)
#define PR_REFLECT_INTEGER(name, attribute, kind, fallback)                                        \
    PR_JS_GET_SET(name, (jsReflectInteger<attribute, kind, fallback>),                             \
                  (jsSetReflectInteger<attribute, kind, fallback>))
#define PR_REFLECT_ENUMERATED(name, attribute, keywords)                                           \
    PR_JS_GET_SET(name, (jsReflectEnumerated<attribute, keywords>), jsSetReflectString<attribute>)

} // namespace pr_impl

PR_ASSUME_NONNULL_END
