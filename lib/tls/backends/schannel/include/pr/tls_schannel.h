// TLS sessions by SChannel, over streams.

#pragma once

#include "pr/base.h"
#include "pr/tls.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a source of sessions on `net`, each of which runs SChannel over a stream: the transport that the session is
// given, or a connection that it makes on `net`. A session negotiates TLS 1.2 or 1.3. `net` must outlive the source.
//
// A certificate is verified by the system's chain engine against the system's roots and the session's anchors, and by
// the SSL policy against the server's name. Revocation is not checked. An anchor is trusted as the root that a chain
// ends in, so an anchor that is not self-signed ends no chain. Verification runs on a thread of the source's own,
// because the chain engine may fetch a missing issuer over the network.
//
// A session can finish (prNetStreamFinish) and still read. It tells a transport that ends without the peer's
// close_notify from the peer's end, and fails with PrTlsFailure_Truncated. A TLS 1.2 server that renegotiates must
// present the certificate that it presented first.
PrTls* prTlsSchannelCreate(PrNet* net);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
