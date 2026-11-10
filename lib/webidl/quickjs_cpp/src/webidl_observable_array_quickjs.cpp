#include "pr/webidl_observable_array_quickjs.h"

#include "pr/alloc.h"
#include "pr/count_of.h"
#include "pr/quickjs_binding.h"
#include "pr/raw_array.h"
#include "pr/webidl_quickjs.h"

namespace pr {

namespace {

// The data of an observable array's proxy handler (webidl/#creating-an-observable-array-exotic-object).
struct Handler {
    const JsObservableArrayHooks* hooks;
    JSValue owner;
    RawArray<JSValue> list;   // the backing list
};

JSClassID handlerClass;

void handlerFinalize(JSRuntime* runtime, JSValue handlerObject) {
    Handler* handler = static_cast<Handler*>(JS_GetOpaque(handlerObject, handlerClass));
    if (handler == nullptr)
        return;
    JS_FreeValueRT(runtime, handler->owner);
    for (uint32_t i = 0; i < handler->list.count; ++i)
        JS_FreeValueRT(runtime, handler->list.data[i]);
    handler->list.reset();
    prMemoryFree(handler);
}

void handlerMark(JSRuntime* runtime, JSValueConst handlerObject, JS_MarkFunc* mark) {
    Handler* handler = static_cast<Handler*>(JS_GetOpaque(handlerObject, handlerClass));
    if (handler == nullptr)
        return;
    JS_MarkValue(runtime, handler->owner, mark);
    for (uint32_t i = 0; i < handler->list.count; ++i)
        JS_MarkValue(runtime, handler->list.data[i], mark);
}

// The traps of a handler. Each trap falls back to the Reflect function of the same name, which the trap holds as its
// function data.
enum {
    Trap_DefineProperty,
    Trap_DeleteProperty,
    Trap_Get,
    Trap_GetOwnPropertyDescriptor,
    Trap_Has,
    Trap_OwnKeys,
    Trap_PreventExtensions,
    Trap_Set,
};
typedef uint8_t Trap;

constexpr const char* kTrapNames[] = {
    "defineProperty", "deleteProperty",    "get", "getOwnPropertyDescriptor", "has",
    "ownKeys",        "preventExtensions", "set",
};

// The kind of property key of an observable array.
enum {
    Key_Other,
    Key_Length,
    Key_Index,
};
typedef uint8_t Key;

// Returns the kind of the key `value`, and for an index, writes the index to `*index`. Returns -1 for an exception.
int keyOf(JSContext* ctx, JSValueConst value, uint32_t* index) {
    JSAtom atom = JS_ValueToAtom(ctx, value);
    if (atom == JS_ATOM_NULL)
        return -1;
    int key = atom == JS_ATOM_length ? Key_Length : Key_Other;
    if (key == Key_Other) {
        int found = jsIndexOf(ctx, atom, index);
        key = found < 0 ? -1 : found != 0 ? Key_Index : Key_Other;
    }
    JS_FreeAtom(ctx, atom);
    return key;
}

// webidl/#observable-array-exotic-object-set-the-length
bool setLength(JSContext* ctx, Handler* handler, JSValueConst value, bool* result) {
    uint32_t length = 0;
    double number = 0;
    if (JS_ToUint32(ctx, &length, value) < 0 || JS_ToFloat64(ctx, &number, value) < 0)
        return false;
    if (double(length) != number) {
        JS_ThrowRangeError(ctx, "Invalid array length");
        return false;
    }
    if (length > handler->list.count) {
        *result = false;
        return true;
    }
    while (handler->list.count > length) {
        uint32_t index = handler->list.count - 1;
        JSValue item = handler->list.data[index];
        if (handler->hooks->remove != nullptr && !handler->hooks->remove(ctx, handler->owner, item, index))
            return false;
        // A hook may have run script that changed the list.
        if (handler->list.count == index + 1) {
            handler->list.pop();
            JS_FreeValue(ctx, item);
        }
    }
    *result = true;
    return true;
}

// webidl/#observable-array-exotic-object-set-the-indexed-value
bool setIndexed(JSContext* ctx, Handler* handler, uint32_t index, JSValueConst value, bool* result) {
    if (index > handler->list.count) {
        *result = false;
        return true;
    }
    JSValue item = handler->hooks->convert(ctx, handler->owner, value);
    if (JS_IsException(item))
        return false;
    const JsObservableArrayHooks* hooks = handler->hooks;
    bool ok = index == handler->list.count || hooks->remove == nullptr ||
        hooks->remove(ctx, handler->owner, handler->list.data[index], index);
    ok = ok && (hooks->set == nullptr || hooks->set(ctx, handler->owner, item, index));
    if (!ok) {
        JS_FreeValue(ctx, item);
        return false;
    }
    // A hook may have run script that changed the list.
    if (index < handler->list.count) {
        JS_FreeValue(ctx, handler->list.data[index]);
        handler->list.data[index] = item;
    } else {
        handler->list.push(item);
    }
    *result = true;
    return true;
}

// Reads the field `name` of a property descriptor object: whether it is present, and its value.
bool descriptorField(JSContext* ctx, JSValueConst descriptor, const char* name, bool* present, JSValue* value) {
    JSAtom atom = JS_NewAtom(ctx, name);
    int has = JS_HasProperty(ctx, descriptor, atom);
    JS_FreeAtom(ctx, atom);
    if (has < 0)
        return false;
    *present = has != 0;
    *value = *present ? JS_GetPropertyStr(ctx, descriptor, name) : JS_UNDEFINED;
    return !JS_IsException(*value);
}

// Returns 1 if the descriptor's field `name` is present with the boolean value `value`, 0 if it is not, and -1 for an
// exception.
int descriptorIs(JSContext* ctx, JSValueConst descriptor, const char* name, bool value) {
    bool present = false;
    JSValue field;
    if (!descriptorField(ctx, descriptor, name, &present, &field))
        return -1;
    int is = present && (JS_ToBool(ctx, field) != 0) == value;
    JS_FreeValue(ctx, field);
    return is;
}

// webidl/#js-observable-array-defineProperty, for the length and an index. The descriptor must describe a data property
// and keep the attributes that the property has: writable for both, and configurable and enumerable only for an index.
JSValue defineOwn(JSContext* ctx, Handler* handler, Key key, uint32_t index, JSValueConst descriptor) {
    bool accessor = false;
    constexpr const char* kAccessors[] = {"get", "set"};
    for (const char* name : kAccessors) {
        bool present = false;
        JSValue field;
        if (!descriptorField(ctx, descriptor, name, &present, &field))
            return JS_EXCEPTION;
        JS_FreeValue(ctx, field);
        accessor = accessor || present;
    }
    if (accessor)
        return JS_FALSE;
    int configurable = descriptorIs(ctx, descriptor, "configurable", key == Key_Length);
    int enumerable = descriptorIs(ctx, descriptor, "enumerable", key == Key_Length);
    int writable = descriptorIs(ctx, descriptor, "writable", false);
    if (configurable < 0 || enumerable < 0 || writable < 0)
        return JS_EXCEPTION;
    if (configurable != 0 || enumerable != 0 || writable != 0)
        return JS_FALSE;
    bool present = false;
    JSValue value;
    if (!descriptorField(ctx, descriptor, "value", &present, &value))
        return JS_EXCEPTION;
    bool result = true;
    bool ok = true;
    if (present && key == Key_Length)
        ok = setLength(ctx, handler, value, &result);
    else if (present)
        ok = setIndexed(ctx, handler, index, value, &result);
    JS_FreeValue(ctx, value);
    return ok ? JS_NewBool(ctx, result) : JS_EXCEPTION;
}

JSValue descriptorOf(JSContext* ctx, JSValue value, bool configurable) {
    JSValue descriptor = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, descriptor, "value", value, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, descriptor, "writable", JS_TRUE, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, descriptor, "enumerable", JS_NewBool(ctx, configurable), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, descriptor, "configurable", JS_NewBool(ctx, configurable), JS_PROP_C_W_E);
    return descriptor;
}

// webidl/#js-observable-array-ownKeys: the indexes of the backing list, then the target's own keys.
JSValue ownKeys(JSContext* ctx, Handler* handler, JSValueConst reflect, JSValueConst target) {
    JSValue own = JS_Call(ctx, reflect, JS_UNDEFINED, 1, &target);
    if (JS_IsException(own))
        return own;
    JSValue keys = JS_NewArray(ctx);
    uint32_t count = 0;
    for (; count < handler->list.count; ++count)
        JS_SetPropertyUint32(ctx, keys, count, JS_ToString(ctx, JS_NewUint32(ctx, count)));
    int64_t length = 0;
    JS_GetLength(ctx, own, &length);
    for (int64_t i = 0; i < length; ++i)
        JS_SetPropertyUint32(ctx, keys, count++, JS_GetPropertyUint32(ctx, own, uint32_t(i)));
    JS_FreeValue(ctx, own);
    return keys;
}

// A trap of webidl/#js-observable-array-abstract-operations, selected by the magic, with the handler as `this`.
JSValue trap(JSContext* ctx, JSValueConst handlerObject, int argc, JSValueConst* argv, int magic, JSValueConst* data) {
    Handler* handler = static_cast<Handler*>(JS_GetOpaque(handlerObject, handlerClass));
    if (handler == nullptr || argc < 1)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    JSValueConst reflect = data[0];
    if (magic == Trap_OwnKeys)
        return ownKeys(ctx, handler, reflect, argv[0]);
    if (magic == Trap_PreventExtensions)
        return JS_FALSE;
    uint32_t index = 0;
    int key = argc > 1 ? keyOf(ctx, argv[1], &index) : Key_Other;
    if (key < 0)
        return JS_EXCEPTION;
    if (key == Key_Other)
        return JS_Call(ctx, reflect, JS_UNDEFINED, argc, argv);

    uint32_t length = handler->list.count;
    switch (magic) {
        case Trap_DefineProperty:
            return defineOwn(ctx, handler, Key(key), index, argc > 2 ? argv[2] : JS_UNDEFINED);
        case Trap_DeleteProperty: {
            // Only the last index can be deleted, and never the length.
            if (key == Key_Length || length == 0 || index != length - 1)
                return JS_FALSE;
            bool result = false;
            return setLength(ctx, handler, JS_NewUint32(ctx, index), &result) ? JS_NewBool(ctx, result) : JS_EXCEPTION;
        }
        case Trap_Get:
            if (key == Key_Length)
                return JS_NewUint32(ctx, length);
            return index < length ? JS_DupValue(ctx, handler->list.data[index]) : JS_UNDEFINED;
        case Trap_GetOwnPropertyDescriptor:
            if (key == Key_Length)
                return descriptorOf(ctx, JS_NewUint32(ctx, length), false);
            return index < length ? descriptorOf(ctx, JS_DupValue(ctx, handler->list.data[index]), true) : JS_UNDEFINED;
        case Trap_Has:
            return JS_NewBool(ctx, key == Key_Length || index < length);
        case Trap_Set: {
            JSValueConst value = argc > 2 ? argv[2] : JS_UNDEFINED;
            bool result = false;
            bool ok =
                key == Key_Length ? setLength(ctx, handler, value, &result) : setIndexed(ctx, handler, index, value, &result);
            return ok ? JS_NewBool(ctx, result) : JS_EXCEPTION;
        }
        default:
            return JS_ThrowTypeError(ctx, "Illegal invocation");
    }
}

Handler* PR_NULLABLE handlerOf(JSContext* ctx, JSValueConst array) {
    if (!JS_IsProxy(array))
        return nullptr;
    JSValue object = JS_GetProxyHandler(ctx, array);
    Handler* handler = static_cast<Handler*>(JS_GetOpaque(object, handlerClass));
    JS_FreeValue(ctx, object);
    return handler;
}

} // namespace

