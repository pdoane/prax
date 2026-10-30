// JavaScript binding of XMLHttpRequest (xhr_quickjs.h).
//
// A request's object holds the listeners of its events, so the object is pinned while the request is active
// (xhr/#garbage-collection). The response's object for arraybuffer and json is the same object each time it is read,
// until open(), send() or abort() starts over.

#include "pr/xhr_quickjs.h"

#include "pr/count_of.h"
#include "pr/dom_quickjs.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/quickjs_binding.h"
#include "pr/text_buffer.h"
#include "pr/url_quickjs.h"
#include "pr/webidl_quickjs.h"
#include "pr/xhr.h"

namespace pr_impl {

namespace {

JSClassID xhrEventTargetClass;
JSClassID xhrUploadClass;
JSClassID xhrClass;
JSClassID xhrProgressEventClass;

// The slot of a request's object that holds the response's object (xhr/#response-object).
enum { XhrSlot_Response };

// Throws the DOMException that a status of the request stands for (xhr/#dom-xmlhttprequest-open), and returns
// JS_EXCEPTION.
JSValue throwStatus(JSContext* ctx, PrStatus status) {
    switch (status) {
        case PrStatus_Syntax: return jsDomExceptionThrow(ctx, "SyntaxError");
        case PrStatus_NotAllowed: return jsDomExceptionThrow(ctx, "SecurityError");
        case PrStatus_InvalidState: return jsDomExceptionThrow(ctx, "InvalidStateError");
        default: return jsDomExceptionThrow(ctx, "NotSupportedError");
    }
}

JSValue result(JSContext* ctx, PrStatus status) {
    return status == PrStatus_Ok ? JS_UNDEFINED : throwStatus(ctx, status);
}

// Drops the response's object, which the next read makes again.
void forgetResponse(JSContext* ctx, JSValueConst self) {
    jsSetReference(ctx, self, XhrSlot_Response, JS_UNDEFINED);
}

// ---------------------------------------------------------------------------------------------------------------------
// The client of a window's requests: the window's document's base URL, and its browsing context's fetches and the
// window's timers. Its context is the window.

PrString windowBaseUrl(void* PR_NULLABLE context) {
    PrDomDocument* document = prHtmlWindowDocument(static_cast<PrHtmlWindow*>(context));
    return prDomNodeBaseUri(prDomDocumentAsNode(document));
}

// A window whose document its browsing context no longer shows fetches nothing.
PrStatus windowFetch(void* PR_NULLABLE context, const PrFetchClientRequest* request, PrFetchResponseFn done,
        void* PR_NULLABLE doneContext) {
    PrHtmlPage* page = prHtmlWindowPage(static_cast<PrHtmlWindow*>(context));
    return page != nullptr ? prHtmlPageFetch(page, request, done, doneContext) : PrStatus_Lost;
}

void windowCancelFetch(void* PR_NULLABLE context, void* PR_NULLABLE doneContext) {
    if (PrHtmlPage* page = prHtmlWindowPage(static_cast<PrHtmlWindow*>(context)))
        prHtmlPageCancelFetch(page, doneContext);
}

int32_t windowSetTimeout(void* PR_NULLABLE context, const PrXhrTask* task, uint32_t milliseconds) {
    PrHtmlTimerHandler handler{
        .invoke = task->invoke,
        .context = task->context,
        .retain = task->retain,
        .release = task->release,
    };
    return prHtmlWindowSetTimeout(static_cast<PrHtmlWindow*>(context), &handler, int32_t(milliseconds), false);
}

void windowClearTimeout(void* PR_NULLABLE context, int32_t id) {
    prHtmlWindowClearTimeout(static_cast<PrHtmlWindow*>(context), id);
}

// Pins the request's object while the request is active, and releases the pin once it is not.
void activity(void* PR_NULLABLE context, PrXhr* request, bool active) {
    jsPin(static_cast<JSRuntime*>(context), prJsDomEventTargetClass, prXhrAsEventTarget(request), active);
}

// ---------------------------------------------------------------------------------------------------------------------
// ProgressEvent (xhr/#progressevent)

JSValue progressConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    PrXhrProgressEventInit init = prXhrProgressEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    if (!jsDictionary(ctx, dictionary, &present))
        return JS_EXCEPTION;
    // The members of ProgressEventInit are read after those of EventInit, in lexicographic order
    // (webidl/#js-dictionary).
    if (present && (!prJsDomEventInitRead(ctx, dictionary, &init.event) ||
            !jsDictBool(ctx, dictionary, "lengthComputable", &init.lengthComputable) ||
            !jsDictNumber(ctx, dictionary, "loaded", &init.loaded) ||
            !jsDictNumber(ctx, dictionary, "total", &init.total)))
        return JS_EXCEPTION;
    PrDomEvent* event = prXhrProgressEventAsEvent(prXhrProgressEventCreate(type.value(), &init));
    JSValue object = jsWrap(ctx, xhrProgressEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object)) {
        prDomEventRelease(event);
        return object;
    }
    return jsConstructed(ctx, newTarget, object);
}

