// The QuickJS layer of core: native handles as JS objects, and what every binding of a module needs to make them.

#pragma once

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/count_of.h"
#include "pr/hash_index.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_ops.h"
#include "pr/string_view.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A callback that native code holds (js_callback.h), and the rarely used data of a wrapper (below).
struct JsCallback;
struct JsWrapperExtra;

// The data of an object of a bound class. Data that few objects need is in `extra`, which is created on first use.
struct JsWrapper {
    void* handle;
    void* object;                           // the wrapper's own JSObject, to which it holds no reference
    void* PR_NULLABLE owner;                // the JSObject it keeps alive, which is its group if it joined one
    JsWrapper* PR_NULLABLE firstMember;     // the members it holds as a group (jsGroupJoin)
    JsWrapper* PR_NULLABLE nextMember;      // the next and previous members of its group
    JsWrapper* PR_NULLABLE previousMember;
    JsWrapperExtra* PR_NULLABLE extra;
    uint32_t hash;                          // the hash of the handle and family, for the runtime's table
    JSClassID family;                       // the root of the hierarchy of its class
    bool ownsHandle;                        // the handle is destroyed or released with the object
    bool grouped;                           // `owner` is its group (jsGroupJoin)
    bool member;                            // it is one of the members that its group holds
    bool stateful;                          // it holds state that a new object for its handle would lack (jsGroupJoin)
};

// A value that an object holds by name (jsSameObject).
struct JsNamedValue {
    const char* name;
    JSValue value;
};

// The rarely used data of an object of a bound class.
struct JsWrapperExtra {
    RawArray<JsNamedValue> sameObjects;    // the values of its [SameObject] attributes

    // The objects that the native value holds, by slot (jsSetReference). Undefined for an unset slot.
    RawArray<JSValue> references;

    // The object itself while it is pinned (jsPin), which keeps it alive when script cannot reach it. Undefined
    // otherwise.
    JSValue pinnedObject;

    JsCallback* PR_NULLABLE callbacks;  // the callbacks registered through it, which it traces
    uint32_t pins;                      // how many times its handle is pinned
};

// A string read from a JS value, as UTF-8 with every unpaired surrogate replaced by U+FFFD.
struct JsString {
    const char* PR_NULLABLE text;
    char* PR_NULLABLE scalar;   // the replaced copy, if there was a surrogate
    size_t size;
};

// Converts `input` to a string. Returns false, with an exception pending, if it does not convert.
bool jsStringRead(JSContext* ctx, JSValueConst input, JsString* out);

// Frees the string and empties it.
void jsStringReset(JSContext* ctx, JsString* string);

// Returns the string's bytes, or the empty string if it was never read.
PrStringView jsStringView(const JsString* string);

// ---------------------------------------------------------------------------------------------------------------------
// A call's scope
//
// A native function registers what it holds for its call with the call's scope, such as the text of its string
// arguments. What it holds is released when the function that entered the scope returns. Every function that QuickJS
// calls runs in a scope (JsCallScoped, JsScoped). Nothing that a binding holds has a destructor.

// How a held entry is released.
enum {
    JsHeldKind_None,      // handed on, and released by its new holder
    JsHeldKind_CString,   // `cstring`, which JS_FreeCString releases
    JsHeldKind_Bytes,     // `bytes`, which prMemoryFree releases
    JsHeldKind_Value,     // `value`, which JS_FreeValue releases
    JsHeldKind_String,    // `string`, which prRawStringReset releases
    JsHeldKind_Custom,    // `custom.data`, which `custom.release` releases
};
typedef uint8_t JsHeldKind;

// An entry that a call's scope holds. Its kind decides which member of the union it uses.
struct JsHeld {
    JsHeldKind kind;
    union {
        const char* PR_NULLABLE cstring;
        void* PR_NULLABLE bytes;
        JSValue value;
        PrRawString string;
        struct {
            void (*PR_NONNULL release)(JSContext* ctx, void* PR_NULLABLE data);
            void* PR_NULLABLE data;
        } custom;
    };
};

// Enters and leaves a call's scope. Leaving releases what the call held, last held first.
void jsCallEnter(JSContext* ctx);
void jsCallLeave(JSContext* ctx);

