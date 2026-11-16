#include "pr/quickjs_binding.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Runs the runtime's pending jobs one at a time, collecting garbage before each, as a host's checkpoint can.
void runJobsWithCollections(JSRuntime* runtime) {
    for (;;) {
        JS_RunGC(runtime);
        JSContext* job = nullptr;
        int ran = JS_ExecutePendingJob(runtime, &job);
        REQUIRE(ran >= 0);
        if (ran == 0)
            return;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a closure keeps alive the suspended async function whose variables it captured") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = jsContextCreate(runtime);
    // The outer function waits on a promise that only the inner closure can resolve, through a variable of the outer
    // function's frame. Nothing else refers to that frame while both functions are suspended.
    const char source[] = "globalThis.rounds = 0; (async () => { for (let i = 0; i < 3; ++i) { let finish = null;"
        " const finished = new Promise((resolve) => { finish = resolve; });"
        " const handler = async () => { await Promise.resolve(); finish(); };"
        " Promise.resolve().then(() => handler()); await finished; ++globalThis.rounds; } })();";
    JSValue result = JS_Eval(ctx, source, sizeof(source) - 1, "gc", JS_EVAL_TYPE_GLOBAL);
    REQUIRE(!JS_IsException(result));
    JS_FreeValue(ctx, result);
    runJobsWithCollections(runtime);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue rounds = JS_GetPropertyStr(ctx, global, "rounds");
    int32_t count = 0;
    CHECK(JS_ToInt32(ctx, &count, rounds) == 0);
    CHECK(count == 3);
    JS_FreeValue(ctx, rounds);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}