// The attributes of a ProgressEvent, selected by the magic: lengthComputable, then loaded and total.
JSValue progressGet(JSContext* ctx, JSValueConst self, int magic) {
    PrXhrProgressEvent* event = prXhrProgressEventQuery(jsThis<PrDomEvent>(ctx, self, xhrProgressEventClass));
    if (event == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case 0: return JS_NewBool(ctx, prXhrProgressEventLengthComputable(event));
        case 1: return JS_NewFloat64(ctx, prXhrProgressEventLoaded(event));
        default: return JS_NewFloat64(ctx, prXhrProgressEventTotal(event));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// XMLHttpRequest (xhr/#interface-xmlhttprequest)

PrXhr* PR_NULLABLE requestOf(JSContext* ctx, JSValueConst self) {
    return prXhrQuery(jsThis<PrDomEventTarget>(ctx, self, xhrClass));
}

// The request of the global's window. Throws a TypeError for a global without a window.
JSValue xhrConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    PrHtmlWindow* window = document != nullptr ? prHtmlDocumentDefaultView(document) : nullptr;
    if (window == nullptr)
        return JS_ThrowTypeError(ctx, "XMLHttpRequest needs a window");
    PrXhrDesc desc{
        .client = {
            .context = window,
            .baseUrl = windowBaseUrl,
            .fetch = windowFetch,
            .cancelFetch = windowCancelFetch,
            .setTimeout = windowSetTimeout,
            .clearTimeout = windowClearTimeout,
        },
    };
    PrXhr* request = prXhrCreate(&desc);
    JSValue object = jsWrap(ctx, xhrClass, JS_UNDEFINED, prXhrAsEventTarget(request), true);
    if (JS_IsException(object)) {
        prXhrRelease(request);
        return object;
    }
    return jsConstructed(ctx, newTarget, object);
}

JSValue xhrReadyState(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    return request != nullptr ? JS_NewInt32(ctx, prXhrReadyState(request)) : JS_EXCEPTION;
}

// open(method, url, async, username, password). A username or a password is not taken from the arguments.
JSValue xhrOpen(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrXhr* request = requestOf(ctx, self);
    StringArgument method = {ctx};
    StringArgument url = {ctx};
    if (request == nullptr || !jsArgCount(ctx, argc, 2) || !method.read(argv[0]) || !url.read(argv[1]))
        return JS_EXCEPTION;
    bool async = argc < 3 || JS_ToBool(ctx, argv[2]) != 0;
    forgetResponse(ctx, self);
    return result(ctx, prXhrOpen(request, method.value(), url.value(), async));
}

JSValue xhrSetRequestHeader(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrXhr* request = requestOf(ctx, self);
    StringArgument name = {ctx};
    StringArgument value = {ctx};
    if (request == nullptr || !jsArgCount(ctx, argc, 2) || !name.read(argv[0]) || !value.read(argv[1]))
        return JS_EXCEPTION;
    return result(ctx, prXhrSetRequestHeader(request, name.value(), value.value()));
}

JSValue xhrTimeout(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    return request != nullptr ? JS_NewUint32(ctx, prXhrTimeout(request)) : JS_EXCEPTION;
}

JSValue xhrSetTimeout(JSContext* ctx, JSValueConst self, JSValueConst value) {
    PrXhr* request = requestOf(ctx, self);
    uint32_t milliseconds = 0;
    if (request == nullptr || JS_ToUint32(ctx, &milliseconds, value) < 0)
        return JS_EXCEPTION;
    prXhrSetTimeout(request, milliseconds);
    return JS_UNDEFINED;
}

JSValue xhrWithCredentials(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    return request != nullptr ? JS_NewBool(ctx, prXhrWithCredentials(request)) : JS_EXCEPTION;
}

JSValue xhrSetWithCredentials(JSContext* ctx, JSValueConst self, JSValueConst value) {
    PrXhr* request = requestOf(ctx, self);
    if (request == nullptr)
        return JS_EXCEPTION;
    return result(ctx, prXhrSetWithCredentials(request, JS_ToBool(ctx, value) != 0));
}

// [SameObject] upload: a view of the request's upload, which the request's object holds.
JSValue xhrUpload(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    if (request == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, "upload", [&] {
        return jsWrap(ctx, xhrUploadClass, self, prXhrUploadAsEventTarget(prXhrUpload(request)), false);
    });
}

// send(body) with the body converted as fetch/#concept-bodyinit-extract extracts it: a string as UTF-8 text, a
// BufferSource as its bytes, and URLSearchParams as a form. Any other object is sent as its string.
JSValue xhrSend(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrXhr* request = requestOf(ctx, self);
    if (request == nullptr)
        return JS_EXCEPTION;
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    PrByteView bytes{nullptr, 0};
    PrStringView contentType{nullptr, 0};
    StringArgument text = {ctx};
    PrString form = prStringNull();
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (JS_IsUndefined(body) || JS_IsNull(body)) {
    } else if (jsBufferSource(ctx, body, &data, &size)) {
        bytes = PrByteView{data != nullptr ? data : reinterpret_cast<const uint8_t*>(""), size};
    } else if (PrUrlSearchParams* params = prJsUrlSearchParamsFromValue(ctx, body)) {
        form = prUrlSearchParamsToString(params);
        PrStringView view = prStringView(&form);
        bytes = PrByteView{reinterpret_cast<const uint8_t*>(view.data != nullptr ? view.data : ""), view.size};
        contentType = "application/x-www-form-urlencoded;charset=UTF-8";
    } else {
        if (!text.read(body))
            return JS_EXCEPTION;
        PrStringView view = text.value();
        bytes = PrByteView{reinterpret_cast<const uint8_t*>(view.data != nullptr ? view.data : ""), view.size};
        contentType = "text/plain;charset=UTF-8";
    }
    forgetResponse(ctx, self);
    prXhrSetActivity(request, activity, JS_GetRuntime(ctx));
    PrStatus status = prXhrSend(request, bytes, contentType);
    prStringReset(&form);
    return result(ctx, status);
}

JSValue xhrAbort(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    PrXhr* request = requestOf(ctx, self);
    if (request == nullptr)
        return JS_EXCEPTION;
    forgetResponse(ctx, self);
    prXhrAbort(request);
    return JS_UNDEFINED;
}

JSValue xhrResponseUrl(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    return request != nullptr ? jsNewStringView(ctx, prXhrResponseUrl(request)) : JS_EXCEPTION;
}

JSValue xhrStatus(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    return request != nullptr ? JS_NewInt32(ctx, prXhrStatus(request)) : JS_EXCEPTION;
}

// statusText: the response's status message, which the fetch does not report, so it is empty.
JSValue xhrStatusText(JSContext* ctx, JSValueConst self) {
    return requestOf(ctx, self) != nullptr ? jsFixedString(ctx, "") : JS_EXCEPTION;
}

JSValue xhrGetResponseHeader(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrXhr* request = requestOf(ctx, self);
    StringArgument name = {ctx};
    if (request == nullptr || !jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return JS_EXCEPTION;
    return jsTakeString(ctx, prXhrResponseHeader(request, name.value()));
}

JSValue xhrGetAllResponseHeaders(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    PrXhr* request = requestOf(ctx, self);
    return request != nullptr ? jsTakeString(ctx, prXhrResponseHeaders(request)) : JS_EXCEPTION;
}

JSValue xhrOverrideMimeType(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrXhr* request = requestOf(ctx, self);
    StringArgument mime = {ctx};
    if (request == nullptr || !jsArgCount(ctx, argc, 1) || !mime.read(argv[0]))
        return JS_EXCEPTION;
    return result(ctx, prXhrOverrideMimeType(request, mime.value()));
}

// The names of XMLHttpRequestResponseType, by PrXhrResponseType.
constexpr const char* kResponseTypes[] = {"", "arraybuffer", "blob", "document", "json", "text"};

JSValue xhrResponseType(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    return request != nullptr ? jsFixedString(ctx, kResponseTypes[prXhrResponseType(request)]) : JS_EXCEPTION;
}

// A value that names no response type is ignored, as an enumeration attribute ignores it (webidl/#es-enumeration).
JSValue xhrSetResponseType(JSContext* ctx, JSValueConst self, JSValueConst value) {
    PrXhr* request = requestOf(ctx, self);
    StringArgument name = {ctx};
    if (request == nullptr || !name.read(value))
        return JS_EXCEPTION;
    for (uint32_t i = 0; i < countOf(kResponseTypes); ++i) {
        if (name.value() == kResponseTypes[i])
            return result(ctx, prXhrSetResponseType(request, PrXhrResponseType(i)));
    }
    return JS_UNDEFINED;
}

// responseText (xhr/#dom-xmlhttprequest-responsetext): the text that has arrived, or the empty string before it loads.
JSValue xhrResponseText(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    if (request == nullptr)
        return JS_EXCEPTION;
    PrXhrResponseType type = prXhrResponseType(request);
    if (type != PrXhrResponseType_Empty && type != PrXhrResponseType_Text)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    PrXhrState state = prXhrReadyState(request);
    if (state != PrXhrState_Loading && state != PrXhrState_Done)
        return jsFixedString(ctx, "");
    return jsTakeString(ctx, prXhrResponseText(request));
}

// Returns the response as an object of the request's response type, once the request is done. A blob or a document
// response is null, since no Blob or document is made of it.
JSValue responseObject(JSContext* ctx, PrXhr* request) {
    PrByteView body = prXhrResponseBody(request);
    switch (prXhrResponseType(request)) {
        case PrXhrResponseType_ArrayBuffer:
            return JS_NewArrayBufferCopy(ctx, body.data != nullptr ? body.data : reinterpret_cast<const uint8_t*>(""),
                body.size);
        case PrXhrResponseType_Json: {
            // A JSON response is UTF-8 whatever its charset (xhr/#json-response), and is null if it does not parse.
            if (body.size == 0)
                return JS_NULL;
            TextBuffer source;
            textAppend(&source, reinterpret_cast<const char*>(body.data), uint32_t(body.size));
            textAppendChar(&source, '\0');
            JSValue parsed = JS_ParseJSON(ctx, textView(&source).data, body.size, "<json>");
            textReset(&source);
            if (JS_IsException(parsed)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return JS_NULL;
            }
            return parsed;
        }
        default: return JS_NULL;
    }
}

// response (xhr/#dom-xmlhttprequest-response).
JSValue xhrResponse(JSContext* ctx, JSValueConst self) {
    PrXhr* request = requestOf(ctx, self);
    if (request == nullptr)
        return JS_EXCEPTION;
    PrXhrResponseType type = prXhrResponseType(request);
    if (type == PrXhrResponseType_Empty || type == PrXhrResponseType_Text)
        return xhrResponseText(ctx, self);
    if (prXhrReadyState(request) != PrXhrState_Done)
        return JS_NULL;
    JSValue held = jsReference(ctx, self, XhrSlot_Response);
    if (!JS_IsUndefined(held))
        return held;
    JSValue made = responseObject(ctx, request);
    if (!JS_IsException(made))
        jsSetReference(ctx, self, XhrSlot_Response, made);
    return made;
}

// Gives the event targets and events of XHR their classes.
JSClassID classifyTarget(void* handle) {
    PrDomEventTarget* target = static_cast<PrDomEventTarget*>(handle);
    if (prXhrQuery(target) != nullptr)
        return xhrClass;
    if (prXhrUploadQuery(target) != nullptr)
        return xhrUploadClass;
    return prJsDomEventTargetClass;
}

JSClassID classifyEvent(void* handle) {
    if (prXhrProgressEventQuery(static_cast<PrDomEvent*>(handle)) != nullptr)
        return xhrProgressEventClass;
    return prJsDomEventClass;
}

// Releases the object's reference to its request. A request that is still active, as one whose fetch the runtime's
// destruction leaves in flight, outlives the runtime, so it reports its activity to the runtime no more.
void releaseRequest(PrDomEventTarget* PR_NULLABLE target) {
    PrXhr* request = prXhrQuery(target);
    if (request != nullptr)
        prXhrSetActivity(request, nullptr, nullptr);
    prXhrRelease(request);
}

constexpr JsReferences kTargetReferences = {
    [](void* handle) { prDomEventTargetRetain(static_cast<PrDomEventTarget*>(handle)); },
    [](void* handle) { prDomEventTargetRelease(static_cast<PrDomEventTarget*>(handle)); },
    true,
};

constexpr JSCFunctionListEntry kProgressEventProto[] = {
    PR_JS_TAG("ProgressEvent"),
    PR_JS_MAGIC_GET("lengthComputable", progressGet, 0),
    PR_JS_MAGIC_GET("loaded", progressGet, 1),
    PR_JS_MAGIC_GET("total", progressGet, 2),
};

constexpr JSCFunctionListEntry kEventTargetProto[] = {
    PR_JS_TAG("XMLHttpRequestEventTarget"),
    PR_JS_DOM_EVENT_HANDLER(onloadstart), PR_JS_DOM_EVENT_HANDLER(onprogress), PR_JS_DOM_EVENT_HANDLER(onabort),
    PR_JS_DOM_EVENT_HANDLER(onerror), PR_JS_DOM_EVENT_HANDLER(onload), PR_JS_DOM_EVENT_HANDLER(ontimeout),
    PR_JS_DOM_EVENT_HANDLER(onloadend),
};

constexpr JSCFunctionListEntry kUploadProto[] = {PR_JS_TAG("XMLHttpRequestUpload")};

constexpr JSCFunctionListEntry kRequestProto[] = {
    PR_JS_TAG("XMLHttpRequest"),
    PR_JS_DOM_EVENT_HANDLER(onreadystatechange), PR_JS_GET("readyState", xhrReadyState),
    PR_JS_METHOD("open", 2, xhrOpen),
    PR_JS_METHOD("setRequestHeader", 2, xhrSetRequestHeader),
    PR_JS_GET_SET("timeout", xhrTimeout, xhrSetTimeout),
    PR_JS_GET_SET("withCredentials", xhrWithCredentials, xhrSetWithCredentials),
    PR_JS_GET("upload", xhrUpload),
    PR_JS_METHOD("send", 0, xhrSend),
    PR_JS_METHOD("abort", 0, xhrAbort),
    PR_JS_GET("responseURL", xhrResponseUrl),
    PR_JS_GET("status", xhrStatus),
    PR_JS_GET("statusText", xhrStatusText),
    PR_JS_METHOD("getResponseHeader", 1, xhrGetResponseHeader),
    PR_JS_METHOD("getAllResponseHeaders", 0, xhrGetAllResponseHeaders),
    PR_JS_METHOD("overrideMimeType", 1, xhrOverrideMimeType),
    PR_JS_GET_SET("responseType", xhrResponseType, xhrSetResponseType),
    PR_JS_GET("response", xhrResponse),
    PR_JS_GET("responseText", xhrResponseText),
};

constexpr JsConstant kRequestConstants[] = {
    {"UNSENT", PrXhrState_Unsent},
    {"OPENED", PrXhrState_Opened},
    {"HEADERS_RECEIVED", PrXhrState_HeadersReceived},
    {"LOADING", PrXhrState_Loading},
    {"DONE", PrXhrState_Done},
};

constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("ProgressEvent", xhrProgressEventClass, jsViewFinalizer<&xhrProgressEventClass>,
            kProgressEventProto),
        .constructor = progressConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("XMLHttpRequestEventTarget", xhrEventTargetClass, jsViewFinalizer<&xhrEventTargetClass>,
            kEventTargetProto),
        .base = &prJsDomEventTargetClass,
        .references = &kTargetReferences,
    },
    {
        PR_JS_CLASS("XMLHttpRequestUpload", xhrUploadClass, jsViewFinalizer<&xhrUploadClass>, kUploadProto),
        .base = &xhrEventTargetClass,
    },
    {
        PR_JS_CLASS("XMLHttpRequest", xhrClass, (jsFinalizer<&xhrClass, releaseRequest>), kRequestProto),
        PR_JS_CONSTANTS(kRequestConstants),
        .constructor = xhrConstruct,
        .base = &xhrEventTargetClass,
    },
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prJsXhrBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    JSRuntime* runtime = JS_GetRuntime(ctx);
    jsAddClassifier(runtime, prJsDomEventTargetClass, classifyTarget);
    jsAddClassifier(runtime, prJsDomEventClass, classifyEvent);
}
