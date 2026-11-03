// JavaScript binding of FileReader (FileAPI/#APIASynch).

#include "xhr/quickjs/src/xhr_file_reader_quickjs.h"

#include "pr/count_of.h"
#include "pr/dom_event_handler.h"
#include "pr/dom_quickjs.h"
#include "pr/fetch_blob_quickjs.h"
#include "pr/html.h"
#include "pr/quickjs_binding.h"
#include "pr/webidl_quickjs.h"
#include "pr/xhr_file_reader.h"

#include "xhr/quickjs/src/xhr_quickjs.h"

namespace pr_impl {

namespace {

JSClassID fileReaderClass;

// The slots of a reader's object, which hold the ArrayBuffer of its result and the result's version.
enum {
    ReaderSlot_Buffer,
    ReaderSlot_Version,
};
typedef uint8_t ReaderSlot;

void readerPending(void* PR_NULLABLE context, PrXhrFileReader* reader, bool pending) {
    jsPin(static_cast<JSRuntime*>(context), prJsDomEventTargetClass, prXhrFileReaderAsEventTarget(reader), pending);
}

constexpr JsReferences kReaderReferences = {
    [](void* handle) { prDomEventTargetRetain(static_cast<PrDomEventTarget*>(handle)); },
    [](void* handle) { prDomEventTargetRelease(static_cast<PrDomEventTarget*>(handle)); },
    true,
};

void releaseReader(PrXhrFileReader* PR_NULLABLE reader) {
    prDomEventTargetRelease(prXhrFileReaderAsEventTarget(reader));
}

JSValue readerConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    PrHtmlWindow* window = document != nullptr ? prHtmlDocumentDefaultView(document) : nullptr;
    if (window == nullptr)
        return JS_ThrowTypeError(ctx, "FileReader needs a window");
    PrXhrClient client = jsXhrWindowClient(window);
    PrXhrFileReader* reader = prXhrFileReaderCreate(&client, readerPending, JS_GetRuntime(ctx));
    JSValue object = jsWrap(ctx, fileReaderClass, JS_UNDEFINED, prXhrFileReaderAsEventTarget(reader), true);
    if (JS_IsException(object)) {
        releaseReader(reader);
        return object;
    }
    return jsConstructed(ctx, newTarget, object);
}

// readAsArrayBuffer, readAsBinaryString, readAsText and readAsDataURL, with the format as the magic.
JSValue readerRead(JSContext* ctx, JSValueConst readerObject, int argc, JSValueConst* argv, int magic) {
    PrXhrFileReader* reader = jsThis<PrXhrFileReader>(ctx, readerObject, fileReaderClass);
    if (reader == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    const PrFetchBlob* blob = prJsFetchBlobFromValue(ctx, argv[0]);
    if (blob == nullptr)
        return JS_ThrowTypeError(ctx, "The argument is not a Blob");
    PrStringView encoding{nullptr, 0};
    if (magic == PrXhrFileReaderFormat_Text && argc > 1 && !JS_IsUndefined(argv[1]) &&
            !jsReadString(ctx, argv[1], &encoding))
        return JS_EXCEPTION;
    if (!prXhrFileReaderRead(reader, blob, PrXhrFileReaderFormat(magic), encoding))
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return JS_UNDEFINED;
}

JSValue readerAbort(JSContext* ctx, JSValueConst readerObject, int, JSValueConst*) {
    PrXhrFileReader* reader = jsThis<PrXhrFileReader>(ctx, readerObject, fileReaderClass);
    if (reader == nullptr)
        return JS_EXCEPTION;
    prXhrFileReaderAbort(reader);
    return JS_UNDEFINED;
}

JSValue readerReadyState(JSContext* ctx, JSValueConst readerObject) {
    PrXhrFileReader* reader = jsThis<PrXhrFileReader>(ctx, readerObject, fileReaderClass);
    return reader != nullptr ? JS_NewInt32(ctx, prXhrFileReaderReadyState(reader)) : JS_EXCEPTION;
}

// result, of which an ArrayBuffer is the same object until the result changes.
JSValue readerResult(JSContext* ctx, JSValueConst readerObject) {
    PrXhrFileReader* reader = jsThis<PrXhrFileReader>(ctx, readerObject, fileReaderClass);
    if (reader == nullptr)
        return JS_EXCEPTION;
    PrXhrFileReaderFormat format = 0;
    if (!prXhrFileReaderResultFormat(reader, &format))
        return JS_NULL;
    if (format != PrXhrFileReaderFormat_ArrayBuffer)
        return jsNewStringView(ctx, prXhrFileReaderResultText(reader));
    uint32_t version = prXhrFileReaderResultVersion(reader);
    JSValue held = jsReference(ctx, readerObject, ReaderSlot_Version);
    uint32_t heldVersion = 0;
    bool current = !JS_IsUndefined(held) && JS_ToUint32(ctx, &heldVersion, held) >= 0 && heldVersion == version;
    JS_FreeValue(ctx, held);
    if (current)
        return jsReference(ctx, readerObject, ReaderSlot_Buffer);
    PrByteView bytes = prXhrFileReaderResultBytes(reader);
    JSValue buffer = JS_NewArrayBufferCopy(ctx, bytes.data, bytes.size);
    JSValue number = JS_NewUint32(ctx, version);
    jsSetReference(ctx, readerObject, ReaderSlot_Buffer, buffer);
    jsSetReference(ctx, readerObject, ReaderSlot_Version, number);
    JS_FreeValue(ctx, number);
    return buffer;
}

// error, which stays null: a blob's bytes are in memory, so no read fails (FileAPI/#APIASynch).
JSValue readerError(JSContext* ctx, JSValueConst object) {
    return jsThis<PrXhrFileReader>(ctx, object, fileReaderClass) != nullptr ? JS_NULL : JS_EXCEPTION;
}

constexpr JSCFunctionListEntry kReaderProto[] = {
    PR_JS_TAG("FileReader"),
    PR_JS_MAGIC("readAsArrayBuffer", 1, readerRead, PrXhrFileReaderFormat_ArrayBuffer),
    PR_JS_MAGIC("readAsBinaryString", 1, readerRead, PrXhrFileReaderFormat_BinaryString),
    PR_JS_MAGIC("readAsText", 1, readerRead, PrXhrFileReaderFormat_Text),
    PR_JS_MAGIC("readAsDataURL", 1, readerRead, PrXhrFileReaderFormat_DataUrl),
    PR_JS_METHOD("abort", 0, readerAbort),
    PR_JS_GET("readyState", readerReadyState),
    PR_JS_GET("result", readerResult),
    PR_JS_GET("error", readerError),
    PR_JS_DOM_EVENT_HANDLER(onloadstart),
    PR_JS_DOM_EVENT_HANDLER(onprogress),
    PR_JS_DOM_EVENT_HANDLER(onload),
    PR_JS_DOM_EVENT_HANDLER(onabort),
    PR_JS_DOM_EVENT_HANDLER(onerror),
    PR_JS_DOM_EVENT_HANDLER(onloadend),
};

constexpr JsConstant kReaderConstants[] = {
    {"EMPTY", PrXhrFileReaderState_Empty},
    {"LOADING", PrXhrFileReaderState_Loading},
    {"DONE", PrXhrFileReaderState_Done},
};

constexpr JsClassDefinition kClass = {
    PR_JS_CLASS("FileReader", fileReaderClass, (jsFinalizer<&fileReaderClass, releaseReader>), kReaderProto),
    .constants = kReaderConstants,
    .constantCount = int(countOf(kReaderConstants)),
    .constructor = readerConstruct,
    .base = &prJsDomEventTargetClass,
    .references = &kReaderReferences,
};

} // namespace

void jsXhrFileReaderBind(JSContext* ctx) {
    JS_FreeValue(ctx, jsDefineClass(ctx, kClass));
}

} // namespace pr_impl
