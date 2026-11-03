#include "pr/quickjs_binding.h"

#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/quickjs_callback.h"

namespace pr {
namespace {

// The stack size available to a runtime's calls before one throws a RangeError: QuickJS's default, or four times it
// under AddressSanitizer, which grows every frame.
constexpr size_t kJsStackSize = (PR_ADDRESS_SANITIZER ? 4 : 1) * JS_DEFAULT_STACK_SIZE;

// A runtime allocates through the library's allocator (pr/alloc.h).
constexpr JSMallocFunctions kJsAllocator = {
    [](void*, size_t count, size_t size) { return prMemoryAllocZeroed(count * size); },
    [](void*, size_t size) { return prMemoryAlloc(size); },
    [](void*, void* memory) { prMemoryFree(memory); },
    [](void*, void* memory, size_t size) { return prMemoryRealloc(memory, size); },
    [](const void* memory) { return prMemoryUsableSize(memory); },
};

// QuickJS interns a canonical numeric string up to 2^31 - 1 as a tagged integer: the atom is the number with this bit
// set. quickjs.h does not document this. jsIndexAtomsTagged checks it.
constexpr JSAtom kAtomTaggedInt = 1u << 31;

// Returns true if the three bytes at `at` are the CESU-8 form of a surrogate. QuickJS writes an unpaired surrogate in
// that form.
bool isSurrogate(const char* text, size_t at) {
    return uint8_t(text[at]) == 0xed && (uint8_t(text[at + 1]) & 0xe0) == 0xa0;
}

// Returns a copy of `text` with every unpaired surrogate replaced by U+FFFD, or null if it has none.
char* PR_NULLABLE scalarize(const char* text, size_t size) {
    size_t at = 0;
    while (at + 2 < size && !isSurrogate(text, at))
        ++at;
    if (at + 2 >= size)
        return nullptr;

    char* copy = static_cast<char*>(prMemoryAlloc(size));
    prMemCopy(copy, text, size);
    for (size_t i = at; i + 2 < size; ++i) {
        if (isSurrogate(copy, i)) {
            copy[i] = char(0xef);
            copy[i + 1] = char(0xbf);
            copy[i + 2] = char(0xbd);
            i += 2;
        }
    }
    return copy;
}

// The runtime's objects, by handle and family. A handle has at most one object among the classes that derive from one
// root. The object is found without determining which class describes it. The table uses open addressing over the
// objects themselves, with no tombstones: a removal moves back the entries that follow it.
struct WrapperTable {
    JsWrapper* PR_NULLABLE* PR_NULLABLE slots;
    uint32_t mask;
    uint32_t count;

    JsWrapper* PR_NULLABLE find(const void* handle, JSClassID family, uint32_t hash) const {
        if (slots == nullptr)
            return nullptr;
        for (uint32_t slot = hash & mask;; slot = (slot + 1) & mask) {
            JsWrapper* at = slots[slot];
            if (at == nullptr || (at->hash == hash && at->handle == handle && at->family == family))
                return at;
        }
    }

    void place(JsWrapper* wrapper) {
        uint32_t slot = wrapper->hash & mask;
        while (slots[slot] != nullptr)
            slot = (slot + 1) & mask;
        slots[slot] = wrapper;
    }

    // Kept at most half full.
    void insert(JsWrapper* wrapper) {
        if (slots == nullptr || (count + 1) * 2 > mask + 1) {
            JsWrapper* PR_NULLABLE* old = slots;
            uint32_t size = slots == nullptr ? 64 : (mask + 1) * 2;
            slots = static_cast<JsWrapper* PR_NULLABLE*>(prMemoryAllocZeroed(size * sizeof(JsWrapper*)));
            uint32_t oldSize = old != nullptr ? mask + 1 : 0;
            mask = size - 1;
            for (uint32_t i = 0; i < oldSize; ++i) {
                if (old[i] != nullptr)
                    place(old[i]);
            }
            prMemoryFree(old);
        }
        place(wrapper);
        ++count;
    }

    void erase(const JsWrapper* wrapper) {
        uint32_t slot = wrapper->hash & mask;
        while (slots[slot] != wrapper)
            slot = (slot + 1) & mask;
        // Each later entry that could no longer be found once the hole is empty moves into the hole.
        for (uint32_t next = (slot + 1) & mask; slots[next] != nullptr; next = (next + 1) & mask) {
            uint32_t home = slots[next]->hash & mask;
            if (((next - home) & mask) >= ((next - slot) & mask)) {
                slots[slot] = slots[next];
                slot = next;
            }
        }
        slots[slot] = nullptr;
        --count;
    }

    void reset() {
        prMemoryFree(slots);
        *this = {};
    }
};

// The maximum number of classifiers that jsAddClassifier adds to one class.
constexpr uint32_t kMaxClassifiers = 4;

// The maximum depth of a class hierarchy, counting its root.
constexpr uint32_t kClassMaxDepth = 8;

// Runtime data for a class, beyond what QuickJS keeps: its base, its place in its hierarchy, and what jsDeclareClass
// and jsAddClassifier gave it.
struct JsClassRecord {
    JSClassID base;
    JSClassID family;                         // the root of its hierarchy, or 0 before it is defined
    uint32_t depth;                           // how many classes it derives from
    JSClassID ancestors[kClassMaxDepth];      // the classes it derives from, by depth, and itself
    JsClassify* PR_NULLABLE classifiers[kMaxClassifiers];
    uint32_t classifierCount;
    const JsReferences* PR_NULLABLE references;                      // the class's own, or else its nearest base's
    void* PR_NULLABLE* PR_NONNULL (*PR_NULLABLE slot)(void* handle); // where a handle keeps its family's object
    JsWrapped* PR_NULLABLE prepared;                                 // the class's own, or else its nearest base's
    JsWrapped* PR_NULLABLE wrapped;                                  // the class's own, or else its nearest base's
    void* PR_NULLABLE data;                                          // what the declaring layer keeps for the class
    bool defined;                                                    // jsDeclareClass declared it, and its objects are
                                                                     // wrappers
};

struct JsRuntimeEntry {
    const void* key;
    const void* PR_NULLABLE value;
    void (* PR_NULLABLE destroy)(void* PR_NONNULL);   // frees an owned value when the runtime is freed
};

// The runtime's strings for texts that live as long as the process, keyed by address. A result from a fixed set is made
// once and shared by every read of it.
struct FixedStrings {
    struct Entry {
        const char* PR_NULLABLE data;
        uint32_t size;
        JSAtom atom;
    };

    Entry* PR_NULLABLE slots;
    uint32_t mask;
    uint32_t count;

