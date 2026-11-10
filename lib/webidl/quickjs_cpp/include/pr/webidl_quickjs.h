// The QuickJS layer of Web IDL: the JavaScript binding of interfaces, their members and the types they take, as every
// module's bindings define them, and DOMException.
//
// webidl - https://webidl.spec.whatwg.org/

#pragma once

#include "pr/count_of.h"
#include "pr/quickjs_binding.h"
#include "pr/string_ops.h"
#include "pr/string_view.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Makes a member enumerable. Web IDL operations and attributes are enumerable, and symbol properties are not.
constexpr JSCFunctionListEntry jsEnumerable(JSCFunctionListEntry entry) {
    entry.prop_flags |= JS_PROP_ENUMERABLE;
    return entry;
}

// Makes a member enumerable and neither configurable nor writable. A [LegacyUnforgeable] member has these attributes.
constexpr JSCFunctionListEntry jsUnforgeable(JSCFunctionListEntry entry) {
    entry.prop_flags = JS_PROP_ENUMERABLE;
    return entry;
}

// Converts `input` to a sequence (webidl/#js-sequence), and calls `visit` with each value under the same contract as
// jsIterate. Throws a TypeError if `input` is not an iterable object.
template <typename Visit>
bool jsSequence(JSContext* ctx, JSValueConst input, Visit visit) {
    JSValue method = JS_IsObject(input) ? jsIteratorMethod(ctx, input) : JS_UNDEFINED;
    if (JS_IsException(method))
        return false;
    bool ok = JS_IsFunction(ctx, method);
    if (ok)
        ok = jsIterate(ctx, input, method, visit);
    else
        JS_ThrowTypeError(ctx, "Expected a sequence");
    JS_FreeValue(ctx, method);
    return ok;
}

// A (sequence<unsigned long> or unsigned long) argument (webidl/#js-union). A single value reads as a sequence of one.
struct Uint32Sequence {
    JSContext* ctx;
    RawArray<uint32_t> values = {};    // the call's scope holds the storage once read

    bool read(JSValueConst input);
};

// Writes to `*names` the own property names of an object with `length` indexed properties, which are its indexes, each
// enumerable (webidl/#legacy-platform-object-ownpropertykeys). Returns -1 if QuickJS could not allocate them.
int jsIndexNames(JSContext* ctx, JSPropertyEnum* PR_NULLABLE* PR_NONNULL names, uint32_t* count, uint32_t length);

// The part of each entry that a map iterator returns (webidl/#js-map-iterator).
enum {
    JsMapPart_Entries,   // the [key, value] array
    JsMapPart_Keys,
    JsMapPart_Values,
};
typedef uint8_t JsMapPart;

// Defines the class `name` of a pair iterator (webidl/#idl-iterable).
void jsPairIteratorBind(JSContext* ctx, JSClassID* id, const char* name);

// jsPairIterator returns a pair iterator of class `id` over `owner`, and jsPairForEach runs the forEach of the pair
// iterable `owner`. The iterator keeps `owner` alive. `entryAt` returns the [key, value] pair at an index, or undefined
// past the end.
JSValue jsPairIterator(JSContext* ctx, JSClassID id, JSValueConst owner, JSCFunction* entryAt, JsMapPart part);
JSValue jsPairForEach(JSContext* ctx, JSValueConst owner, JSCFunction* entryAt, int argc, JSValueConst* argv);

// Returns the array [key, value]. Takes ownership of both.
JSValue jsEntry(JSContext* ctx, JSValue key, JSValue value);

// Returns an iterator over `entries`, an array of jsEntry arrays. Takes ownership of `entries`.
JSValue jsMapIterate(JSContext* ctx, JSValue entries, JsMapPart part);

// Runs the forEach of a maplike (webidl/#js-maplike). Calls the callback `argv[0]` with each value, its key and `map`,
// with `argv[1]` as `this`. Iterates `entries`, an array of [key, value] arrays, and takes ownership of it.
JSValue jsMapForEach(JSContext* ctx, JSValueConst map, JSValue entries, int argc, JSValueConst* argv);

// A (DOMString or sequence<unrestricted double>) argument, as the string or the values. An object with @@iterator is
// the sequence (webidl/#js-union).
struct JsDoubleSequenceOrString {
    RawArray<double> values;
    JsString string;
    bool sequence;

    void reset(JSContext* ctx) {
        values.reset();
        jsStringReset(ctx, &string);
    }

