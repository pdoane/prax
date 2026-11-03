// The QuickJS layer of Web IDL.
//
// webidl - https://webidl.spec.whatwg.org/

#include "pr/webidl_quickjs.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/webidl.h"

namespace pr {
namespace {

// What an interface gives its class beyond what jsDeclareClass records, which the runtime keeps as the class's data.
struct Interface {
    JSClassID base;
    const JsIndexed* PR_NULLABLE indexed;
    const JsNamed* PR_NULLABLE named;
    const JSCFunctionListEntry* PR_NULLABLE unforgeable;
    int unforgeableCount;
    const JsCollection* PR_NULLABLE collection;
    JSCFunction* PR_NULLABLE constructor;
};

// The runtime's interfaces, which it frees with itself.
constexpr char kInterfacesKey = 0;

Array<Interface*>* interfacesOf(JSRuntime* runtime) {
    return static_cast<Array<Interface*>*>(jsRuntimeOwned(
        runtime, &kInterfacesKey, []() -> void* { return allocZeroed<Array<Interface*>>(); }, [](void* list) {
            Array<Interface*>* interfaces = static_cast<Array<Interface*>*>(list);
            for (uint32_t i = 0; i < interfaces->count; ++i)
                prMemoryFree(interfaces->data[i]);
            interfaces->reset();
            prMemoryFree(interfaces);
        }));
}

// Returns the interface of class `id`, or null for a class that jsDefineClass did not define.
const Interface* PR_NULLABLE interfaceOf(JSRuntime* runtime, JSClassID id) {
    return static_cast<const Interface*>(jsClassData(runtime, id));
}

// The class's hook that defines the [LegacyUnforgeable] members of the interface and of each interface it inherits from
// on each of its objects, as their own properties (webidl/#LegacyUnforgeable).
void defineUnforgeable(JSContext* ctx, JSValueConst object, void*) {
    JSRuntime* runtime = JS_GetRuntime(ctx);
    for (const Interface* at = interfaceOf(runtime, JS_GetClassID(object)); at != nullptr;
            at = at->base != 0 ? interfaceOf(runtime, at->base) : nullptr) {
        if (at->unforgeable != nullptr)
            JS_SetPropertyFunctionList(ctx, object, at->unforgeable, at->unforgeableCount);
    }
}

// The indexed and named properties of an object of a bound class, if its class has either.
struct Platform {
    void* handle;
    const JsIndexed* PR_NULLABLE indexed;
    const JsNamed* PR_NULLABLE named;
};

bool platformOf(JSContext* ctx, JSValueConst object, Platform* out) {
    JSClassID id = JS_GetClassID(object);
    JsWrapper* wrapper = static_cast<JsWrapper*>(JS_GetOpaque(object, id));
    const Interface* interface = wrapper != nullptr ? interfaceOf(JS_GetRuntime(ctx), id) : nullptr;
    if (interface == nullptr)
        return false;
    *out = {wrapper->handle, interface->indexed, interface->named};
    return out->indexed != nullptr || out->named != nullptr;
}

void freeAtoms(JSContext* ctx, Array<JSAtom>* atoms) {
    for (uint32_t i = 0; i < atoms->count; ++i)
        JS_FreeAtom(ctx, atoms->data[i]);
    atoms->reset();
}

// How a platform object interprets a property name: as an index of its indexed properties if it has them, or else as a
// string that may be one of its named properties if it has them, or else as neither.
enum {
    PlatformName_Ordinary,
    PlatformName_Index,
    PlatformName_Name,
};
typedef uint8_t PlatformName;

// Returns how the platform object interprets `atom` (PlatformName), or -1 for an exception. A name's text is read only
// when needed (readName).
int nameKind(JSContext* ctx, const Platform& platform, JSAtom atom, uint32_t* index) {
    int isIndex = jsIndexOf(ctx, atom, index);
    if (isIndex < 0)
        return -1;
    if (isIndex > 0 && platform.indexed != nullptr)
        return PlatformName_Index;
    if (platform.named == nullptr)
        return PlatformName_Ordinary;
    JSValue value = JS_AtomToValue(ctx, atom);
    int kind = JS_IsSymbol(value) ? PlatformName_Ordinary : PlatformName_Name;
    JS_FreeValue(ctx, value);
    return kind;
}

// Reads the text of a name for which nameKind returned PlatformName_Name. Returns false, with the exception pending, if
// reading fails.
bool readName(JSContext* ctx, JSAtom atom, JsString* out) {
    JSValue value = JS_AtomToValue(ctx, atom);
    bool ok = jsStringRead(ctx, value, out);
    JS_FreeValue(ctx, value);
    return ok;
}

// Returns 1 if a prototype of the object has the property `atom`, 0 if none does, and -1 for an exception. Such a
// property hides a named property with that name unless the interface overrides built-ins.
int shadowed(JSContext* ctx, JSValueConst object, const Platform& platform, JSAtom atom) {
    if ((platform.named->flags & JsNamedFlag_OverrideBuiltIns) != 0)
        return 0;
    JSValue proto = JS_GetPrototype(ctx, object);
    int has = JS_IsObject(proto) ? JS_HasProperty(ctx, proto, atom) : 0;
    JS_FreeValue(ctx, proto);
    return has;
}

// Returns the value of the named property `atom` if it is visible (webidl/#dfn-named-property-visibility). A visible
// name is a supported name for which no prototype has a property, unless the interface overrides built-ins. Returns
// undefined if it is not visible, and an exception if one was thrown. A name that a prototype shadows is resolved from
// the prototype alone, since most reads of such an object are of its members. Its text is read into `name` only when
// the object must be queried for it.
JSValue visibleNamed(JSContext* ctx, JSValueConst object, const Platform& platform, JSAtom atom, JsString* name) {
    int hidden = shadowed(ctx, object, platform, atom);
    if (hidden != 0)
        return hidden < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    if (!readName(ctx, atom, name))
        return JS_EXCEPTION;
    return platform.named->item(ctx, object, platform.handle, jsStringView(name));
}

// The exotic methods of a legacy platform object (webidl/#js-legacy-platform-objects). They reach its JsIndexed and
// JsNamed through the runtime. QuickJS calls them for a property that the object does not hold as its own.
int platformOwnProperty(JSContext* ctx, JSPropertyDescriptor* PR_NULLABLE desc, JSValueConst object, JSAtom atom) {
    Platform platform = {};
    if (!platformOf(ctx, object, &platform))
        return 0;
    uint32_t index = 0;
    JsString name = {};
    int kind = nameKind(ctx, platform, atom, &index);
    JSValue value = JS_UNDEFINED;
    bool writable = false;
    bool enumerable = true;
    if (kind == PlatformName_Index && index < platform.indexed->length(platform.handle)) {
        value = platform.indexed->item(ctx, object, platform.handle, index);
        writable = platform.indexed->setItem != nullptr;
    } else if (kind == PlatformName_Name) {
        value = visibleNamed(ctx, object, platform, atom, &name);
        writable = platform.named->setItem != nullptr;
        enumerable = (platform.named->flags & JsNamedFlag_Unenumerable) == 0;
    }
    jsStringReset(ctx, &name);
    if (kind < 0 || JS_IsException(value))
        return -1;
    if (JS_IsUndefined(value))
        return 0;
    if (desc == nullptr) {
        JS_FreeValue(ctx, value);
        return 1;
    }
    desc->flags = JS_PROP_CONFIGURABLE | (writable ? JS_PROP_WRITABLE : 0) | (enumerable ? JS_PROP_ENUMERABLE : 0);
    desc->value = value;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    return 1;
}

int platformOwnPropertyNames(JSContext* ctx, JSPropertyEnum* PR_NULLABLE* PR_NONNULL names,
        uint32_t* count, JSValueConst object) {
    Platform platform = {};
    platformOf(ctx, object, &platform);
    uint32_t length = platform.indexed != nullptr ? platform.indexed->length(platform.handle) : 0;
    Array<JSAtom> named;
    if (platform.named != nullptr && !platform.named->names(ctx, platform.handle, &named)) {
        freeAtoms(ctx, &named);
        return -1;
    }
    JSPropertyEnum* list = static_cast<JSPropertyEnum*>(js_malloc(ctx, sizeof(JSPropertyEnum) * (length + named.count + 1)));
    if (list == nullptr) {
        freeAtoms(ctx, &named);
        return -1;
    }
    for (uint32_t i = 0; i < length; ++i)
        list[i] = JSPropertyEnum{true, JS_NewAtomUInt32(ctx, i)};
    // A named property that a prototype shadows is not the object's own. An index of its indexed properties is not a
    // name.
    uint32_t total = length;
    bool enumerable = platform.named != nullptr && (platform.named->flags & JsNamedFlag_Unenumerable) == 0;
    for (uint32_t i = 0; i < named.count; ++i) {
        uint32_t index = 0;
        int isIndex = platform.indexed != nullptr ? jsIndexOf(ctx, named.data[i], &index) : 0;
        if (isIndex == 0 && shadowed(ctx, object, platform, named.data[i]) == 0)
            list[total++] = JSPropertyEnum{enumerable, JS_DupAtom(ctx, named.data[i])};
    }
    freeAtoms(ctx, &named);
    *names = list;
    *count = total;
    return 0;
}

int platformDefineProperty(JSContext* ctx, JSValueConst object, JSAtom atom, JSValueConst value,
        JSValueConst getter, JSValueConst setter, int flags) {
    Platform platform = {};
    uint32_t index = 0;
    JsString name = {};
    int kind = platformOf(ctx, object, &platform) ? nameKind(ctx, platform, atom, &index) : PlatformName_Ordinary;
    if (kind == PlatformName_Name && !readName(ctx, atom, &name))
        kind = -1;
    if (kind < 0)
        return -1;
    bool accessor = (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) != 0;
    JSValueConst given = (flags & JS_PROP_HAS_VALUE) != 0 ? value : JS_UNDEFINED;
    int result = -2;   // the ordinary definition
    if (kind == PlatformName_Index) {
        if (platform.indexed->setItem == nullptr || accessor)
            result = 0;
        else if (platform.indexed->setItem(ctx, object, platform.handle, index, given))
            result = 1;
        else
            result = -1;
    } else if (kind == PlatformName_Name) {
        // webidl/#legacy-platform-object-defineownproperty: a supported name, or any name if the interface has a named
        // setter.
        JSValue current = platform.named->item(ctx, object, platform.handle, jsStringView(&name));
        bool supported = !JS_IsUndefined(current);
        if (JS_IsException(current))
            result = -1;
        else if (supported || platform.named->setItem != nullptr) {
            if (platform.named->setItem == nullptr || accessor)
                result = 0;
            else
                result = platform.named->setItem(ctx, object, platform.handle, jsStringView(&name), given) ? 1 : -1;
        }
        JS_FreeValue(ctx, current);
    }
    jsStringReset(ctx, &name);
    if (result == -2)
        return JS_DefineProperty(ctx, object, atom, value, getter, setter, flags | JS_PROP_NO_EXOTIC);
    if (result == 0 && (flags & JS_PROP_THROW) != 0) {
        JS_ThrowTypeError(ctx, "Cannot define the property of a platform object");
        return -1;
    }
    return result;
}

int platformDeleteProperty(JSContext* ctx, JSValueConst object, JSAtom atom) {
    Platform platform = {};
    uint32_t index = 0;
    JsString name = {};
    int kind = platformOf(ctx, object, &platform) ? nameKind(ctx, platform, atom, &index) : PlatformName_Ordinary;
    int result = kind < 0 ? -1 : 1;
    if (kind == PlatformName_Index) {
        result = index < platform.indexed->length(platform.handle) ? 0 : 1;
    } else if (kind == PlatformName_Name) {
        JSValue value = visibleNamed(ctx, object, platform, atom, &name);
        if (JS_IsException(value)) {
            result = -1;
        } else if (!JS_IsUndefined(value)) {
            result = platform.named->deleteItem != nullptr ? 1 : 0;
            if (platform.named->deleteItem != nullptr)
                platform.named->deleteItem(platform.handle, jsStringView(&name));
        }
        JS_FreeValue(ctx, value);
    }
    jsStringReset(ctx, &name);
    return result;
}

// webidl/#legacy-platform-object-set: an array index or a name set on the object itself, rather than on an object that
// inherits from it, goes to its indexed or named setter, before any prototype's property. Anything else is the ordinary
// [[Set]], which QuickJS continues with, reading the object's own property through platformOwnProperty.
int platformSetProperty(JSContext* ctx, JSValueConst object, JSAtom atom, JSValueConst value, JSValueConst receiver, int) {
    Platform platform = {};
    if (!JS_IsObject(receiver) || JS_VALUE_GET_PTR(receiver) != JS_VALUE_GET_PTR(object) ||
            !platformOf(ctx, object, &platform))
        return JS_SET_PROPERTY_ORDINARY;
    uint32_t index = 0;
    int kind = nameKind(ctx, platform, atom, &index);
    if (kind < 0)
        return -1;
    if (kind == PlatformName_Index && platform.indexed->setItem != nullptr)
        return platform.indexed->setItem(ctx, object, platform.handle, index, value) ? 1 : -1;
    if (kind != PlatformName_Name || platform.named->setItem == nullptr)
        return JS_SET_PROPERTY_ORDINARY;
    JsString name = {};
    bool set = readName(ctx, atom, &name) && platform.named->setItem(ctx, object, platform.handle, jsStringView(&name), value);
    jsStringReset(ctx, &name);
    return set ? 1 : -1;
}

JSClassExoticMethods kPlatformExotic = {
    JsCallScoped<platformOwnProperty>::call,
    JsCallScoped<platformOwnPropertyNames>::call,
    JsCallScoped<platformDeleteProperty>::call,
    JsCallScoped<platformDefineProperty>::call,
    nullptr,
    nullptr,
    JsCallScoped<platformSetProperty>::call,
};

// webidl/#dfn-default-iterator-object: the object being iterated, the part of each entry returned, and the index of the
// next entry.
struct PairIterator {
    JSValue owner;
    JSCFunction* entryAt;
    uint32_t index;
    JsMapPart part;
    bool done;
};

PairIterator* PR_NULLABLE pairIteratorOf(JSValueConst value) {
    return static_cast<PairIterator*>(JS_GetOpaque(value, JS_GetClassID(value)));
}

void pairFinalizer(JSRuntime* runtime, JSValue value) {
    PairIterator* iterator = pairIteratorOf(value);
    if (iterator == nullptr)
        return;
    JS_FreeValueRT(runtime, iterator->owner);
    prMemoryFree(iterator);
}

void pairMark(JSRuntime* runtime, JSValueConst value, JS_MarkFunc* mark) {
    PairIterator* iterator = pairIteratorOf(value);
    if (iterator != nullptr)
        JS_MarkValue(runtime, iterator->owner, mark);
}

// next, of webidl/#js-iterator-prototype-object.
JSValue pairNext(JSContext* ctx, JSValueConst iteratorObject, int, JSValueConst*, int id) {
    PairIterator* iterator = static_cast<PairIterator*>(JS_GetOpaque2(ctx, iteratorObject, JSClassID(id)));
    if (iterator == nullptr)
        return JS_EXCEPTION;
    JSValue index = JS_NewUint32(ctx, iterator->index);
    JSValue value = iterator->done ? JS_UNDEFINED : iterator->entryAt(ctx, iterator->owner, 1, &index);
    JS_FreeValue(ctx, index);
    if (JS_IsException(value))
        return value;
    iterator->done = JS_IsUndefined(value);
    if (!iterator->done) {
        ++iterator->index;
        if (iterator->part != JsMapPart_Entries) {
            JSValue part = JS_GetPropertyUint32(ctx, value, iterator->part == JsMapPart_Keys ? 0 : 1);
            JS_FreeValue(ctx, value);
            value = part;
        }
    }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "value", value);
    JS_SetPropertyStr(ctx, result, "done", JS_NewBool(ctx, iterator->done));
    return result;
}

// Reads member `name` of a dictionary, converted by `convert` if given.
template <typename Convert>
bool dictMember(JSContext* ctx, JSValueConst dictionary, const char* name, bool* PR_NULLABLE given, Convert convert) {
    JSValue member = JS_GetPropertyStr(ctx, dictionary, name);
    if (JS_IsException(member))
        return false;
    bool present = !JS_IsUndefined(member);
    if (given != nullptr)
        *given = present;
    bool ok = !present || convert(member);
    JS_FreeValue(ctx, member);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Setlike and maplike classes

// The members of a setlike or maplike class. Each is a function whose data are, in order: the class's ID, the realm's
// Set or Map constructor, the prototype methods that a member calls, and the member's own method.
enum {
    CollectionData_Class,
    CollectionData_Constructor,
    CollectionData_Add,        // add, or set for a map
    CollectionData_Clear,
    CollectionData_Has,
    CollectionData_Delete,
    CollectionData_Member,     // the backing's method that the member calls
    CollectionData_Count,
};
typedef uint8_t CollectionData;

enum {
    CollectionMember_Size,
    CollectionMember_Iterate,  // entries, keys and values
    CollectionMember_ForEach,
    CollectionMember_Has,
    CollectionMember_Get,
    CollectionMember_Add,      // add, or set for a map
    CollectionMember_Delete,
    CollectionMember_Clear,
};
typedef uint8_t CollectionMember;

constexpr char kBacking[] = "[[Backing]]";
constexpr char kBackingVersion[] = "[[BackingVersion]]";

// Returns the backing of `backingObject`, or an exception. The backing is refilled if the platform object's version
// differs from the version that the backing last synchronized with.
JSValue backingOf(JSContext* ctx, JSValueConst backingObject, void* handle, const JsCollection& collection,
        JSValueConst* data) {
    JSValue backing = jsSameObject(ctx, backingObject, kBacking);
    if (JS_IsUndefined(backing)) {
        backing = JS_CallConstructor(ctx, data[CollectionData_Constructor], 0, nullptr);
        if (JS_IsException(backing))
            return backing;
        jsSetSameObject(ctx, backingObject, kBacking, backing);
    }
    uint32_t version = collection.version(handle);
    JSValue taken = jsSameObject(ctx, backingObject, kBackingVersion);
    uint32_t last = 0;
    bool current = JS_IsNumber(taken) && JS_ToUint32(ctx, &last, taken) == 0 && last == version;
    JS_FreeValue(ctx, taken);
    if (current)
        return backing;
    JSValue entries = collection.entries(ctx, backingObject, handle);
    bool ok = !JS_IsException(entries);
    if (ok)
        JS_FreeValue(ctx, JS_Call(ctx, data[CollectionData_Clear], backing, 0, nullptr));
    int64_t length = 0;
    ok = ok && JS_GetLength(ctx, entries, &length) >= 0;
    for (int64_t i = 0; ok && i < length; ++i) {
        JSValue entry = JS_GetPropertyInt64(ctx, entries, i);
        JSValue arguments[2] = {
            collection.map ? JS_GetPropertyUint32(ctx, entry, 0) : JS_DupValue(ctx, entry),
            collection.map ? JS_GetPropertyUint32(ctx, entry, 1) : JS_UNDEFINED,
        };
        JSValue added = JS_Call(ctx, data[CollectionData_Add], backing, collection.map ? 2 : 1, arguments);
        ok = !JS_IsException(added);
        JS_FreeValue(ctx, added);
        JS_FreeValue(ctx, arguments[0]);
        JS_FreeValue(ctx, arguments[1]);
        JS_FreeValue(ctx, entry);
    }
    JS_FreeValue(ctx, entries);
    if (!ok) {
        JS_FreeValue(ctx, backing);
        return JS_EXCEPTION;
    }
    jsSetSameObject(ctx, backingObject, kBackingVersion, JS_NewUint32(ctx, version));
    return backing;
}

// Records that the backing now reflects the platform object's change, by updating its version.
void tookChange(JSContext* ctx, JSValueConst object, void* handle, const JsCollection& collection) {
    jsSetSameObject(ctx, object, kBackingVersion, JS_NewUint32(ctx, collection.version(handle)));
}

// webidl/#js-set-forEach and webidl/#js-map-forEach. The callback receives the value, the key and the platform object.
// The function's data are the callback, the this value and the platform object.
JSValue forEachCallback(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValueConst* data) {
    JSValueConst arguments[] = {argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, data[2]};
    return JS_Call(ctx, data[0], data[1], 3, arguments);
}

JSValue collectionMember(JSContext* ctx, JSValueConst backingObject, int argc, JSValueConst* argv, int magic,
        JSValueConst* data) {
    uint32_t id = 0;
    JS_ToUint32(ctx, &id, data[CollectionData_Class]);
    const JsCollection& collection = *interfaceOf(JS_GetRuntime(ctx), id)->collection;
    void* handle = jsThis<void>(ctx, backingObject, id);
    if (handle == nullptr)
        return JS_EXCEPTION;
    JSValue key = JS_UNDEFINED;
    JSValue value = JS_UNDEFINED;
    bool keyed = magic == CollectionMember_Has || magic == CollectionMember_Get ||
        magic == CollectionMember_Add || magic == CollectionMember_Delete;
    if (keyed) {
        key = collection.key(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (JS_IsException(key))
            return key;
    }
    if (magic == CollectionMember_Add && collection.map) {
        value = collection.value(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
        if (JS_IsException(value)) {
            JS_FreeValue(ctx, key);
            return value;
        }
    }
    if (magic == CollectionMember_ForEach && (argc < 1 || !JS_IsFunction(ctx, argv[0])))
        return JS_ThrowTypeError(ctx, "forEach needs a function");
    JSValue backing = backingOf(ctx, backingObject, handle, collection, data);
    if (JS_IsException(backing)) {
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, value);
        return backing;
    }
    JSValue result = JS_UNDEFINED;
    switch (magic) {
        case CollectionMember_Size:
        case CollectionMember_Iterate:
            result = JS_Call(ctx, data[CollectionData_Member], backing, 0, nullptr);
            break;
        case CollectionMember_ForEach: {
            JSValueConst callbackData[] = {argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, backingObject};
            JSValue callback = JS_NewCFunctionData(ctx, JsCallScoped<forEachCallback>::call, 2, 0, 3, callbackData);
            result = JS_Call(ctx, data[CollectionData_Member], backing, 1, &callback);
            JS_FreeValue(ctx, callback);
            break;
        }
        case CollectionMember_Has:
        case CollectionMember_Get:
            result = JS_Call(ctx, data[CollectionData_Member], backing, 1, &key);
            break;
        case CollectionMember_Add: {
            if (!collection.add(ctx, handle, key, value)) {
                result = JS_EXCEPTION;
                break;
            }
            JSValueConst arguments[] = {key, value};
            result = JS_Call(ctx, data[CollectionData_Add], backing, collection.map ? 2 : 1, arguments);
            tookChange(ctx, backingObject, handle, collection);
            if (!JS_IsException(result)) {
                JS_FreeValue(ctx, result);
                result = JS_DupValue(ctx, backingObject);
            }
            break;
        }
        case CollectionMember_Delete: {
            JSValue had = JS_Call(ctx, data[CollectionData_Has], backing, 1, &key);
            if (JS_ToBool(ctx, had) > 0) {
                collection.remove(ctx, handle, key);
                result = JS_Call(ctx, data[CollectionData_Delete], backing, 1, &key);
                tookChange(ctx, backingObject, handle, collection);
            } else {
                result = JS_IsException(had) ? JS_EXCEPTION : JS_FALSE;
            }
            JS_FreeValue(ctx, had);
            break;
        }
        case CollectionMember_Clear:
            collection.clear(ctx, handle);
            result = JS_Call(ctx, data[CollectionData_Clear], backing, 0, nullptr);
            tookChange(ctx, backingObject, handle, collection);
            break;
    }
    JS_FreeValue(ctx, backing);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, value);
    return result;
}

// Defines @@iterator on `proto` as the method `name`.
void defineIteratorAlias(JSContext* ctx, JSValueConst proto, const char* name) {
    JS_DefinePropertyValue(ctx, proto, JS_ATOM_Symbol_iterator, JS_GetPropertyStr(ctx, proto, name),
        JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

// Defines the members on the prototype of a setlike or maplike class. Each is a function that calls `member` of the
// realm's Set or Map prototype.
void defineCollection(JSContext* ctx, JSValueConst proto, JSClassID id, const JsCollection& collection) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue constructor = JS_GetPropertyStr(ctx, global, collection.map ? "Map" : "Set");
    JSValue prototype = JS_GetPropertyStr(ctx, constructor, "prototype");
    JSValue data[CollectionData_Count] = {
        JS_NewUint32(ctx, id),
        constructor,
        JS_GetPropertyStr(ctx, prototype, collection.map ? "set" : "add"),
        JS_GetPropertyStr(ctx, prototype, "clear"),
        JS_GetPropertyStr(ctx, prototype, "has"),
        JS_GetPropertyStr(ctx, prototype, "delete"),
        JS_UNDEFINED
    };
    auto function = [&](const char* name, int length, int member, JSValue method) {
        data[CollectionData_Member] = method;
        JSValue made = JS_NewCFunctionData(ctx, JsScoped<collectionMember>::call, length, member, CollectionData_Count, data);
        JS_FreeValue(ctx, method);
        data[CollectionData_Member] = JS_UNDEFINED;
        JS_DefinePropertyValueStr(ctx, made, "name", JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
        return made;
    };
    constexpr int kFlags = JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE;
    auto define = [&](const char* name, int length, int member, JSValue method) {
        JS_DefinePropertyValueStr(ctx, proto, name, function(name, length, member, method), kFlags);
    };

    JSAtom sizeAtom = JS_NewAtom(ctx, "size");
    JSPropertyDescriptor size;
    if (JS_GetOwnProperty(ctx, &size, prototype, sizeAtom) > 0) {
        JS_FreeValue(ctx, size.value);
        JS_FreeValue(ctx, size.setter);
        JS_DefinePropertyGetSet(ctx, proto, sizeAtom, function("get size", 0, CollectionMember_Size, size.getter),
            JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    }
    JS_FreeAtom(ctx, sizeAtom);
    constexpr const char* kIterations[] = {"entries", "keys", "values"};
    for (const char* name : kIterations)
        define(name, 0, CollectionMember_Iterate, JS_GetPropertyStr(ctx, prototype, name));
    define("forEach", 1, CollectionMember_ForEach, JS_GetPropertyStr(ctx, prototype, "forEach"));
    define("has", 1, CollectionMember_Has, JS_GetPropertyStr(ctx, prototype, "has"));
    if (collection.map)
        define("get", 1, CollectionMember_Get, JS_GetPropertyStr(ctx, prototype, "get"));
    define(collection.map ? "set" : "add", collection.map ? 2 : 1, CollectionMember_Add, JS_UNDEFINED);
    define("delete", 1, CollectionMember_Delete, JS_UNDEFINED);
    define("clear", 0, CollectionMember_Clear, JS_UNDEFINED);
    // webidl/#js-setlike: @@iterator is values; webidl/#js-maplike: it is entries.
    defineIteratorAlias(ctx, proto, collection.map ? "entries" : "values");

    for (JSValue value : data)
        JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, prototype);
    JS_FreeValue(ctx, global);
}

// Gives `proto` an array's @@iterator, which reads its objects' indexes and length. If `methods` is true, it also gives
// `proto` an array's entries, keys, values and forEach.
void installArrayIteration(JSContext* ctx, JSValueConst proto, bool methods) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue array = JS_GetPropertyStr(ctx, global, "Array");
    JSValue arrayProto = JS_GetPropertyStr(ctx, array, "prototype");
    if (methods) {
        const char* const names[] = {"entries", "keys", "values", "forEach"};
        for (const char* name : names)
            JS_SetPropertyStr(ctx, proto, name, JS_GetPropertyStr(ctx, arrayProto, name));
    }

    // The iterator is the values function itself.
    JS_DefinePropertyValue(ctx, proto, JS_ATOM_Symbol_iterator, JS_GetPropertyStr(ctx, arrayProto, "values"),
        JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_FreeValue(ctx, arrayProto);
    JS_FreeValue(ctx, array);
    JS_FreeValue(ctx, global);
}

void defineConstants(JSContext* ctx, JSValueConst constructor, JSValueConst proto, const JsConstant* constants, int count) {
    for (int i = 0; i < count; ++i) {
        JS_DefinePropertyValueStr(ctx, constructor, constants[i].name,
            JS_NewUint32(ctx, constants[i].value), JS_PROP_ENUMERABLE);
        JS_DefinePropertyValueStr(ctx, proto, constants[i].name,
            JS_NewUint32(ctx, constants[i].value), JS_PROP_ENUMERABLE);
    }
}

// The interface object of a class. It runs the class's constructor in a call's scope. The magic is the class.
JSValue constructInScope(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv, int magic) {
    const Interface* interface = interfaceOf(JS_GetRuntime(ctx), JSClassID(magic));
    if (interface == nullptr || interface->constructor == nullptr)
        return jsIllegalConstructor(ctx, newTarget, argc, argv);
    jsCallEnter(ctx);
    JSValue result = interface->constructor(ctx, newTarget, argc, argv);
    jsCallLeave(ctx);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMException

JSClassID domExceptionClass;
JSClassID quotaExceededErrorClass;

// webidl/#idl-DOMException: a name and a message. The code is derived from the name. A QuotaExceededError
// (webidl/#quotaexceedederror) also has a quota and a requested amount, each NaN for null, as for every other
// exception.
struct DomException {
    PrString name;
    PrString message;
    double quota;
    double requested;
};

DomException* PR_NULLABLE exceptionOf(JSContext* ctx, JSValueConst object) {
    return jsValueOf<DomException>(ctx, object, domExceptionClass, "DOMException");
}

JSValue newException(JSContext* ctx, JSClassID id, PrStringView message, PrStringView name,
        double quota = kNanF64, double requested = kNanF64) {
    DomException exception = {prStringCopy(name), prStringCopy(message), quota, requested};
    JSValue object = jsNewValue(ctx, id, exception);
    if (JS_IsException(object)) {
        prStringReset(&exception.name);
        prStringReset(&exception.message);
    }
    return object;
}

template <const JSClassID* PR_NONNULL Id>
void finalizer(JSRuntime* runtime, JSValue object) {
    jsFinalize<DomException>(runtime, object, *Id, [](DomException* exception) {
        prStringReset(&exception->name);
        prStringReset(&exception->message);
        prMemoryFree(exception);
    });
}

// webidl/#dom-domexception-domexception
JSValue construct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument message = {ctx};
    StringArgument name = {ctx, "Error"};
    if ((jsArgGiven(argc, argv, 0) && !message.read(argv[0])) || (jsArgGiven(argc, argv, 1) && !name.read(argv[1])))
        return JS_EXCEPTION;
    return jsConstructed(ctx, newTarget, newException(ctx, domExceptionClass, message.value(), name.value()));
}

JSValue nameGet(JSContext* ctx, JSValueConst exceptionObject) {
    DomException* exception = exceptionOf(ctx, exceptionObject);
    return exception != nullptr ? jsNewString(ctx, &exception->name) : JS_EXCEPTION;
}

JSValue messageGet(JSContext* ctx, JSValueConst exceptionObject) {
    DomException* exception = exceptionOf(ctx, exceptionObject);
    return exception != nullptr ? jsNewString(ctx, &exception->message) : JS_EXCEPTION;
}

JSValue codeGet(JSContext* ctx, JSValueConst exceptionObject) {
    DomException* exception = exceptionOf(ctx, exceptionObject);
    return exception != nullptr ? JS_NewUint32(ctx, prWebIdlErrorCode(prStringView(&exception->name))) : JS_EXCEPTION;
}

// Reads a member of QuotaExceededErrorOptions, a double, which is not NaN or an infinity (webidl/#js-double). `*out`
// stays NaN if the member is not given.
bool readQuotaMember(JSContext* ctx, JSValueConst options, const char* name, double* out) {
    bool given = false;
    if (!jsDictNumber(ctx, options, name, out, &given))
        return false;
    if (given && !isFiniteF64(*out)) {
        JS_ThrowTypeError(ctx, "%s is not a finite number", name);
        return false;
    }
    return true;
}

// webidl/#dom-quotaexceedederror-quotaexceedederror
JSValue constructQuota(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument message = {ctx};
    if (jsArgGiven(argc, argv, 0) && !message.read(argv[0]))
        return JS_EXCEPTION;
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    double quota = kNanF64;
    double requested = kNanF64;
    if (!jsDictionary(ctx, options, &present) || (present && (!readQuotaMember(ctx, options, "quota", &quota) ||
            !readQuotaMember(ctx, options, "requested", &requested))))
        return JS_EXCEPTION;
    if (quota < 0 || requested < 0)
        return JS_ThrowRangeError(ctx, "A quota or a requested amount is negative");
    if (requested < quota)
        return JS_ThrowRangeError(ctx, "The requested amount is less than the quota");
    return jsConstructed(ctx, newTarget, newException(ctx, quotaExceededErrorClass, message.value(),
        "QuotaExceededError", quota, requested));
}

// The quota and requested getters, whose magic is 0 and 1.
JSValue quotaGet(JSContext* ctx, JSValueConst exceptionObject, int magic) {
    DomException* exception = jsValueOf<DomException>(ctx, exceptionObject, quotaExceededErrorClass, "QuotaExceededError");
    if (exception == nullptr)
        return JS_EXCEPTION;
    double value = magic == 0 ? exception->quota : exception->requested;
    return isNanF64(value) ? JS_NULL : JS_NewFloat64(ctx, value);
}

constexpr JSCFunctionListEntry kDomExceptionProto[] = {
    PR_JS_TAG("DOMException"),
    PR_JS_GET("name", nameGet),
    PR_JS_GET("message", messageGet),
    PR_JS_GET("code", codeGet),
};

constexpr JSCFunctionListEntry kQuotaExceededErrorProto[] = {
    PR_JS_TAG("QuotaExceededError"),
    PR_JS_MAGIC_GET("quota", quotaGet, 0),
    PR_JS_MAGIC_GET("requested", quotaGet, 1),
};

constexpr JsConstant kConstants[] = {
    {"INDEX_SIZE_ERR", PrWebIdlErrorCode_IndexSizeErr},
    {"DOMSTRING_SIZE_ERR", PrWebIdlErrorCode_DomstringSizeErr},
    {"HIERARCHY_REQUEST_ERR", PrWebIdlErrorCode_HierarchyRequestErr},
    {"WRONG_DOCUMENT_ERR", PrWebIdlErrorCode_WrongDocumentErr},
    {"INVALID_CHARACTER_ERR", PrWebIdlErrorCode_InvalidCharacterErr},
    {"NO_DATA_ALLOWED_ERR", PrWebIdlErrorCode_NoDataAllowedErr},
    {"NO_MODIFICATION_ALLOWED_ERR", PrWebIdlErrorCode_NoModificationAllowedErr},
    {"NOT_FOUND_ERR", PrWebIdlErrorCode_NotFoundErr},
    {"NOT_SUPPORTED_ERR", PrWebIdlErrorCode_NotSupportedErr},
    {"INUSE_ATTRIBUTE_ERR", PrWebIdlErrorCode_InuseAttributeErr},
    {"INVALID_STATE_ERR", PrWebIdlErrorCode_InvalidStateErr},
    {"SYNTAX_ERR", PrWebIdlErrorCode_SyntaxErr},
    {"INVALID_MODIFICATION_ERR", PrWebIdlErrorCode_InvalidModificationErr},
    {"NAMESPACE_ERR", PrWebIdlErrorCode_NamespaceErr},
    {"INVALID_ACCESS_ERR", PrWebIdlErrorCode_InvalidAccessErr},
    {"VALIDATION_ERR", PrWebIdlErrorCode_ValidationErr},
    {"TYPE_MISMATCH_ERR", PrWebIdlErrorCode_TypeMismatchErr},
    {"SECURITY_ERR", PrWebIdlErrorCode_SecurityErr},
    {"NETWORK_ERR", PrWebIdlErrorCode_NetworkErr},
    {"ABORT_ERR", PrWebIdlErrorCode_AbortErr},
    {"URL_MISMATCH_ERR", PrWebIdlErrorCode_UrlMismatchErr},
    {"QUOTA_EXCEEDED_ERR", PrWebIdlErrorCode_QuotaExceededErr},
    {"TIMEOUT_ERR", PrWebIdlErrorCode_TimeoutErr},
    {"INVALID_NODE_TYPE_ERR", PrWebIdlErrorCode_InvalidNodeTypeErr},
    {"DATA_CLONE_ERR", PrWebIdlErrorCode_DataCloneErr},
};

// Calls the getter of DataView.prototype that `name` names on `value`. The getter throws for a receiver that is not a
// DataView, and the call then returns undefined with nothing thrown.
JSValue dataViewGet(JSContext* ctx, JSValueConst prototype, JSValueConst value, const char* name) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JSPropertyDescriptor descriptor = {};
    int found = JS_GetOwnProperty(ctx, &descriptor, prototype, atom);
    JS_FreeAtom(ctx, atom);
    if (found <= 0) {
        if (found < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    JSValue result = JS_Call(ctx, descriptor.getter, value, 0, nullptr);
    JS_FreeValue(ctx, descriptor.value);
    JS_FreeValue(ctx, descriptor.getter);
    JS_FreeValue(ctx, descriptor.setter);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return result;
}

// Returns true if `value` is a DataView, and writes the buffer that it views, with a reference for the caller, and the
// range of the buffer that it views.
bool isDataView(JSContext* ctx, JSValueConst value, JSValue* buffer, size_t* offset, size_t* length) {
    if (!JS_IsObject(value))
        return false;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue constructor = JS_GetPropertyStr(ctx, global, "DataView");
    JSValue prototype = JS_GetPropertyStr(ctx, constructor, "prototype");
    JS_FreeValue(ctx, constructor);
    JS_FreeValue(ctx, global);
    JSValue viewed = dataViewGet(ctx, prototype, value, "buffer");
    JSValue start = dataViewGet(ctx, prototype, value, "byteOffset");
    JSValue size = dataViewGet(ctx, prototype, value, "byteLength");
    JS_FreeValue(ctx, prototype);
    double first = 0;
    double count = 0;
    bool is = JS_IsArrayBuffer(viewed) && JS_ToFloat64(ctx, &first, start) == 0 && JS_ToFloat64(ctx, &count, size) == 0;
    JS_FreeValue(ctx, start);
    JS_FreeValue(ctx, size);
    if (!is) {
        JS_FreeValue(ctx, viewed);
        return false;
    }
    *buffer = viewed;
    *offset = size_t(first);
    *length = size_t(count);
    return true;
}

} // namespace

bool Uint32Sequence::read(JSValueConst input) {
    uint32_t value = 0;
    bool read = true;
    if (!JS_IsObject(input)) {
        read = JS_ToUint32(ctx, &value, input) >= 0;
        if (read)
            values.push(value);
    } else {
        JSValue length = JS_GetPropertyStr(ctx, input, "length");
        uint32_t count = 0;
        read = JS_ToUint32(ctx, &count, length) >= 0;
        JS_FreeValue(ctx, length);
        for (uint32_t i = 0; read && i < count; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, input, i);
            read = JS_ToUint32(ctx, &value, item) >= 0;
            JS_FreeValue(ctx, item);
            values.push(value);
        }
    }
    jsHoldBytes(ctx, values.data);
    return read;
}

int jsIndexNames(JSContext* ctx, JSPropertyEnum* PR_NULLABLE* PR_NONNULL names, uint32_t* count, uint32_t length) {
    JSPropertyEnum* indexes = static_cast<JSPropertyEnum*>(js_malloc(ctx, sizeof(JSPropertyEnum) * (length + 1)));
    if (indexes == nullptr)
        return -1;
    for (uint32_t i = 0; i < length; ++i)
        indexes[i] = JSPropertyEnum{true, JS_NewAtomUInt32(ctx, i)};
    *names = indexes;
    *count = length;
    return 0;
}

void jsPairIteratorBind(JSContext* ctx, JSClassID* id, const char* name) {
    JSRuntime* runtime = JS_GetRuntime(ctx);
    JSClassDef definition = {name, pairFinalizer, pairMark, nullptr, nullptr, nullptr};
    jsRegisterClass(runtime, id, definition);
    // The intrinsic iterator prototype is the prototype above an array iterator's prototype.
    JSValue array = JS_NewArray(ctx);
    JSValue values = JS_GetPropertyStr(ctx, array, "values");
    JSValue iterator = JS_Call(ctx, values, array, 0, nullptr);
    JSValue arrayProto = JS_GetPrototype(ctx, iterator);
    JSValue base = JS_GetPrototype(ctx, arrayProto);
    JSValue proto = JS_NewObjectProto(ctx, base);
    JS_DefinePropertyValueStr(ctx, proto, "next", JS_NewCFunctionMagic(ctx, JsCallScoped<pairNext>::call, "next", 0,
        JS_CFUNC_generic_magic, int(*id)), JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_DefinePropertyValue(ctx, proto, JS_ATOM_Symbol_toStringTag, JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    JS_SetClassProto(ctx, *id, proto);
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, arrayProto);
    JS_FreeValue(ctx, iterator);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, array);
}

JSValue jsPairIterator(JSContext* ctx, JSClassID id, JSValueConst owner, JSCFunction* entryAt, JsMapPart part) {
    JSValue value = JS_NewObjectClass(ctx, int(id));
    if (JS_IsException(value))
        return value;
    PairIterator* iterator = allocZeroed<PairIterator>();
    iterator->owner = JS_DupValue(ctx, owner);
    iterator->entryAt = entryAt;
    iterator->part = part;
    JS_SetOpaque(value, iterator);
    return value;
}

// forEach (webidl/#js-iterable). Calls the callback with (value, key, owner) for each entry.
JSValue jsPairForEach(JSContext* ctx, JSValueConst owner, JSCFunction* entryAt, int argc, JSValueConst* argv) {
    if (argc == 0 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "forEach needs a function");
    for (uint32_t i = 0;; ++i) {
        JSValue index = JS_NewUint32(ctx, i);
        JSValue entry = entryAt(ctx, owner, 1, &index);
        JS_FreeValue(ctx, index);
        if (JS_IsException(entry) || JS_IsUndefined(entry))
            return entry;
        JSValue args[] = {
            JS_GetPropertyUint32(ctx, entry, 1), JS_GetPropertyUint32(ctx, entry, 0),
            JS_DupValue(ctx, owner),
        };
        JSValue result = JS_Call(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, 3, args);
        for (JSValue value : args)
            JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, entry);
        if (JS_IsException(result))
            return result;
        JS_FreeValue(ctx, result);
    }
}

JSValue jsEntry(JSContext* ctx, JSValue key, JSValue value) {
    JSValue entry = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, entry, 0, key);
    JS_SetPropertyUint32(ctx, entry, 1, value);
    return entry;
}

JSValue jsMapIterate(JSContext* ctx, JSValue entries, JsMapPart part) {
    if (JS_IsException(entries))
        return entries;

    // Keys and values are the first and second items of the entries. Each part is iterated with the values iterator of
    // an array.
    JSValue items = entries;
    if (part != JsMapPart_Entries) {
        items = JS_NewArray(ctx);
        JSValue length = JS_GetPropertyStr(ctx, entries, "length");
        uint32_t count = 0;
        JS_ToUint32(ctx, &count, length);
        JS_FreeValue(ctx, length);
        for (uint32_t i = 0; i < count; ++i) {
            JSValue entry = JS_GetPropertyUint32(ctx, entries, i);
            JS_SetPropertyUint32(ctx, items, i, JS_GetPropertyUint32(ctx, entry, part == JsMapPart_Keys ? 0 : 1));
            JS_FreeValue(ctx, entry);
        }
        JS_FreeValue(ctx, entries);
    }
    JSValue values = JS_GetPropertyStr(ctx, items, "values");
    JSValue iterator = JS_Call(ctx, values, items, 0, nullptr);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, items);
    return iterator;
}