    Entry* slotOf(const char* data, uint32_t size) const {
        for (uint32_t slot = prHashPointer(data, size) & mask;; slot = (slot + 1) & mask) {
            Entry* at = &slots[slot];
            if (at->data == nullptr || (at->data == data && at->size == size))
                return at;
        }
    }

    // Kept at most half full.
    Entry* find(const char* data, uint32_t size) {
        if (slots == nullptr || (count + 1) * 2 > mask + 1) {
            Entry* old = slots;
            uint32_t oldSize = old != nullptr ? mask + 1 : 0;
            uint32_t newSize = old != nullptr ? oldSize * 2 : 64;
            slots = static_cast<Entry*>(prMemoryAllocZeroed(newSize * sizeof(Entry)));
            mask = newSize - 1;
            for (uint32_t i = 0; i < oldSize; ++i) {
                if (old[i].data != nullptr)
                    *slotOf(old[i].data, old[i].size) = old[i];
            }
            prMemoryFree(old);
        }
        return slotOf(data, size);
    }

    void reset(JSRuntime* runtime) {
        for (uint32_t i = 0; slots != nullptr && i <= mask; ++i) {
            if (slots[i].data != nullptr)
                JS_FreeAtomRT(runtime, slots[i].atom);
        }
        prMemoryFree(slots);
        *this = {};
    }
};

struct JsWrappers {
    WrapperTable table;
    FixedStrings fixed;
    Array<JsClassRecord> classes;     // for each class id
    Array<JsRuntimeEntry> data;
    JsCallback* PR_NULLABLE callbacks;    // every callback alive
    Array<JSContext* PR_NONNULL> realms;  // entered, innermost last
    const JsMemberScope* PR_NULLABLE scope;
    void* PR_NULLABLE scopeContext;
    Array<void*> pending;                 // handles pinned with no object, once for each pin

    // The objects whose last pin was removed during a member call. Each holds itself until the outermost call returns.
    Array<JSValue> unpinned;

