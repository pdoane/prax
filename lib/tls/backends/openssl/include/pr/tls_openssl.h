// TLS sessions by OpenSSL, over streams.

#pragma once

#include "pr/base.h"
#include "pr/tls.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a source of sessions on `net`, each of which runs OpenSSL over a stream: the transport that the session is
// given, or a connection that it makes on `net`. Certificates are verified by OpenSSL against its default roots and the
// session's anchors. `net` must outlive the source.
PrTls* prTlsOpensslCreate(PrNet* net);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
