// JavaScript binding of MathML.
//
// mathml-core - https://w3c.github.io/mathml-core/

#include "pr/mathml_quickjs.h"

#include "pr/css_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/mathml.h"
#include "pr/quickjs_binding.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

namespace {

JSClassID classifyElement(void* handle) {
    PrDomEventTarget* target = prDomElementAsEventTarget(static_cast<PrDomElement*>(handle));
    return prMathMlElementQuery(target) != nullptr ? prJsMathMlElementClass : prJsDomElementClass;
}

constexpr JSCFunctionListEntry kElementProto[] = {PR_JS_TAG("MathMLElement"), PR_JS_CSS_INLINE_STYLE};

constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("MathMLElement", prJsMathMlElementClass, jsViewFinalizer<&prJsMathMlElementClass>, kElementProto),
        .base = &prJsDomElementClass,
    },
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// The classes that other modules' bindings refer to.
JSClassID prJsMathMlElementClass;

void prJsMathMlBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    jsAddClassifier(JS_GetRuntime(ctx), prJsDomElementClass, classifyElement);
}
