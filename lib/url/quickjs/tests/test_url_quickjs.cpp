#include "pr/quickjs_binding.h"
#include "pr/url_quickjs.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

void evaluate(JSContext* ctx, const char* source) {
    JSValue result = JS_Eval(ctx, source, PrStringView(source).size, "url projection", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char* text = JS_ToCString(ctx, error);
        INFO((text ? text : "exception"));
        CHECK(false);
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, error);
    }
    CHECK(!JS_IsException(result));
    JS_FreeValue(ctx, result);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("URL JS ownership, iteration and conversion survive collection") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsUrlBind(ctx);
    evaluate(ctx, R"JS(
        function check(value) { if (!value) throw new Error('check failed'); }
        var url = new URL('https://example.test/?a=1&a=2');
        var params = url.searchParams;
        check(params === url.searchParams);
        url.href = 'https://example.test/?b=3';
        check(params.get('b') === '3');
        params.append('c', '4');
        check(url.search === '?b=3&c=4');
        var iterator = params.entries();
        check(iterator[Symbol.iterator]() === iterator);
        check(Object.prototype.toString.call(iterator) === '[object URLSearchParams Iterator]');
        check(iterator.next().value.join() === 'b,3');
        var rejected = false;
        try { iterator.next.call({}); } catch (e) { rejected = e instanceof TypeError; }
        check(rejected);
        url = null;
        params = null;
        class DerivedURL extends URL {}
        check(new DerivedURL('https://example.test/') instanceof DerivedURL);
        class DerivedParams extends URLSearchParams {}
        check(new DerivedParams('a=1') instanceof DerivedParams);
        const converted = [];
        const input = { *[Symbol.iterator]() {
            converted.push('first');
            yield [{toString() {converted.push('name'); return 'a';}}, '1'];
            converted.push('second');
            yield ['b', '2'];
        }};
        check(String(new URLSearchParams(input)) === 'a=1&b=2');
        check(converted.join() === 'first,name,second');
        let closed = false;
        const bad = { *[Symbol.iterator]() {
            try { yield [Symbol(), 'value']; } finally { closed = true; }
        }};
        try { new URLSearchParams(bad); } catch (e) { check(e instanceof TypeError); }
        check(closed);
    )JS");
    JS_RunGC(runtime);
    evaluate(ctx, R"JS(
        check(iterator.next().value.join() === 'c,4');
        check(iterator.next().done);
        iterator = null;
        var held = new URL('https://example.test/?keep=alive').searchParams;
    )JS");
    JS_RunGC(runtime);
    evaluate(ctx, "check(held.get('keep') === 'alive'); held = null;");
    JS_RunGC(runtime);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}
