#include "pr/css_quickjs.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/quickjs_binding.h"
#include "pr/webidl_quickjs.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// Returns the boolean value of evaluating `source`.
bool evaluates(JSContext* ctx, const char* source) {
    JSValue result = JS_Eval(ctx, source, strlen(source), "exception", JS_EVAL_TYPE_GLOBAL);
    bool value = !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a failing member throws the DOMException its status stands for") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);

    // webidl/#js-DOMException-specialness: an Error, whose constructor is DOMException.
    CHECK(evaluates(ctx, R"JS((() => {
        try {
            new CSSStyleSheet().insertRule("a {}", 5);
        } catch (e) {
            return e instanceof DOMException && e instanceof Error && e.constructor === DOMException
                && e.name === "IndexSizeError" && e.code === DOMException.INDEX_SIZE_ERR && e.message === "";
        }
        return false;
    })())JS"));
    CHECK(evaluates(ctx, R"JS((() => {
        const e = new DOMException();
        const named = new DOMException("gone", "NotFoundError");
        return e.name === "Error" && e.code === 0 && named.message === "gone" && named.code === 8
            && named.NOT_FOUND_ERR === 8;
    })())JS"));

    // An argument that does not convert throws a TypeError.
    CHECK(evaluates(ctx, R"JS((() => {
        try {
            CSS.supports();
        } catch (e) {
            return e instanceof TypeError;
        }
        return false;
    })())JS"));
    CHECK(evaluates(
        ctx,
        R"JS(CSS.supports("color", "red") && CSS.supports("color: red") && !CSS.supports("color", "blah"))JS"));

    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("projected CSS rules retain live child handles after removal") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);

    CHECK(evaluates(ctx, R"JS((() => {
        const sheet = new CSSStyleSheet();
        sheet.replaceSync('@custom-media --yes true; @custom-media --no false; @custom-media --m print;');
        const rules = sheet.cssRules;
        if (!(rules[0] instanceof CSSCustomMediaRule) || rules[0].query !== true || rules[1].query !== false)
            return false;
        const rule = rules[2];
        const media = rule.query;
        if (!(media instanceof MediaList) || media !== rule.query)
            return false;
        sheet.deleteRule(2);
        media.mediaText = 'screen';
        globalThis.retainedMedia = media;
        return rule.parentStyleSheet === null && rule.cssText === '@custom-media --m screen;';
    })())JS"));
    JS_RunGC(runtime);
    CHECK(evaluates(ctx, "retainedMedia.mediaText === 'screen'"));
    CHECK(evaluates(ctx, R"JS((() => {
        const sheet = new CSSStyleSheet();
        sheet.replaceSync('@mixin --m { @contents; @contents { color: blue; } } a { @apply --m(1px); @apply --m { color: red; } }');
        const mixin = sheet.cssRules[0];
        const style = sheet.cssRules[1];
        return mixin instanceof CSSMixinRule && mixin instanceof CSSGroupingRule && mixin.contents
            && mixin.cssRules[0] instanceof CSSContentsStatementRule
            && mixin.cssRules[1] instanceof CSSContentsBlockRule
            && style.cssRules[0] instanceof CSSApplyStatementRule
            && style.cssRules[0].getArguments()[0] === '1px'
            && style.cssRules[1] instanceof CSSApplyBlockRule
            && style.cssRules[1].cssRules[0] instanceof CSSNestedDeclarations;
    })())JS"));
    CHECK(evaluates(ctx, R"JS((() => {
        const sheet = new CSSStyleSheet();
        sheet.replaceSync('@color-profile --p { src: url(profile.icc); } @supports-condition --c { color: red; }');
        const condition = sheet.cssRules[1];
        return typeof CSSColorProfileRule === 'undefined'
            && condition instanceof CSSSupportsConditionRule && condition.name === '--c'
            && condition.cssRules[0] instanceof CSSNestedDeclarations;
    })())JS"));

    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}