JSValue jsMapForEach(JSContext* ctx, JSValueConst map, JSValue entries, int argc, JSValueConst* argv) {
    if (JS_IsException(entries))
        return entries;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        JS_FreeValue(ctx, entries);
        return JS_ThrowTypeError(ctx, "forEach needs a function");
    }

    JSValueConst thisArg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue length = JS_GetPropertyStr(ctx, entries, "length");
    uint32_t count = 0;
    JS_ToUint32(ctx, &count, length);
    JS_FreeValue(ctx, length);
    JSValue result = JS_UNDEFINED;
    for (uint32_t i = 0; i < count && !JS_IsException(result); ++i) {
        JSValue entry = JS_GetPropertyUint32(ctx, entries, i);
        JSValue arguments[] = {
            JS_GetPropertyUint32(ctx, entry, 1),
            JS_GetPropertyUint32(ctx, entry, 0), JS_DupValue(ctx, map),
        };
        result = JS_Call(ctx, argv[0], thisArg, 3, arguments);
        for (JSValue argument : arguments)
            JS_FreeValue(ctx, argument);
        JS_FreeValue(ctx, entry);
        if (!JS_IsException(result)) {
            JS_FreeValue(ctx, result);
            result = JS_UNDEFINED;
        }
    }
    JS_FreeValue(ctx, entries);
    return result;
}

