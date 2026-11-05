// XMLHttpRequest and ProgressEvent: a script's requests of resources by URL, and the events that report their progress.
//
// A request fetches through its client (PrXhrClient), which reads the whole response before it reports any of it. A
// request is asynchronous. The objects are single-threaded, and run on their client's event loop.
//
// xhr - https://xhr.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/dom.h"
#include "pr/fetch.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A ProgressEvent, a request, and the upload of a request.
typedef struct PrXhrProgressEvent PrXhrProgressEvent;
typedef struct PrXhr PrXhr;
typedef struct PrXhrUpload PrXhrUpload;

// ---------------------------------------------------------------------------------------------------------------------
// ProgressEvent : Event (xhr)

// A ProgressEvent as the interface it inherits from.
PR_HANDLE_UPCAST(XhrProgressEvent, DomEvent, Event)

// The members of ProgressEventInit (xhr/#progressevent).
typedef struct PrXhrProgressEventInit {
    PrDomEventInit event;
    bool lengthComputable;
    double loaded;
    double total;
} PrXhrProgressEventInit;

// The defaults of ProgressEventInit: no length, and nothing loaded.
static inline PrXhrProgressEventInit prXhrProgressEventInitDefault(void) {
    PrXhrProgressEventInit init = {.event = prDomEventInitDefault()};
    return init;
}

// Returns `object` as a ProgressEvent, or null if it is not one.
PrXhrProgressEvent* PR_NULLABLE prXhrProgressEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a ProgressEvent of `type`. A null `init` means the defaults. The caller holds the one reference.
PrXhrProgressEvent* prXhrProgressEventCreate(PrStringView type, const PrXhrProgressEventInit* PR_NULLABLE init);