// Holds `held` until the innermost call's scope ends, and returns its index.
uint32_t jsHold(JSContext* ctx, JsHeld held);

// Returns the entry held at `index`. A holder transfers ownership out by setting its kind to JsHeldKind_None.
JsHeld* jsHeldAt(JSContext* ctx, uint32_t index);

// Holds the allocation `bytes`, or `string`, until the call's scope ends.
uint32_t jsHoldBytes(JSContext* ctx, void* PR_NULLABLE bytes);
uint32_t jsHoldString(JSContext* ctx, PrRawString string);

// Holds `data` until the call's scope ends, when `release` releases it.
uint32_t jsHoldCustom(JSContext* ctx, void (*PR_NONNULL release)(JSContext* ctx, void* PR_NULLABLE data),
    void* PR_NULLABLE data);

// A native function that runs inside a call's scope. `call` is the function that QuickJS calls.
template <auto Function>
struct JsCallScoped;
template <typename Result, typename... Args, Result (*PR_NONNULL Function)(JSContext*, Args...)>
struct JsCallScoped<Function> {
    static Result call(JSContext* ctx, Args... args) {
        jsCallEnter(ctx);
        Result result = Function(ctx, args...);
        jsCallLeave(ctx);
        return result;
    }
};

// Converts `input` to a string, which the call's scope holds. Returns false, with an exception pending, if it does not
// convert.
bool jsReadString(JSContext* ctx, JSValueConst input, PrStringView* out);

// A string argument, whose text the call's scope holds.
struct StringArgument {
    JSContext* ctx;
    PrStringView fallback = "";   // the value of an argument that was never read
    PrStringView text = PrStringView{};
    bool given = false;

    bool read(JSValueConst input) {
        given = jsReadString(ctx, input, &text);
        return given;
    }
    // Reads `input`, unless it is null, which keeps the fallback ([LegacyNullToEmptyString] and nullable strings).
    bool readNullable(JSValueConst input) { return JS_IsNull(input) || read(input); }

    PrStringView value() const { return given ? text : fallback; }
};

// Returns `result` with the prototype from `target`, the constructor that `new` was called on. Takes ownership of
// `result`.
JSValue jsConstructed(JSContext* ctx, JSValueConst target, JSValue result);

// Returns the value of the @@iterator property of `input`.
JSValue jsIteratorMethod(JSContext* ctx, JSValueConst input);

// An iterator record: an iterator and its next method. `open` gets the iterator from `method`, and `step` reads its
// next value. Each returns false, with an exception pending, if it fails. `reset` releases the record.
struct JsIterator {
    JSContext* ctx;
    JSValue iterator = JS_UNDEFINED;
    JSValue next = JS_UNDEFINED;

    void reset();
    bool open(JSValueConst input, JSValueConst method);
    bool step(JSValue* value, bool* done);

    // Calls the iterator's return method, keeping the pending exception.
    void closeAfterException();
};

// Calls `visit` with each value that `method` produces when iterating `input`. `visit` takes ownership of the value,
// and returns false, with an exception pending, to stop and close the iterator. Returns false if an exception is
// pending.
template <typename Visit>
bool jsIterate(JSContext* ctx, JSValueConst input, JSValueConst method, Visit visit) {
    JsIterator iterator = {ctx};
    bool ok = iterator.open(input, method);
    while (ok) {
        JSValue item = JS_UNDEFINED;
        bool done = false;
        ok = iterator.step(&item, &done);
        if (!ok || done)
            break;
        ok = visit(item);
        if (!ok)
            iterator.closeAfterException();
    }
    iterator.reset();
    return ok;
}

// Creates a runtime whose opaque pointer holds the bindings' wrappers. A handle has at most one live object in each
// class. Handles of different classes can share an address. For example, a list at the start of an object shares the
// object's address.
JSRuntime* jsRuntimeCreate();

// Frees the runtime, then its wrappers. First the values of callbacks still held are dropped, and the callbacks are
// detached from the runtime, so native code can release them afterward. Finalizing a JS object releases its native
// object, so the runtime must be destroyed before the native objects that its scripts reach.
void jsRuntimeDestroy(JSRuntime* PR_NULLABLE runtime);