bool jsArgCount(JSContext* ctx, int argc, int required) {
    if (argc >= required)
        return true;
    JS_ThrowTypeError(ctx, "Not enough arguments");
    return false;
}

bool jsArgNumber(JSContext* ctx, int argc, JSValueConst* argv, int index, double fallback, double* out) {
    *out = fallback;
    return !jsArgGiven(argc, argv, index) || JS_ToFloat64(ctx, out, argv[index]) >= 0;
}

bool jsEnforceRange(JSContext* ctx, JSValueConst value, double lower, double upper, double* out) {
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value) < 0)
        return false;
    number = number < 0 ? ceilF64(number) : floorF64(number);
    if (!isFiniteF64(number) || number < lower || number > upper) {
        JS_ThrowTypeError(ctx, "Value is outside the range of the type");
        return false;
    }
    *out = number + 0.0;
    return true;
}

bool jsDictionary(JSContext* ctx, JSValueConst value, bool* present) {
    *present = !JS_IsUndefined(value) && !JS_IsNull(value);
    if (*present && !JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "Expected a dictionary");
        return false;
    }
    return true;
}

bool jsDictNumber(JSContext* ctx, JSValueConst dictionary, const char* name, double* out, bool* PR_NULLABLE given) {
    return dictMember(ctx, dictionary, name, given, [&](JSValueConst member) {
        return JS_ToFloat64(ctx, out, member) >= 0;
    });
}

