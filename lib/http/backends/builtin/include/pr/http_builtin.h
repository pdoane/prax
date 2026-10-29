// The built-in source of HTTP exchanges: HTTP/1.1 (rfc 9112) over the streams of a network and of a TLS backend.

#pragma once

#include "pr/base.h"
#include "pr/http.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a source that sends each request on a connection of its own until the response ends, and keeps a connection
// that the server leaves open for the next request to its origin. A request that a kept connection closes before any of
// the response arrives is sent once more, on a new connection. `desc`'s network and TLS source must outlive the source.
PrHttp* prHttpBuiltinCreate(const PrHttpDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
