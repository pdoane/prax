// TLS sessions by Network.framework.

#pragma once

#include "pr/base.h"
#include "pr/tls.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a source of sessions on `net` by Network.framework. Network.framework resolves a session's host, connects to
// it over TCP and runs TLS as one connection, so a session connects on its own and cannot run over a transport that it
// is given. Certificates are verified by the system's trust evaluation, against the system's roots and the session's
// anchors. Network.framework calls back on a queue of the source's, which posts to `net`'s loop. `net` must outlive the
// source.
//
// Network.framework's TLS has no half-close, so a session cannot finish (prNetStreamFinish) and sends its close_notify
// when it is destroyed. It also reports a transport that ends without the peer's close_notify as the peer's end, so a
// session delivers End where another backend fails with PrTlsFailure_Truncated.
PrTls* prTlsAppleCreate(PrNet* net);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
