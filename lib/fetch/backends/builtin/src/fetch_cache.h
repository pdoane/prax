// The HTTP cache's rules (rfc 9111): what a response may be stored as, how long it stays fresh, and the record that a
// store keeps for it.

#pragma once

#include "pr/byte_view.h"
#include "pr/fetch.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The directives of a Cache-Control header (rfc 9111 section 5.2) that a private cache reads.
struct FetchCacheControl {
    bool noStore;
    bool noCache;
    bool hasMaxAge;
    double maxAge;              // in seconds
};

// Reads the directives of the headers named Cache-Control in `headers`. A Pragma of no-cache counts as Cache-Control's
// no-cache (rfc 9111 section 5.4).
FetchCacheControl fetchCacheControlFromHeaders(const PrFetchHeader* PR_NULLABLE headers, uint32_t count);

// A response as the cache keeps it: its status and headers, the request's values of the headers that its Vary names,
// when it was requested and received, and its decoded body. Its views are into the record that it was read from, or
// into the response that it was made from.
struct FetchCachedResponse {
    uint16_t status;
    PrTimeSystem requested;
    PrTimeSystem received;
    RawArray<PrFetchHeader> headers;
    RawArray<PrFetchHeader> varied;    // each header that Vary names, with the request's value or the null view
    PrByteView body;
};

// Returns the value of the first header in `headers` named `name`, or the null view.
PrStringView fetchCacheHeader(const PrFetchHeader* PR_NULLABLE headers, uint32_t count, PrStringView name);

// Returns true if a private cache may store the response to a GET with `status` and `headers` (rfc 9111 section 3).
bool fetchCacheIsStorable(uint16_t status, const PrFetchHeader* PR_NULLABLE headers, uint32_t count);

// Returns how long the response stays fresh after it was made, in milliseconds (rfc 9111 section 4.2.1).
double fetchCacheLifetime(const FetchCachedResponse* response);

// Returns the response's age at `now`, in milliseconds (rfc 9111 section 4.2.3).
double fetchCacheAge(const FetchCachedResponse* response, PrTimeSystem now);

// Returns true if the request's headers match the values that the response's Vary recorded (rfc 9111 section 4.1). A
// Vary of `*` matches nothing.
bool fetchCacheVaryMatches(const FetchCachedResponse* response, const PrFetchHeader* PR_NULLABLE request, uint32_t count);

// Appends the record of `response` to `out`.
void fetchCacheWrite(const FetchCachedResponse* response, PrTextBuffer* out);

// Reads the record `bytes` into `*out`, whose views are into `bytes`. Returns false if the record does not parse.
bool fetchCacheRead(PrByteView bytes, FetchCachedResponse* out);

// Frees what the response's arrays hold.
void fetchCachedResponseReset(FetchCachedResponse* response);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