// Lists a callback with `owner`, if `owner` is an object of a bound class, or else with the runtime. The owner's mark
// traces the callback's value, and its finalizer detaches the callback. A callback listed with the runtime has a value
// that is a root. Unlinking removes the callback from whichever list holds it.
void jsCallbackLink(JSRuntime* runtime, JsCallback* callback, JSValueConst owner);
void jsCallbackUnlink(JsCallback* callback);

// Enters and leaves the realm of a script or callback that the host runs. A promise job that the engine runs does not
// enter a realm.
void jsEnterRealm(JSContext* ctx);
void jsLeaveRealm(JSContext* ctx);

// Returns the incumbent realm of a native function, which is the realm of the innermost script function on the stack. A
// function called from another realm runs in its own realm, so `ctx` can differ from it. If no script function is on
// the stack, returns the innermost realm entered, or else `ctx`.
JSContext* jsIncumbentRealm(JSContext* ctx);

// Queues `run` with `data` as a microtask, in the queue of the engine's promise jobs.
void jsEnqueueNative(JSContext* ctx, void (*run)(void* data), void* data);

// Returns the object of class `id` for `handle`, which keeps `owner` alive. Returns null for a null handle. If the
// handle already has an object of class `id`, returns that object, whatever `owner` is. If `ownsHandle` is false, the
// object takes a reference through `retain`, or through the class's JsReferences if `retain` is null.
JSValue jsWrap(JSContext* ctx, JSClassID id, JSValueConst owner, void* PR_NULLABLE handle,
    bool ownsHandle, void (*PR_NULLABLE retain)(void*) = nullptr);

// Reads or stores the value of the [SameObject] attribute `name` of `object`, an object of a bound class. The value is
// undefined until stored. `object` keeps the value alive, and storing one makes `object` stateful.
JSValue jsSameObject(JSContext* ctx, JSValueConst object, const char* name);
void jsSetSameObject(JSContext* ctx, JSValueConst object, const char* name, JSValueConst value);

// Returns the value of the [SameObject] attribute `name` of `object`. If no value is stored, stores and returns the
// value that `make` returns. A result that is not an object is not stored.
template <typename Make>
JSValue jsSameObject(JSContext* ctx, JSValueConst object, const char* name, Make make) {
    JSValue held = jsSameObject(ctx, object, name);
    if (!JS_IsUndefined(held))
        return held;
    JSValue value = make();
    if (JS_IsObject(value))
        jsSetSameObject(ctx, object, name, value);
    return value;
}

// Holds `value` in the array that `owner` stores as its [SameObject] value `name`, or releases it if `hold` is false. A
// held value lives at least as long as the owner. Native code that names an object without holding it keeps the object
// alive this way. A value is held once, however many times it is held. Returns false, with an exception pending, if it
// fails.
bool jsHold(JSContext* ctx, JSValueConst owner, const char* name, JSValueConst value, bool hold);

// Stores `value` in the wrapper's reference `index`, which the collector traces with the wrapper.
void jsSetReference(JSContext* ctx, JSValueConst object, uint32_t index, JSValueConst value);

// Returns the value in the wrapper's reference `index`, or undefined if it was never set.
JSValue jsReference(JSContext* ctx, JSValueConst object, uint32_t index);

// Returns `text` as a JS value, or null for the null string.
JSValue jsNewString(JSContext* ctx, const PrRawString* text);

// Returns `text` as a JS value, or null for the null string. Frees `text`.
JSValue jsTakeString(JSContext* ctx, PrRawString text);

// Returns the runtime's single JS string for `text`. The string lives as long as the process, and is meant for text
// such as a literal or the name of an enumeration member. Every call for the same text returns the same string.
JSValue jsFixedString(JSContext* ctx, PrStringView text);

// Returns `text` as a JS value, or null for the null view.
JSValue jsNewStringView(JSContext* ctx, PrStringView text);

// The text of a string that a C function returns or writes, which is an owned string or a view.
inline PrStringView jsStringText(const PrRawString& text) {
    return prRawStringView(&text);
}
inline PrStringView jsStringText(PrStringView text) {
    return text;
}

// Frees a string that a C function returns or writes, if it is owned.
inline void jsStringRelease(PrRawString* text) {
    prRawStringReset(text);
}
inline void jsStringRelease(PrStringView*) {}

