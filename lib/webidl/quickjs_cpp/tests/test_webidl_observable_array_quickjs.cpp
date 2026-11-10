#include "pr/quickjs_binding.h"
#include "pr/webidl_observable_array_quickjs.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Hooks of an array of positive numbers, which logs each value that it sets or deletes on its owner, in order.
JSValue positive(JSContext* ctx, JSValueConst, JSValueConst value) {
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value) < 0)
        return JS_EXCEPTION;
    if (!(number > 0))
        return JS_ThrowRangeError(ctx, "not positive");
    return JS_NewFloat64(ctx, number);
}

// Appends [value, index] to the owner's log `name`.
bool record(JSContext* ctx, JSValueConst owner, const char* name, JSValueConst value, uint32_t index) {
    JSValue list = JS_GetPropertyStr(ctx, owner, name);
    JSValue entry = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, entry, 0, JS_DupValue(ctx, value));
    JS_SetPropertyUint32(ctx, entry, 1, JS_NewUint32(ctx, index));
    int64_t length = 0;
    JS_GetLength(ctx, list, &length);
    JS_SetPropertyUint32(ctx, list, uint32_t(length), entry);
    JS_FreeValue(ctx, list);
    return true;
}

bool logSet(JSContext* ctx, JSValueConst owner, JSValueConst value, uint32_t index) {
    return record(ctx, owner, "steps", value, index);
}

bool logRemoved(JSContext* ctx, JSValueConst owner, JSValueConst value, uint32_t index) {
    double number = 0;
    JS_ToFloat64(ctx, &number, value);
    JSValue negated = JS_NewFloat64(ctx, -number);
    bool ok = record(ctx, owner, "removed", value, index) && record(ctx, owner, "steps", negated, index);
    JS_FreeValue(ctx, negated);
    return ok;
}

constexpr JsObservableArrayHooks kHooks = {positive, logSet, logRemoved};

bool evaluates(JSContext* ctx, const char* source) {
    JSValue result = JS_Eval(ctx, source, strlen(source), "observable", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char* message = JS_ToCString(ctx, error);
        INFO((message != nullptr ? message : "exception"));
        CHECK(!JS_IsException(result));
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    bool value = !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an observable array is an array whose indexes pass through its owner's hooks") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    jsObservableArrayBind(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue owner = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, owner, "removed", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, owner, "steps", JS_NewArray(ctx));
    JSValue array = jsObservableArrayNew(ctx, owner, &kHooks);
    REQUIRE(!JS_IsException(array));
    JS_SetPropertyStr(ctx, global, "owner", owner);
    JS_SetPropertyStr(ctx, global, "array", JS_DupValue(ctx, array));

    // Script replacing the globals that the array was made with changes nothing.
    CHECK(evaluates(ctx, "globalThis.Proxy = null; globalThis.Reflect = null; true"));
    CHECK(evaluates(ctx, R"JS((() => {
        if (!Array.isArray(array) || array.length !== 0) return false;
        array.push(1, '2');
        array[2] = 3;
        if (array.length !== 3 || array[1] !== 2 || array.join() !== '1,2,3') return false;
        try { array[1] = -1; return false; } catch (e) { if (!(e instanceof RangeError)) return false; }
        if (array[1] !== 2) return false;
        array[5] = 6;
        if (array.length !== 3 || 5 in array) return false;
        if (Object.keys(array).join() !== '0,1,2' || Object.isExtensible(array) === false) return false;
        const descriptor = Object.getOwnPropertyDescriptor(array, 0);
        if (!descriptor.configurable || !descriptor.writable || descriptor.value !== 1) return false;
        return JSON.stringify(owner.removed) === '[]';
    })())JS"));
    CHECK(jsObservableArrayLength(ctx, array) == 3);
    JSValue item = jsObservableArrayItem(ctx, array, 2);
    double number = 0;
    CHECK(JS_ToFloat64(ctx, &number, item) == 0);
    CHECK(number == 3);
    JS_FreeValue(ctx, item);

    CHECK(evaluates(ctx, R"JS((() => {
        array[0] = 4;
        if (delete array[0]) return false;
        array.pop();
        array.length = 1;
        return array.length === 1 && array[0] === 4 && JSON.stringify(owner.removed) === '[[1,0],[3,2],[2,1]]';
    })())JS"));
    CHECK(evaluates(ctx, "try { array.length = 1.5; false } catch (e) { e instanceof RangeError }"));
    // A set at an index that holds a value runs the delete steps for that value first, which a delete logs as the value
    // negated. A value that does not convert runs neither.
    CHECK(evaluates(ctx, R"JS((() => {
        owner.steps.length = 0;
        array[0] = 5;
        try { array[0] = 0; } catch (e) {}
        return JSON.stringify(owner.steps) === '[[-4,0],[5,0]]';
    })())JS"));

    JSValue values = JS_Eval(ctx, "[7, 8]", 6, "values", JS_EVAL_TYPE_GLOBAL);
    CHECK(jsObservableArraySet(ctx, array, values));
    JS_FreeValue(ctx, values);
    CHECK(evaluates(ctx, "array.join() === '7,8' && JSON.stringify(owner.removed.at(-1)) === '[5,0]'"));

    JS_FreeValue(ctx, array);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}
