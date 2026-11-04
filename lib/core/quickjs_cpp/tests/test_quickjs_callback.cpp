#include "pr/quickjs_binding.h"
#include "pr/quickjs_callback.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("a callback holds its function on its own and leaves the context's opaque state alone") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    int hostState = 17;
    JS_SetContextOpaque(ctx, &hostState);

    const char source[] = "(function() { return 42; })";
    JSValue function = JS_Eval(ctx, source, sizeof(source) - 1, "callback", JS_EVAL_TYPE_GLOBAL);
    REQUIRE(!JS_IsException(function));
    JsCallback* a = jsCallbackCreate(ctx, function, JS_UNDEFINED);
    JsCallback* b = jsCallbackCreate(ctx, function, JS_UNDEFINED);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a != b);
    CHECK(JS_IsStrictEqual(ctx, a->value, b->value));
    JS_FreeValue(ctx, function);
    jsCallbackRelease(a);
    jsCallbackRetain(b);
    jsCallbackRelease(b);
    JSValue result = jsCallbackCall(b, "handleEvent", JS_UNDEFINED, 0, nullptr);
    CHECK(!JS_IsException(result));
    int32_t number = 0;
    CHECK(JS_ToInt32(ctx, &number, result) == 0);
    CHECK(number == 42);
    CHECK(JS_GetContextOpaque(ctx) == &hostState);
    JS_FreeValue(ctx, result);
    jsCallbackRelease(b);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}