// Returns as a JS value a string that a C function returns, an owned string or a view, or null for the null string.
// Frees an owned string.
template <typename S>
JSValue jsResultString(JSContext* ctx, S text) {
    JSValue value = jsNewStringView(ctx, jsStringText(text));
    jsStringRelease(&text);
    return value;
}

// Appends to `out` the atom of each string that `names` writes for the handle (jsStringArray). Returns false if an atom
// cannot be made.
template <typename T, typename S>
bool jsNameAtoms(JSContext* ctx, const T* PR_NONNULL handle,
        uint32_t (*PR_NONNULL names)(const T* PR_NONNULL, S* PR_NULLABLE, uint32_t), RawArray<JSAtom>* out) {
    RawArray<S> strings;
    strings.reserve(names(handle, nullptr, 0));
    strings.count = names(handle, strings.data, strings.capacity);
    bool ok = true;
    for (uint32_t i = 0; i < strings.count; ++i) {
        PrStringView name = jsStringText(strings.data[i]);
        JSAtom atom = ok ? JS_NewAtomLen(ctx, name.data, name.size) : JS_ATOM_NULL;
        ok = ok && atom != JS_ATOM_NULL;
        if (ok)
            out->push(atom);
        jsStringRelease(&strings.data[i]);
    }
    strings.reset();
    return ok;
}

// Returns true if `value` is the global object, or undefined or null. A call with no receiver has undefined or null.
bool jsIsGlobal(JSContext* ctx, JSValueConst value);

// Releases the reference that an object of class `id` holds to `handle`, if the class counts references (JsReferences).
void jsReleaseHandle(JSRuntime* runtime, JSClassID id, void* handle);

// Removes an object that is being finalized from the runtime's objects, and detaches the callbacks registered through
// it.
void jsWrapperForget(JSRuntime* runtime, JsWrapper* wrapper);

// Releases the JS values that a finalized object holds, and frees the wrapper. Objects freed as a cycle are finalized
// in no particular order. A member finalized before its group leaves the group. A group finalized first releases its
// members.
void jsWrapperDestroy(JSRuntime* runtime, JsWrapper* wrapper);

// Finalizes an object of class `id`. Detaches the callbacks registered through the object, and passes an owned handle
// to `cleanup`, or to the class's release if `cleanup` is null. Then releases the wrapper's JS values.
template <typename Handle>
void jsFinalize(JSRuntime* runtime, JSValue object, JSClassID id, void (*PR_NULLABLE cleanup)(Handle* PR_NONNULL)) {
    JsWrapper* wrapper = static_cast<JsWrapper*>(JS_GetOpaque(object, id));
    if (wrapper == nullptr)
        return;

    jsWrapperForget(runtime, wrapper);
    if (wrapper->ownsHandle) {
        if (cleanup != nullptr)
            cleanup(static_cast<Handle*>(wrapper->handle));
        else
            jsReleaseHandle(runtime, id, wrapper->handle);
    }
    jsWrapperDestroy(runtime, wrapper);
}

// The mark function of class `id`. Traces every JS value that the object holds. The collector counts each value that a
// mark reports as one reference.
void jsMark(JSRuntime* runtime, JSValueConst object, JSClassID id, JS_MarkFunc* mark);

// If `name` is an array index, writes it to `*out` and returns 1. Returns 0 for any other property name, and -1 for an
// exception.
int jsIndexOf(JSContext* ctx, JSAtom name, uint32_t* out);

// Returns true if QuickJS tags an index atom the way jsIndexOf reads it.
bool jsIndexAtomsTagged(JSContext* ctx);

// Returns a promise settled with `value`: rejected with it if `rejected`, and otherwise resolved.
JSValue jsSettledPromise(JSContext* ctx, JSValueConst value, bool rejected);

// The constructor for a class that has no constructor. It always throws.
JSValue jsIllegalConstructor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* PR_NULLABLE argv);

// Returns a new Float32Array or Float64Array of `count` values.
JSValue jsNewFloatArray(JSContext* ctx, JSTypedArrayEnum type, const double* values, uint32_t count);

