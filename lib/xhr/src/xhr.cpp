// XMLHttpRequest and ProgressEvent (pr/xhr.h).

#include "xhr/src/xhr.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/charset.h"
#include "pr/fetch.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/url.h"

#include "dom/src/dom_event.h"
#include "dom/src/dom_mime.h"

namespace pr_impl {

namespace {

void headersReset(Array<XhrHeader>* headers) {
    for (uint32_t i = 0; i < headers->count; ++i) {
        prStringReset(&headers->data[i].name);
        prStringReset(&headers->data[i].value);
    }
    headers->clear();
}

// Empties the response.
void resetResponse(Xhr* request) {
    request->status = 0;
    prStringReset(&request->responseUrl);
    headersReset(&request->responseHeaders);
    request->body.clear();
}

void destroyRequest(DomCountedTarget* target) {
    Xhr* request = static_cast<Xhr*>(target);
    PR_ASSERT(!request->fetching && request->timer == 0);
    prStringReset(&request->method);
    prStringReset(&request->url);
    headersReset(&request->requestHeaders);
    request->requestHeaders.reset();
    prStringReset(&request->overrideMime);
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
void receive(Xhr* request, const PrFetchClientResponse* response) {
    uint32_t generation = request->generation;
    request->status = response->responseStatus;
    request->responseUrl = prStringCopy(response->url);
    for (uint32_t i = 0; i < response->headerCount; ++i)
        request->responseHeaders.push(XhrHeader{prStringCopy(response->headers[i].name),
                                                prStringCopy(response->headers[i].value)});
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
void fetched(void* PR_NULLABLE context, const PrFetchClientResponse* response) {
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
uint32_t findHeader(const Array<XhrHeader>& headers, PrStringView name) {
    for (uint32_t i = 0; i < headers.count; ++i) {
        if (asciiEqualsFold(prStringView(&headers.data[i].name), name))
            return i;
    }
    return kArrayMissing;
}

// Appends `value` to the header `name`, combined with the value that the list has for it
// (fetch/#concept-header-list-combine).
void combineHeader(Array<XhrHeader>* headers, PrStringView name, PrStringView value) {
    uint32_t index = findHeader(*headers, name);
    if (index == kArrayMissing) {
        headers->push(XhrHeader{prStringCopy(name), prStringCopy(value)});
        return;
    }
    TextBuffer combined;
    textAppend(&combined, prStringView(&headers->data[index].value));
    textAppend(&combined, ", ");
    textAppend(&combined, value);
    prStringReset(&headers->data[index].value);
    headers->data[index].value = textTake(&combined);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// ProgressEvent

PrXhrProgressEvent* PR_NULLABLE prXhrProgressEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Progress ? static_cast<PrXhrProgressEvent*>(object)
                                                                        : nullptr;
}

PrXhrProgressEvent* prXhrProgressEventCreate(PrStringView type, const PrXhrProgressEventInit* PR_NULLABLE init) {
    PrXhrProgressEventInit values = init != nullptr ? *init : prXhrProgressEventInitDefault();
    PrXhrProgressEvent* event = domEventCreate<PrXhrProgressEvent>(type, values.event, PrDomEventKind_Progress);
    XhrProgressData* data = allocZeroed<XhrProgressData>();
    *data = XhrProgressData{{[](DomEventExtra* extra) { prMemoryFree(extra); }},
                             values.lengthComputable,
                             values.loaded,
                             values.total};
    event->extra = data;
    return event;
}

bool prXhrProgressEventLengthComputable(const PrXhrProgressEvent* self) {
    return static_cast<const XhrProgressData*>(self->extra)->lengthComputable;
}

double prXhrProgressEventLoaded(const PrXhrProgressEvent* self) {
    return static_cast<const XhrProgressData*>(self->extra)->loaded;
}

double prXhrProgressEventTotal(const PrXhrProgressEvent* self) {
    return static_cast<const XhrProgressData*>(self->extra)->total;
}

// ---------------------------------------------------------------------------------------------------------------------
// XMLHttpRequestUpload

PrXhrUpload* PR_NULLABLE prXhrUploadQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_XmlHttpRequestUpload
               ? static_cast<PrXhrUpload*>(object)
               : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// XMLHttpRequest

PrXhr* PR_NULLABLE prXhrQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_XmlHttpRequest
               ? static_cast<PrXhr*>(object)
               : nullptr;
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

void prXhrRetain(PrXhr* self) {
    retainRequest(self);
}

void prXhrRelease(PrXhr* PR_NULLABLE self) {
    if (self != nullptr)
        releaseRequest(self);
}

void prXhrSetActivity(PrXhr* self, PrXhrActivityFn PR_NULLABLE activity, void* PR_NULLABLE context) {
    self->activity = activity;
    self->activityContext = context;
    if (self->active && activity != nullptr)
        activity(context, self, true);
}

PrXhrUpload* prXhrUpload(PrXhr* self) {
    return static_cast<PrXhrUpload*>(&self->upload);
}

PrXhrState prXhrReadyState(const PrXhr* self) {
    return self->state;
}

PrStatus prXhrOpen(PrXhr* self, PrStringView method, PrStringView url, bool async) {
    if (!prFetchIsMethod(method))
        return PrStatus_Syntax;
    if (prFetchIsForbiddenMethod(method))
        return PrStatus_NotAllowed;
    PrString baseText = self->client.baseUrl(self->client.context);
    PrUrl* base = nullptr;
    PrUrl* parsed = nullptr;
    PrStatus status = prUrlCreate(prStringView(&baseText), nullptr, PrCharset_Utf8, &base);
    prStringReset(&baseText);
    if (status == PrStatus_Ok)
        status = prUrlCreate(url, base, PrCharset_Utf8, &parsed);
    prUrlDestroy(base);
    if (status != PrStatus_Ok)
        return PrStatus_Syntax;
    if (!async) {
        prUrlDestroy(parsed);
        return PrStatus_NotSupported;
    }
    ++self->generation;
    stopFetch(self);
    PrStringView normalized = prFetchNormalizeMethod(method);
    prStringReset(&self->method);
    if (!(normalized == "GET"))
        self->method = prStringCopy(normalized);
    prStringReset(&self->url);
    self->url = prUrlSerialize(parsed, PrUrlSerialization_Whole);
    prUrlDestroy(parsed);
    headersReset(&self->requestHeaders);
    resetResponse(self);
    self->sendFlag = false;
    self->uploadListener = false;
    // A request that open() ends has no more pending activity, and fires nothing more.
    setActive(self, false);
    if (self->state != PrXhrState_Opened) {
        self->state = PrXhrState_Opened;
        fireReadyStateChange(self);
    }
    return PrStatus_Ok;
}

PrStatus prXhrSetRequestHeader(PrXhr* self, PrStringView name, PrStringView value) {
    if (self->state != PrXhrState_Opened || self->sendFlag)
        return PrStatus_InvalidState;
    PrStringView normalized = prFetchNormalizeHeaderValue(value);
    if (!prFetchIsHeaderName(name) || !prFetchIsHeaderValue(normalized))
        return PrStatus_Syntax;
    if (!prFetchIsForbiddenRequestHeader(name, normalized))
        combineHeader(&self->requestHeaders, name, normalized);
    return PrStatus_Ok;
}

uint32_t prXhrTimeout(const PrXhr* self) {
    return self->timeout;
}

void prXhrSetTimeout(PrXhr* self, uint32_t milliseconds) {
    self->timeout = milliseconds;
}

bool prXhrWithCredentials(const PrXhr* self) {
    return self->withCredentials;
}

PrStatus prXhrSetWithCredentials(PrXhr* self, bool withCredentials) {
    if ((self->state != PrXhrState_Unsent && self->state != PrXhrState_Opened)
        || self->sendFlag)
        return PrStatus_InvalidState;
    self->withCredentials = withCredentials;
    return PrStatus_Ok;
}

PrStatus prXhrSend(PrXhr* self, PrByteView body, PrStringView contentType) {
    if (self->state != PrXhrState_Opened || self->sendFlag)
        return PrStatus_InvalidState;
    PrStringView method = prStringIsNull(&self->method) ? PrStringView("GET") : prStringView(&self->method);
    if (method == "GET" || method == "HEAD")
        body = PrByteView{nullptr, 0};
    if (body.data != nullptr && contentType.data != nullptr
        && findHeader(self->requestHeaders, "Content-Type") == kArrayMissing)
        self->requestHeaders.push(XhrHeader{prStringCopy("Content-Type"), prStringCopy(contentType)});
    self->requestBytes = body.size;
    self->uploadComplete = body.size == 0;
    self->uploadListener = self->upload.listeners != nullptr;
    self->sendFlag = true;
    setActive(self, true);
    uint32_t generation = self->generation;
    fireProgress(self, "loadstart", 0, 0);
    if (self->generation != generation || self->state != PrXhrState_Opened || !self->sendFlag)
        return PrStatus_Ok;
    if (self->uploadListener && !self->uploadComplete) {
        fireProgress(&self->upload, "loadstart", 0, self->requestBytes);
        if (self->generation != generation || self->state != PrXhrState_Opened || !self->sendFlag)
            return PrStatus_Ok;
    }
    Array<PrFetchHeader> headers;
    for (uint32_t i = 0; i < self->requestHeaders.count; ++i)
        headers.push(PrFetchHeader{prStringView(&self->requestHeaders.data[i].name),
                                   prStringView(&self->requestHeaders.data[i].value)});
    PrFetchClientRequest request{.url = prStringView(&self->url),
                               .method = prStringView(&self->method),
                               .headers = headers.data,
                               .headerCount = headers.count,
                               .body = body,
                               .mode = PrFetchMode_Cors,
                               .credentials = PrFetchCredentials(self->withCredentials ? PrFetchCredentials_Include
                                                                                       : PrFetchCredentials_SameOrigin),
                               .redirect = PrFetchRedirect_Follow};
    if (self->client.fetch(self->client.context, &request, fetched, self) == PrStatus_Ok) {
        retainRequest(self);
        self->fetching = true;
        if (self->timeout != 0)
            setTimer(self, self->timeout);
    } else {
        setTimer(self, 0);
    }
    headers.reset();
    return PrStatus_Ok;
}

void prXhrAbort(PrXhr* self) {
    stopFetch(self);
    bool inFlight = (self->state == PrXhrState_Opened && self->sendFlag)
                    || self->state == PrXhrState_HeadersReceived
                    || self->state == PrXhrState_Loading;
    ++self->generation;
    if (inFlight)
        requestError(self, "abort");
    if (self->state == PrXhrState_Done) {
        self->state = PrXhrState_Unsent;
        resetResponse(self);
    }
    setActive(self, false);
}

PrStatus prXhrOverrideMimeType(PrXhr* self, PrStringView mime) {
    if (self->state == PrXhrState_Loading || self->state == PrXhrState_Done)
        return PrStatus_InvalidState;
    prStringReset(&self->overrideMime);
    PrStringView essence = domMimeTypeEssence(mime);
    self->overrideMime = prStringCopy(essence.size != 0 ? mime : PrStringView("application/octet-stream"));
    return PrStatus_Ok;
}

PrXhrResponseType prXhrResponseType(const PrXhr* self) {
    return self->responseType;
}

PrStatus prXhrSetResponseType(PrXhr* self, PrXhrResponseType type) {
    if (self->state == PrXhrState_Loading || self->state == PrXhrState_Done)
        return PrStatus_InvalidState;
    self->responseType = type;
    return PrStatus_Ok;
}

PrStringView prXhrResponseUrl(const PrXhr* self) {
    return prStringIsNull(&self->responseUrl) ? PrStringView("") : prStringView(&self->responseUrl);
}

uint16_t prXhrStatus(const PrXhr* self) {
    return self->status;
}

PrString prXhrResponseHeader(const PrXhr* self, PrStringView name) {
    Array<XhrHeader> combined;
    for (uint32_t i = 0; i < self->responseHeaders.count; ++i) {
        const XhrHeader& header = self->responseHeaders.data[i];
        if (asciiEqualsFold(prStringView(&header.name), name))
            combineHeader(&combined, name, prStringView(&header.value));
    }
    PrString result = prStringNull();
    if (combined.count != 0) {
        result = combined.data[0].value;
        combined.data[0].value = prStringNull();
    }
    headersReset(&combined);
    combined.reset();
    return result;
}

PrString prXhrResponseHeaders(const PrXhr* self) {
    // The headers sorted by their lowercase names, with the values of each name combined
    // (fetch/#concept-header-list-sort-and-combine).
    Array<XhrHeader> sorted;
    for (uint32_t i = 0; i < self->responseHeaders.count; ++i) {
        const XhrHeader& header = self->responseHeaders.data[i];
        TextBuffer lower;
        textAppend(&lower, prStringView(&header.name));
        asciiLower(lower.text.data, lower.text.count);
        combineHeader(&sorted, textView(&lower), prStringView(&header.value));
        textReset(&lower);
    }
    for (uint32_t i = 1; i < sorted.count; ++i) {
        for (uint32_t j = i; j > 0; --j) {
            if (stringCompare(prStringView(&sorted.data[j].name), prStringView(&sorted.data[j - 1].name)) >= 0)
                break;
            XhrHeader swap = sorted.data[j];
            sorted.data[j] = sorted.data[j - 1];
            sorted.data[j - 1] = swap;
        }
    }
    TextBuffer text;
    for (uint32_t i = 0; i < sorted.count; ++i) {
        textAppend(&text, prStringView(&sorted.data[i].name));
        textAppend(&text, ": ");
        textAppend(&text, prStringView(&sorted.data[i].value));
        textAppend(&text, "\r\n");
    }
    headersReset(&sorted);
    sorted.reset();
    return textTake(&text);
}

PrByteView prXhrResponseBody(const PrXhr* self) {
    return PrByteView{self->body.data, self->body.count};
}

PrString prXhrResponseText(const PrXhr* self) {
    // The final MIME type is the override, or else the response's Content-Type (xhr/#final-mime-type).
    PrStringView mime = prStringView(&self->overrideMime);
    if (prStringIsNull(&self->overrideMime)) {
        uint32_t index = findHeader(self->responseHeaders, "Content-Type");
        mime = index != kArrayMissing ? prStringView(&self->responseHeaders.data[index].value) : PrStringView("");
    }
    PrCharset charset = domMimeTypeCharset(mime);
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset != PrCharset_None ? charset : PrCharset_Utf8, 0);
    PrStringView bytes{reinterpret_cast<const char*>(self->body.data), self->body.count};
    PrString out = prStringNull();
    PrStringView text = prCharsetDecodeView(&decoder, bytes, PrCharsetInput_Final, &out);
    // A decode that copies nothing returns a view of the body.
    if (prStringIsNull(&out))
        return prStringCopy(text.data != nullptr ? text : PrStringView(""));
    return out;
}
