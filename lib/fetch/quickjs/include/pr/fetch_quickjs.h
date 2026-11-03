// JavaScript binding of the Fetch API's objects: Headers, Request and Response, and what fetch() reads of its
// arguments.
//
// The global's fetch() belongs to the module that knows the global's fetcher, such as HTML's window, which converts its
// arguments with prJsFetchRequestRead and resolves with prJsFetchResponseNew.
//
// fetch - https://fetch.spec.whatwg.org/#fetch-api

#pragma once

#include "pr/byte_view.h"
#include "pr/fetch.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines Headers, Request and Response on the global object of `ctx`.
void prJsFetchBind(JSContext* ctx);

// Returns the API base URL of the realm (html/#api-base-url), or the null string for none, which Request's constructor
// parses against.
typedef PrRawString (*PrJsFetchBaseUrlFn)(JSContext* ctx);

// Sets how the layer finds a realm's base URL, in every realm. The module that knows the global sets it.
void prJsFetchSetBaseUrl(PrJsFetchBaseUrlFn fn);

// A request as fetch()'s arguments describe it (fetch/#dom-request), converted from script. The views are held by the
// call's scope, by `parsedUrl`, or by `bytes`. prJsFetchRequestReset frees what the request owns.
typedef struct PrJsFetchRequest {
    PrStringView url;                       // parsed against the base URL, and serialized
    PrRawString parsedUrl;                  // the storage of the URL
    PrStringView method;                    // normalized, or the null view for GET

    // The request's headers, which the request owns.
    PrFetchHeader* PR_NULLABLE headers;
    uint32_t headerCount;

    PrByteView body;
    PrFetchMode mode;
    PrFetchCredentials credentials;
    PrFetchRedirect redirect;
    PrFetchReferrerPolicy referrerPolicy;
    PrRawString referrer;                      // a URL, the empty string for none, or the null string for the client's
    PrRawString integrity;
    uint8_t cache;                          // the index of the request's cache mode (fetch/#requestcache)
    bool keepalive;
    bool hasBody;                           // false for the null body, and true for an empty body
    JSValue signal;                         // the AbortSignal object that init gives, or undefined; the call's scope
                                            // holds it
    uint8_t* PR_NULLABLE bytes;             // the storage of the headers and the body
} PrJsFetchRequest;

// Converts `input` and `init`, fetch()'s arguments, into `out`, with input's URL parsed against `baseUrl`, the API base
// URL of the caller's realm. Returns false, with a TypeError or the exception of a conversion pending, for arguments
// that make no request: among them a URL that does not parse or that has a username or a password.
bool prJsFetchRequestRead(JSContext* ctx, JSValueConst input, JSValueConst init, PrStringView baseUrl, PrJsFetchRequest* out);

// Frees what the request holds.
void prJsFetchRequestReset(PrJsFetchRequest* request);

// Extracts the bytes and the type of a BodyInit (fetch/#concept-bodyinit-extract). The type is the null view for none.
// The call's scope holds the views. Returns false with an exception pending.
bool prJsFetchBodyExtract(JSContext* ctx, JSValueConst value, PrByteView* bytes, PrStringView* type);

// A response that the network gave (fetch/#concept-response), filtered as its type says.
typedef struct PrJsFetchResponseDesc {
    PrFetchResponseType type;
    uint16_t status;
    PrStringView url;
    const PrFetchHeader* PR_NULLABLE headers;
    uint32_t headerCount;
    PrByteView body;
    JSValueConst signal;                    // the AbortSignal object whose abort errors the body, or undefined
} PrJsFetchResponseDesc;

// Returns a Response of `desc`, whose headers are immutable, as fetch() resolves with it.
JSValue prJsFetchResponseNew(JSContext* ctx, const PrJsFetchResponseDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