bool jsDictBool(JSContext* ctx, JSValueConst dictionary, const char* name, bool* out, bool* PR_NULLABLE given) {
    return dictMember(ctx, dictionary, name, given, [&](JSValueConst member) {
        *out = JS_ToBool(ctx, member) != 0;
        return true;
    });
}

bool jsDictUint32(JSContext* ctx, JSValueConst dictionary, const char* name, uint32_t* out, bool* PR_NULLABLE given) {
    return dictMember(ctx, dictionary, name, given, [&](JSValueConst member) {
        return JS_ToUint32(ctx, out, member) >= 0;
    });
}

bool jsDictString(JSContext* ctx, JSValueConst dictionary, const char* name, StringArgument* out, bool* PR_NULLABLE given) {
    return dictMember(ctx, dictionary, name, given, [&](JSValueConst member) { return out->read(member); });
}

bool jsBufferSource(JSContext* ctx, JSValueConst value, const uint8_t* PR_NULLABLE* PR_NONNULL data, size_t* size) {
    *data = nullptr;
    *size = 0;
    if (JS_IsArrayBuffer(value)) {
        *data = JS_GetArrayBuffer(ctx, size, value);
        return *data != nullptr || *size == 0;
    }
    size_t offset = 0;
    size_t length = 0;
    size_t elementSize = 0;
    JSValue buffer = JS_UNDEFINED;
    if (JS_GetTypedArrayType(value) >= 0) {
        buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &elementSize);
    } else if (!isDataView(ctx, value, &buffer, &offset, &length)) {
        return false;
    }
    if (JS_IsException(buffer)) {
        // A typed array whose buffer was detached is a view of no bytes.
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_GetTypedArrayType(value) >= 0;
    }
    size_t total = 0;
    const uint8_t* bytes = JS_GetArrayBuffer(ctx, &total, buffer);
    JS_FreeValue(ctx, buffer);
    *data = bytes != nullptr ? bytes + offset : nullptr;
    *size = bytes != nullptr ? length : 0;
    return true;
}

