// The fetcher whose host answers its requests: each request goes to the host, which answers it with a response or a
// network error, or passes it on to the next fetcher, as an application's own resources, a test's files or a capture of
// a page answer.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/fetch.h"
#include "pr/net.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A request that the host answers once: the request as its fetch was created with it, with its URL serialized without
// its fragment, and a null callback and context. The views are valid until the request is answered.
typedef struct PrFetchInterceptRequest {
    PrFetchRequestDesc desc;
} PrFetchInterceptRequest;

// Called on the loop with each request that the fetcher receives, after the call to prFetchCreate that made it. The
// host answers the request once: with prFetchInterceptRequestRespond or prFetchInterceptRequestFail, or by passing it
// on with prFetchInterceptRequestPass. It answers during the call or later, on the loop. The answer frees the request,
// even after its fetch is destroyed.
typedef void (*PrFetchInterceptRequestFn)(void* PR_NULLABLE context, PrFetchInterceptRequest* request);

// What an intercepting fetcher answers with.
typedef struct PrFetchInterceptDesc {
    PrNet* net;                 // the network whose loop runs the fetches

    // Called with each request, with its context.
    PrFetchInterceptRequestFn request;
    void* PR_NULLABLE context;

    // The fetcher that requests are passed on to, or null if none are. It runs on the same network, and must outlive
    // this fetcher.
    PrFetchNetwork* PR_NULLABLE next;

    // The fetcher's default User-Agent value (prFetchNetworkUserAgent), which the view is copied from. The null view
    // stands for the next fetcher's, or for kPrFetchDefaultUserAgent without a next fetcher.
    PrStringView userAgent;
} PrFetchInterceptDesc;

// A response that the host gives. The fetch copies it.
typedef struct PrFetchInterceptResponse {
    uint16_t status;
    PrFetchResponseType type;
    const PrFetchHeader* PR_NULLABLE headers;
    uint32_t headerCount;
    PrByteView body;

    // The response's URL, as redirects that the host followed left it, without its fragment; or the null view for the
    // request's URL.
    PrStringView url;
} PrFetchInterceptResponse;

// Creates a fetcher whose host answers each request. Its fetches deliver the response that the host gives, whole, as
// the host gives it.
PrFetchNetwork* prFetchInterceptCreate(const PrFetchInterceptDesc* desc);

// Answers the request with `response`.
void prFetchInterceptRequestRespond(PrFetchInterceptRequest* request, const PrFetchInterceptResponse* response);

// Answers the request with a network error, for the reason `error`, which is not None.
void prFetchInterceptRequestFail(PrFetchInterceptRequest* request, PrNetError error);

// Passes the request on to the next fetcher, whose fetch answers it. The fetcher must have a next fetcher.
void prFetchInterceptRequestPass(PrFetchInterceptRequest* request);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
