// JavaScript binding of Blob and File (FileAPI/#blob-section).

#include "pr/fetch_blob_quickjs.h"

#include "pr/charset.h"
#include "pr/count_of.h"
#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/quickjs_binding.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

namespace {

JSClassID blobClass;
JSClassID fileClass;

double truncated(double value) {
    return value < 0 ? -prF64Floor(-value) : prF64Floor(value);
}

void releaseBlob(const PrFetchBlob* PR_NULLABLE blob) {
    prFetchBlobRelease(blob);
}

constexpr JsReferences kBlobReferences = {
    [](void* handle) { prFetchBlobRetain(static_cast<const PrFetchBlob*>(handle)); },
    [](void* handle) { prFetchBlobRelease(static_cast<const PrFetchBlob*>(handle)); },
    false,
};

// The contents of a Blob's or a File's constructor: the bytes of its parts, in order (FileAPI/#process-blob-parts), and
// the members of its property bag.
struct BlobInit {
    PrTextBuffer bytes;
    PrStringView type;
    bool nativeEndings;
    bool hasLastModified;
    double lastModified;
};

// Reads the members of a BlobPropertyBag, in lexicographic order, and a FilePropertyBag's, which follow them.
bool readBag(JSContext* ctx, JSValueConst bag, bool file, BlobInit* out) {
    bool present = false;
    if (!jsDictionary(ctx, bag, &present))
        return false;
    out->type = "";
    if (!present)
        return true;
    JSValue endings = JS_GetPropertyStr(ctx, bag, "endings");
    if (JS_IsException(endings))
        return false;
    if (!JS_IsUndefined(endings)) {
        PrStringView text;
        bool read = jsReadString(ctx, endings, &text);
        JS_FreeValue(ctx, endings);
        if (!read)
            return false;
        if (text != "transparent" && text != "native") {
            JS_ThrowTypeError(ctx, "'endings' is not a valid value");
            return false;
        }
        out->nativeEndings = text == "native";
    }
    JSValue type = JS_GetPropertyStr(ctx, bag, "type");
    if (JS_IsException(type))
        return false;
    if (!JS_IsUndefined(type)) {
        bool read = jsReadString(ctx, type, &out->type);
        JS_FreeValue(ctx, type);
        if (!read)
            return false;
    }
    if (file) {
        JSValue modified = JS_GetPropertyStr(ctx, bag, "lastModified");
        if (JS_IsException(modified))
            return false;
        if (!JS_IsUndefined(modified)) {
            bool read = JS_ToFloat64(ctx, &out->lastModified, modified) >= 0;
            JS_FreeValue(ctx, modified);
            if (!read)
                return false;
            // A long long (webidl/#abstract-opdef-converttoint): a value that is not finite is 0.
            out->lastModified = prF64IsFinite(out->lastModified) ? truncated(out->lastModified) : 0;
            out->hasLastModified = true;
        }
    }
    return true;
}

// Appends the bytes of one part, with its text's line endings converted if the bag asked for that.
bool appendPart(JSContext* ctx, JSValueConst part, BlobInit* init, PrTextBuffer* out) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (jsBufferSource(ctx, part, &data, &size)) {
        if (size != 0)
            prTextBufferAppend(out, PrStringView{reinterpret_cast<const char*>(data), size});
        return true;
    }
    if (const PrFetchBlob* blob = prJsFetchBlobFromValue(ctx, part)) {
        PrByteView bytes = prFetchBlobBytes(blob);
        prTextBufferAppend(out, PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size});
        return true;
    }
    PrStringView text;
    if (!jsReadString(ctx, part, &text))
        return false;
    if (init->nativeEndings) {
        PrRawString converted = prFetchBlobConvertLineEndings(text);
        prTextBufferAppend(out, prRawStringView(&converted));
        prRawStringReset(&converted);
    } else {
        prTextBufferAppend(out, text);
    }
    return true;
}