// Writes up to `capacity` elements of the typed array `value` to `out`, and its element count to `*count`. `type` is
// JS_TYPED_ARRAY_FLOAT32 or FLOAT64, and a value that is not a typed array of `type` throws a TypeError.
bool jsReadFloatArray(JSContext* ctx, JSValueConst value, JSTypedArrayEnum type, double* out,
    uint32_t capacity, uint32_t* count);

// ---------------------------------------------------------------------------------------------------------------------
// Classes

// How the objects of a class hold handles that are reference counted. An object created for a handle that it does not
// own takes a reference, and releases it when it is finalized. A derived class inherits these. The handles of a
// `pinnable` class can be pinned (jsPin).
struct JsReferences {
    void (*PR_NONNULL retain)(void* handle);
    void (*PR_NONNULL release)(void* handle);
    bool pinnable;
};

// Called with each object that a class creates for a handle, when the object is created. A class without one uses the
// callback of its nearest base class.
typedef void JsWrapped(JSContext* ctx, JSValueConst object, void* handle);

// Enters and leaves the runtime's member scope (JsMemberScope) around a member's call.
void jsMemberEnter(JSContext* ctx);
void jsMemberLeave(JSContext* ctx);

// A method or setter that runs inside the runtime's member scope. `call` is the function that QuickJS calls.
template <auto Member>
struct JsScoped;
template <typename... Args, JSValue (*PR_NONNULL Member)(JSContext*, Args...)>
struct JsScoped<Member> {
    static JSValue call(JSContext* ctx, Args... args) {
        jsMemberEnter(ctx);
        JSValue result = Member(ctx, args...);
        jsMemberLeave(ctx);
        return result;
    }
};

// The scope that each method and setter of the runtime's classes runs inside (JsScoped). The scope is entered when the
// member is called and left when it returns.
struct JsMemberScope {
    void (*PR_NONNULL enter)(void* context);
    void (*PR_NONNULL leave)(void* context);
};

// Sets the runtime's member scope, which `enter` and `leave` receive with `context`. Null sets none.
void jsSetMemberScope(JSRuntime* runtime, const JsMemberScope* PR_NULLABLE scope, void* PR_NULLABLE context);

// Registers the class that `definition` describes in the runtime under the ID `*id`, unless the runtime has it already.
// If `*id` is zero, the process assigns the class its next ID first. A class has the same ID in every runtime of the
// process, although QuickJS numbers classes per runtime.
void jsRegisterClass(JSRuntime* runtime, JSClassID* id, const JSClassDef& definition);

// A class whose objects hold native handles (jsWrap), as jsDeclareClass declares it. A class inherits the references,
// hooks and slot of its base unless it gives its own.
struct JsClass {
    const char* name;
    JSClassID* id;
    JSClassFinalizer* finalizer;
    JSClassGCMark* mark;

    // The class that it derives from, which the runtime declares first.
    const JSClassID* PR_NULLABLE base;

    const JsReferences* PR_NULLABLE references;

    // Called with each object that the class creates for a handle, before the object holds the handle, so that what it
    // defines on the object is not state that the object would lose if it were recreated.
    JsWrapped* PR_NULLABLE prepared;

    // Called with each object that the class creates for a handle, once the object holds it.
    JsWrapped* PR_NULLABLE wrapped;

    // The slot in which a handle keeps its object, for the root of a hierarchy whose handles have such a slot. The
    // objects of other hierarchies are found in a table.
    void* PR_NULLABLE* PR_NONNULL (*PR_NULLABLE slot)(void* handle);

    // The class's exotic behavior, for objects whose properties are not all their own.
    JSClassExoticMethods* PR_NULLABLE exotic;

    // What the layer that declares the class keeps for it in the runtime (jsClassData), which that layer owns.
    void* PR_NULLABLE data;
};

// Registers the class (jsRegisterClass) and records it in the runtime's hierarchy. jsDerives and the handle lookups
// treat the class as derived from its base, whichever modules declare the two. Declaring a class again, as each realm
// of the runtime does, records it again.
void jsDeclareClass(JSRuntime* runtime, const JsClass& definition);

// Returns the data that the class was declared with, or null.
void* PR_NULLABLE jsClassData(JSRuntime* runtime, JSClassID id);

// Returns true if class `id` is `base` or derives from it.
bool jsDerives(JSRuntime* runtime, JSClassID id, JSClassID base);