    uint32_t members;                     // the number of member calls running
    Array<JsHeld> held;                   // the values that running calls hold, innermost last
    Array<uint32_t> calls;                // the start of each call's entries in `held`, innermost last
    bool destroying;                      // the runtime is being freed, and pins keep no objects alive
};

JsWrappers* wrappersOf(JSRuntime* runtime) {
    JsWrappers* wrappers = static_cast<JsWrappers*>(JS_GetRuntimeOpaque(runtime));
    PR_ASSERT(wrappers != nullptr);
    return wrappers;
}

// Returns the root of the hierarchy of class `id`, or the class itself if it was not defined here.
JSClassID familyOf(const JsWrappers* wrappers, JSClassID id) {
    return id < wrappers->classes.count && wrappers->classes.data[id].family != 0 ? wrappers->classes.data[id].family : id;
}

// Returns the class that most specifically describes a handle of class `id`, according to its classifiers.
JSClassID classify(const JsWrappers* wrappers, JSClassID id, void* handle) {
    for (bool narrowed = true; narrowed && id < wrappers->classes.count;) {
        const JsClassRecord& record = wrappers->classes.data[id];
        narrowed = false;
        for (uint32_t i = 0; i < record.classifierCount && !narrowed; ++i) {
            JSClassID next = record.classifiers[i](handle);
            narrowed = next != id;
            id = next;
        }
    }
    return id;
}

// Returns the entry that the runtime keeps for `key`, or null if there is none.
JsRuntimeEntry* PR_NULLABLE runtimeEntry(JsWrappers* wrappers, const void* key) {
    for (uint32_t i = 0; i < wrappers->data.count; ++i) {
        if (wrappers->data.data[i].key == key)
            return &wrappers->data.data[i];
    }
    return nullptr;
}

JsClassRecord* recordOf(JsWrappers* wrappers, JSClassID id) {
    while (wrappers->classes.count <= id)
        wrappers->classes.push(JsClassRecord{});
    return &wrappers->classes.data[id];
}

// Returns the references of class `id`, or of the nearest class it derives from that has them.
const JsReferences* PR_NULLABLE referencesOf(const JsWrappers* wrappers, JSClassID id) {
    return id < wrappers->classes.count ? wrappers->classes.data[id].references : nullptr;
}

// Returns the slot in which `handle` keeps its object in `family`, or null for a family whose objects are in the table.
void* PR_NULLABLE* PR_NULLABLE slotOf(const JsWrappers* wrappers, JSClassID family, const void* handle) {
    if (family >= wrappers->classes.count || wrappers->classes.data[family].slot == nullptr)
        return nullptr;
    return wrappers->classes.data[family].slot(const_cast<void*>(handle));
}

// Returns the object of `handle` in the family of class `id`, or null if it has none.
JsWrapper* PR_NULLABLE findWrapperOf(const JsWrappers* wrappers, const void* handle, JSClassID id) {
    JSClassID family = familyOf(wrappers, id);
    if (void* PR_NULLABLE* slot = slotOf(wrappers, family, handle))
        return static_cast<JsWrapper*>(*slot);
    return wrappers->table.find(handle, family, prHashPointer(handle, family));
}

JSValue objectOf(const JsWrapper* wrapper) {
    return JS_MKPTR(JS_TAG_OBJECT, wrapper->object);
}

JsWrapper* wrapperOf(JSValueConst object) {
    JSClassID id = 0;
    return static_cast<JsWrapper*>(JS_GetAnyOpaque(object, &id));
}

// Removes the object from its group's members. The caller then releases the group's reference to it.
void leaveGroup(JsWrapper* wrapper) {
    PR_ASSERT(wrapper->member);
    JsWrapper* group = wrapperOf(JS_MKPTR(JS_TAG_OBJECT, wrapper->owner));
    if (wrapper->previousMember != nullptr)
        wrapper->previousMember->nextMember = wrapper->nextMember;
    else
        group->firstMember = wrapper->nextMember;
    if (wrapper->nextMember != nullptr)
        wrapper->nextMember->previousMember = wrapper->previousMember;
    wrapper->nextMember = nullptr;
    wrapper->previousMember = nullptr;
    wrapper->member = false;
}

// Returns true if the object's group must hold it. That is the case if the object holds state that would be lost if it
// were freed, or if it holds members of its own, which would lose their state.
bool anchored(const JsWrapper* wrapper) {
    return wrapper->stateful || wrapper->firstMember != nullptr;
}

// Makes a newly anchored object a member of its group. Repeats for each group above it that was not anchored.
void anchor(JSRuntime* runtime, JsWrapper* wrapper) {
    for (JsWrapper* at = wrapper; at->grouped && !at->member;) {
        JsWrapper* group = wrapperOf(JS_MKPTR(JS_TAG_OBJECT, at->owner));
        at->nextMember = group->firstMember;
        if (group->firstMember != nullptr)
            group->firstMember->previousMember = at;
        group->firstMember = at;
        at->member = true;
        JS_DupValueRT(runtime, objectOf(at));
        at = group;
    }
}

// Removes the memberships of an object that is no longer anchored. Repeats for each group above it that is then no
// longer anchored. Each removal may release the last reference to what it held.
void unanchor(JSRuntime* runtime, JsWrapper* wrapper) {
    JSValue held = JS_UNDEFINED;
    for (JsWrapper* at = wrapper; at->member && !anchored(at);) {
        JSValue group = JS_DupValueRT(runtime, JS_MKPTR(JS_TAG_OBJECT, at->owner));
        leaveGroup(at);
        JS_FreeValueRT(runtime, objectOf(at));
        JS_FreeValueRT(runtime, held);
        held = group;
        at = wrapperOf(group);
    }
    JS_FreeValueRT(runtime, held);
}

// Marks that the object now holds state that a new object for its handle would not have.
void becomeStateful(JSRuntime* runtime, JsWrapper* wrapper) {
    if (wrapper->stateful)
        return;
    wrapper->stateful = true;
    anchor(runtime, wrapper);
}

// The state hook of the bound classes (JSClassStateHook). An object that is still being created has no wrapper.
void objectState(JSRuntime* runtime, JSValueConst object) {
    if (JsWrapper* wrapper = wrapperOf(object))
        becomeStateful(runtime, wrapper);
}

// Returns the entry stored under `name`. Names are compared by address first, since a name is usually the same literal.
template <typename Named>
Named* PR_NULLABLE findNamed(Array<Named>* held, const char* name) {
    for (uint32_t i = 0; i < held->count; ++i) {
        if (held->data[i].name == name)
            return &held->data[i];
    }
    for (uint32_t i = 0; i < held->count; ++i) {
        const char* a = held->data[i].name;
        const char* b = name;
        while (*a != 0 && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == *b)
            return &held->data[i];
    }
    return nullptr;
}

template <typename Named>
const Named* PR_NULLABLE findNamed(const Array<Named>* held, const char* name) {
    return findNamed(const_cast<Array<Named>*>(held), name);
}

// Returns the object's rarely used data, which is created if `make` is set.
JsWrapperExtra* PR_NULLABLE extraOf(JsWrapper* wrapper, bool make) {
    if (wrapper->extra == nullptr && make) {
        wrapper->extra = allocZeroed<JsWrapperExtra>();
        wrapper->extra->pinnedObject = JS_UNDEFINED;
    }
    return wrapper->extra;
}

} // namespace

bool jsStringRead(JSContext* ctx, JSValueConst input, JsString* out) {
    out->text = JS_ToCStringLen(ctx, &out->size, input);
    if (out->text == nullptr)
        return false;
    out->scalar = scalarize(out->text, out->size);
    return true;
}

void jsStringReset(JSContext* ctx, JsString* string) {
    prMemoryFree(string->scalar);
    JS_FreeCString(ctx, string->text);
    *string = {};
}

PrStringView jsStringView(const JsString* string) {
    const char* data = string->scalar != nullptr ? string->scalar : string->text;
    return PrStringView{data != nullptr ? data : "", string->size};
}

void jsCallEnter(JSContext* ctx) {
    JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    wrappers->calls.push(wrappers->held.count);
}

void jsCallLeave(JSContext* ctx) {
    JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    PR_ASSERT(wrappers->calls.count != 0);
    uint32_t first = wrappers->calls.data[--wrappers->calls.count];
    // A release can run script, which can hold more values above the ones being released.
    while (wrappers->held.count > first) {
        JsHeld held = wrappers->held.data[--wrappers->held.count];
        switch (held.kind) {
            case JsHeldKind_CString: JS_FreeCString(ctx, held.cstring); break;
            case JsHeldKind_Bytes: prMemoryFree(held.bytes); break;
            case JsHeldKind_Value: JS_FreeValue(ctx, held.value); break;
            case JsHeldKind_String: prStringReset(&held.string); break;
            case JsHeldKind_Custom: held.custom.release(ctx, held.custom.data); break;
            default: break;
        }
    }
}

uint32_t jsHold(JSContext* ctx, JsHeld held) {
    JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    PR_ASSERT(wrappers->calls.count != 0);
    wrappers->held.push(held);
    return wrappers->held.count - 1;
}

JsHeld* jsHeldAt(JSContext* ctx, uint32_t index) {
    return &wrappersOf(JS_GetRuntime(ctx))->held.data[index];
}

uint32_t jsHoldBytes(JSContext* ctx, void* PR_NULLABLE bytes) {
    JsHeld held = {};
    held.kind = JsHeldKind_Bytes;
    held.bytes = bytes;
    return jsHold(ctx, held);
}

uint32_t jsHoldString(JSContext* ctx, PrString string) {
    JsHeld held = {};
    held.kind = JsHeldKind_String;
    held.string = string;
    return jsHold(ctx, held);
}

uint32_t jsHoldCustom(JSContext* ctx, void (*PR_NONNULL release)(JSContext* ctx, void* PR_NULLABLE data),
        void* PR_NULLABLE data) {
    JsHeld held = {};
    held.kind = JsHeldKind_Custom;
    held.custom.release = release;
    held.custom.data = data;
    return jsHold(ctx, held);
}

bool jsReadString(JSContext* ctx, JSValueConst input, PrStringView* out) {
    JsString string = {};
    if (!jsStringRead(ctx, input, &string))
        return false;
    JsHeld held = {};
    held.kind = JsHeldKind_CString;
    held.cstring = string.text;
    jsHold(ctx, held);
    if (string.scalar != nullptr) {
        held.kind = JsHeldKind_Bytes;
        held.bytes = string.scalar;
        jsHold(ctx, held);
    }
    *out = jsStringView(&string);
    return true;
}

JSValue jsConstructed(JSContext* ctx, JSValueConst target, JSValue result) {
    if (JS_IsException(result))
        return result;
    JSValue proto = JS_GetProperty(ctx, target, JS_ATOM_prototype);
    int status = JS_IsException(proto) ? -1 : JS_IsObject(proto)  ? JS_SetPrototype(ctx, result, proto) : 0;
    JS_FreeValue(ctx, proto);
    if (status < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

JSValue jsIteratorMethod(JSContext* ctx, JSValueConst input) {
    return JS_GetProperty(ctx, input, JS_ATOM_Symbol_iterator);
}

void JsIterator::reset() {
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, iterator);
    next = JS_UNDEFINED;
    iterator = JS_UNDEFINED;
}

bool JsIterator::open(JSValueConst input, JSValueConst method) {
    iterator = JS_Call(ctx, method, input, 0, nullptr);
    if (JS_IsException(iterator))
        return false;
    if (!JS_IsObject(iterator)) {
        JS_ThrowTypeError(ctx, "Iterator must be an object");
        return false;
    }
    next = JS_GetProperty(ctx, iterator, JS_ATOM_next);
    return !JS_IsException(next);
}

bool JsIterator::step(JSValue* value, bool* done) {
    *value = JS_UNDEFINED;
    *done = false;
    JSValue item = JS_Call(ctx, next, iterator, 0, nullptr);
    if (JS_IsException(item))
        return false;
    if (!JS_IsObject(item)) {
        JS_FreeValue(ctx, item);
        JS_ThrowTypeError(ctx, "Iterator result must be an object");
        return false;
    }
    JSValue finished = JS_GetProperty(ctx, item, JS_ATOM_done);
    bool ok = !JS_IsException(finished);
    *done = ok && JS_ToBool(ctx, finished) != 0;
    JS_FreeValue(ctx, finished);
    if (ok && !*done) {
        *value = JS_GetProperty(ctx, item, JS_ATOM_value);
        ok = !JS_IsException(*value);
    }
    JS_FreeValue(ctx, item);
    return ok;
}

void JsIterator::closeAfterException() {
    JSValue error = JS_GetException(ctx);
    JSValue method = JS_GetProperty(ctx, iterator, JS_ATOM_return);
    if (!JS_IsException(method) && !JS_IsUndefined(method) && !JS_IsNull(method)) {
        JSValue result = JS_Call(ctx, method, iterator, 0, nullptr);
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_Throw(ctx, error);
}

JSRuntime* jsRuntimeCreate() {
    JSRuntime* runtime = JS_NewRuntime2(&kJsAllocator, nullptr);
    JS_SetMaxStackSize(runtime, kJsStackSize);
    JS_SetRuntimeOpaque(runtime, allocZeroed<JsWrappers>());
    return runtime;
}

void jsRuntimeDestroy(JSRuntime* PR_NULLABLE runtime) {
    if (runtime == nullptr)
        return;
    JsWrappers* wrappers = wrappersOf(runtime);
    // Freeing a value runs finalizers that release other callbacks, so every value is removed from the list before any
    // is freed. A callback that native code still holds outlives the runtime, detached. Its release frees only the
    // callback.
    Array<JSValue> values;
    for (JsCallback* callback = wrappers->callbacks; callback != nullptr;) {
        JsCallback* next = callback->next;
        values.push(callback->value);
        callback->value = JS_UNDEFINED;
        callback->runtime = nullptr;
        callback->previous = nullptr;
        callback->next = nullptr;
        callback = next;
    }
    wrappers->callbacks = nullptr;
    for (uint32_t i = 0; i < values.count; ++i)
        JS_FreeValueRT(runtime, values.data[i]);
    values.reset();
    for (uint32_t i = 0; i < wrappers->unpinned.count; ++i)
        JS_FreeValueRT(runtime, wrappers->unpinned.data[i]);
    wrappers->unpinned.reset();
    wrappers->fixed.reset(runtime);
    wrappers->destroying = true;
    JS_FreeRuntime(runtime);
    wrappers->pending.reset();
    wrappers->held.reset();
    wrappers->calls.reset();
    wrappers->table.reset();
    wrappers->classes.reset();
    for (uint32_t i = 0; i < wrappers->data.count; ++i) {
        if (wrappers->data.data[i].destroy != nullptr)
            wrappers->data.data[i].destroy(const_cast<void*>(wrappers->data.data[i].value));
    }
    wrappers->data.reset();
    wrappers->realms.reset();
    prMemoryFree(wrappers);
}

void jsCallbackLink(JSRuntime* runtime, JsCallback* callback, JSValueConst owner) {
    JSClassID id = 0;
    JsWrapper* wrapper = JS_IsObject(owner) ? static_cast<JsWrapper*>(JS_GetAnyOpaque(owner, &id)) : nullptr;
    // Only an object of a bound class has a mark that traces its callbacks.
    const JsWrappers* wrappers = wrappersOf(runtime);
    if (wrapper != nullptr && (id >= wrappers->classes.count || !wrappers->classes.data[id].defined))
        wrapper = nullptr;
    if (wrapper != nullptr)
        becomeStateful(runtime, wrapper);
    JsCallback** list = wrapper != nullptr ? &extraOf(wrapper, true)->callbacks : &wrappersOf(runtime)->callbacks;
    callback->owner = wrapper;
    callback->previous = nullptr;
    callback->next = *list;
    if (*list != nullptr)
        (*list)->previous = callback;
    *list = callback;
}

void jsCallbackUnlink(JsCallback* callback) {
    JsCallback** list;
    if (callback->owner != nullptr)
        list = &callback->owner->extra->callbacks;
    else
        list = &wrappersOf(callback->runtime)->callbacks;
    if (callback->previous != nullptr)
        callback->previous->next = callback->next;
    else
        *list = callback->next;
    if (callback->next != nullptr)
        callback->next->previous = callback->previous;
    callback->owner = nullptr;
    callback->previous = nullptr;
    callback->next = nullptr;
}

void jsEnterRealm(JSContext* ctx) {
    wrappersOf(JS_GetRuntime(ctx))->realms.push(ctx);
}

void jsLeaveRealm(JSContext* ctx) {
    JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    PR_ASSERT(wrappers->realms.count != 0 && wrappers->realms.last() == ctx);
    wrappers->realms.pop();
}

JSContext* jsIncumbentRealm(JSContext* ctx) {
    JSContext* caller = JS_GetCallerRealm(ctx);
    if (caller != nullptr)
        return caller;
    JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    return wrappers->realms.count != 0 ? wrappers->realms.last() : ctx;
}

void jsEnqueueNative(JSContext* ctx, void (*run)(void* data), void* data) {
    // The job's two arguments are the function and its data, passed as the numeric values of their addresses.
    auto job = [](JSContext* context, int, JSValueConst* argv) {
        int64_t function = 0;
        int64_t argument = 0;
        JS_ToInt64(context, &function, argv[0]);
        JS_ToInt64(context, &argument, argv[1]);
        reinterpret_cast<void (*)(void*)>(uintptr_t(function))(reinterpret_cast<void*>(uintptr_t(argument)));
        return JS_UNDEFINED;
    };
    JSValue args[] = {JS_NewInt64(ctx, int64_t(uintptr_t(run))), JS_NewInt64(ctx, int64_t(uintptr_t(data)))};
    JS_EnqueueJob(ctx, job, 2, args);
}

JSValue jsWrap(JSContext* ctx, JSClassID id, JSValueConst owner, void* PR_NULLABLE handle,
        bool ownsHandle, void (*PR_NULLABLE retain)(void*)) {
    if (handle == nullptr)
        return JS_NULL;

    JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    JSClassID family = familyOf(wrappers, id);
    uint32_t hash = prHashPointer(handle, family);
    void* PR_NULLABLE* slot = slotOf(wrappers, family, handle);
    const JsWrapper* found = slot != nullptr ? static_cast<JsWrapper*>(*slot) : wrappers->table.find(handle, family, hash);
    if (found != nullptr)
        return JS_DupValue(ctx, objectOf(found));

    JSValue object = JS_NewObjectClass(ctx, int(id));
    if (JS_IsException(object))
        return object;

    const JsClassRecord* record = id < wrappers->classes.count ? &wrappers->classes.data[id] : nullptr;
    const JsReferences* references = record != nullptr ? record->references : nullptr;
    if (!ownsHandle && retain == nullptr && references != nullptr)
        retain = references->retain;
    if (!ownsHandle && retain != nullptr) {
        retain(handle);
        ownsHandle = true;
    }
    PR_ASSERT(JS_IsObject(owner) || JS_IsUndefined(owner) || JS_IsNull(owner));
    JsWrapper* wrapper = alloc<JsWrapper>();
    *wrapper = JsWrapper{
        handle,  JS_VALUE_GET_PTR(object),
        nullptr, nullptr,
        nullptr, nullptr,
        nullptr, hash,
        family,  ownsHandle,
        false,   false,
        false,
    };
    if (JS_IsObject(owner))
        wrapper->owner = JS_VALUE_GET_PTR(JS_DupValue(ctx, owner));
    if (references != nullptr && references->pinnable && wrappers->pending.count != 0) {
        for (uint32_t i = wrappers->pending.count; i-- != 0;) {
            if (wrappers->pending.data[i] == handle) {
                wrappers->pending.data[i] = wrappers->pending.data[--wrappers->pending.count];
                ++extraOf(wrapper, true)->pins;
            }
        }
        if (wrapper->extra != nullptr)
            wrapper->extra->pinnedObject = JS_DupValue(ctx, object);
    }
    // What the class prepares belongs to every object, so it is defined before the object has its wrapper. Otherwise
    // the state hook would treat it as state.
    if (record != nullptr && record->prepared != nullptr)
        record->prepared(ctx, object, handle);
    JS_SetOpaque(object, wrapper);
    if (slot != nullptr)
        *slot = wrapper;
    else
        wrappers->table.insert(wrapper);
    if (record != nullptr && record->wrapped != nullptr)
        record->wrapped(ctx, object, handle);
    return object;
}

JSValue jsSameObject(JSContext* ctx, JSValueConst object, const char* name) {
    const JsWrapperExtra* extra = extraOf(wrapperOf(object), false);
    const JsNamedValue* held = extra != nullptr ? findNamed(&extra->sameObjects, name) : nullptr;
    return held != nullptr ? JS_DupValue(ctx, held->value) : JS_UNDEFINED;
}

void jsSetSameObject(JSContext* ctx, JSValueConst object, const char* name, JSValueConst value) {
    becomeStateful(JS_GetRuntime(ctx), wrapperOf(object));
    JsWrapperExtra* extra = extraOf(wrapperOf(object), true);
    JsNamedValue* held = findNamed(&extra->sameObjects, name);
    if (held == nullptr) {
        extra->sameObjects.push(JsNamedValue{name, JS_UNDEFINED});
        held = &extra->sameObjects.last();
    }
    JSValue previous = held->value;
    held->value = JS_DupValue(ctx, value);
    JS_FreeValue(ctx, previous);
}

bool jsHold(JSContext* ctx, JSValueConst owner, const char* name, JSValueConst value, bool hold) {
    JSValue held = jsSameObject(ctx, owner, name);
    if (JS_IsUndefined(held)) {
        if (!hold)
            return true;
        held = JS_NewArray(ctx);
        if (JS_IsException(held))
            return false;
        jsSetSameObject(ctx, owner, name, held);
    }
    int64_t length = 0;
    bool ok = JS_GetLength(ctx, held, &length) >= 0;
    int64_t at = -1;
    for (int64_t i = 0; ok && at < 0 && i < length; ++i) {
        JSValue item = JS_GetPropertyInt64(ctx, held, i);
        if (JS_VALUE_GET_PTR(item) == JS_VALUE_GET_PTR(value))
            at = i;
        JS_FreeValue(ctx, item);
    }
    if (ok && hold && at < 0) {
        ok = JS_SetPropertyInt64(ctx, held, length, JS_DupValue(ctx, value)) >= 0;
    } else if (ok && !hold && at >= 0) {
        // The last entry takes the place of the released one.
        ok = JS_SetPropertyInt64(ctx, held, at, JS_GetPropertyInt64(ctx, held, length - 1)) >= 0 &&
            JS_SetPropertyStr(ctx, held, "length", JS_NewInt64(ctx, length - 1)) >= 0;
    }
    JS_FreeValue(ctx, held);
    return ok;
}

void jsSetReference(JSContext* ctx, JSValueConst object, uint32_t index, JSValueConst value) {
    JsWrapperExtra* extra = extraOf(wrapperOf(object), !JS_IsUndefined(value));
    if (extra == nullptr)
        return;
    if (!JS_IsUndefined(value))
        becomeStateful(JS_GetRuntime(ctx), wrapperOf(object));
    Array<JSValue>& references = extra->references;
    while (references.count <= index)
        references.push(JS_UNDEFINED);
    JSValue previous = references.data[index];
    references.data[index] = JS_DupValue(ctx, value);
    JS_FreeValue(ctx, previous);
}

JSValue jsReference(JSContext* ctx, JSValueConst object, uint32_t index) {
    JsWrapper* wrapper = wrapperOf(object);
    const JsWrapperExtra* extra = wrapper != nullptr ? extraOf(wrapper, false) : nullptr;
    if (extra == nullptr || index >= extra->references.count)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, extra->references.data[index]);
}

JSValue jsNewString(JSContext* ctx, const PrString* text) {
    return jsNewStringView(ctx, prStringView(text));
}

JSValue jsTakeString(JSContext* ctx, PrString text) {
    JSValue value = jsNewString(ctx, &text);
    prStringReset(&text);
    return value;
}

JSValue jsFixedString(JSContext* ctx, PrStringView text) {
    PR_ASSERT(text.data != nullptr && text.size <= 0xffffffffu);
    FixedStrings::Entry* entry = wrappersOf(JS_GetRuntime(ctx))->fixed.find(text.data, uint32_t(text.size));
    if (entry->data == nullptr) {
        JSAtom atom = JS_NewAtomLen(ctx, text.data, text.size);
        if (atom == JS_ATOM_NULL)
            return JS_EXCEPTION;
        *entry = {text.data, uint32_t(text.size), atom};
        ++wrappersOf(JS_GetRuntime(ctx))->fixed.count;
    }
    return JS_AtomToString(ctx, entry->atom);
}

JSValue jsNewStringView(JSContext* ctx, PrStringView text) {
    return text.data != nullptr ? JS_NewStringLen(ctx, text.data, text.size) : JS_NULL;
}

bool jsIsGlobal(JSContext* ctx, JSValueConst value) {
    if (JS_IsUndefined(value) || JS_IsNull(value))
        return true;
    if (!JS_IsObject(value))
        return false;
    JSValue global = JS_GetGlobalObject(ctx);
    bool same = JS_VALUE_GET_PTR(value) == JS_VALUE_GET_PTR(global);
    JS_FreeValue(ctx, global);
    return same;
}

void jsReleaseHandle(JSRuntime* runtime, JSClassID id, void* handle) {
    if (const JsReferences* references = referencesOf(wrappersOf(runtime), id))
        references->release(handle);
}

void jsWrapperForget(JSRuntime* runtime, JsWrapper* wrapper) {
    JsWrappers* wrappers = wrappersOf(runtime);
    if (void* PR_NULLABLE* slot = slotOf(wrappers, wrapper->family, wrapper->handle))
        *slot = nullptr;
    else
        wrappers->table.erase(wrapper);
    if (JsWrapperExtra* extra = wrapper->extra) {
        while (JsCallback* callback = extra->callbacks) {
            JSRuntime* owner = callback->runtime;
            jsCallbackUnlink(callback);
            JSValue value = callback->value;
            callback->value = JS_UNDEFINED;
            callback->runtime = nullptr;
            JS_FreeValueRT(owner, value);
        }
    }
}

void jsWrapperDestroy(JSRuntime* runtime, JsWrapper* wrapper) {
    // A group whose members still hold it is being freed with them in a cycle. Each member is detached from it before
    // the collector reaches the member.
    for (JsWrapper* member = wrapper->firstMember; member != nullptr;) {
        JsWrapper* next = member->nextMember;
        member->nextMember = nullptr;
        member->previousMember = nullptr;
        member->member = false;
        JS_FreeValueRT(runtime, objectOf(member));
        member = next;
    }
    wrapper->firstMember = nullptr;
    if (wrapper->member)
        leaveGroup(wrapper);
    if (wrapper->owner != nullptr)
        JS_FreeValueRT(runtime, JS_MKPTR(JS_TAG_OBJECT, wrapper->owner));
    if (JsWrapperExtra* extra = wrapper->extra) {
        for (uint32_t i = 0; i < extra->sameObjects.count; ++i)
            JS_FreeValueRT(runtime, extra->sameObjects.data[i].value);
        extra->sameObjects.reset();
        for (uint32_t i = 0; i < extra->references.count; ++i)
            JS_FreeValueRT(runtime, extra->references.data[i]);
        extra->references.reset();
        JS_FreeValueRT(runtime, extra->pinnedObject);
        prMemoryFree(extra);
    }
    prMemoryFree(wrapper);
}

void jsMark(JSRuntime* runtime, JSValueConst object, JSClassID id, JS_MarkFunc* mark) {
    JsWrapper* wrapper = static_cast<JsWrapper*>(JS_GetOpaque(object, id));
    if (wrapper == nullptr)
        return;
    if (wrapper->owner != nullptr)
        JS_MarkValue(runtime, JS_MKPTR(JS_TAG_OBJECT, wrapper->owner), mark);
    for (const JsWrapper* member = wrapper->firstMember; member != nullptr; member = member->nextMember)
        JS_MarkValue(runtime, objectOf(member), mark);
    const JsWrapperExtra* extra = wrapper->extra;
    if (extra == nullptr)
        return;
    for (uint32_t i = 0; i < extra->sameObjects.count; ++i)
        JS_MarkValue(runtime, extra->sameObjects.data[i].value, mark);
    for (uint32_t i = 0; i < extra->references.count; ++i)
        JS_MarkValue(runtime, extra->references.data[i], mark);
    for (const JsCallback* callback = extra->callbacks; callback != nullptr; callback = callback->next)
        JS_MarkValue(runtime, callback->value, mark);
    // The collector does not free a pinned object, except when the runtime is destroyed.
    if (!JS_IsUndefined(extra->pinnedObject) && wrappersOf(runtime)->destroying)
        JS_MarkValue(runtime, extra->pinnedObject, mark);
}

int jsIndexOf(JSContext* ctx, JSAtom name, uint32_t* out) {
    if ((name & kAtomTaggedInt) != 0) {
        *out = name & ~kAtomTaggedInt;
        return 1;
    }
    JSValue value = JS_AtomToValue(ctx, name);
    if (JS_IsException(value))
        return -1;
    if (!JS_IsString(value)) {
        JS_FreeValue(ctx, value);
        return 0;
    }
    uint32_t index = 0;
    int status = JS_ToUint32(ctx, &index, value);
    JS_FreeValue(ctx, value);
    if (status < 0)
        return -1;
    if (index < kAtomTaggedInt || index == 0xffffffffu)
        return 0;
    JSAtom canonical = JS_NewAtomUInt32(ctx, index);
    if (canonical == JS_ATOM_NULL)
        return -1;
    bool matches = canonical == name;
    JS_FreeAtom(ctx, canonical);
    if (matches)
        *out = index;
    return matches ? 1 : 0;
}

bool jsIndexAtomsTagged(JSContext* ctx) {
    JSAtom probe = JS_NewAtomUInt32(ctx, 7);
    bool tagged = (probe & kAtomTaggedInt) != 0 && (probe & ~kAtomTaggedInt) == 7;
    JS_FreeAtom(ctx, probe);
    return tagged;
}

JSValue jsSettledPromise(JSContext* ctx, JSValueConst value, bool rejected) {
    JSValue functions[2];
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(promise))
        return promise;
    JS_FreeValue(ctx, JS_Call(ctx, functions[rejected ? 1 : 0], JS_UNDEFINED, 1, &value));
    JS_FreeValue(ctx, functions[0]);
    JS_FreeValue(ctx, functions[1]);
    return promise;
}

JSValue jsIllegalConstructor(JSContext* ctx, JSValueConst, int, JSValueConst* PR_NULLABLE) {
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

JSValue jsNewFloatArray(JSContext* ctx, JSTypedArrayEnum type, const double* values, uint32_t count) {
    PR_ASSERT(type == JS_TYPED_ARRAY_FLOAT32 || type == JS_TYPED_ARRAY_FLOAT64);
    Array<uint8_t> bytes;
    if (type == JS_TYPED_ARRAY_FLOAT32) {
        bytes.reserve(count * uint32_t(sizeof(float)));
        for (uint32_t i = 0; i < count; ++i) {
            float value = float(values[i]);
            bytes.append(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
        }
    } else {
        bytes.append(reinterpret_cast<const uint8_t*>(values), count * uint32_t(sizeof(double)));
    }
    JSValue buffer = JS_NewArrayBufferCopy(ctx, bytes.data, bytes.count);
    bytes.reset();
    if (JS_IsException(buffer))
        return buffer;
    // The constructor reads an offset and a length whatever the count, because its call path pads the arguments.
    JSValueConst arguments[] = {buffer, JS_NewInt32(ctx, 0), JS_UNDEFINED};
    JSValue array = JS_NewTypedArray(ctx, 3, arguments, type);
    JS_FreeValue(ctx, buffer);
    return array;
}

bool jsReadFloatArray(JSContext* ctx, JSValueConst value, JSTypedArrayEnum type, double* out,
        uint32_t capacity, uint32_t* count) {
    if (JS_GetTypedArrayType(value) != int(type)) {
        JS_ThrowTypeError(ctx, type == JS_TYPED_ARRAY_FLOAT32 ? "Expected a Float32Array" : "Expected a Float64Array");
        return false;
    }
    size_t offset = 0;
    size_t length = 0;
    size_t elementSize = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &elementSize);
    if (JS_IsException(buffer))
        return false;
    size_t size = 0;
    const uint8_t* data = JS_GetArrayBuffer(ctx, &size, buffer);
    JS_FreeValue(ctx, buffer);
    if (data == nullptr)
        return false;
    *count = uint32_t(length / elementSize);
    for (uint32_t i = 0; i < *count && i < capacity; ++i) {
        if (type == JS_TYPED_ARRAY_FLOAT32) {
            float element;
            prMemCopy(&element, data + offset + i * sizeof(float), sizeof(float));
            out[i] = double(element);
        } else {
            prMemCopy(&out[i], data + offset + i * sizeof(double), sizeof(double));
        }
    }
    return true;
}

void jsMemberEnter(JSContext* ctx) {
    JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    if (wrappers->scope != nullptr)
        wrappers->scope->enter(wrappers->scopeContext);
    ++wrappers->members;
    jsCallEnter(ctx);
}

void jsMemberLeave(JSContext* ctx) {
    jsCallLeave(ctx);
    JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    // Objects that lost their last pin while the member ran are released once the member's result holds them.
    if (--wrappers->members == 0) {
        while (wrappers->unpinned.count != 0)
            JS_FreeValue(ctx, wrappers->unpinned.data[--wrappers->unpinned.count]);
    }
    if (wrappers->scope != nullptr)
        wrappers->scope->leave(wrappers->scopeContext);
}

void jsSetMemberScope(JSRuntime* runtime, const JsMemberScope* PR_NULLABLE scope, void* PR_NULLABLE context) {
    JsWrappers* wrappers = wrappersOf(runtime);
    wrappers->scope = scope;
    wrappers->scopeContext = context;
}

void jsRegisterClass(JSRuntime* runtime, JSClassID* id, const JSClassDef& definition) {
    // The process assigns numbers to the classes that every runtime shares, starting after the classes that QuickJS
    // defines. Runtimes on different threads assign numbers under a lock. A runtime reads a number only after its own
    // registration has seen it written.
    static Atomic<bool> numbering;
    static JSClassID next = 0;
    bool unlocked = false;
    while (!numbering.cas(unlocked, true))
        unlocked = false;
    if (*id == 0) {
        if (next == 0)
            JS_NewClassID(runtime, &next);
        if (next == kJsMaxClasses)
            PR_FATAL("the process defines at most kJsMaxClasses classes");
        *id = next++;
    }
    JSClassID number = *id;
    numbering.store(false);
    if (!JS_IsRegisteredClass(runtime, number))
        JS_NewClass(runtime, number, &definition);
}

void jsDeclareClass(JSRuntime* runtime, const JsClass& definition) {
    JSClassDef classDef = {
        definition.name, definition.finalizer, definition.mark, nullptr,
        definition.exotic, objectState,
    };
    jsRegisterClass(runtime, definition.id, classDef);
    JsWrappers* wrappers = wrappersOf(runtime);
    JsClassRecord* record = recordOf(wrappers, *definition.id);
    JSClassID id = *definition.id;
    record->base = definition.base != nullptr ? *definition.base : 0;
    const JsClassRecord* parent = record->base != 0 ? &wrappers->classes.data[record->base] : nullptr;
    PR_ASSERT(parent == nullptr || parent->family != 0);
    record->family = parent != nullptr ? parent->family : id;
    record->depth = parent != nullptr ? parent->depth + 1 : 0;
    if (record->depth >= kClassMaxDepth)
        PR_FATAL("a class hierarchy is at most kClassMaxDepth deep");
    for (uint32_t i = 0; i < record->depth; ++i)
        record->ancestors[i] = parent->ancestors[i];
    record->ancestors[record->depth] = id;
    if (definition.references != nullptr)
        record->references = definition.references;
    else if (parent != nullptr)
        record->references = parent->references;
    else
        record->references = nullptr;
    record->slot = parent != nullptr ? parent->slot : definition.slot;
    if (definition.prepared != nullptr)
        record->prepared = definition.prepared;
    else if (parent != nullptr)
        record->prepared = parent->prepared;
    else
        record->prepared = nullptr;
    record->wrapped =
        definition.wrapped != nullptr ? definition.wrapped : parent != nullptr           ? parent->wrapped : nullptr;
    record->data = definition.data;
    record->defined = true;
}

void* PR_NULLABLE jsClassData(JSRuntime* runtime, JSClassID id) {
    const JsWrappers* wrappers = wrappersOf(runtime);
    return id < wrappers->classes.count ? wrappers->classes.data[id].data : nullptr;
}

bool jsDerives(JSRuntime* runtime, JSClassID id, JSClassID base) {
    if (id == base)
        return id != 0;
    const JsWrappers* wrappers = wrappersOf(runtime);
    if (id >= wrappers->classes.count || base >= wrappers->classes.count)
        return false;
    const JsClassRecord& record = wrappers->classes.data[id];
    const JsClassRecord& ancestor = wrappers->classes.data[base];
    return ancestor.family != 0 && record.depth > ancestor.depth && record.ancestors[ancestor.depth] == base;
}

void* PR_NULLABLE jsHandleIn(JSContext* ctx, JSValueConst value, JSClassID base) {
    JSClassID actual = 0;
    void* opaque = JS_GetAnyOpaque(value, &actual);
    if (opaque == nullptr || !jsDerives(JS_GetRuntime(ctx), actual, base))
        return nullptr;
    return static_cast<JsWrapper*>(opaque)->handle;
}

void* PR_NULLABLE jsHandleInSet(JSValueConst value, const JsClassSet* set) {
    JSClassID actual = 0;
    void* opaque = JS_GetAnyOpaque(value, &actual);
    return opaque != nullptr && jsClassSetHas(set, actual) ? static_cast<JsWrapper*>(opaque)->handle : nullptr;
}

void* PR_NULLABLE jsGlobalHandleIn(JSContext* ctx, JSClassID base) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue prototype = JS_GetPrototype(ctx, global);
    void* handle = jsHandleIn(ctx, prototype, base);
    JS_FreeValue(ctx, prototype);
    JS_FreeValue(ctx, global);
    return handle;
}

void jsAddClassifier(JSRuntime* runtime, JSClassID id, JsClassify* classify) {
    JsClassRecord* record = recordOf(wrappersOf(runtime), id);
    for (uint32_t i = 0; i < record->classifierCount; ++i) {
        if (record->classifiers[i] == classify)
            return;
    }
    if (record->classifierCount == kMaxClassifiers)
        PR_FATAL("a class takes at most kMaxClassifiers classifiers");
    record->classifiers[record->classifierCount++] = classify;
}

void jsSetOwner(JSContext* ctx, JSValueConst object, JSValueConst owner) {
    PR_ASSERT(JS_IsObject(owner) || JS_IsUndefined(owner) || JS_IsNull(owner));
    JSRuntime* runtime = JS_GetRuntime(ctx);
    JsWrapper* wrapper = wrapperOf(object);
    // The new owner is referenced before the group releases the object, since the group's reference may be the only
    // one. The previous owner is released last. `object` is the caller's value, so the object stays alive when the
    // group releases it.
    void* current = JS_IsObject(owner) ? JS_VALUE_GET_PTR(JS_DupValue(ctx, owner)) : nullptr;
    void* previous = wrapper->owner;
    if (wrapper->member) {
        JsWrapper* group = wrapperOf(JS_MKPTR(JS_TAG_OBJECT, previous));
        leaveGroup(wrapper);
        // Releases the group's reference to the object.
        JS_FreeValue(ctx, objectOf(wrapper));
        unanchor(runtime, group);
    }
    wrapper->grouped = false;
    wrapper->owner = current;
    if (previous != nullptr)
        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, previous));
}