JSValue jsDefineClass(JSContext* ctx, const JsClassDefinition& definition) {
    JSRuntime* runtime = JS_GetRuntime(ctx);
    Interface* interface = *definition.id != 0 ? const_cast<Interface*>(interfaceOf(runtime, *definition.id)) : nullptr;
    if (interface == nullptr) {
        interface = alloc<Interface>();
        interfacesOf(runtime)->push(interface);
    }
    const Interface* parent = definition.base != nullptr ? interfaceOf(runtime, *definition.base) : nullptr;
    *interface = {
        definition.base != nullptr ? *definition.base : 0, definition.indexed, definition.named,
        definition.unforgeable, definition.unforgeableCount, definition.collection,
        definition.constructor,
    };
    bool unforgeable = definition.unforgeable != nullptr;
    for (const Interface* at = parent; at != nullptr && !unforgeable;
            at = at->base != 0 ? interfaceOf(runtime, at->base) : nullptr)
        unforgeable = at->unforgeable != nullptr;
    JsClass declared = {
        .name = definition.name,
        .id = definition.id,
        .finalizer = definition.finalizer,
        .mark = definition.mark,
        .base = definition.base,
        .references = definition.references,
        .prepared = unforgeable ? defineUnforgeable : nullptr,
        .wrapped = definition.wrapped,
        .slot = definition.slot,
        .exotic = definition.indexed != nullptr || definition.named != nullptr ? &kPlatformExotic : nullptr,
        .data = interface,
    };
    jsDeclareClass(runtime, declared);
    JSClassID id = *definition.id;

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, definition.proto, definition.protoCount);
    if (definition.collection != nullptr)
        defineCollection(ctx, proto, id, *definition.collection);
    if (definition.iterator != nullptr)
        defineIteratorAlias(ctx, proto, definition.iterator);
    JSCFunctionType construct;
    construct.constructor_magic = constructInScope;
    JSValue constructor = JS_NewCFunction2(ctx, construct.generic, definition.name,
        definition.length, JS_CFUNC_constructor_magic, int(id));
    // The prototype inherits from the base's prototype, and the interface object from the base's interface object
    // (webidl/#interface-object).
    if (definition.base != nullptr) {
        PR_ASSERT(*definition.base != 0 && JS_IsRegisteredClass(runtime, *definition.base));
        JSValue base = JS_GetClassProto(ctx, *definition.base);
        JS_SetPrototype(ctx, proto, base);
        JSValue baseConstructor = JS_GetPropertyStr(ctx, base, "constructor");
        if (JS_IsFunction(ctx, baseConstructor))
            JS_SetPrototype(ctx, constructor, baseConstructor);
        JS_FreeValue(ctx, baseConstructor);
        JS_FreeValue(ctx, base);
    }
    JS_SetConstructor(ctx, constructor, proto);
    if (definition.indexed != nullptr && (parent == nullptr || parent->indexed == nullptr))
        installArrayIteration(ctx, proto, definition.iterable);
    if (definition.constants != nullptr)
        defineConstants(ctx, constructor, proto, definition.constants, definition.constantCount);
    if (definition.statics != nullptr)
        JS_SetPropertyFunctionList(ctx, constructor, definition.statics, definition.staticCount);
    JS_SetClassProto(ctx, id, proto);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, global, definition.name, JS_DupValue(ctx, constructor),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);
    return constructor;
}