// Returns the handle of `value` if its class is `base` or derives from it, and otherwise null.
void* PR_NULLABLE jsHandleIn(JSContext* ctx, JSValueConst value, JSClassID base);

// The maximum number of classes that the process defines. The process numbers them densely, starting after QuickJS's
// own classes.
constexpr uint32_t kJsMaxClasses = 1024;

// A set of classes, one bit each, that several threads may add to at once. The classes belong to the process, and
// runtimes on different threads add the same ones.
struct JsClassSet {
    Atomic<uint64_t> bits[kJsMaxClasses / 64];
};

// Adds class `id` to the set, and tests whether the set has it.
inline void jsClassSetAdd(JsClassSet* set, JSClassID id) {
    Atomic<uint64_t>* word = &set->bits[id / 64];
    uint64_t bit = uint64_t(1) << (id % 64);
    uint64_t seen = word->loadRelaxed();
    while ((seen & bit) == 0 && !word->casRelaxed(seen, seen | bit)) {
    }
}
inline bool jsClassSetHas(const JsClassSet* set, JSClassID id) {
    return id < kJsMaxClasses && (set->bits[id / 64].loadRelaxed() & (uint64_t(1) << (id % 64))) != 0;
}

// Returns the handle of `value` if its class is in `set`, which holds classes that jsDeclareClass declared. Otherwise
// returns null.
void* PR_NULLABLE jsHandleInSet(JSValueConst value, const JsClassSet* set);

// Returns the handle of `value` as a `Handle` if its class is `base` or derives from it, and otherwise null.
template <typename Handle>
Handle* PR_NULLABLE jsHandleAs(JSContext* ctx, JSValueConst value, JSClassID base) {
    return static_cast<Handle*>(jsHandleIn(ctx, value, base));
}

// Returns the handle of the global object, if its class is `base` or derives from it.
void* PR_NULLABLE jsGlobalHandleIn(JSContext* ctx, JSClassID base);

// Returns the handle that `object` holds as a `base`. A call with no receiver, or with the global object as its
// receiver, uses the global's handle. Otherwise returns null and throws a TypeError.
template <typename Handle>
Handle* PR_NULLABLE jsThis(JSContext* ctx, JSValueConst object, JSClassID base) {
    void* handle = jsHandleIn(ctx, object, base);
    if (handle == nullptr && jsIsGlobal(ctx, object))
        handle = jsGlobalHandleIn(ctx, base);
    if (handle == nullptr)
        JS_ThrowTypeError(ctx, "Illegal invocation");
    return static_cast<Handle*>(handle);
}

// The class that describes `handle` more specifically than the class that the classifier is added to, or that class
// itself.
typedef JSClassID JsClassify(void* handle);

// Adds a classifier to class `id`. A module that defines classes derived from `id` adds one, once per runtime, however
// many contexts bind it. The first classifier that returns another class decides.
void jsAddClassifier(JSRuntime* runtime, JSClassID id, JsClassify* classify);

// Replaces the object that `object` keeps alive as its owner, removing `object` from any group it is in.
void jsSetOwner(JSContext* ctx, JSValueConst object, JSValueConst owner);

// Makes `member` an object of `group`, removing it from any group it is in.
//
// The stateful objects of the handles in one tree live as long as script can reach any object of the tree. An object is
// stateless if it holds nothing that a new object for its handle would lack: it has no own property beyond the class's,
// it has the class's prototype, it is extensible, nothing refers to it weakly, and the binding holds nothing for it. A
// stateless object may be freed and recreated without any script noticing. An object of a group keeps its group alive.
// A group holds those of its objects that are stateful or that hold members of their own. Every object therefore keeps
// its tree's root alive, and the root keeps every stateful object alive.
void jsGroupJoin(JSContext* ctx, JSValueConst group, JSValueConst member);

// Makes the group of `member` hold `member`, in the same way that it holds a stateful object.
void jsGroupKeep(JSContext* ctx, JSValueConst member);

// Pins the handle's object of class `base` if `pinned` is true, and otherwise releases one pin. A pin says that native
// code will pass the handle to script again, and whatever script stored on the object lasts until then. A handle pinned
// before it has an object gives the object its pins when the object is created. An object whose last pin is released
// while a member runs lives until the outermost member call returns, since that call may return it.
void jsPin(JSRuntime* runtime, JSClassID base, void* handle, bool pinned);