void jsGroupJoin(JSContext* ctx, JSValueConst group, JSValueConst member) {
    JsWrapper* wrapper = wrapperOf(member);
    if (wrapper->grouped && wrapper->owner == JS_VALUE_GET_PTR(group))
        return;
    jsSetOwner(ctx, member, group);
    wrapper->grouped = true;
    if (anchored(wrapper))
        anchor(JS_GetRuntime(ctx), wrapper);
}

void jsGroupKeep(JSContext* ctx, JSValueConst member) {
    becomeStateful(JS_GetRuntime(ctx), wrapperOf(member));
}

void jsPin(JSRuntime* runtime, JSClassID base, void* handle, bool pinned) {
    JsWrappers* wrappers = wrappersOf(runtime);
    if (JsWrapper* wrapper = findWrapperOf(wrappers, handle, base)) {
        JsWrapperExtra* extra = extraOf(wrapper, pinned);
        if (pinned && extra->pins++ == 0) {
            extra->pinnedObject = JS_DupValueRT(runtime, objectOf(wrapper));
        } else if (!pinned && extra != nullptr && extra->pins != 0 && --extra->pins == 0) {
            // Releasing its own reference may drop the object's last reference, while a running member may still return
            // it to script.
            JSValue object = extra->pinnedObject;
            extra->pinnedObject = JS_UNDEFINED;
            if (wrappers->members != 0)
                wrappers->unpinned.push(object);
            else
                JS_FreeValueRT(runtime, object);
        }
        return;
    }
    if (pinned) {
        wrappers->pending.push(handle);
        return;
    }
    for (uint32_t i = 0; i < wrappers->pending.count; ++i) {
        if (wrappers->pending.data[i] == handle) {
            wrappers->pending.data[i] = wrappers->pending.data[--wrappers->pending.count];
            return;
        }
    }
}

