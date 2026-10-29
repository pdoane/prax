// JavaScript binding of the Fetch API's objects: Headers and Response, and what fetch() reads of its arguments.
//
// The global's fetch() belongs to the module that knows the global's fetcher, such as HTML's window, which converts its
// arguments with prJsFetchRequestRead and resolves with prJsFetchResponseNew.
//
// fetch - https://fetch.spec.whatwg.org/#fetch-api

#pragma once

#include "pr/byte_view.h"
#include "pr/fetch.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines Headers and Response on the global object of `ctx`.
void prJsFetchBind(JSContext* ctx);

// A request as fetch()'s arguments describe it (fetch/#dom-request), converted from script. The views are held by the
// call's scope, by `parsedUrl`, or by `bytes`. prJsFetchRequestReset frees what the request owns.
typedef struct PrJsFetchRequest {
    PrStringView url;                       // parsed against the base URL, and serialized
    PrString parsedUrl;                     // the storage of the URL
    PrStringView method;                    // normalized, or the null view for GET

    // The request's headers, which the request owns.
    PrFetchHeader* PR_NULLABLE headers;
    uint32_t headerCount;

    PrByteView body;
    PrFetchMode mode;
    PrFetchCredentials credentials;
    PrFetchRedirect redirect;
    JSValue signal;                         // the AbortSignal object that init gives, or undefined; the call's scope
                                            // holds it
    uint8_t* PR_NULLABLE bytes;             // the storage of the headers and the body
} PrJsFetchRequest;

// Converts `input` and `init`, fetch()'s arguments, into `out`, with input's URL parsed against `baseUrl`, the API base
// URL of the caller's realm. Returns false, with a TypeError or the exception of a conversion pending, for arguments
// that make no request: among them a URL that does not parse or that has a username or a password.
bool prJsFetchRequestRead(JSContext* ctx, JSValueConst input, JSValueConst init, PrStringView baseUrl,
                          PrJsFetchRequest* out);

// Frees what the request holds.
void prJsFetchRequestReset(PrJsFetchRequest* request);

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