// Reads the sequence of parts into `*out`. The parts are read before they are converted, since a conversion can run
// script (webidl/#es-sequence), and the bag's members decide how the strings convert.
bool readParts(JSContext* ctx, JSValueConst parts, BlobInit* init, PrTextBuffer* out) {
    if (JS_IsUndefined(parts))
        return true;
    if (!JS_IsObject(parts)) {
        JS_ThrowTypeError(ctx, "A blob's parts must be a sequence");
        return false;
    }
    RawArray<JSValue> items;
    bool ok = jsSequence(ctx, parts, [&](JSValue item) {
        items.push(item);
        return true;
    });
    // Each string part converts as it is met, so that a part that throws stops the sequence before the bag is read.
    RawArray<PrTextBuffer> pieces;
    for (uint32_t i = 0; ok && i < items.count; ++i) {
        pieces.push(PrTextBuffer{});
        ok = appendPart(ctx, items.data[i], init, &pieces.data[i]);
    }
    for (uint32_t i = 0; i < items.count; ++i)
        JS_FreeValue(ctx, items.data[i]);
    items.reset();
    for (uint32_t i = 0; i < pieces.count; ++i) {
        prTextBufferAppend(out, prTextBufferView(&pieces.data[i]));
        prTextBufferReset(&pieces.data[i]);
    }
    pieces.reset();
    return ok;
}

PrByteView byteViewOf(const PrTextBuffer* buffer) {
    PrStringView view = prTextBufferView(buffer);
    return PrByteView{reinterpret_cast<const uint8_t*>(view.data), view.size};
}

JSValue blobConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    BlobInit init{};
    bool ok = readBag(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, false, &init) &&
        readParts(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &init, &init.bytes);
    if (!ok) {
        prTextBufferReset(&init.bytes);
        return JS_EXCEPTION;
    }
    const PrFetchBlob* blob = prFetchBlobCreate(byteViewOf(&init.bytes), init.type);
    prTextBufferReset(&init.bytes);
    return jsConstructed(ctx, newTarget, prJsFetchBlobNew(ctx, blob));
}

JSValue fileConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    if (!jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    BlobInit init{};
    PrStringView name;
    bool ok = jsReadString(ctx, argv[1], &name) && readBag(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, true, &init) &&
        readParts(ctx, argv[0], &init, &init.bytes);
    if (!ok) {
        prTextBufferReset(&init.bytes);
        return JS_EXCEPTION;
    }
    // The time of construction is the realm's current time, as Date reads it.
    double modified = init.hasLastModified ? init.lastModified : double(JS_DateNow(ctx));
    const PrFetchBlob* blob = prFetchBlobCreateFile(byteViewOf(&init.bytes), init.type, name, modified);
    prTextBufferReset(&init.bytes);
    return jsConstructed(ctx, newTarget, prJsFetchBlobNew(ctx, blob));
}

const PrFetchBlob* PR_NULLABLE thisBlob(JSContext* ctx, JSValueConst object) {
    return jsThis<PrFetchBlob>(ctx, object, blobClass);
}

JSValue blobSize(JSContext* ctx, JSValueConst blobObject) {
    const PrFetchBlob* blob = thisBlob(ctx, blobObject);
    return blob != nullptr ? JS_NewFloat64(ctx, double(prFetchBlobBytes(blob).size)) : JS_EXCEPTION;
}

JSValue blobType(JSContext* ctx, JSValueConst blobObject) {
    const PrFetchBlob* blob = thisBlob(ctx, blobObject);
    return blob != nullptr ? jsNewStringView(ctx, prFetchBlobType(blob)) : JS_EXCEPTION;
}

// A long long argument (webidl/#abstract-opdef-converttoint), which `fallback` stands for if it is undefined.
bool readPosition(JSContext* ctx, int argc, JSValueConst* argv, int index, int64_t fallback, int64_t* out) {
    *out = fallback;
    if (index >= argc || JS_IsUndefined(argv[index]))
        return true;
    double value = 0;
    if (JS_ToFloat64(ctx, &value, argv[index]) < 0)
        return false;
    constexpr double kLimit = 9007199254740992.0;
    if (!prF64IsFinite(value))
        value = value > 0 ? kLimit : (value < 0 ? -kLimit : 0);
    *out = int64_t(truncated(value));
    return true;
}

// slice(start, end, contentType) (FileAPI/#dfn-slice).
JSValue blobSlice(JSContext* ctx, JSValueConst blobObject, int argc, JSValueConst* argv) {
    const PrFetchBlob* blob = thisBlob(ctx, blobObject);
    if (blob == nullptr)
        return JS_EXCEPTION;
    int64_t start = 0;
    int64_t end = 0;
    PrStringView type = "";
    int64_t size = int64_t(prFetchBlobBytes(blob).size);
    if (!readPosition(ctx, argc, argv, 0, 0, &start) || !readPosition(ctx, argc, argv, 1, size, &end))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !jsReadString(ctx, argv[2], &type))
        return JS_EXCEPTION;
    return prJsFetchBlobNew(ctx, prFetchBlobSlice(blob, start, end, type));
}