// Return the event's attributes (xhr/#dom-progressevent-lengthcomputable).
bool prXhrProgressEventLengthComputable(const PrXhrProgressEvent* event);
double prXhrProgressEventLoaded(const PrXhrProgressEvent* event);
double prXhrProgressEventTotal(const PrXhrProgressEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// XMLHttpRequestUpload : EventTarget (xhr)
//
// The target of the events that report a request's upload of its body (xhr/#xmlhttprequestupload). A reference to the
// upload keeps its request alive.

// An upload as the interface it inherits from.
PR_HANDLE_UPCAST(XhrUpload, DomEventTarget, EventTarget)

// Returns `object` as an XMLHttpRequestUpload, or null if it is not one.
PrXhrUpload* PR_NULLABLE prXhrUploadQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// XMLHttpRequest : EventTarget (xhr)

// A request as the interface it inherits from.
PR_HANDLE_UPCAST(Xhr, DomEventTarget, EventTarget)

// The state of a request (xhr/#dom-xmlhttprequest-readystate). The values are those of readyState.
enum {
    PrXhrState_Unsent,
    PrXhrState_Opened,
    PrXhrState_HeadersReceived,
    PrXhrState_Loading,
    PrXhrState_Done,
};
typedef uint8_t PrXhrState;

// A function that a client runs later, with its context. The client keeps a copy, and calls `retain` and `release` on
// the copy's context if they are given.
typedef struct PrXhrTask {
    void (*invoke)(void* PR_NULLABLE context);
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
} PrXhrTask;

// The client of a request: the environment of the document or worker whose script made it
// (html/#relevant-settings-object). The client fetches the request, and runs the request's timers on its event loop.
// Each function is called with `context`, on the client's event loop.
typedef struct PrXhrClient {
    void* PR_NULLABLE context;

    // Returns the client's API base URL (html/#api-base-url), serialized, which a request's URL parses against. The
    // caller releases the string with prRawStringReset.
    PrRawString (*baseUrl)(void* PR_NULLABLE context);

    // Fetches `request` from the client (PrFetchClientRequest), and calls `done` with `doneContext` once, later than
    // this call. A response with the status Lost says that the client went away. Returns a status other than Ok, and
    // never calls `done`, if the client cannot fetch the request.
    PrStatus (*fetch)(void* PR_NULLABLE context, const PrFetchClientRequest* request, PrFetchResponseFn done,
        void* PR_NULLABLE doneContext);

    // Cancels the fetch that `fetch` started with `doneContext`, whose `done` is then never called.
    void (*cancelFetch)(void* PR_NULLABLE context, void* PR_NULLABLE doneContext);

    // Runs `task` after `milliseconds` in a task of the client's event loop, and returns the timer's id, which is not
    // zero. clearTimeout cancels the timer with that id, and releases its task. A client that goes away releases the
    // tasks of its timers.
    int32_t (*setTimeout)(void* PR_NULLABLE context, const PrXhrTask* task, uint32_t milliseconds);
    void (*clearTimeout)(void* PR_NULLABLE context, int32_t id);
} PrXhrClient;

// What a request is created with.
typedef struct PrXhrDesc {
    // The request's client, which the request copies. The client's context must outlive the request.
    PrXhrClient client;
} PrXhrDesc;

// Returns `object` as an XMLHttpRequest, or null if it is not one.
PrXhr* PR_NULLABLE prXhrQuery(PrDomEventTarget* PR_NULLABLE object);

// Creates an unsent request (xhr/#dom-xmlhttprequest). The caller holds the one reference.
PrXhr* prXhrCreate(const PrXhrDesc* desc);

// Adds or releases a reference. The last release frees the request. An active request holds a reference to itself.
void prXhrRetain(PrXhr* xhr);
void prXhrRelease(PrXhr* PR_NULLABLE xhr);

// Called when the request starts or stops having pending activity (xhr/#garbage-collection). A request is active from
// send() until it fires loadend or is aborted. A script binding keeps the request's object alive while it is active,
// because the object holds the listeners that its events reach.
typedef void (*PrXhrActivityFn)(void* PR_NULLABLE context, PrXhr* request, bool active);

// Sets the function that the request calls with each change of its activity, with `context`, or null for none. If the
// request is active, `activity` is called at once to say so. The function that it replaces is not called again.
void prXhrSetActivity(PrXhr* xhr, PrXhrActivityFn PR_NULLABLE activity, void* PR_NULLABLE context);

// Returns the request's upload (xhr/#dom-xmlhttprequest-upload), which lives as long as the request.
PrXhrUpload* prXhrUpload(PrXhr* xhr);

// Returns the request's state.
PrXhrState prXhrReadyState(const PrXhr* xhr);

// Opens the request with `method` and `url` (xhr/#dom-xmlhttprequest-open), which ends any request in flight. The URL
// parses against the client's base URL. Returns Syntax for a method that is not a token or a URL that does not parse,
// NotAllowed for a forbidden method, and NotSupported for a synchronous request.
PrStatus prXhrOpen(PrXhr* xhr, PrStringView method, PrStringView url, bool async);

// Appends `value` to the request's header `name` (xhr/#dom-xmlhttprequest-setrequestheader). A forbidden request header
// is ignored. Returns InvalidState unless the request is opened and not yet sent, and Syntax for a name or a value that
// is not one.
PrStatus prXhrSetRequestHeader(PrXhr* xhr, PrStringView name, PrStringView value);

// The milliseconds after which a request in flight times out (xhr/#dom-xmlhttprequest-timeout), or zero for none.
uint32_t prXhrTimeout(const PrXhr* xhr);
void prXhrSetTimeout(PrXhr* xhr, uint32_t milliseconds);

// Whether the request sends credentials to another origin (xhr/#dom-xmlhttprequest-withcredentials). Setting it returns
// InvalidState once the request is sent, or after it is opened.
bool prXhrWithCredentials(const PrXhr* xhr);
PrStatus prXhrSetWithCredentials(PrXhr* xhr, bool withCredentials);

// Sends the request with `body`, or with no body for the null view (xhr/#dom-xmlhttprequest-send). `contentType` is the
// Content-Type of the body, which the request sends if it sets none of its own, or the null view for none. A GET or
// HEAD request sends no body. Fires loadstart, and later the events of the response. Returns InvalidState unless the
// request is opened and not yet sent.
PrStatus prXhrSend(PrXhr* xhr, PrByteView body, PrStringView contentType);

// Aborts the request (xhr/#dom-xmlhttprequest-abort). A request in flight fires readystatechange, abort and loadend,
// and its state becomes unsent.
void prXhrAbort(PrXhr* xhr);

// Sets the MIME type that the response is read as (xhr/#dom-xmlhttprequest-overridemimetype). A type that does not
// parse is application/octet-stream. Returns InvalidState once the response is loading.
PrStatus prXhrOverrideMimeType(PrXhr* xhr, PrStringView mime);

// What the response is read as (xhr/#dom-xmlhttprequest-responsetype). The request holds the type, and a script binding
// reads the body as it says.
enum {
    PrXhrResponseType_Empty,        // text, which "" names
    PrXhrResponseType_ArrayBuffer,
    PrXhrResponseType_Blob,
    PrXhrResponseType_Document,
    PrXhrResponseType_Json,
    PrXhrResponseType_Text,
};
typedef uint8_t PrXhrResponseType;

// The type that the response is read as. Setting it returns InvalidState once the response is loading.
PrXhrResponseType prXhrResponseType(const PrXhr* xhr);
PrStatus prXhrSetResponseType(PrXhr* xhr, PrXhrResponseType type);

// Returns the response's URL, without its fragment (xhr/#dom-xmlhttprequest-responseurl), or the empty view before the
// response arrives. The view is valid until the request is opened again.
PrStringView prXhrResponseUrl(const PrXhr* xhr);

// Returns the response's status (xhr/#dom-xmlhttprequest-status), or zero before the response arrives and for a network
// error.
uint16_t prXhrStatus(const PrXhr* xhr);

// Returns the response's values of the header `name`, joined with ", " (xhr/#dom-xmlhttprequest-getresponseheader), or
// the null string if the response has no such header. The caller releases the string with prRawStringReset.
PrRawString prXhrResponseHeader(const PrXhr* xhr, PrStringView name);

// Returns the response's headers, each as "name: value" and a CRLF, with lowercase names in order and the values of a
// name joined (xhr/#dom-xmlhttprequest-getallresponseheaders). The caller releases the string with prRawStringReset.
PrRawString prXhrResponseHeaders(const PrXhr* xhr);

// Returns the bytes of the response's body that arrived, or the empty view before the response is loading and after a
// network error. The view is valid until the request is opened again.
PrByteView prXhrResponseBody(const PrXhr* xhr);

// The final MIME type (xhr/#final-mime-type): the override, or else the response's Content-Type, or the null view. The
// view is valid until the request's response changes.
PrStringView prXhrFinalMimeType(const PrXhr* object);

// Returns the response's body decoded as text (xhr/#text-response), in the charset that the final MIME type names, or
// UTF-8. The caller releases the string with prRawStringReset.
PrRawString prXhrResponseText(const PrXhr* xhr);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