void jsObservableArrayBind(JSContext* ctx) {
    JSRuntime* runtime = JS_GetRuntime(ctx);
    JSClassDef definition = {"ObservableArrayHandler", handlerFinalize, handlerMark, nullptr, nullptr, nullptr};
    jsRegisterClass(runtime, &handlerClass, definition);
    JSValue existing = JS_GetClassProto(ctx, handlerClass);
    bool bound = JS_IsObject(existing);
    JS_FreeValue(ctx, existing);
    if (bound)
        return;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue reflect = JS_GetPropertyStr(ctx, global, "Reflect");
    JSValue proto = JS_NewObjectProto(ctx, JS_NULL);
    for (int i = 0; i < int(countOf(kTrapNames)); ++i) {
        JSValue fallback = JS_GetPropertyStr(ctx, reflect, kTrapNames[i]);
        JSValue function = JS_NewCFunctionData(ctx, JsCallScoped<trap>::call, 2, i, 1, &fallback);
        JS_FreeValue(ctx, fallback);
        JS_DefinePropertyValueStr(ctx, proto, kTrapNames[i], function, 0);
    }
    JS_DefinePropertyValueStr(ctx, proto, "Proxy", JS_GetPropertyStr(ctx, global, "Proxy"), 0);
    JS_SetClassProto(ctx, handlerClass, proto);
    JS_FreeValue(ctx, reflect);
    JS_FreeValue(ctx, global);
}

