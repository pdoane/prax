#include "pr/quickjs_callback.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/quickjs_binding.h"

#include <stdio.h>

namespace pr {

JsCallback* PR_NULLABLE jsCallbackCreate(JSContext* ctx, JSValueConst value, JSValueConst owner) {
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "Expected a callback object");
        return nullptr;
    }
    JsCallback* callback = alloc<JsCallback>();
    *callback =
        JsCallback{ctx, JS_GetRuntime(ctx), JS_DupValue(ctx, value), 1, nullptr, nullptr, nullptr};
    jsCallbackLink(JS_GetRuntime(ctx), callback, owner);
    return callback;
}

void jsCallbackOwn(JsCallback* callback, JSValueConst owner) {
    if (callback->runtime == nullptr)
        return;
    jsCallbackUnlink(callback);
    jsCallbackLink(callback->runtime, callback, owner);
}

void jsCallbackRetain(void* PR_NULLABLE context) {
    if (context != nullptr)
        ++static_cast<JsCallback*>(context)->references;
}

void jsCallbackRelease(void* PR_NULLABLE context) {
    if (context == nullptr)
        return;
    JsCallback* callback = static_cast<JsCallback*>(context);
    if (--callback->references != 0)
        return;
    if (callback->runtime != nullptr) {
        jsCallbackUnlink(callback);
        JS_FreeValueRT(callback->runtime, callback->value);
    }
    prMemoryFree(callback);
}

JSValue jsCallbackCall(JsCallback* callback, const char* PR_NULLABLE operation,
                       JSValueConst receiver, int argc, JSValueConst* PR_NULLABLE argv) {
    if (callback->runtime == nullptr)
        return JS_UNDEFINED;
    JSContext* ctx = callback->ctx;
    jsEnterRealm(ctx);
    JSValue result = JS_UNDEFINED;
    if (operation == nullptr || JS_IsFunction(ctx, callback->value)) {
        result = JS_Call(ctx, callback->value, receiver, argc, argv);
    } else {
        JSValue method = JS_GetPropertyStr(ctx, callback->value, operation);
        result =
            JS_IsException(method) ? method : JS_Call(ctx, method, callback->value, argc, argv);
        JS_FreeValue(ctx, method);
    }
    jsLeaveRealm(ctx);
    return result;
}

namespace {

constexpr char kReporterKey = 0;

} // namespace

void jsSetExceptionReporter(JSRuntime* runtime, const JsExceptionReporter* reporter) {
    jsSetRuntimeData(runtime, &kReporterKey, reporter);
}

void jsReportException(JSContext* ctx) {
    JSValue error = JS_GetException(ctx);
    const JsExceptionReporter* reporter =
        static_cast<const JsExceptionReporter*>(jsRuntimeData(JS_GetRuntime(ctx), &kReporterKey));
    // A report is a separate call, which the host makes outside any script's call.
    if (reporter != nullptr) {
        jsCallEnter(ctx);
        reporter->report(ctx, error);
        jsCallLeave(ctx);
    } else {
        jsPrintException(ctx, error);
    }
    JS_FreeValue(ctx, error);
    // Reporting can itself throw, and that exception must not affect the scripts that follow.
    JS_FreeValue(ctx, JS_GetException(ctx));
}

void jsPrintException(JSContext* ctx, JSValueConst error) {
    const char* text = JS_ToCString(ctx, error);
    fprintf(stderr, "%s\n", text != nullptr ? text : "uncaught exception");
    JS_FreeCString(ctx, text);
    JSValue stack = JS_IsObject(error) ? JS_GetPropertyStr(ctx, error, "stack") : JS_UNDEFINED;
    if (JS_IsString(stack)) {
        const char* trace = JS_ToCString(ctx, stack);
        if (trace != nullptr)
            fprintf(stderr, "%s\n", trace);
        JS_FreeCString(ctx, trace);
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, JS_GetException(ctx));
}

} // namespace pr
