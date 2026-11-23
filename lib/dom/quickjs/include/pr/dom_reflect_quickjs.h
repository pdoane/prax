// IDL attributes that reflect an element's content attributes (html/#reflecting-content-attributes-in-idl-attributes),
// for the elements of every namespace: strings, booleans, enumerated attributes limited to only known values, and token
// lists.
//
// html - https://html.spec.whatwg.org/multipage/

#pragma once

#include "pr/ascii.h"
#include "pr/dom.h"
#include "pr/dom_quickjs.h"
#include "pr/quickjs_binding.h"
#include "pr/string_view.h"
#include "pr/webidl_quickjs.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A DOMString attribute, which is the content attribute, or the empty string without one. Its setter sets the content
// attribute.
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

// The keywords of an enumerated attribute limited to only known values: the canonical keyword of each state, and the
// keywords that a missing value and an invalid value reflect as. Each default is the empty string if the attribute has
// none.
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
        if (prAsciiEqualsFold(value, Keywords->keywords[i]))
            result = Keywords->keywords[i];
    }
    return jsFixedString(ctx, result);
}

// A nullable DOMString attribute limited to only known values: null without the content attribute, and otherwise as
// jsReflectEnumerated reflects it. Its setter removes the content attribute for null, and otherwise sets it.
template <JsLiteral Name, const JsReflectedKeywords* PR_NONNULL Keywords>
JSValue jsReflectNullableEnumerated(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    if (prDomElementGetAttribute(element, Name.text).data == nullptr)
        return JS_NULL;
    return jsReflectEnumerated<Name, Keywords>(ctx, elementObject);
}
template <JsLiteral Name>
JSValue jsSetReflectNullableString(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    if (!JS_IsNull(input))
        return jsSetReflectString<Name>(ctx, elementObject, input);
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    prDomElementRemoveAttribute(element, Name.text);
    return JS_UNDEFINED;
}

// The keywords of a CORS settings attribute (html/#cors-settings-attribute), whose invalid value is Anonymous.
inline constexpr PrStringView kJsCorsSettingsKeywords[] = {"anonymous", "use-credentials"};
inline constexpr JsReflectedKeywords kJsCorsSettings = {kJsCorsSettingsKeywords, 2, "", "anonymous"};

// The keywords of a referrer policy attribute (html/#referrer-policy-attribute), whose missing and invalid values are
// the empty string.
inline constexpr PrStringView kJsReferrerPolicyKeywords[] = {
    "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin",
    "strict-origin-when-cross-origin", "unsafe-url",
};
inline constexpr JsReflectedKeywords kJsReferrerPolicy = {kJsReferrerPolicyKeywords, 8, "", ""};

// The supported tokens of a hyperlink's rel attribute: the link types that it acts on (html/#linkTypes).
inline constexpr PrStringView kJsHyperlinkRelTokens[] = {"noopener", "noreferrer", "opener"};
inline constexpr PrDomTokenSet kJsHyperlinkRel = {kJsHyperlinkRelTokens, 3};

// A DOMTokenList of the content attribute, the same object at each read, with the attribute's supported tokens, or none
// if `Supported` is null. Its setter sets the list's value ([PutForwards=value]).
template <JsLiteral Property, JsLiteral Name, const PrDomTokenSet* PR_NULLABLE Supported = nullptr>
JSValue jsReflectTokenList(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, elementObject, Property.text, [&] {
        PrDomTokenList* list = prDomTokenListCreate(element, Name.text, Supported);
        JSValue object = jsWrapAs(ctx, prJsDomTokenListClass, elementObject, list, true);
        if (JS_IsException(object))
            prDomTokenListDestroy(list);
        return object;
    });
}

// The members of a prototype that reflect the content attribute `attribute` in the IDL attribute `name`.
#define PR_JS_DOM_REFLECT_STRING(name, attribute)                                                         \
    PR_JS_GET_SET(name, jsReflectString<attribute>, jsSetReflectString<attribute>)
#define PR_JS_DOM_REFLECT_BOOL(name, attribute)                                                           \
    PR_JS_GET_SET(name, jsReflectBool<attribute>, jsSetReflectBool<attribute>)
#define PR_JS_DOM_REFLECT_ENUMERATED(name, attribute, keywords)                                           \
    PR_JS_GET_SET(name, (jsReflectEnumerated<attribute, keywords>), jsSetReflectString<attribute>)
#define PR_JS_DOM_REFLECT_NULLABLE_ENUMERATED(name, attribute, keywords)                                  \
    PR_JS_GET_SET(name, (jsReflectNullableEnumerated<attribute, keywords>), jsSetReflectNullableString<attribute>)
#define PR_JS_DOM_REFLECT_CORS_SETTINGS(name, attribute)                                                  \
    PR_JS_DOM_REFLECT_NULLABLE_ENUMERATED(name, attribute, &kJsCorsSettings)
#define PR_JS_DOM_REFLECT_REFERRER_POLICY(name, attribute)                                                \
    PR_JS_DOM_REFLECT_ENUMERATED(name, attribute, &kJsReferrerPolicy)
#define PR_JS_DOM_REFLECT_TOKEN_LIST(name, attribute)                                                     \
    PR_JS_GET_SET(name, (jsReflectTokenList<name, attribute>), (jsPutForwards<name, "value">))
#define PR_JS_DOM_REFLECT_SUPPORTED_TOKEN_LIST(name, attribute, supported)                                \
    PR_JS_GET_SET(name, (jsReflectTokenList<name, attribute, supported>), (jsPutForwards<name, "value">))

} // namespace pr_impl

PR_ASSUME_NONNULL_END
