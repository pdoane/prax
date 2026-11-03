// XMLHttpRequest and ProgressEvent (pr/xhr.h).

#include "xhr/src/xhr.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/charset.h"
#include "pr/fetch.h"
#include "pr/mime.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/url.h"

#include "dom/src/dom_event.h"

namespace pr_impl {

namespace {

void headersReset(RawArray<XhrHeader>* headers) {
    for (uint32_t i = 0; i < headers->count; ++i) {
        prRawStringReset(&headers->data[i].name);
        prRawStringReset(&headers->data[i].value);
    }
    headers->clear();
}

// Empties the response.
void resetResponse(Xhr* request) {
    request->status = 0;
    prRawStringReset(&request->responseUrl);
    headersReset(&request->responseHeaders);
    request->body.clear();
}

void destroyRequest(DomCountedTarget* target) {
    Xhr* request = static_cast<Xhr*>(target);
    PR_ASSERT(!request->fetching && request->timer == 0);
    prRawStringReset(&request->method);
    prRawStringReset(&request->url);
    headersReset(&request->requestHeaders);
    request->requestHeaders.reset();
    prRawStringReset(&request->overrideMime);
    resetResponse(request);
    request->responseHeaders.reset();
    request->body.reset();
    domListenersDestroy(request->upload.listeners);
    domListenersDestroy(request->listeners);
    prMemoryFree(request);
}

void retainRequest(Xhr* request) {
    ++request->references;
}

void releaseRequest(Xhr* request) {
    if (--request->references == 0)
        destroyRequest(request);
}

// Starts or stops the request's pending activity, during which it holds a reference to itself. The reference is
// released last, since it may be the last one.
void setActive(Xhr* request, bool active) {
    if (request->active == active)
        return;
    request->active = active;
    if (active)
        retainRequest(request);
    if (request->activity != nullptr)
        request->activity(request->activityContext, static_cast<PrXhr*>(request), active);
    if (!active)
        releaseRequest(request);
}

// Fires a trusted ProgressEvent of `type` at `target` (xhr/#concept-event-fire-progress).
void fireProgress(PrDomEventTarget* target, PrStringView type, uint64_t transmitted, uint64_t length) {
    PrXhrProgressEventInit init = prXhrProgressEventInitDefault();
    init.lengthComputable = length != 0;
    init.loaded = double(transmitted);
    init.total = double(length);
    PrXhrProgressEvent* event = prXhrProgressEventCreate(type, &init);
    event->trusted = true;
    domEventDispatch(target, event);
    domEventRelease(event);
}

void fireReadyStateChange(Xhr* request) {
    domEventFire(request, "readystatechange");
}

// Cancels the request's timer, if it is set.
void clearTimer(Xhr* request) {
    if (request->timer != 0) {
        request->client.clearTimeout(request->client.context, request->timer);
        request->timer = 0;
    }
}

// Stops the fetch and the timer of the request, if they run.
void stopFetch(Xhr* request) {
    if (request->fetching) {
        request->fetching = false;
        request->client.cancelFetch(request->client.context, request);
        releaseRequest(request);
    }
    clearTimer(request);
}

// Ends the request with a network error, and fires `type` and loadend at it and at its upload
// (xhr/#request-error-steps). `type` is error, abort or timeout.
void requestError(Xhr* request, PrStringView type) {
    uint32_t generation = request->generation;
    request->state = PrXhrState_Done;
    request->sendFlag = false;
    resetResponse(request);
    fireReadyStateChange(request);
    if (request->generation != generation)
        return;
    if (!request->uploadComplete) {
        request->uploadComplete = true;
        if (request->uploadListener) {
            fireProgress(&request->upload, type, 0, 0);
            fireProgress(&request->upload, "loadend", 0, 0);
        }
    }
    if (request->generation != generation)
        return;
    fireProgress(request, type, 0, 0);
    if (request->generation != generation)
        return;
    fireProgress(request, "loadend", 0, 0);
    if (request->generation == generation)
        setActive(request, false);
}

// Reads the response into the request, and fires the events of its arrival and of its end
// (xhr/#dom-xmlhttprequest-send, processResponse and processResponseEndOfBody). The client passes the whole response at
// once.
void receive(Xhr* request, const PrFetchResponse* response) {
    uint32_t generation = request->generation;
    request->status = response->responseStatus;
    request->responseUrl = prRawStringCopy(response->url);
    for (uint32_t i = 0; i < response->headerCount; ++i)
        request->responseHeaders.push(XhrHeader{
            prRawStringCopy(response->headers[i].name),
            prRawStringCopy(response->headers[i].value),
        });
    if (response->body.size != 0)
        request->body.append(response->body.data, uint32_t(response->body.size));
    if (!request->uploadComplete) {
        request->uploadComplete = true;
        if (request->uploadListener) {
            fireProgress(&request->upload, "progress", request->requestBytes, request->requestBytes);
            fireProgress(&request->upload, "load", request->requestBytes, request->requestBytes);
            fireProgress(&request->upload, "loadend", request->requestBytes, request->requestBytes);
        }
        if (request->generation != generation)
            return;
    }
    request->state = PrXhrState_HeadersReceived;
    fireReadyStateChange(request);
    if (request->generation != generation)
        return;
    uint64_t length = request->body.count;
    request->state = PrXhrState_Loading;
    fireReadyStateChange(request);
    if (request->generation != generation)
        return;
    fireProgress(request, "progress", length, length);
    if (request->generation != generation)
        return;
    request->state = PrXhrState_Done;
    request->sendFlag = false;
    fireReadyStateChange(request);
    if (request->generation != generation)
        return;
    fireProgress(request, "load", length, length);
    if (request->generation != generation)
        return;
    fireProgress(request, "loadend", length, length);
    if (request->generation == generation)
        setActive(request, false);
}

// Called with the response that the client fetched for the request. A response for a client that is gone ends the
// request without its events.
void fetched(void* PR_NULLABLE context, const PrFetchResponse* response) {
    Xhr* request = static_cast<Xhr*>(context);
    request->fetching = false;
    clearTimer(request);
    if (response->status == PrStatus_Lost)
        setActive(request, false);
    else if (response->status != PrStatus_Ok)
        requestError(request, "error");
    else
        receive(request, response);
    releaseRequest(request);
}

// Times the request out, or fails a request whose fetch did not start as a fetch that fails does: with a network error,
// later than send() (xhr/#dom-xmlhttprequest-send).
void timerFired(void* PR_NULLABLE context) {
    Xhr* request = static_cast<Xhr*>(context);
    if (request->timer == 0)
        return;
    request->timer = 0;
    bool timedOut = request->fetching;
    stopFetch(request);
    if (request->sendFlag)
        requestError(request, timedOut ? "timeout" : "error");
}

void retainTimer(void* PR_NULLABLE context) {
    retainRequest(static_cast<Xhr*>(context));
}

void releaseTimer(void* PR_NULLABLE context) {
    releaseRequest(static_cast<Xhr*>(context));
}

// Sets the request's timer to fire after `milliseconds`. The timer holds a reference to the request.
void setTimer(Xhr* request, uint32_t milliseconds) {
    PrXhrTask task{.invoke = timerFired, .context = request, .retain = retainTimer, .release = releaseTimer};
    request->timer = request->client.setTimeout(request->client.context, &task, milliseconds);
}

// Returns the index of the header named `name`, or kArrayMissing.
uint32_t findHeader(const RawArray<XhrHeader>& headers, PrStringView name) {
    for (uint32_t i = 0; i < headers.count; ++i) {
        if (prAsciiEqualsFold(prRawStringView(&headers.data[i].name), name))
            return i;
    }
    return kArrayMissing;
}

// Appends `value` to the header `name`, combined with the value that the list has for it
// (fetch/#concept-header-list-combine).
void combineHeader(RawArray<XhrHeader>* headers, PrStringView name, PrStringView value) {
    uint32_t index = findHeader(*headers, name);
    if (index == kArrayMissing) {
        headers->push(XhrHeader{prRawStringCopy(name), prRawStringCopy(value)});
        return;
    }
    PrTextBuffer combined = {};
    prTextBufferAppend(&combined, prRawStringView(&headers->data[index].value));
    prTextBufferAppend(&combined, ", ");
    prTextBufferAppend(&combined, value);
    prRawStringReset(&headers->data[index].value);
    headers->data[index].value = prTextBufferTake(&combined);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// ProgressEvent

PrXhrProgressEvent* PR_NULLABLE prXhrProgressEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Progress ? static_cast<PrXhrProgressEvent*>(object) : nullptr;
}

PrXhrProgressEvent* prXhrProgressEventCreate(PrStringView type, const PrXhrProgressEventInit* PR_NULLABLE init) {
    PrXhrProgressEventInit values = init != nullptr ? *init : prXhrProgressEventInitDefault();
    PrXhrProgressEvent* event = domEventCreate<PrXhrProgressEvent>(type, values.event, PrDomEventKind_Progress);
    XhrProgressData* data = allocZeroed<XhrProgressData>();
    *data = XhrProgressData{
        {[](DomEventExtra* extra) { prMemoryFree(extra); }},
        values.lengthComputable,
        values.loaded,
        values.total,
    };
    event->extra = data;
    return event;
}

bool prXhrProgressEventLengthComputable(const PrXhrProgressEvent* event) {
    return static_cast<const XhrProgressData*>(event->extra)->lengthComputable;
}

double prXhrProgressEventLoaded(const PrXhrProgressEvent* event) {
    return static_cast<const XhrProgressData*>(event->extra)->loaded;
}

double prXhrProgressEventTotal(const PrXhrProgressEvent* event) {
    return static_cast<const XhrProgressData*>(event->extra)->total;
}

// ---------------------------------------------------------------------------------------------------------------------
// XMLHttpRequestUpload

PrXhrUpload* PR_NULLABLE prXhrUploadQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_XmlHttpRequestUpload ? static_cast<PrXhrUpload*>(object) : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// XMLHttpRequest

PrXhr* PR_NULLABLE prXhrQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_XmlHttpRequest ? static_cast<PrXhr*>(object) : nullptr;
}

PrXhr* prXhrCreate(const PrXhrDesc* desc) {
    PrXhr* request = allocZeroed<PrXhr>();
    request->type = DomTargetType_XmlHttpRequest;
    request->destroy = destroyRequest;
    request->references = 1;
    request->client = desc->client;
    request->upload.type = DomTargetType_XmlHttpRequestUpload;
    request->upload.owner = request;
    return request;
}

void prXhrRetain(PrXhr* xhr) {
    retainRequest(xhr);
}

void prXhrRelease(PrXhr* PR_NULLABLE xhr) {
    if (xhr != nullptr)
        releaseRequest(xhr);
}

void prXhrSetActivity(PrXhr* xhr, PrXhrActivityFn PR_NULLABLE activity, void* PR_NULLABLE context) {
    xhr->activity = activity;
    xhr->activityContext = context;
    if (xhr->active && activity != nullptr)
        activity(context, xhr, true);
}

PrXhrUpload* prXhrUpload(PrXhr* xhr) {
    return static_cast<PrXhrUpload*>(&xhr->upload);
}

PrXhrState prXhrReadyState(const PrXhr* xhr) {
    return xhr->state;
}

PrStatus prXhrOpen(PrXhr* xhr, PrStringView method, PrStringView url, bool async) {
    if (!prFetchIsMethod(method))
        return PrStatus_Syntax;
    if (prFetchIsForbiddenMethod(method))
        return PrStatus_NotAllowed;
    PrRawString baseText = xhr->client.baseUrl(xhr->client.context);
    PrUrl* base = nullptr;
    PrUrl* parsed = nullptr;
    PrStatus status = prUrlCreate(prRawStringView(&baseText), nullptr, PrCharset_Utf8, &base);
    prRawStringReset(&baseText);
    if (status == PrStatus_Ok)
        status = prUrlCreate(url, base, PrCharset_Utf8, &parsed);
    prUrlDestroy(base);
    if (status != PrStatus_Ok)
        return PrStatus_Syntax;
    if (!async) {
        prUrlDestroy(parsed);
        return PrStatus_NotSupported;
    }
    ++xhr->generation;
    stopFetch(xhr);
    PrStringView normalized = prFetchNormalizeMethod(method);
    prRawStringReset(&xhr->method);
    if (!(normalized == "GET"))
        xhr->method = prRawStringCopy(normalized);
    prRawStringReset(&xhr->url);
    xhr->url = prUrlSerialize(parsed, PrUrlSerialization_Whole);
    prUrlDestroy(parsed);
    headersReset(&xhr->requestHeaders);
    resetResponse(xhr);
    xhr->sendFlag = false;
    xhr->uploadListener = false;
    // A request that open() ends has no more pending activity, and fires nothing more.
    setActive(xhr, false);
    if (xhr->state != PrXhrState_Opened) {
        xhr->state = PrXhrState_Opened;
        fireReadyStateChange(xhr);
    }
    return PrStatus_Ok;
}

PrStatus prXhrSetRequestHeader(PrXhr* xhr, PrStringView name, PrStringView value) {
    if (xhr->state != PrXhrState_Opened || xhr->sendFlag)
        return PrStatus_InvalidState;
    PrStringView normalized = prFetchNormalizeHeaderValue(value);
    if (!prFetchIsHeaderName(name) || !prFetchIsHeaderValue(normalized))
        return PrStatus_Syntax;
    if (!prFetchIsForbiddenRequestHeader(name, normalized))
        combineHeader(&xhr->requestHeaders, name, normalized);
    return PrStatus_Ok;
}

uint32_t prXhrTimeout(const PrXhr* xhr) {
    return xhr->timeout;
}

void prXhrSetTimeout(PrXhr* xhr, uint32_t milliseconds) {
    xhr->timeout = milliseconds;
}

bool prXhrWithCredentials(const PrXhr* xhr) {
    return xhr->withCredentials;
}

PrStatus prXhrSetWithCredentials(PrXhr* xhr, bool withCredentials) {
    if ((xhr->state != PrXhrState_Unsent && xhr->state != PrXhrState_Opened) || xhr->sendFlag)
        return PrStatus_InvalidState;
    xhr->withCredentials = withCredentials;
    return PrStatus_Ok;
}

PrStatus prXhrSend(PrXhr* xhr, PrByteView body, PrStringView contentType) {
    if (xhr->state != PrXhrState_Opened || xhr->sendFlag)
        return PrStatus_InvalidState;
    PrStringView method = prRawStringIsNull(&xhr->method) ? PrStringView("GET") : prRawStringView(&xhr->method);
    if (method == "GET" || method == "HEAD")
        body = PrByteView{nullptr, 0};
    if (body.data != nullptr && contentType.data != nullptr &&
            findHeader(xhr->requestHeaders, "Content-Type") == kArrayMissing)
        xhr->requestHeaders.push(XhrHeader{prRawStringCopy("Content-Type"), prRawStringCopy(contentType)});
    xhr->requestBytes = body.size;
    xhr->uploadComplete = body.size == 0;
    xhr->uploadListener = xhr->upload.listeners != nullptr;
    xhr->sendFlag = true;
    setActive(xhr, true);
    uint32_t generation = xhr->generation;
    fireProgress(xhr, "loadstart", 0, 0);
    if (xhr->generation != generation || xhr->state != PrXhrState_Opened || !xhr->sendFlag)
        return PrStatus_Ok;
    if (xhr->uploadListener && !xhr->uploadComplete) {
        fireProgress(&xhr->upload, "loadstart", 0, xhr->requestBytes);
        if (xhr->generation != generation || xhr->state != PrXhrState_Opened || !xhr->sendFlag)
            return PrStatus_Ok;
    }
    RawArray<PrFetchHeader> headers;
    for (uint32_t i = 0; i < xhr->requestHeaders.count; ++i)
        headers.push(PrFetchHeader{
            prRawStringView(&xhr->requestHeaders.data[i].name),
            prRawStringView(&xhr->requestHeaders.data[i].value),
        });
    PrFetchClientRequest request{
        .url = prRawStringView(&xhr->url),
        .method = prRawStringView(&xhr->method),
        .headers = headers.data,
        .headerCount = headers.count,
        .body = body,
        .mode = PrFetchMode_Cors,
        .credentials = PrFetchCredentials_SameOrigin,
        .redirect = PrFetchRedirect_Follow,
        .initiator = PrFetchInitiator_XmlHttpRequest,
    };
    if (xhr->withCredentials)
        request.credentials = PrFetchCredentials_Include;
    if (xhr->client.fetch(xhr->client.context, &request, fetched, xhr) == PrStatus_Ok) {
        retainRequest(xhr);
        xhr->fetching = true;
        if (xhr->timeout != 0)
            setTimer(xhr, xhr->timeout);
    } else {
        setTimer(xhr, 0);
    }
    headers.reset();
    return PrStatus_Ok;
}

void prXhrAbort(PrXhr* xhr) {
    stopFetch(xhr);
    bool inFlight = (xhr->state == PrXhrState_Opened && xhr->sendFlag) || xhr->state == PrXhrState_HeadersReceived ||
        xhr->state == PrXhrState_Loading;
    ++xhr->generation;
    if (inFlight)
        requestError(xhr, "abort");
    if (xhr->state == PrXhrState_Done) {
        xhr->state = PrXhrState_Unsent;
        resetResponse(xhr);
    }
    setActive(xhr, false);
}

PrStatus prXhrOverrideMimeType(PrXhr* xhr, PrStringView mime) {
    if (xhr->state == PrXhrState_Loading || xhr->state == PrXhrState_Done)
        return PrStatus_InvalidState;
    prRawStringReset(&xhr->overrideMime);
    PrStringView essence = prMimeTypeEssence(mime);
    xhr->overrideMime = prRawStringCopy(essence.size != 0 ? mime : PrStringView("application/octet-stream"));
    return PrStatus_Ok;
}

PrXhrResponseType prXhrResponseType(const PrXhr* xhr) {
    return xhr->responseType;
}

PrStatus prXhrSetResponseType(PrXhr* xhr, PrXhrResponseType type) {
    if (xhr->state == PrXhrState_Loading || xhr->state == PrXhrState_Done)
        return PrStatus_InvalidState;
    xhr->responseType = type;
    return PrStatus_Ok;
}

PrStringView prXhrResponseUrl(const PrXhr* xhr) {
    return prRawStringIsNull(&xhr->responseUrl) ? PrStringView("") : prRawStringView(&xhr->responseUrl);
}

uint16_t prXhrStatus(const PrXhr* xhr) {
    return xhr->status;
}

PrRawString prXhrResponseHeader(const PrXhr* xhr, PrStringView name) {
    RawArray<XhrHeader> combined;
    for (uint32_t i = 0; i < xhr->responseHeaders.count; ++i) {
        const XhrHeader& header = xhr->responseHeaders.data[i];
        if (prAsciiEqualsFold(prRawStringView(&header.name), name))
            combineHeader(&combined, name, prRawStringView(&header.value));
    }
    PrRawString result = prRawStringNull();
    if (combined.count != 0) {
        result = combined.data[0].value;
        combined.data[0].value = prRawStringNull();
    }
    headersReset(&combined);
    combined.reset();
    return result;
}

PrRawString prXhrResponseHeaders(const PrXhr* xhr) {
    // The headers sorted by their lowercase names, with the values of each name combined
    // (fetch/#concept-header-list-sort-and-combine).
    RawArray<XhrHeader> sorted;
    for (uint32_t i = 0; i < xhr->responseHeaders.count; ++i) {
        const XhrHeader& header = xhr->responseHeaders.data[i];
        PrTextBuffer lower = {};
        prTextBufferAppend(&lower, prRawStringView(&header.name));
        prAsciiLowerText(lower.data, lower.size);
        combineHeader(&sorted, prTextBufferView(&lower), prRawStringView(&header.value));
        prTextBufferReset(&lower);
    }
    for (uint32_t i = 1; i < sorted.count; ++i) {
        for (uint32_t j = i; j > 0; --j) {
            if (prStringCompare(prRawStringView(&sorted.data[j].name), prRawStringView(&sorted.data[j - 1].name)) >= 0)
                break;
            XhrHeader swap = sorted.data[j];
            sorted.data[j] = sorted.data[j - 1];
            sorted.data[j - 1] = swap;
        }
    }
    PrTextBuffer text = {};
    for (uint32_t i = 0; i < sorted.count; ++i) {
        prTextBufferAppend(&text, prRawStringView(&sorted.data[i].name));
        prTextBufferAppend(&text, ": ");
        prTextBufferAppend(&text, prRawStringView(&sorted.data[i].value));
        prTextBufferAppend(&text, "\r\n");
    }
    headersReset(&sorted);
    sorted.reset();
    return prTextBufferTake(&text);
}

PrByteView prXhrResponseBody(const PrXhr* xhr) {
    return PrByteView{xhr->body.data, xhr->body.count};
}

PrStringView prXhrFinalMimeType(const PrXhr* object) {
    if (!prRawStringIsNull(&object->overrideMime))
        return prRawStringView(&object->overrideMime);
    uint32_t index = findHeader(object->responseHeaders, "Content-Type");
    return index != kArrayMissing ? prRawStringView(&object->responseHeaders.data[index].value) : PrStringView{nullptr, 0};
}

PrRawString prXhrResponseText(const PrXhr* xhr) {
    PrStringView mime = prXhrFinalMimeType(xhr);
    PrCharset charset = prCharsetFromMimeType(mime.data != nullptr ? mime : PrStringView{""});
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset != PrCharset_None ? charset : PrCharset_Utf8, 0);
    PrStringView bytes{reinterpret_cast<const char*>(xhr->body.data), xhr->body.count};
    PrRawString out = prRawStringNull();
    PrStringView text = prCharsetDecodeView(&decoder, bytes, PrCharsetInput_Final, &out);
    // A decode that copies nothing returns a view of the body.
    if (prRawStringIsNull(&out))
        return prRawStringCopy(text.data != nullptr ? text : PrStringView(""));
    return out;
}
