// JavaScript binding of Media Source Extensions (media-source-2/).

#include "html/quickjs/src/html_media_source_quickjs.h"

#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/quickjs_binding.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

namespace pr_impl {

JSClassID htmlMediaSourceClass;
JSClassID htmlSourceBufferClass;
JSClassID htmlSourceBufferListClass;

namespace {

constexpr JsReferences kCountedReferences = {
    [](void* handle) { prDomEventTargetRetain(static_cast<PrDomEventTarget*>(handle)); },
    [](void* handle) { prDomEventTargetRelease(static_cast<PrDomEventTarget*>(handle)); },
    true,
};

// A list's object holds a reference to its media source, as the object of any part does.
constexpr JsReferences kPartReferences = {
    [](void* handle) { prDomEventTargetRetain(static_cast<PrDomEventTarget*>(handle)); },
    [](void* handle) { prDomEventTargetRelease(static_cast<PrDomEventTarget*>(handle)); },
    false,
};

void releaseTarget(PrDomEventTarget* PR_NULLABLE target) {
    prDomEventTargetRelease(target);
}

void releaseSource(PrHtmlMediaSource* PR_NULLABLE source) {
    releaseTarget(source != nullptr ? prHtmlMediaSourceAsEventTarget(source) : nullptr);
}

void releaseBuffer(PrHtmlSourceBuffer* PR_NULLABLE buffer) {
    releaseTarget(buffer != nullptr ? prHtmlSourceBufferAsEventTarget(buffer) : nullptr);
}

// Throws the exception of a status that the media source API returns (pr/html_media_source.h).
JSValue throwStatus(JSContext* ctx, PrStatus status) {
    switch (status) {
        case PrStatus_Ok: return JS_UNDEFINED;
        case PrStatus_OutOfRange: return JS_ThrowTypeError(ctx, "The value is out of range");
        case PrStatus_NotSupported: return jsDomExceptionThrow(ctx, "NotSupportedError");
        case PrStatus_Limit: return jsDomExceptionThrow(ctx, "QuotaExceededError");
        case PrStatus_NotFound: return jsDomExceptionThrow(ctx, "NotFoundError");
        default: return jsDomExceptionThrow(ctx, "InvalidStateError");
    }
}

PrHtmlMediaSource* PR_NULLABLE sourceThis(JSContext* ctx, JSValueConst object) {
    return jsThis<PrHtmlMediaSource>(ctx, object, htmlMediaSourceClass);
}

PrHtmlSourceBuffer* PR_NULLABLE bufferThis(JSContext* ctx, JSValueConst object) {
    return jsThis<PrHtmlSourceBuffer>(ctx, object, htmlSourceBufferClass);
}

JSValue bufferToJs(JSContext* ctx, PrHtmlSourceBuffer* buffer) {
    return jsWrap(ctx, htmlSourceBufferClass, JS_UNDEFINED, prHtmlSourceBufferAsEventTarget(buffer), false);
}

JSValue listToJs(JSContext* ctx, JSValueConst owner, PrHtmlSourceBufferList* list) {
    return jsWrap(ctx, htmlSourceBufferListClass, owner, prHtmlSourceBufferListAsEventTarget(list), false);
}

// ---------------------------------------------------------------------------------------------------------------------
// MediaSource

JSValue sourceConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    PrHtmlWindow* window = jsHtmlGlobalWindow(ctx);
    if (window == nullptr)
        return JS_ThrowTypeError(ctx, "MediaSource needs a window");
    PrHtmlMediaSource* source = prHtmlMediaSourceCreate(window);
    JSValue object = jsWrap(ctx, htmlMediaSourceClass, JS_UNDEFINED, prHtmlMediaSourceAsEventTarget(source), true);
    if (JS_IsException(object))
        releaseSource(source);
    return jsConstructed(ctx, newTarget, object);
}

// MediaSource.isTypeSupported (media-source-2/#dom-mediasource-istypesupported).
JSValue sourceIsTypeSupported(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    PrHtmlWindow* window = jsHtmlGlobalWindow(ctx);
    return JS_NewBool(ctx, window != nullptr && prHtmlMediaSourceIsTypeSupported(window, type.value()));
}

// MediaSource.canConstructInDedicatedWorker: the engine runs no workers.
JSValue sourceCanConstructInWorker(JSContext*, JSValueConst) {
    return JS_FALSE;
}

// sourceBuffers and activeSourceBuffers, selected by the magic.
JSValue sourceLists(JSContext* ctx, JSValueConst sourceObject, int magic) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    if (source == nullptr)
        return JS_EXCEPTION;
    if (magic == 0)
        return listToJs(ctx, sourceObject, prHtmlMediaSourceSourceBuffers(source));
    return listToJs(ctx, sourceObject, prHtmlMediaSourceActiveSourceBuffers(source));
}

