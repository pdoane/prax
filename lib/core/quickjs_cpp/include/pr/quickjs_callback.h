// The JS values of callbacks that native code holds.

#pragma once

#include "pr/base.h"
#include "pr/quickjs_binding.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The data of an object of a bound class (js_binding.h).
struct JsWrapper;

// A JS value that a native registration holds. A registration made through an object belongs to that object. The
// object's mark traces the value, so the collector can free a cycle through the value and the object. The object's
// finalizer detaches the callback, because nothing can call it once the object is unreachable. A registration of
// pending work, such as a timer, belongs to the runtime. Its value is a root until the work is done, or until the
// runtime is destroyed. A detached callback does nothing when called.
struct JsCallback {
    JSContext* ctx;
    JSRuntime* PR_NULLABLE runtime;     // the runtime of the context, and null once detached
    JSValue value;                      // undefined once detached
    uint32_t references;
    JsWrapper* PR_NULLABLE owner;       // the object it belongs to; null for the runtime
    JsCallback* PR_NULLABLE previous;   // the neighbors in its owner's list or the runtime's
    JsCallback* PR_NULLABLE next;
};

// Creates a callback of `value` that belongs to `owner` if `owner` is an object of a bound class, and otherwise to the
// runtime. Throws a TypeError if `value` is not an object.
JsCallback* PR_NULLABLE jsCallbackCreate(JSContext* ctx, JSValueConst value, JSValueConst owner);

// Transfers a callback to `owner`. A registration whose object is created after its callback uses this.
void jsCallbackOwn(JsCallback* callback, JSValueConst owner);

// Adds or releases a reference to the callback `context`. The last release frees the callback.
void jsCallbackRetain(void* PR_NULLABLE context);
void jsCallbackRelease(void* PR_NULLABLE context);

// Calls the callback in its realm with `receiver` as `this`, and returns the result. If `operation` is not null and the
// value is not a function, calls the value's method `operation` instead, with the value as `this`. A detached callback
// returns undefined.
JSValue jsCallbackCall(JsCallback* callback, const char* PR_NULLABLE operation,
                       JSValueConst receiver, int argc, JSValueConst* PR_NULLABLE argv);

// How a runtime reports an exception that no script caught. The module that has a global object to report it at sets
// the runtime's reporter.
struct JsExceptionReporter {
    void (*PR_NONNULL report)(JSContext* ctx, JSValueConst error);
};

// Sets the runtime's reporter.
void jsSetExceptionReporter(JSRuntime* runtime, const JsExceptionReporter* reporter);

// Takes the pending exception and reports it through the runtime's reporter, or else writes it and its stack to stderr.
void jsReportException(JSContext* ctx);

// Writes an exception and its stack to stderr, in the form of a report that no listener handled.
void jsPrintException(JSContext* ctx, JSValueConst error);

// A callback argument. The call's scope holds a reference to the callback, and a registration retains its own. The
// callback belongs to `owner` (jsCallbackCreate).
template <typename Callback>
struct CallbackArgument {
    JSContext* ctx;
    JSValueConst owner;
    Callback value = {};
    JsCallback* PR_NULLABLE callback = nullptr;

    // Reads a callback function, which `invoke` calls. Throws a TypeError if `input` is not a function.
    template <typename Fn>
    bool readFunction(JSValueConst input, Fn invoke) {
        if (!JS_IsFunction(ctx, input)) {
            JS_ThrowTypeError(ctx, "Expected a callback function");
            return false;
        }
        if (!create(input))
            return false;
        value = Callback{};
        value.invoke = invoke;
        value.context = callback;
        value.retain = jsCallbackRetain;
        value.release = jsCallbackRelease;
        return true;
    }

    // Reads a callback, which `invoke` calls. If `nullable` is true, null and undefined read as no callback.
    template <typename Fn>
    bool read(JSValueConst input, Fn invoke, bool nullable) {
        if (nullable && (JS_IsNull(input) || JS_IsUndefined(input)))
            return true;
        if (!create(input))
            return false;

        // The retained object keeps this identity valid for the registration's lifetime.
        uint64_t identity = uint64_t(uintptr_t(JS_VALUE_GET_PTR(input)));
        value = Callback{invoke, callback, identity, jsCallbackRetain, jsCallbackRelease};
        return true;
    }

    // Creates the callback of `input`, which the call's scope holds.
    bool create(JSValueConst input) {
        callback = jsCallbackCreate(ctx, input, owner);
        if (callback != nullptr)
            jsHoldCustom(ctx, [](JSContext*, void* held) { jsCallbackRelease(held); }, callback);
        return callback != nullptr;
    }

    // Returns the callback, or null if none was read.
    const Callback* PR_NULLABLE get() const { return callback != nullptr ? &value : nullptr; }
};

} // namespace pr

PR_ASSUME_NONNULL_END