    bool read(JSContext* ctx, JSValueConst input) {
        JSValue method = JS_IsObject(input) ? jsIteratorMethod(ctx, input) : JS_UNDEFINED;
        if (JS_IsException(method))
            return false;
        if (JS_IsUndefined(method) || JS_IsNull(method))
            return jsStringRead(ctx, input, &string);
        sequence = true;
        bool ok = jsIterate(ctx, input, method, [&](JSValue item) {
            double number = 0;
            bool converted = JS_ToFloat64(ctx, &number, item) >= 0;
            JS_FreeValue(ctx, item);
            if (converted)
                values.push(number);
            return converted;
        });
        JS_FreeValue(ctx, method);
        return ok;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Conversions

// Returns true if argument `index` is given. An undefined argument is not given, as for an optional argument with no
// default.
inline bool jsArgGiven(int argc, JSValueConst* argv, int index) {
    return index < argc && !JS_IsUndefined(argv[index]);
}

// Returns true if there are at least `required` arguments (webidl/#js-overloads). Throws a TypeError if there are not.
bool jsArgCount(JSContext* ctx, int argc, int required);

// Converts argument `index` by ToNumber (webidl/#js-unrestricted-double), or uses `fallback` if the argument is not
// given. Returns false if the conversion threw.
bool jsArgNumber(JSContext* ctx, int argc, JSValueConst* argv, int index, double fallback, double* out);

// The largest integer that a JS number holds exactly, which bounds the 64-bit integer types
// (webidl/#abstract-opdef-converttoint).
constexpr double kJsMaxSafeInteger = 9007199254740991.0;

// Converts `value` to an integer type whose range is `lower` to `upper`, with [EnforceRange]
// (webidl/#abstract-opdef-converttoint). Throws a TypeError for a value that is not finite or whose integer part is out
// of the range.
bool jsEnforceRange(JSContext* ctx, JSValueConst value, double lower, double upper, double* out);

// Returns true if `value` converts to a dictionary (webidl/#js-dictionary), and writes to `*present` whether it is
// neither undefined nor null. Any other value that is not an object throws a TypeError.
bool jsDictionary(JSContext* ctx, JSValueConst value, bool* present);

// Reads member `name` of a dictionary, and whether it is given. `*out` is unchanged if it is not given. Returns false
// if reading or converting the member threw. Members are read in the order of webidl/#js-dictionary: the inherited
// dictionary's members first, and each dictionary's members in lexicographic order.
bool jsDictNumber(JSContext* ctx, JSValueConst dictionary, const char* name, double* out, bool* PR_NULLABLE given = nullptr);
bool jsDictBool(JSContext* ctx, JSValueConst dictionary, const char* name, bool* out, bool* PR_NULLABLE given = nullptr);

// Reads an unsigned long member (webidl/#js-unsigned-long).
bool jsDictUint32(JSContext* ctx, JSValueConst dictionary, const char* name, uint32_t* out, bool* PR_NULLABLE given = nullptr);

// Reads a DOMString member, whose view stays valid until `out` is destroyed.
bool jsDictString(JSContext* ctx, JSValueConst dictionary, const char* name, StringArgument* out,
    bool* PR_NULLABLE given = nullptr);

// Reads the bytes of `value` if it is a BufferSource (webidl/#BufferSource): an ArrayBuffer, or a view of one. A
// detached buffer, or a view of one, has no bytes. Returns false, with nothing thrown, for any other value.
bool jsBufferSource(JSContext* ctx, JSValueConst value, const uint8_t* PR_NULLABLE* PR_NONNULL data, size_t* size);

// ---------------------------------------------------------------------------------------------------------------------
// Interfaces

// The indexed properties of a class's objects (webidl/#dfn-support-indexed-properties). `item` takes an index below
// `length`. A class with an indexed setter has `setItem`, which returns false if it threw.
struct JsIndexed {
    uint32_t (*length)(void* handle);
    JSValue (*item)(JSContext* ctx, JSValueConst object, void* handle, uint32_t index);
    bool (*PR_NULLABLE setItem)(JSContext* ctx, JSValueConst object, void* handle, uint32_t index, JSValueConst value);
};

// How a class's named properties behave: whether they are found before the prototype's properties
// ([LegacyOverrideBuiltIns]), and whether they are enumerable ([LegacyUnenumerableNamedProperties]).
enum {
    JsNamedFlag_OverrideBuiltIns = 1 << 0,
    JsNamedFlag_Unenumerable = 1 << 1,
};
typedef uint8_t JsNamedFlags;

// The named properties of a class's objects (webidl/#dfn-support-named-properties). A class with a named setter or
// named deleter has `setItem` or `deleteItem`. Each function returns false, or an exception, if it threw.
struct JsNamed {
    bool (*names)(JSContext* ctx, void* handle, RawArray<JSAtom>* out);   // appends the supported names

    // Returns the value of `name`, or undefined for a name that is not supported.
    JSValue (*item)(JSContext* ctx, JSValueConst object, void* handle, PrStringView name);

    bool (*PR_NULLABLE setItem)(JSContext* ctx, JSValueConst object, void* handle, PrStringView name, JSValueConst value);
    void (*PR_NULLABLE deleteItem)(void* handle, PrStringView name);
    JsNamedFlags flags;
};

// A setlike or maplike class (webidl/#js-setlike, webidl/#js-maplike). Each object keeps a backing Set or Map that
// holds its entries. Iterating the object iterates the backing, and sees entries come and go as it runs. The class's
// members call the Set or Map methods that the realm had when the class was defined, whatever a script later does to
// Set.prototype and Map.prototype. The platform object owns the entries. Add, set, delete and clear change the platform
// object before the backing. If the platform object changed some other way, the backing is refilled the next time a
// member is called.
struct JsCollection {
    bool map;   // maplike, whose entries have values

    // The key or value that an argument converts to under its IDL type, or an exception.
    JSValue (*key)(JSContext* ctx, JSValueConst argument);
    JSValue (*PR_NULLABLE value)(JSContext* ctx, JSValueConst argument);

    // The changes to the platform object's entries. `add` adds or sets an entry, and returns false if it threw.
    // `remove` deletes a key that the object has, and `clear` deletes every entry.
    bool (*add)(JSContext* ctx, void* handle, JSValueConst key, JSValueConst value);
    void (*remove)(JSContext* ctx, void* handle, JSValueConst key);
    void (*clear)(JSContext* ctx, void* handle);

    // A count that changes whenever the platform object's entries change.
    uint32_t (*version)(void* handle);

    // The platform object's entries: an array of its keys for a set, or of [key, value] arrays for a map.
    JSValue (*entries)(JSContext* ctx, JSValueConst object, void* handle);
};

// A constant of an interface (webidl/#js-constants), defined on its interface object and its prototype.
struct JsConstant {
    const char* name;
    uint32_t value;
};

// An interface object and its prototype (webidl/#interface-object), defined on the global object.
struct JsClassDefinition {
    const char* name;
    JSClassID* id;
    JSClassFinalizer* finalizer;
    JSClassGCMark* mark;
    const JSCFunctionListEntry* proto;
    int protoCount;
    const JSCFunctionListEntry* PR_NULLABLE statics;
    int staticCount;
    const JsConstant* PR_NULLABLE constants;
    int constantCount;

    // The constructor, which runs in a call's scope. Null for an interface that script cannot construct.
    JSCFunction* PR_NULLABLE constructor;

    int length;                             // the constructor's length: how many arguments it declares

    // The class that the prototype inherits from, which the realm must define first.
    const JSClassID* PR_NULLABLE base;

    // An indexed class iterates like an array (webidl/#define-the-iteration-methods). A value iterable class
    // (webidl/#idl-iterable) also has an array's iteration methods. Any other iterable class names the prototype method
    // that is its @@iterator.
    const JsIndexed* PR_NULLABLE indexed;
    bool iterable;
    const char* PR_NULLABLE iterator;

    const JsNamed* PR_NULLABLE named;
    const JsReferences* PR_NULLABLE references;

    // The [LegacyUnforgeable] members, which each object has as its own properties (webidl/#LegacyUnforgeable), as do
    // the objects of every class that derives from this one.
    const JSCFunctionListEntry* PR_NULLABLE unforgeable;
    int unforgeableCount;

    JsWrapped* PR_NULLABLE wrapped;
    const JsCollection* PR_NULLABLE collection;   // the members of a setlike or maplike class

    // The slot in which a handle keeps its object, for the root of a hierarchy whose handles have such a slot. The
    // objects of other hierarchies are found in a table.
    void* PR_NULLABLE* PR_NONNULL (*PR_NULLABLE slot)(void* handle);

    bool unexposed;     // the global does not name the interface
};

// The entries of a prototype's member list. Web IDL operations and attributes are enumerable (webidl/#js-attributes,
// webidl/#js-operations), and @@toStringTag names the interface. Each method and setter runs inside the runtime's
// member scope, and each getter in a call's scope.
#define PR_JS_GET(name, getter)                                                                    \
    jsEnumerable(JS_CGETSET_DEF(name, JsCallScoped<getter>::call, nullptr))
#define PR_JS_GET_SET(name, getter, setter)                                                        \
    jsEnumerable(JS_CGETSET_DEF(name, JsCallScoped<getter>::call, JsScoped<setter>::call))
#define PR_JS_MAGIC_GET(name, getter, magic)                                                       \
    jsEnumerable(JS_CGETSET_MAGIC_DEF(name, JsCallScoped<getter>::call, nullptr, magic))
#define PR_JS_MAGIC_GET_SET(name, getter, setter, magic)                                           \
    jsEnumerable(                                                                                  \
                 JS_CGETSET_MAGIC_DEF(name, JsCallScoped<getter>::call, JsScoped<setter>::call, magic))
#define PR_JS_METHOD(name, length, method)                                                         \
    jsEnumerable(JS_CFUNC_DEF(name, length, JsScoped<method>::call))
#define PR_JS_MAGIC(name, length, method, magic)                                                   \
    jsEnumerable(JS_CFUNC_MAGIC_DEF(name, length, JsScoped<method>::call, magic))
#define PR_JS_TAG(name) JS_PROP_STRING_DEF("[Symbol.toStringTag]", name, JS_PROP_CONFIGURABLE)

// A [LegacyUnforgeable] read-only attribute, which is not configurable.
#define PR_JS_UNFORGEABLE_GET(name, getter)                                                        \
    jsUnforgeable(JS_CGETSET_DEF(name, JsCallScoped<getter>::call, nullptr))

// The fields of a JsClassDefinition that every class of handles fills. The mark function is jsMarker of `classId`, and
// the count is the number of entries in `protoList`. The remaining fields follow.
#define PR_JS_CLASS(interface, classId, classFinalizer, protoList)                                 \
    .name = interface, .id = &classId, .finalizer = classFinalizer, .mark = jsMarker<&classId>,   \
    .proto = protoList, .protoCount = int(countOf(protoList))

// The constant fields of a JsClassDefinition, from the array `list`.
#define PR_JS_CONSTANTS(list) .constants = list, .constantCount = int(countOf(list))

// Defines the class that `definition` describes, and returns its constructor, which the caller frees. jsDerives and the
// handle lookups treat the class as derived from `base`, whichever modules define the two classes.
JSValue jsDefineClass(JSContext* ctx, const JsClassDefinition& definition);

// A FrozenArray<DOMString> attribute's getter.
template <const JSClassID* PR_NONNULL Id, auto Write>
JSValue jsFrozenStringsGetter(JSContext* ctx, JSValueConst jsObject) {
    using T = typename JsHandleOf<decltype(Write)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    return object != nullptr ? jsStringArray(ctx, object, Write, true) : JS_EXCEPTION;
}

// A method that returns a sequence<DOMString>.
template <const JSClassID* PR_NONNULL Id, auto Write>
JSValue jsStringsMethod(JSContext* ctx, JSValueConst jsObject, int, JSValueConst*) {
    using T = typename JsHandleOf<decltype(Write)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    return object != nullptr ? jsStringArray(ctx, object, Write, false) : JS_EXCEPTION;
}

// The item(index) method of class `*Id`, whose indexed properties are `*Indexed`. Returns null past the last item.
template <const JSClassID* PR_NONNULL Id, const JsIndexed* PR_NONNULL Indexed>
JSValue jsItemMethod(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    void* handle = jsThis<void>(ctx, object, *Id);
    uint32_t index = 0;
    if (handle == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    return index < Indexed->length(handle) ? Indexed->item(ctx, object, handle, index) : JS_NULL;
}

// A [PutForwards] attribute's setter (webidl/#PutForwards), which sets `Forward` on the attribute's object.
template <JsLiteral Attribute, JsLiteral Forward>
JSValue jsPutForwards(JSContext* ctx, JSValueConst targetObject, JSValueConst input) {
    JSValue target = JS_GetPropertyStr(ctx, targetObject, Attribute.text);
    if (JS_IsException(target))
        return target;
    int set = JS_SetPropertyStr(ctx, target, Forward.text, JS_DupValue(ctx, input));
    JS_FreeValue(ctx, target);
    return set < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMException

// Defines DOMException and the interfaces derived from it on the global object of `ctx`.
void jsWebidlBind(JSContext* ctx);

// Returns a new DOMException named `name`, for example to reject a promise.
JSValue jsDomExceptionNew(JSContext* ctx, PrStringView name);

// Throws a DOMException named `name`, and returns JS_EXCEPTION.
JSValue jsDomExceptionThrow(JSContext* ctx, PrStringView name);

// Throws a QuotaExceededError (webidl/#quotaexceedederror) whose quota and requested amount are `quota` and
// `requested`, each NaN for null, and returns JS_EXCEPTION.
JSValue jsQuotaExceededErrorThrow(JSContext* ctx, double quota, double requested);

} // namespace pr

PR_ASSUME_NONNULL_END