JSValue sourceReadyState(JSContext* ctx, JSValueConst sourceObject) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    if (source == nullptr)
        return JS_EXCEPTION;
    constexpr const char* kStates[] = {"closed", "open", "ended"};
    return JS_NewString(ctx, kStates[prHtmlMediaSourceReadyState(source)]);
}

JSValue sourceDuration(JSContext* ctx, JSValueConst sourceObject) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    return source != nullptr ? JS_NewFloat64(ctx, prHtmlMediaSourceDuration(source)) : JS_EXCEPTION;
}

JSValue sourceSetDuration(JSContext* ctx, JSValueConst sourceObject, JSValueConst value) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    double duration = 0;
    if (source == nullptr || JS_ToFloat64(ctx, &duration, value) < 0)
        return JS_EXCEPTION;
    return throwStatus(ctx, prHtmlMediaSourceSetDuration(source, duration));
}

JSValue sourceAddSourceBuffer(JSContext* ctx, JSValueConst sourceObject, int argc, JSValueConst* argv) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    StringArgument type = {ctx};
    if (source == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    PrHtmlSourceBuffer* buffer = nullptr;
    PrStatus status = prHtmlMediaSourceAddSourceBuffer(source, type.value(), &buffer);
    if (status != PrStatus_Ok)
        return throwStatus(ctx, status);
    JSValue object = bufferToJs(ctx, buffer);
    releaseBuffer(buffer);
    return object;
}

JSValue sourceRemoveSourceBuffer(JSContext* ctx, JSValueConst sourceObject, int argc, JSValueConst* argv) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    if (source == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrHtmlSourceBuffer* buffer = jsHandleAs<PrHtmlSourceBuffer>(ctx, argv[0], htmlSourceBufferClass);
    if (buffer == nullptr)
        return JS_ThrowTypeError(ctx, "The argument is not a SourceBuffer");
    return throwStatus(ctx, prHtmlMediaSourceRemoveSourceBuffer(source, buffer));
}

JSValue sourceEndOfStream(JSContext* ctx, JSValueConst sourceObject, int argc, JSValueConst* argv) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    if (source == nullptr)
        return JS_EXCEPTION;
    PrHtmlEndOfStreamError error = PrHtmlEndOfStreamError_None;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        StringArgument name = {ctx};
        if (!name.read(argv[0]))
            return JS_EXCEPTION;
        if (name.value() == "network")
            error = PrHtmlEndOfStreamError_Network;
        else if (name.value() == "decode")
            error = PrHtmlEndOfStreamError_Decode;
        else
            return JS_ThrowTypeError(ctx, "The value is not an EndOfStreamError");
    }
    return throwStatus(ctx, prHtmlMediaSourceEndOfStream(source, error));
}

JSValue sourceSetLiveSeekableRange(JSContext* ctx, JSValueConst sourceObject, int argc, JSValueConst* argv) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    double start = 0;
    double end = 0;
    if (source == nullptr || !jsArgCount(ctx, argc, 2) || JS_ToFloat64(ctx, &start, argv[0]) < 0 ||
            JS_ToFloat64(ctx, &end, argv[1]) < 0)
        return JS_EXCEPTION;
    if (!isFiniteF64(start) || !isFiniteF64(end))
        return JS_ThrowTypeError(ctx, "The value is not finite");
    return throwStatus(ctx, prHtmlMediaSourceSetLiveSeekableRange(source, start, end));
}

JSValue sourceClearLiveSeekableRange(JSContext* ctx, JSValueConst sourceObject, int, JSValueConst*) {
    PrHtmlMediaSource* source = sourceThis(ctx, sourceObject);
    if (source == nullptr)
        return JS_EXCEPTION;
    return throwStatus(ctx, prHtmlMediaSourceClearLiveSeekableRange(source));
}

constexpr JSCFunctionListEntry kSourceProto[] = {
    PR_JS_TAG("MediaSource"),
    PR_JS_MAGIC_GET("sourceBuffers", sourceLists, 0),
    PR_JS_MAGIC_GET("activeSourceBuffers", sourceLists, 1),
    PR_JS_GET("readyState", sourceReadyState),
    PR_JS_GET_SET("duration", sourceDuration, sourceSetDuration),
    PR_JS_METHOD("addSourceBuffer", 1, sourceAddSourceBuffer),
    PR_JS_METHOD("removeSourceBuffer", 1, sourceRemoveSourceBuffer),
    PR_JS_METHOD("endOfStream", 0, sourceEndOfStream),
    PR_JS_METHOD("setLiveSeekableRange", 2, sourceSetLiveSeekableRange),
    PR_JS_METHOD("clearLiveSeekableRange", 0, sourceClearLiveSeekableRange),
    PR_JS_DOM_EVENT_HANDLER(onsourceopen),
    PR_JS_DOM_EVENT_HANDLER(onsourceended),
    PR_JS_DOM_EVENT_HANDLER(onsourceclose),
};