JSValue jsWrapperOf(JSContext* ctx, JSClassID base, void* PR_NULLABLE handle) {
    if (handle == nullptr)
        return JS_UNDEFINED;
    const JsWrapper* wrapper = findWrapperOf(wrappersOf(JS_GetRuntime(ctx)), handle, base);
    return wrapper != nullptr ? JS_DupValue(ctx, objectOf(wrapper)) : JS_UNDEFINED;
}

JSValue jsWrapAs(JSContext* ctx, JSClassID id, JSValueConst owner, void* PR_NULLABLE handle, bool ownsHandle) {
    if (handle == nullptr)
        return JS_NULL;
    // If the handle already has an object, it is returned without determining which class describes the handle.
    const JsWrappers* wrappers = wrappersOf(JS_GetRuntime(ctx));
    if (const JsWrapper* wrapper = findWrapperOf(wrappers, handle, id))
        return JS_DupValue(ctx, objectOf(wrapper));
    return jsWrap(ctx, classify(wrappers, id, handle), owner, handle, ownsHandle);
}

void jsSetRuntimeData(JSRuntime* runtime, const void* key, const void* PR_NULLABLE value) {
    if (JsRuntimeEntry* entry = runtimeEntry(wrappersOf(runtime), key))
        entry->value = value;
    else
        wrappersOf(runtime)->data.push(JsRuntimeEntry{key, value, nullptr});
}

const void* PR_NULLABLE jsRuntimeData(JSRuntime* runtime, const void* key) {
    const JsRuntimeEntry* entry = runtimeEntry(wrappersOf(runtime), key);
    return entry != nullptr ? entry->value : nullptr;
}

void* jsRuntimeOwned(JSRuntime* runtime, const void* key, void* PR_NONNULL (* PR_NONNULL make)(),
        void (* PR_NONNULL destroy)(void* PR_NONNULL)) {
    if (const JsRuntimeEntry* entry = runtimeEntry(wrappersOf(runtime), key))
        return const_cast<void*>(entry->value);
    void* value = make();
    wrappersOf(runtime)->data.push(JsRuntimeEntry{key, value, destroy});
    return value;
}

void jsValueFinalizer(JSRuntime* runtime, JSValue object) {
    jsFinalize<void>(runtime, object, JS_GetClassID(object), [](void* held) { prMemoryFree(held); });
}

void jsValueMark(JSRuntime* runtime, JSValueConst object, JS_MarkFunc* mark) {
    jsMark(runtime, object, JS_GetClassID(object), mark);
}

} // namespace pr