// Returns the object of `handle` as the class `base` classifies it, or undefined if it has none.
JSValue jsWrapperOf(JSContext* ctx, JSClassID base, void* PR_NULLABLE handle);

// Returns the object for `handle` in the class that the classifiers of `id` and its derived classes find most specific,
// or in `id` if they find none. Returns null for a null handle.
JSValue jsWrapAs(JSContext* ctx, JSClassID id, JSValueConst owner, void* PR_NULLABLE handle, bool ownsHandle);

// Stores or reads a value that a module keeps for a runtime, keyed by an address that the module owns. Reading a key
// that was never stored gives null.
void jsSetRuntimeData(JSRuntime* runtime, const void* key, const void* PR_NULLABLE value);
const void* PR_NULLABLE jsRuntimeData(JSRuntime* runtime, const void* key);

// A value that a module keeps for a runtime and that the runtime owns, keyed by an address that the module owns. `make`
// creates it on first request, and `destroy` frees it with the runtime.
void* jsRuntimeOwned(JSRuntime* runtime, const void* key, void* PR_NONNULL (* PR_NONNULL make)(),
    void (* PR_NONNULL destroy)(void* PR_NONNULL));

// ---------------------------------------------------------------------------------------------------------------------
// Value classes, whose objects each hold their own copy of a C value

// Returns a new object of class `id` that holds a copy of `value`.
template <typename T>
JSValue jsNewValue(JSContext* ctx, JSClassID id, const T& value) {
    T* held = alloc<T>();
    *held = value;
    JSValue object = jsWrap(ctx, id, JS_NULL, held, true);
    if (JS_IsException(object))
        prMemoryFree(held);
    return object;
}

// Returns the value held by an object of class `base`, or of a class derived from it. Otherwise returns null, and
// throws a TypeError naming `expected`.
template <typename T>
T* PR_NULLABLE jsValueOf(JSContext* ctx, JSValueConst object, JSClassID base, const char* expected) {
    T* held = static_cast<T*>(jsHandleIn(ctx, object, base));
    if (held == nullptr)
        JS_ThrowTypeError(ctx, "Expected %s", expected);
    return held;
}

// The finalizer and the mark function of a value class.
void jsValueFinalizer(JSRuntime* runtime, JSValue object);
void jsValueMark(JSRuntime* runtime, JSValueConst object, JS_MarkFunc* mark);

// ---------------------------------------------------------------------------------------------------------------------
// Members
//
// The finalizer, mark function, getters and setters for common shapes. Each is for class `*Id`, and calls a C function
// that takes the object's handle.

// The handle type that a C function takes first, whether it takes it const or not.
template <typename F>
struct JsHandleOf;
template <typename R, typename T, typename... A>
struct JsHandleOf<R (*PR_NONNULL)(T* PR_NONNULL, A...)> {
    using Type = T;
};
template <typename R, typename T, typename... A>
struct JsHandleOf<R (*PR_NONNULL)(const T* PR_NONNULL, A...)> {
    using Type = T;
};

// The finalizer, which passes a handle that the object owns to `Cleanup`.
template <const JSClassID* PR_NONNULL Id, auto Cleanup>
void jsFinalizer(JSRuntime* runtime, JSValue object) {
    using T = typename JsHandleOf<decltype(Cleanup)>::Type;
    jsFinalize<T>(runtime, object, *Id, [](T* handle) { Cleanup(handle); });
}

// The finalizer of a class whose handles the object never owns.
template <const JSClassID* PR_NONNULL Id>
void jsViewFinalizer(JSRuntime* runtime, JSValue object) {
    jsFinalize<void>(runtime, object, *Id, nullptr);
}

// The mark function of class `*Id`.
template <const JSClassID* PR_NONNULL Id>
void jsMarker(JSRuntime* runtime, JSValueConst object, JS_MarkFunc* mark) {
    jsMark(runtime, object, *Id, mark);
}

// The getter of a DOMString attribute. `Get` returns an owned string, which the getter frees, or a view.
template <const JSClassID* PR_NONNULL Id, auto Get>
JSValue jsStringGetter(JSContext* ctx, JSValueConst jsObject) {
    using T = typename JsHandleOf<decltype(Get)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    if (object == nullptr)
        return JS_EXCEPTION;
    return jsResultString(ctx, Get(object));
}

