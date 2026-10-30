// Observable arrays: the values of attributes of ObservableArray types (webidl/#idl-observable-array).
//
// webidl - https://webidl.spec.whatwg.org/

#pragma once

#include "pr/base.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// How an observable array attribute handles the changes to its backing list.
struct JsObservableArrayHooks {
    // Converts `value`, given for `index`, to the IDL type, and runs the steps of
    // webidl/#observable-array-attribute-set-an-indexed-value. Returns the value that the backing list then holds at
    // `index`, or JS_EXCEPTION.
    JSValue (*set)(JSContext* ctx, JSValueConst owner, JSValueConst value, uint32_t index);

    // Runs the steps of webidl/#observable-array-attribute-delete-an-indexed-value for `value`, which the backing list
    // holds at `index`. Returns false, with an exception pending, if the steps threw. The hook is null for an attribute
    // with no steps.
    bool (*PR_NULLABLE remove)(JSContext* ctx, JSValueConst owner, JSValueConst value, uint32_t index);
};

// Prepares the realm for observable arrays, before any script in it runs. The arrays are proxies made with the realm's
// own Proxy and Reflect, whatever script later does to those globals.
void jsObservableArrayBind(JSContext* ctx);

// Creates an empty observable array for an attribute of `owner`, which the array keeps alive
// (webidl/#creating-an-observable-array-exotic-object). The array is a proxy over an array whose indexed properties are
// the backing list. `hooks` see each change to the list.
JSValue jsObservableArrayNew(JSContext* ctx, JSValueConst owner, const JsObservableArrayHooks* hooks);

// Return the size of the backing list of the observable array `array`, and the value that the list holds at an `index`
// below that size.
uint32_t jsObservableArrayLength(JSContext* ctx, JSValueConst array);
JSValue jsObservableArrayItem(JSContext* ctx, JSValueConst array, uint32_t index);

// Runs the setter of an observable array attribute (webidl/#js-attributes). Replaces the backing list of `array` with
// the values of the sequence `values`, each passed through the hooks. Returns false, with an exception pending, if it
// fails.
bool jsObservableArraySet(JSContext* ctx, JSValueConst array, JSValueConst values);

} // namespace pr

PR_ASSUME_NONNULL_END
