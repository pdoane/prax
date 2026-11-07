// The interface that TLS backends implement. Callers use the functions of pr/tls.h, which call these. Only backends
// include this header.

#pragma once

#include "pr/base.h"
#include "pr/net_backend.h"
#include "pr/tls.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a session's backend, at the start of its state: the functions of its stream, and `infoFn`, which
// implements prTlsSessionInfo.
struct PrTlsSession {
    PrNetStream stream;
    PrTlsSessionInfo (*infoFn)(const PrTlsSession* session);
};

// The functions of a source's backend, at the start of its state. Each implements the function of pr/tls.h that it is
// named for.
struct PrTls {
    void (*destroyFn)(PrTls* tls);
    PrStatus (*connectFn)(PrTls* tls, const PrTlsSessionDesc* desc, PrTlsSession* PR_NULLABLE* PR_NONNULL out);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