// The setter of a DOMString attribute. If `Nullable` is true, null reads as the empty string, as for a nullable or
// [LegacyNullToEmptyString] attribute.
template <const JSClassID* PR_NONNULL Id, auto Set, bool Nullable = false>
JSValue jsStringSetter(JSContext* ctx, JSValueConst jsObject, JSValueConst input) {
    using T = typename JsHandleOf<decltype(Set)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    StringArgument value = {ctx};
    if (object == nullptr || !(Nullable ? value.readNullable(input) : value.read(input)))
        return JS_EXCEPTION;
    Set(object, value.value());
    return JS_UNDEFINED;
}

// A boolean attribute's getter.
template <const JSClassID* PR_NONNULL Id, auto Get>
JSValue jsBoolGetter(JSContext* ctx, JSValueConst jsObject) {
    using T = typename JsHandleOf<decltype(Get)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    return object != nullptr ? JS_NewBool(ctx, Get(object)) : JS_EXCEPTION;
}

// A numeric attribute's getter. The value converts to a double.
template <const JSClassID* PR_NONNULL Id, auto Get>
JSValue jsNumberGetter(JSContext* ctx, JSValueConst jsObject) {
    using T = typename JsHandleOf<decltype(Get)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    return object != nullptr ? JS_NewFloat64(ctx, double(Get(object))) : JS_EXCEPTION;
}

// The setter of an attribute whose value converts by ToUint32.
template <const JSClassID* PR_NONNULL Id, auto Set>
JSValue jsUint32Setter(JSContext* ctx, JSValueConst jsObject, JSValueConst input) {
    using T = typename JsHandleOf<decltype(Set)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    uint32_t value = 0;
    if (object == nullptr || JS_ToUint32(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    Set(object, value);
    return JS_UNDEFINED;
}

// Returns the strings that `write` writes for the handle, as an array. `write` is a C function that writes up to
// `capacity` strings and returns the total count. If `frozen` is true, the array is frozen, as a FrozenArray requires.
template <typename T, typename S>
JSValue jsStringArray(JSContext* ctx, const T* PR_NONNULL handle,
        uint32_t (*PR_NONNULL write)(const T* PR_NONNULL, S* PR_NULLABLE, uint32_t), bool frozen) {
    RawArray<S> items;
    items.reserve(write(handle, nullptr, 0));
    items.count = write(handle, items.data, items.capacity);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < items.count; ++i)
        JS_SetPropertyUint32(ctx, array, i, jsResultString(ctx, items.data[i]));
    items.reset();
    if (frozen)
        JS_FreezeObject(ctx, array);
    return array;
}

// A string literal as a template argument.
template <size_t N>
struct JsLiteral {
    char text[N];

    constexpr JsLiteral(const char (&literal)[N]) {
        for (size_t i = 0; i < N; ++i)
            text[i] = literal[i];
    }
};

// The getter of the [SameObject] attribute `Name`, which returns an object of class `*As` or of a class derived from
// it. The object keeps the receiver alive.
template <const JSClassID* PR_NONNULL Id, auto Get, const JSClassID* PR_NONNULL As, JsLiteral Name>
JSValue jsSameObjectGetter(JSContext* ctx, JSValueConst jsObject) {
    using T = typename JsHandleOf<decltype(Get)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    if (object == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, jsObject, Name.text, [&] { return jsWrapAs(ctx, *As, jsObject, Get(object), false); });
}

// An attribute's getter, which returns an object of class `*As` or of a class derived from it. The object keeps the
// receiver alive.
template <const JSClassID* PR_NONNULL Id, auto Get, const JSClassID* PR_NONNULL As>
JSValue jsObjectGetter(JSContext* ctx, JSValueConst jsObject) {
    using T = typename JsHandleOf<decltype(Get)>::Type;
    T* object = jsThis<T>(ctx, jsObject, *Id);
    return object != nullptr ? jsWrapAs(ctx, *As, jsObject, Get(object), false) : JS_EXCEPTION;
}

} // namespace pr

PR_ASSUME_NONNULL_END
