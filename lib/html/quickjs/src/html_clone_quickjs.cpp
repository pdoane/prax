// Structured serialization of JavaScript values (html/#safe-passing-of-structured-data).
//
// A serialization is a stream of tagged values. Each object takes the next number in the order that the writer first
// meets it, and the reader's memory numbers objects the same way, so that a later reference to the object, including a
// cycle, reads as the same object. Strings and the objects with no parts that script can reach are written by the
// engine's own writer, which preserves lone surrogates and the state that script cannot read.

#include "html/quickjs/src/html_clone_quickjs.h"

#include "pr/count_of.h"
#include "pr/fetch_blob_quickjs.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/memory.h"
#include "pr/quickjs_binding.h"
#include "pr/text_buffer.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_message_quickjs.h"

namespace pr_impl {

namespace {

enum {
    CloneTag_Undefined,
    CloneTag_Null,
    CloneTag_True,
    CloneTag_False,
    CloneTag_Number,
    CloneTag_Primitive,     // a string or a BigInt, in the engine's format
    CloneTag_Leaf,          // an object in the engine's format
    CloneTag_Reference,     // an object that the stream already holds, by number
    CloneTag_Array,
    CloneTag_Object,
    CloneTag_Map,
    CloneTag_Set,
    CloneTag_TypedArray,
    CloneTag_DataView,
    CloneTag_Error,
    CloneTag_Port,
    CloneTag_Blob,
};
typedef uint8_t CloneTag;

// The deepest value that a serialization follows. A deeper value throws a DataCloneError, as a stack limit allows.
constexpr uint32_t kCloneMaxDepth = 1000;

// The error types that a serialization preserves, in the order that JS_NewNativeError numbers them. Any other name is
// "Error" (html/#structuredserializeinternal).
constexpr PrStringView kErrorNames[] = {"EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError"};
constexpr uint32_t kErrorNameCount = countOf(kErrorNames);

struct CloneWriter {
    JSContext* ctx;
    PrTextBuffer out = {};
    Array<void*> objects;       // the objects met so far, by number
    HashIndex index;            // positions in `objects`
    const Array<JSValue>* PR_NULLABLE transfer;
    uint32_t depth;
};

// Throws a DataCloneError, and returns false.
bool cloneFail(JSContext* ctx) {
    jsDomExceptionThrow(ctx, "DataCloneError");
    return false;
}

void putBytes(CloneWriter* w, const void* data, size_t size) {
    prTextBufferAppend(&w->out, PrStringView{static_cast<const char*>(data), size});
}

void putTag(CloneWriter* w, CloneTag tag) {
    putBytes(w, &tag, 1);
}

void putU32(CloneWriter* w, uint32_t value) {
    putBytes(w, &value, sizeof(value));
}

void putF64(CloneWriter* w, double value) {
    putBytes(w, &value, sizeof(value));
}

uint32_t hashOf(void* object) {
    return prHashPointer(object, 0);
}

// Records `object` and returns false, or writes a reference to it and returns true if the stream already holds it.
bool writeSeen(CloneWriter* w, JSValueConst value) {
    void* object = JS_VALUE_GET_PTR(value);
    uint32_t hash = hashOf(object);
    uint32_t at = w->index.find(hash, [&](uint32_t i) { return w->objects.data[i] == object; });
    if (at != kHashMissing) {
        putTag(w, CloneTag_Reference);
        putU32(w, at);
        return true;
    }
    w->index.insert(hash, w->objects.count);
    w->objects.push(object);
    return false;
}

// Writes `value` in the engine's format, after `tag`.
bool writeEngine(CloneWriter* w, CloneTag tag, JSValueConst value) {
    size_t size = 0;
    uint8_t* bytes = JS_WriteObject(w->ctx, &size, value, 0);
    if (bytes == nullptr) {
        JS_FreeValue(w->ctx, JS_GetException(w->ctx));
        return cloneFail(w->ctx);
    }
    putTag(w, tag);
    putU32(w, uint32_t(size));
    putBytes(w, bytes, size);
    js_free(w->ctx, bytes);
    return true;
}

bool writeValue(CloneWriter* w, JSValueConst value);

// Writes the own enumerable string-keyed properties of `object`, after a count of those written. A getter may remove a
// property that has not been reached, which is then skipped.
bool writeProperties(CloneWriter* w, JSValueConst object) {
    JSPropertyEnum* names = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(w->ctx, &names, &count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return false;
    uint32_t countAt = uint32_t(prTextBufferView(&w->out).size);
    putU32(w, 0);
    uint32_t written = 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; ++i) {
        JSAtom atom = names[i].atom;
        int present = JS_GetOwnProperty(w->ctx, nullptr, object, atom);
        ok = present >= 0;
        if (!ok || present == 0)
            continue;
        JSValue key = JS_AtomToValue(w->ctx, atom);
        JSValue item = JS_GetProperty(w->ctx, object, atom);
        ok = !JS_IsException(key) && !JS_IsException(item) && writeValue(w, key) && writeValue(w, item);
        JS_FreeValue(w->ctx, key);
        JS_FreeValue(w->ctx, item);
        ++written;
    }
    JS_FreePropertyEnum(w->ctx, names, count);
    if (ok)
        memcpy(w->out.data + countAt, &written, sizeof(written));
    return ok;
}

bool writeMap(CloneWriter* w, JSValueConst map, bool isSet) {
    putTag(w, isSet ? CloneTag_Set : CloneTag_Map);
    // The entries are copied first, because a getter of an entry can change the map.
    JSValue entries = JS_MapEntries(w->ctx, map);
    if (JS_IsException(entries))
        return false;
    int64_t length = 0;
    bool ok = JS_GetLength(w->ctx, entries, &length) == 0;
    if (ok)
        putU32(w, uint32_t(length / 2));
    for (int64_t i = 0; ok && i < length; i += isSet ? 2 : 1) {
        JSValue item = JS_GetPropertyInt64(w->ctx, entries, i);
        ok = !JS_IsException(item) && writeValue(w, item);
        JS_FreeValue(w->ctx, item);
    }
    JS_FreeValue(w->ctx, entries);
    return ok;
}

// An error's name, which is read through its prototypes, and its own message, stack and cause if they are data
// properties.
bool writeError(CloneWriter* w, JSValueConst error) {
    JSContext* ctx = w->ctx;
    JSValue nameValue = JS_GetPropertyStr(ctx, error, "name");
    if (JS_IsException(nameValue))
        return false;
    size_t nameSize = 0;
    const char* name = JS_IsUndefined(nameValue) ? nullptr : JS_ToCStringLen(ctx, &nameSize, nameValue);
    JS_FreeValue(ctx, nameValue);
    if (!JS_IsUndefined(nameValue) && name == nullptr)
        return false;
    uint32_t kind = kErrorNameCount;
    for (uint32_t i = 0; name != nullptr && i < kErrorNameCount; ++i) {
        if (PrStringView{name, nameSize} == kErrorNames[i])
            kind = i;
    }
    JS_FreeCString(ctx, name);
    putTag(w, CloneTag_Error);
    putU32(w, kind);
    constexpr const char* kOwn[] = {"message", "stack", "cause"};
    for (const char* member : kOwn) {
        JSAtom atom = JS_NewAtom(ctx, member);
        JSPropertyDescriptor descriptor;
        int present = JS_GetOwnProperty(ctx, &descriptor, error, atom);
        JS_FreeAtom(ctx, atom);
        if (present < 0)
            return false;
        JSValue item = JS_UNDEFINED;
        if (present > 0) {
            // A cause is any value, and the others are strings.
            bool isCause = PrStringView{member} == "cause";
            if ((descriptor.flags & JS_PROP_GETSET) == 0)
                item = isCause ? JS_DupValue(ctx, descriptor.value) : JS_ToString(ctx, descriptor.value);
            JS_FreeValue(ctx, descriptor.value);
            JS_FreeValue(ctx, descriptor.getter);
            JS_FreeValue(ctx, descriptor.setter);
        }
        if (JS_IsException(item))
            return false;
        bool ok = writeValue(w, item);
        JS_FreeValue(ctx, item);
        if (!ok)
            return false;
    }
    return true;
}

// Writes a view: its buffer first, whose reader needs it before it can make the view. A view that its buffer no longer
// covers cannot be serialized (html/#structuredserializeinternal).
bool writeView(CloneWriter* w, JSValueConst view, bool isDataView) {
    JSContext* ctx = w->ctx;
    size_t offset = 0;
    size_t length = 0;
    bool track = false;
    JSValue buffer = JS_GetViewBuffer(ctx, view, &offset, &length, &track);
    size_t available = 0;
    bool attached = JS_GetArrayBuffer(ctx, &available, buffer) != nullptr;
    if (!attached)
        JS_FreeValue(ctx, JS_GetException(ctx));
    if (attached && (offset > available || (!track && length > available - offset))) {
        JS_FreeValue(ctx, buffer);
        return cloneFail(ctx);
    }
    size_t element = 1;
    if (!isDataView) {
        size_t unusedOffset = 0;
        size_t unusedLength = 0;
        JSValue same = JS_GetTypedArrayBuffer(ctx, view, &unusedOffset, &unusedLength, &element);
        if (JS_IsException(same))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, same);
    }
    putTag(w, isDataView ? CloneTag_DataView : CloneTag_TypedArray);
    putU32(w, isDataView ? 0 : uint32_t(JS_GetTypedArrayType(view)));
    putF64(w, double(offset));
    putF64(w, double(element != 0 ? length / element : length));
    putU32(w, track ? 1 : 0);
    bool ok = writeValue(w, buffer);
    JS_FreeValue(ctx, buffer);
    return ok;
}

// Writes an object that a transfer list holds, as its position among the list's ports. Returns false if it is not one.
bool writePort(CloneWriter* w, JSValueConst value) {
    uint32_t position = 0;
    for (uint32_t i = 0; w->transfer != nullptr && i < w->transfer->count; ++i) {
        JSValueConst item = w->transfer->data[i];
        if (JS_VALUE_GET_PTR(item) == JS_VALUE_GET_PTR(value)) {
            putTag(w, CloneTag_Port);
            putU32(w, position);
            return true;
        }
        if (jsHtmlMessagePortOf(w->ctx, item) != nullptr)
            ++position;
    }
    return false;
}

// Writes a blob or a file (FileAPI/#blob-section): its type, its bytes, and a file's name and time.
void writeBlob(CloneWriter* w, const PrFetchBlob* blob) {
    putTag(w, CloneTag_Blob);
    PrStringView type = prFetchBlobType(blob);
    PrByteView bytes = prFetchBlobBytes(blob);
    PrStringView name = prFetchBlobIsFile(blob) ? prFetchBlobFileName(blob) : PrStringView{nullptr, 0};
    putU32(w, prFetchBlobIsFile(blob) ? 1 : 0);
    putF64(w, prFetchBlobIsFile(blob) ? prFetchBlobFileLastModified(blob) : 0);
    putU32(w, uint32_t(type.size));
    putBytes(w, type.data, type.size);
    putU32(w, uint32_t(name.size));
    putBytes(w, name.data, name.size);
    putU32(w, uint32_t(bytes.size));
    putBytes(w, bytes.data, bytes.size);
}

bool writeObject(CloneWriter* w, JSValueConst value) {
    JSContext* ctx = w->ctx;
    if (writeSeen(w, value))
        return true;
    if (w->depth >= kCloneMaxDepth)
        return cloneFail(ctx);
    if (JS_IsFunction(ctx, value) || JS_IsProxy(value))
        return cloneFail(ctx);
    if (jsHtmlMessagePortOf(ctx, value) != nullptr)
        return writePort(w, value) || cloneFail(ctx);
    if (const PrFetchBlob* blob = prJsFetchBlobFromValue(ctx, value)) {
        writeBlob(w, blob);
        return true;
    }
    ++w->depth;
    bool ok = false;
    switch (JS_GetCloneKind(value)) {
        case JS_CLONE_LEAF:
            ok = writeEngine(w, CloneTag_Leaf, value);
            break;
        case JS_CLONE_ARRAY_BUFFER: {
            size_t size = 0;
            uint8_t* data = JS_GetArrayBuffer(ctx, &size, value);
            if (data == nullptr && JS_HasException(ctx)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                ok = cloneFail(ctx);
            } else {
                ok = writeEngine(w, CloneTag_Leaf, value);
            }
            break;
        }
        case JS_CLONE_ARRAY: {
            int64_t length = 0;
            ok = JS_GetLength(ctx, value, &length) == 0;
            if (ok) {
                putTag(w, CloneTag_Array);
                putU32(w, uint32_t(length));
                ok = writeProperties(w, value);
            }
            break;
        }
        case JS_CLONE_PLAIN:
            putTag(w, CloneTag_Object);
            ok = writeProperties(w, value);
            break;
        case JS_CLONE_MAP:
            ok = writeMap(w, value, false);
            break;
        case JS_CLONE_SET:
            ok = writeMap(w, value, true);
            break;
        case JS_CLONE_TYPED_ARRAY:
            ok = writeView(w, value, false);
            break;
        case JS_CLONE_DATA_VIEW:
            ok = writeView(w, value, true);
            break;
        case JS_CLONE_ERROR:
            ok = writeError(w, value);
            break;
        default:
            ok = cloneFail(ctx);
            break;
    }
    --w->depth;
    return ok;
}

bool writeValue(CloneWriter* w, JSValueConst value) {
    switch (JS_VALUE_GET_NORM_TAG(value)) {
        case JS_TAG_UNDEFINED:
            putTag(w, CloneTag_Undefined);
            return true;
        case JS_TAG_NULL:
            putTag(w, CloneTag_Null);
            return true;
        case JS_TAG_BOOL:
            putTag(w, JS_VALUE_GET_BOOL(value) ? CloneTag_True : CloneTag_False);
            return true;
        case JS_TAG_INT:
        case JS_TAG_FLOAT64: {
            double number = 0;
            JS_ToFloat64(w->ctx, &number, value);
            putTag(w, CloneTag_Number);
            putF64(w, number);
            return true;
        }
        case JS_TAG_STRING:
        case JS_TAG_BIG_INT:
            return writeEngine(w, CloneTag_Primitive, value);
        case JS_TAG_OBJECT:
            return writeObject(w, value);
        default:
            return cloneFail(w->ctx);
    }
}

struct CloneReader {
    JSContext* ctx;
    const char* at;
    const char* end;
    JSValueConst ports;
    Array<JSValue> memory;      // the objects read so far, by number, each with a reference
};

bool getBytes(CloneReader* r, void* out, size_t size) {
    if (size_t(r->end - r->at) < size)
        return false;
    memcpy(out, r->at, size);
    r->at += size;
    return true;
}

JSValue corrupt(JSContext* ctx) {
    return JS_ThrowTypeError(ctx, "A serialized value is corrupt");
}

JSValue readValue(CloneReader* r);

// Records `object`, which the reader holds, and returns it.
JSValue remember(CloneReader* r, JSValue object) {
    r->memory.push(JS_DupValue(r->ctx, object));
    return object;
}

JSValue readEngine(CloneReader* r) {
    uint32_t size = 0;
    if (!getBytes(r, &size, sizeof(size)) || size_t(r->end - r->at) < size)
        return corrupt(r->ctx);
    JSValue value = JS_ReadObject(r->ctx, reinterpret_cast<const uint8_t*>(r->at), size, 0);
    r->at += size;
    return value;
}

// Reads a count, and that many keys and values, into `object`.
bool readProperties(CloneReader* r, JSValueConst object) {
    uint32_t count = 0;
    if (!getBytes(r, &count, sizeof(count))) {
        corrupt(r->ctx);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        JSValue key = readValue(r);
        JSValue item = JS_IsException(key) ? JS_EXCEPTION : readValue(r);
        if (JS_IsException(item)) {
            JS_FreeValue(r->ctx, key);
            return false;
        }
        JSAtom atom = JS_ValueToAtom(r->ctx, key);
        JS_FreeValue(r->ctx, key);
        int done = JS_DefinePropertyValue(r->ctx, object, atom, item, JS_PROP_C_W_E);
        JS_FreeAtom(r->ctx, atom);
        if (done < 0)
            return false;
    }
    return true;
}

JSValue readMap(CloneReader* r, bool isSet) {
    uint32_t count = 0;
    if (!getBytes(r, &count, sizeof(count)))
        return corrupt(r->ctx);
    JSValue map = remember(r, JS_NewMapObject(r->ctx, isSet));
    for (uint32_t i = 0; i < count; ++i) {
        JSValue key = readValue(r);
        JSValue item = isSet || JS_IsException(key) ? JS_UNDEFINED : readValue(r);
        bool ok = !JS_IsException(key) && !JS_IsException(item) && JS_MapAppend(r->ctx, map, key, item) == 0;
        JS_FreeValue(r->ctx, key);
        JS_FreeValue(r->ctx, item);
        if (!ok) {
            JS_FreeValue(r->ctx, map);
            return JS_EXCEPTION;
        }
    }
    return map;
}

JSValue readError(CloneReader* r) {
    JSContext* ctx = r->ctx;
    uint32_t kind = 0;
    if (!getBytes(r, &kind, sizeof(kind)) || kind > kErrorNameCount)
        return corrupt(ctx);
    JSValue error = remember(r, kind == kErrorNameCount ? JS_NewError(ctx) : JS_NewNativeError(ctx, int(kind)));
    constexpr const char* kOwn[] = {"message", "stack", "cause"};
    for (const char* member : kOwn) {
        JSValue text = readValue(r);
        if (JS_IsException(text)) {
            JS_FreeValue(ctx, error);
            return text;
        }
        if (JS_IsUndefined(text))
            continue;
        JS_DefinePropertyValueStr(ctx, error, member, text, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    return error;
}

JSValue readView(CloneReader* r, bool isDataView) {
    JSContext* ctx = r->ctx;
    uint32_t type = 0;
    double offset = 0;
    double length = 0;
    uint32_t track = 0;
    if (!getBytes(r, &type, sizeof(type)) || !getBytes(r, &offset, sizeof(offset)) ||
            !getBytes(r, &length, sizeof(length)) || !getBytes(r, &track, sizeof(track)))
        return corrupt(ctx);
    // The view takes its number before its buffer, as the writer met them.
    uint32_t number = r->memory.count;
    r->memory.push(JS_UNDEFINED);
    JSValue buffer = readValue(r);
    if (JS_IsException(buffer))
        return buffer;
    JSValue view;
    if (isDataView) {
        view = JS_NewDataView(ctx, buffer, size_t(offset), size_t(length), track != 0);
    } else {
        // An undefined length is the one that tracks the buffer.
        JSValue count = track != 0 ? JS_UNDEFINED : JS_NewFloat64(ctx, length);
        JSValueConst args[] = {buffer, JS_NewFloat64(ctx, offset), count};
        view = JS_NewTypedArray(ctx, 3, args, JSTypedArrayEnum(type));
    }
    JS_FreeValue(ctx, buffer);
    if (!JS_IsException(view))
        r->memory.data[number] = JS_DupValue(ctx, view);
    return view;
}

// Reads a size and returns that many bytes of the stream as a view.
bool getView(CloneReader* r, PrStringView* out) {
    uint32_t size = 0;
    if (!getBytes(r, &size, sizeof(size)) || size_t(r->end - r->at) < size)
        return false;
    *out = PrStringView{r->at, size};
    r->at += size;
    return true;
}

JSValue readBlob(CloneReader* r) {
    uint32_t isFile = 0;
    double lastModified = 0;
    PrStringView type;
    PrStringView name;
    PrStringView bytes;
    if (!getBytes(r, &isFile, sizeof(isFile)) || !getBytes(r, &lastModified, sizeof(lastModified)) ||
            !getView(r, &type) || !getView(r, &name) || !getView(r, &bytes))
        return corrupt(r->ctx);
    PrByteView content{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    const PrFetchBlob* blob =
        isFile != 0 ? prFetchBlobCreateFile(content, type, name, lastModified) : prFetchBlobCreate(content, type);
    return remember(r, prJsFetchBlobNew(r->ctx, blob));
}

JSValue readValue(CloneReader* r) {
    JSContext* ctx = r->ctx;
    CloneTag tag = 0;
    if (!getBytes(r, &tag, 1))
        return corrupt(ctx);
    switch (tag) {
        case CloneTag_Undefined:
            return JS_UNDEFINED;
        case CloneTag_Null:
            return JS_NULL;
        case CloneTag_True:
            return JS_TRUE;
        case CloneTag_False:
            return JS_FALSE;
        case CloneTag_Number: {
            double number = 0;
            return getBytes(r, &number, sizeof(number)) ? JS_NewFloat64(ctx, number) : corrupt(ctx);
        }
        case CloneTag_Primitive:
            return readEngine(r);
        case CloneTag_Leaf: {
            JSValue object = readEngine(r);
            return JS_IsException(object) ? object : remember(r, object);
        }
        case CloneTag_Reference: {
            uint32_t number = 0;
            if (!getBytes(r, &number, sizeof(number)) || number >= r->memory.count)
                return corrupt(ctx);
            return JS_DupValue(ctx, r->memory.data[number]);
        }
        case CloneTag_Array: {
            uint32_t length = 0;
            if (!getBytes(r, &length, sizeof(length)))
                return corrupt(ctx);
            JSValue array = remember(r, JS_NewArray(ctx));
            if (JS_SetLength(ctx, array, length) < 0 || !readProperties(r, array)) {
                JS_FreeValue(ctx, array);
                return JS_EXCEPTION;
            }
            return array;
        }
        case CloneTag_Object: {
            JSValue object = remember(r, JS_NewObject(ctx));
            if (!readProperties(r, object)) {
                JS_FreeValue(ctx, object);
                return JS_EXCEPTION;
            }
            return object;
        }
        case CloneTag_Map:
            return readMap(r, false);
        case CloneTag_Set:
            return readMap(r, true);
        case CloneTag_TypedArray:
            return readView(r, false);
        case CloneTag_DataView:
            return readView(r, true);
        case CloneTag_Error:
            return readError(r);
        case CloneTag_Blob:
            return readBlob(r);
        case CloneTag_Port: {
            uint32_t position = 0;
            if (!getBytes(r, &position, sizeof(position)) || JS_IsUndefined(r->ports))
                return corrupt(ctx);
            // A port takes the number that the writer gave it, as any object does.
            return remember(r, JS_GetPropertyUint32(ctx, r->ports, position));
        }
        default:
            return corrupt(ctx);
    }
}

// Checks the transfer list before anything runs (html/#structuredserializewithtransfer).
bool checkTransfer(JSContext* ctx, const Array<JSValue>* transfer, const PrHtmlMessagePort* PR_NULLABLE sender) {
    for (uint32_t i = 0; i < transfer->count; ++i) {
        JSValueConst item = transfer->data[i];
        for (uint32_t k = 0; k < i; ++k) {
            if (JS_VALUE_GET_PTR(transfer->data[k]) == JS_VALUE_GET_PTR(item))
                return cloneFail(ctx);
        }
        PrHtmlMessagePort* port = jsHtmlMessagePortOf(ctx, item);
        if (port != nullptr) {
            if (port == sender || prHtmlMessagePortIsDetached(port))
                return cloneFail(ctx);
            continue;
        }
        if (JS_GetCloneKind(item) != JS_CLONE_ARRAY_BUFFER)
            return cloneFail(ctx);
        size_t size = 0;
        if (JS_GetArrayBuffer(ctx, &size, item) == nullptr && JS_HasException(ctx)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return cloneFail(ctx);
        }
    }
    return true;
}

} // namespace

bool jsHtmlTransferRead(JSContext* ctx, JSValueConst sequence, Array<JSValue>* out) {
    bool ok = jsSequence(ctx, sequence, [&](JSValue item) {
        if (!JS_IsObject(item)) {
            JS_FreeValue(ctx, item);
            JS_ThrowTypeError(ctx, "A transfer list holds an object that is not an object");
            return false;
        }
        out->push(item);
        return true;
    });
    return ok;
}

bool jsHtmlTransferOption(JSContext* ctx, JSValueConst options, Array<JSValue>* out) {
    JSValue list = JS_GetPropertyStr(ctx, options, "transfer");
    bool ok = !JS_IsException(list) && (JS_IsUndefined(list) || jsHtmlTransferRead(ctx, list, out));
    JS_FreeValue(ctx, list);
    return ok;
}

void jsHtmlTransferReset(JSContext* ctx, Array<JSValue>* transfer) {
    for (uint32_t i = 0; i < transfer->count; ++i)
        JS_FreeValue(ctx, transfer->data[i]);
    transfer->reset();
}

bool jsHtmlSerialize(JSContext* ctx, JSValueConst value, const Array<JSValue>* PR_NULLABLE transfer,
        const PrHtmlMessagePort* PR_NULLABLE sender, JsHtmlSerialized* out) {
    if (transfer != nullptr && !checkTransfer(ctx, transfer, sender))
        return false;
    CloneWriter writer{.ctx = ctx, .out = {}, .objects = {}, .index = {}, .transfer = transfer, .depth = 0};
    bool ok = writeValue(&writer, value);
    writer.objects.reset();
    writer.index.reset();
    if (!ok) {
        prTextBufferReset(&writer.out);
        return false;
    }
    out->bytes = prTextBufferTake(&writer.out);
    for (uint32_t i = 0; transfer != nullptr && i < transfer->count; ++i) {
        PrHtmlMessagePort* port = jsHtmlMessagePortOf(ctx, transfer->data[i]);
        if (port != nullptr)
            out->ports.push(prHtmlMessagePortTransfer(port));
        else
            JS_DetachArrayBuffer(ctx, transfer->data[i]);
    }
    return true;
}

void jsHtmlSerializedReset(JsHtmlSerialized* serialized) {
    prStringReset(&serialized->bytes);
    for (uint32_t i = 0; i < serialized->ports.count; ++i)
        prDomEventTargetRelease(prHtmlMessagePortAsEventTarget(serialized->ports.data[i]));
    serialized->ports.reset();
}

JSValue jsHtmlDeserialize(JSContext* ctx, PrStringView bytes, JSValueConst ports) {
    CloneReader reader{.ctx = ctx, .at = bytes.data, .end = bytes.data + bytes.size, .ports = ports, .memory = {}};
    JSValue value = readValue(&reader);
    for (uint32_t i = 0; i < reader.memory.count; ++i)
        JS_FreeValue(ctx, reader.memory.data[i]);
    reader.memory.reset();
    return value;
}

} // namespace pr_impl