JSValue jsObservableArrayNew(JSContext* ctx, JSValueConst owner, const JsObservableArrayHooks* hooks) {
    JSValue object = JS_NewObjectClass(ctx, int(handlerClass));
    if (JS_IsException(object))
        return object;
    Handler* handler = alloc<Handler>();
    *handler = Handler{hooks, JS_DupValue(ctx, owner), {}};
    JS_SetOpaque(object, handler);

    JSValue proto = JS_GetClassProto(ctx, handlerClass);
    JSValue proxy = JS_GetPropertyStr(ctx, proto, "Proxy");
    JSValue args[2] = {JS_NewArray(ctx), object};
    JSValue array = JS_CallConstructor(ctx, proxy, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, object);
    JS_FreeValue(ctx, proxy);
    JS_FreeValue(ctx, proto);
    return array;
}

uint32_t jsObservableArrayLength(JSContext* ctx, JSValueConst array) {
    Handler* handler = handlerOf(ctx, array);
    return handler != nullptr ? handler->list.count : 0;
}

JSValue jsObservableArrayItem(JSContext* ctx, JSValueConst array, uint32_t index) {
    Handler* handler = handlerOf(ctx, array);
    if (handler == nullptr || index >= handler->list.count)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, handler->list.data[index]);
}

bool jsObservableArraySet(JSContext* ctx, JSValueConst array, JSValueConst values) {
    RawArray<JSValue> items;
    bool ok = jsSequence(ctx, values, [&](JSValue item) {
        items.push(item);
        return true;
    });

    Handler* handler = handlerOf(ctx, array);
    bool result = false;
    ok = ok && setLength(ctx, handler, JS_NewUint32(ctx, 0), &result);
    for (uint32_t i = 0; ok && i < items.count; ++i)
        ok = setIndexed(ctx, handler, i, items.data[i], &result);
    for (uint32_t i = 0; i < items.count; ++i)
        JS_FreeValue(ctx, items.data[i]);
    items.reset();
    return ok;
}

} // namespace pr