constexpr JSCFunctionListEntry kSourceStatics[] = {
    PR_JS_METHOD("isTypeSupported", 1, sourceIsTypeSupported),
    PR_JS_GET("canConstructInDedicatedWorker", sourceCanConstructInWorker),
};

// ---------------------------------------------------------------------------------------------------------------------
// SourceBuffer

JSValue bufferMode(JSContext* ctx, JSValueConst bufferObject) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    if (buffer == nullptr)
        return JS_EXCEPTION;
    return JS_NewString(ctx, prHtmlSourceBufferMode(buffer) == PrMediaAppendMode_Sequence ? "sequence" : "segments");
}

// A value that names no AppendMode is ignored, as an enumeration attribute's setter ignores it
// (webidl/#es-enumeration).
JSValue bufferSetMode(JSContext* ctx, JSValueConst bufferObject, JSValueConst value) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    StringArgument name = {ctx};
    if (buffer == nullptr || !name.read(value))
        return JS_EXCEPTION;
    if (name.value() != "segments" && name.value() != "sequence")
        return JS_UNDEFINED;
    PrMediaAppendMode mode = name.value() == "sequence" ? PrMediaAppendMode_Sequence : PrMediaAppendMode_Segments;
    return throwStatus(ctx, prHtmlSourceBufferSetMode(buffer, mode));
}

JSValue bufferUpdating(JSContext* ctx, JSValueConst bufferObject) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    return buffer != nullptr ? JS_NewBool(ctx, prHtmlSourceBufferIsUpdating(buffer)) : JS_EXCEPTION;
}

JSValue bufferBuffered(JSContext* ctx, JSValueConst bufferObject) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    if (buffer == nullptr)
        return JS_EXCEPTION;
    uint32_t count = 0;
    PrStatus status = prHtmlSourceBufferBuffered(buffer, nullptr, 0, &count);
    if (status != PrStatus_Ok)
        return throwStatus(ctx, status);
    Array<PrMediaTimeRange> ranges;
    ranges.resize(count);
    prHtmlSourceBufferBuffered(buffer, ranges.data, count, &count);
    JSValue object = jsHtmlTimeRangesNew(ctx, ranges.data, ranges.count);
    ranges.reset();
    return object;
}

// timestampOffset, appendWindowStart and appendWindowEnd, selected by the magic.
JSValue bufferTime(JSContext* ctx, JSValueConst bufferObject, int magic) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    if (buffer == nullptr)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewFloat64(ctx, prHtmlSourceBufferTimestampOffset(buffer));
    if (magic == 1)
        return JS_NewFloat64(ctx, prHtmlSourceBufferAppendWindowStart(buffer));
    return JS_NewFloat64(ctx, prHtmlSourceBufferAppendWindowEnd(buffer));
}

JSValue bufferSetTime(JSContext* ctx, JSValueConst bufferObject, JSValueConst input, int magic) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    double value = 0;
    if (buffer == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    // timestampOffset and appendWindowStart are doubles, which are finite; appendWindowEnd is unrestricted.
    if (magic != 2 && !isFiniteF64(value))
        return JS_ThrowTypeError(ctx, "The value is not finite");
    if (magic == 0)
        return throwStatus(ctx, prHtmlSourceBufferSetTimestampOffset(buffer, value));
    if (magic == 1)
        return throwStatus(ctx, prHtmlSourceBufferSetAppendWindowStart(buffer, value));
    return throwStatus(ctx, prHtmlSourceBufferSetAppendWindowEnd(buffer, value));
}

JSValue bufferAppendBuffer(JSContext* ctx, JSValueConst bufferObject, int argc, JSValueConst* argv) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    if (buffer == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!jsBufferSource(ctx, argv[0], &data, &size))
        return JS_ThrowTypeError(ctx, "The argument is not a BufferSource");
    return throwStatus(ctx, prHtmlSourceBufferAppendBuffer(buffer, PrByteView{data, size}));
}

JSValue bufferAbort(JSContext* ctx, JSValueConst bufferObject, int, JSValueConst*) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    if (buffer == nullptr)
        return JS_EXCEPTION;
    return throwStatus(ctx, prHtmlSourceBufferAbort(buffer));
}