void jsWebidlBind(JSContext* ctx) {
    JS_FreeValue(ctx, jsDefineClass(ctx, {
        .name = "DOMException",
        .id = &domExceptionClass,
        .finalizer = finalizer<&domExceptionClass>,
        .mark = jsValueMark,
        .proto = kDomExceptionProto,
        .protoCount = int(countOf(kDomExceptionProto)),
        PR_JS_CONSTANTS(kConstants),
        .constructor = construct,
    }));
    JSValue proto = JS_GetClassProto(ctx, domExceptionClass);
    // webidl/#js-DOMException-specialness: the prototype's prototype is %Error.prototype%.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue error = JS_GetPropertyStr(ctx, global, "Error");
    JSValue errorProto = JS_GetPropertyStr(ctx, error, "prototype");
    JS_SetPrototype(ctx, proto, errorProto);
    JS_FreeValue(ctx, errorProto);
    JS_FreeValue(ctx, error);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, jsDefineClass(ctx, {
        .name = "QuotaExceededError",
        .id = &quotaExceededErrorClass,
        .finalizer = finalizer<&quotaExceededErrorClass>,
        .mark = jsValueMark,
        .proto = kQuotaExceededErrorProto,
        .protoCount = int(countOf(kQuotaExceededErrorProto)),
        .constructor = constructQuota,
        .base = &domExceptionClass,
    }));
}

JSValue jsDomExceptionNew(JSContext* ctx, PrStringView name) {
    return newException(ctx, domExceptionClass, "", name);
}

JSValue jsDomExceptionThrow(JSContext* ctx, PrStringView name) {
    JSValue exception = newException(ctx, domExceptionClass, "", name);
    return JS_IsException(exception) ? exception : JS_Throw(ctx, exception);
}

JSValue jsQuotaExceededErrorThrow(JSContext* ctx, double quota, double requested) {
    JSValue exception = newException(ctx, quotaExceededErrorClass, "", "QuotaExceededError", quota, requested);
    return JS_IsException(exception) ? exception : JS_Throw(ctx, exception);
}

} // namespace pr
