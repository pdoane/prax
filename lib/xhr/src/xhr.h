// The state of XMLHttpRequest and ProgressEvent (pr/xhr.h).
//
// xhr - https://xhr.spec.whatwg.org/

#pragma once

#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/xhr.h"

#include "dom/src/dom_event.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The attributes of a ProgressEvent.
struct XhrProgressData : DomEventExtra {
    bool lengthComputable;
    double loaded;
    double total;
};

// A header's name and value, which the request owns.
struct XhrHeader {
    PrRawString name;
    PrRawString value;
};

// The upload of a request, which is part of the request.
struct XhrUpload : DomPartTarget {};

// A request (xhr/#interface-xmlhttprequest). Its fetch and its timer each hold a reference to it.
struct Xhr : DomCountedTarget {
    PrXhrClient client;
    PrXhrActivityFn PR_NULLABLE activity;
    void* PR_NULLABLE activityContext;
    XhrUpload upload;

    // The request that open() and setRequestHeader() describe.
    PrRawString method;                     // normalized, or the null string for GET
    PrRawString url;                        // parsed and serialized
    RawArray<XhrHeader> requestHeaders;     // in the order set, with the values of a name combined
    uint32_t timeout;                       // in milliseconds, or zero for none
    bool withCredentials;
    PrXhrResponseType responseType;
    PrRawString overrideMime;                  // the MIME type that overrideMimeType() set, or the null string

    // The response, which is empty before it arrives and after a network error.
    uint16_t status;
    PrRawString responseUrl;
    RawArray<XhrHeader> responseHeaders;
    RawArray<uint8_t> body;

    PrXhrState state;
    bool sendFlag;                          // xhr/#send-flag
    bool uploadComplete;                    // xhr/#upload-complete-flag
    bool uploadListener;                    // xhr/#upload-listener-flag
    bool fetching;                          // true while the client fetches for the request
    bool active;                            // true while the request has pending activity
    int32_t timer;                          // the client's timer that times out or fails the request, or zero
    uint64_t requestBytes;                  // the size of the body that the request sends

    // Changed by each open() and abort(), so that the steps that follow an event stop if a listener started another
    // request.
    uint32_t generation;
};

} // namespace pr_impl

// C opaque types
struct PrXhrProgressEvent : PrDomEvent {};
struct PrXhrUpload : pr_impl::XhrUpload {};
struct PrXhr : pr_impl::Xhr {};

PR_ASSUME_NONNULL_END