JSValue bufferRemove(JSContext* ctx, JSValueConst bufferObject, int argc, JSValueConst* argv) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    double start = 0;
    double end = 0;
    if (buffer == nullptr || !jsArgCount(ctx, argc, 2) || JS_ToFloat64(ctx, &start, argv[0]) < 0 ||
            JS_ToFloat64(ctx, &end, argv[1]) < 0)
        return JS_EXCEPTION;
    if (!isFiniteF64(start))
        return JS_ThrowTypeError(ctx, "The start is not finite");
    return throwStatus(ctx, prHtmlSourceBufferRemove(buffer, start, end));
}

JSValue bufferChangeType(JSContext* ctx, JSValueConst bufferObject, int argc, JSValueConst* argv) {
    PrHtmlSourceBuffer* buffer = bufferThis(ctx, bufferObject);
    StringArgument type = {ctx};
    if (buffer == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    return throwStatus(ctx, prHtmlSourceBufferChangeType(buffer, type.value()));
}

constexpr JSCFunctionListEntry kBufferProto[] = {
    PR_JS_TAG("SourceBuffer"),
    PR_JS_GET_SET("mode", bufferMode, bufferSetMode),
    PR_JS_GET("updating", bufferUpdating),
    PR_JS_GET("buffered", bufferBuffered),
    PR_JS_MAGIC_GET_SET("timestampOffset", bufferTime, bufferSetTime, 0),
    PR_JS_MAGIC_GET_SET("appendWindowStart", bufferTime, bufferSetTime, 1),
    PR_JS_MAGIC_GET_SET("appendWindowEnd", bufferTime, bufferSetTime, 2),
    PR_JS_METHOD("appendBuffer", 1, bufferAppendBuffer),
    PR_JS_METHOD("abort", 0, bufferAbort),
    PR_JS_METHOD("remove", 2, bufferRemove),
    PR_JS_METHOD("changeType", 1, bufferChangeType),
    PR_JS_DOM_EVENT_HANDLER(onupdatestart),
    PR_JS_DOM_EVENT_HANDLER(onupdate),
    PR_JS_DOM_EVENT_HANDLER(onupdateend),
    PR_JS_DOM_EVENT_HANDLER(onerror),
    PR_JS_DOM_EVENT_HANDLER(onabort),
};

// ---------------------------------------------------------------------------------------------------------------------
// SourceBufferList

JSValue listLength(JSContext* ctx, JSValueConst listObject) {
    PrHtmlSourceBufferList* list = jsThis<PrHtmlSourceBufferList>(ctx, listObject, htmlSourceBufferListClass);
    return list != nullptr ? JS_NewUint32(ctx, prHtmlSourceBufferListLength(list)) : JS_EXCEPTION;
}

constexpr JsIndexed kListIndexed = {
    [](void* handle) {
        return prHtmlSourceBufferListLength(static_cast<PrHtmlSourceBufferList*>(handle));
    },
    [](JSContext* ctx, JSValueConst, void* handle, uint32_t index) {
        return bufferToJs(ctx, prHtmlSourceBufferListItem(static_cast<PrHtmlSourceBufferList*>(handle), index));
    },
    nullptr,
};

constexpr JSCFunctionListEntry kListProto[] = {
    PR_JS_TAG("SourceBufferList"),
    PR_JS_GET("length", listLength),
    PR_JS_DOM_EVENT_HANDLER(onaddsourcebuffer),
    PR_JS_DOM_EVENT_HANDLER(onremovesourcebuffer),
};

constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("MediaSource", htmlMediaSourceClass, (jsFinalizer<&htmlMediaSourceClass, releaseSource>),
            kSourceProto),
        .statics = kSourceStatics,
        .staticCount = int(countOf(kSourceStatics)),
        .constructor = sourceConstruct,
        .base = &prJsDomEventTargetClass,
        .references = &kCountedReferences,
    },
    {
        PR_JS_CLASS("SourceBuffer", htmlSourceBufferClass, (jsFinalizer<&htmlSourceBufferClass, releaseBuffer>),
            kBufferProto),
        .base = &prJsDomEventTargetClass,
        .references = &kCountedReferences,
    },
    {
        PR_JS_CLASS("SourceBufferList", htmlSourceBufferListClass, jsViewFinalizer<&htmlSourceBufferListClass>,
            kListProto),
        .base = &prJsDomEventTargetClass,
        .indexed = &kListIndexed,
        .references = &kPartReferences,
    },
};

} // namespace

PrHtmlMediaSource* PR_NULLABLE jsHtmlMediaSourceOf(JSContext* ctx, JSValueConst value) {
    return jsHandleAs<PrHtmlMediaSource>(ctx, value, htmlMediaSourceClass);
}

void jsHtmlMediaSourceBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
}

} // namespace pr_impl
