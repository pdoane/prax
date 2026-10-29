// The built-in fetcher: Fetch's algorithm over a source of HTTP exchanges.

#pragma once

#include "pr/base.h"
#include "pr/fetch.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a fetcher of the schemes of fetch/#scheme-fetch: http: and https:, whose redirects it follows as each
// request's mode says, and data:, file: and about:blank, which it answers itself. Fetching any other scheme is a
// network error. A file: URL is read from the disk, on the loop, with the MIME type of its extension as its
// Content-Type. Like a data: URL, it is answered whatever the request's mode. A GET is answered from the cache while
// the cached response is fresh, and a stale one is revalidated if it has a validator. `desc`'s HTTP source, codec and
// store must outlive the fetcher.
PrFetchNetwork* prFetchBuiltinCreate(const PrFetchNetworkDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