enum {
    BlobRead_Text,
    BlobRead_ArrayBuffer,
    BlobRead_Bytes,
};
typedef uint8_t BlobRead;

// text(), arrayBuffer() and bytes(), selected by the magic. A blob's bytes are all here, so each settles at once.
JSValue blobRead(JSContext* ctx, JSValueConst blobObject, int, JSValueConst*, int magic) {
    const PrFetchBlob* blob = thisBlob(ctx, blobObject);
    if (blob == nullptr) {
        JSValue error = JS_GetException(ctx);
        JSValue rejected = jsSettledPromise(ctx, error, true);
        JS_FreeValue(ctx, error);
        return rejected;
    }
    PrByteView bytes = prFetchBlobBytes(blob);
    JSValue value = JS_UNDEFINED;
    if (magic == BlobRead_ArrayBuffer) {
        value = JS_NewArrayBufferCopy(ctx, bytes.data, bytes.size);
    } else if (magic == BlobRead_Bytes) {
        value = JS_NewUint8ArrayCopy(ctx, bytes.data, bytes.size);
    } else {
        PrCharsetDecoder decoder = prCharsetDecoderMake(PrCharset_Utf8, 0);
        PrRawString decoded = prRawStringNull();
        PrStringView text =
            prCharsetDecodeView(&decoder, PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size},
                PrCharsetInput_Final, &decoded);
        value = jsNewStringView(ctx, text);
        prRawStringReset(&decoded);
    }
    JSValue promise = jsSettledPromise(ctx, value, false);
    JS_FreeValue(ctx, value);
    return promise;
}

JSValue fileName(JSContext* ctx, JSValueConst blobObject) {
    const PrFetchBlob* blob = jsThis<PrFetchBlob>(ctx, blobObject, fileClass);
    return blob != nullptr ? jsNewStringView(ctx, prFetchBlobFileName(blob)) : JS_EXCEPTION;
}

JSValue fileLastModified(JSContext* ctx, JSValueConst blobObject) {
    const PrFetchBlob* blob = jsThis<PrFetchBlob>(ctx, blobObject, fileClass);
    return blob != nullptr ? JS_NewFloat64(ctx, prFetchBlobFileLastModified(blob)) : JS_EXCEPTION;
}

JSValue fileRelativePath(JSContext* ctx, JSValueConst object) {
    return jsThis<PrFetchBlob>(ctx, object, fileClass) != nullptr ? jsNewStringView(ctx, "") : JS_EXCEPTION;
}

constexpr JSCFunctionListEntry kBlobProto[] = {
    PR_JS_TAG("Blob"),
    PR_JS_GET("size", blobSize),
    PR_JS_GET("type", blobType),
    PR_JS_METHOD("slice", 0, blobSlice),
    PR_JS_MAGIC("text", 0, blobRead, BlobRead_Text),
    PR_JS_MAGIC("arrayBuffer", 0, blobRead, BlobRead_ArrayBuffer),
    PR_JS_MAGIC("bytes", 0, blobRead, BlobRead_Bytes),
};

constexpr JSCFunctionListEntry kFileProto[] = {
    PR_JS_TAG("File"),
    PR_JS_GET("name", fileName),
    PR_JS_GET("lastModified", fileLastModified),
    PR_JS_GET("webkitRelativePath", fileRelativePath),
};

constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("Blob", blobClass, (jsFinalizer<&blobClass, releaseBlob>), kBlobProto),
        .constructor = blobConstruct,
        .references = &kBlobReferences,
    },
    {
        PR_JS_CLASS("File", fileClass, (jsFinalizer<&fileClass, releaseBlob>), kFileProto),
        .constructor = fileConstruct,
        .length = 2,
        .base = &blobClass,
    },
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prJsFetchBlobBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
}

const PrFetchBlob* PR_NULLABLE prJsFetchBlobFromValue(JSContext* ctx, JSValueConst value) {
    return jsHandleAs<PrFetchBlob>(ctx, value, blobClass);
}

JSValue prJsFetchBlobNew(JSContext* ctx, const PrFetchBlob* blob) {
    JSClassID id = prFetchBlobIsFile(blob) ? fileClass : blobClass;
    JSValue object = jsWrap(ctx, id, JS_UNDEFINED, const_cast<PrFetchBlob*>(blob), true);
    if (JS_IsException(object))
        prFetchBlobRelease(blob);
    return object;
}
