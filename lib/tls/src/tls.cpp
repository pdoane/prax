// TLS sessions: streams secured by TLS, with the peer's certificate verified by the backend.

#include "pr/tls.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/tls_backend.h"

#if defined(PR_TLS_HAS_APPLE)
#include "pr/tls_apple.h"
#endif
#if defined(PR_TLS_HAS_SCHANNEL)
#include "pr/tls_schannel.h"
#endif
#if defined(PR_TLS_HAS_OPENSSL)
#include "pr/tls_openssl.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrTlsBackend_Count] = {"default", "apple", "schannel", "openssl"};

// The backends that the build has, the platform's own first, which Default creates the first of, and then Default, so
// that a build without backends creates none.
constexpr PrTlsBackend kBuilt[] = {
#if defined(PR_TLS_HAS_APPLE)
    PrTlsBackend_Apple,
#endif
#if defined(PR_TLS_HAS_SCHANNEL)
    PrTlsBackend_Schannel,
#endif
#if defined(PR_TLS_HAS_OPENSSL)
    PrTlsBackend_Openssl,
#endif
    PrTlsBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTlsBackend prTlsBackendFromName(PrStringView name) {
    for (PrTlsBackend backend = 0; backend < PrTlsBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrTlsBackend_Count;
}

PrStringView prTlsBackendName(PrTlsBackend backend) {
    return backend < PrTlsBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prTlsBackendIsBuilt(PrTlsBackend backend) {
    for (PrTlsBackend built : kBuilt) {
        if (built != PrTlsBackend_Default && (built == backend || backend == PrTlsBackend_Default))
            return true;
    }
    return false;
}

PrTls* PR_NULLABLE prTlsCreate(PrTlsBackend backend, PrNet* net) {
    // A build without a backend has no use for the network.
    (void)net;
    switch (backend == PrTlsBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_TLS_HAS_APPLE)
        case PrTlsBackend_Apple: return prTlsAppleCreate(net);
#endif
#if defined(PR_TLS_HAS_SCHANNEL)
        case PrTlsBackend_Schannel: return prTlsSchannelCreate(net);
#endif
#if defined(PR_TLS_HAS_OPENSSL)
        case PrTlsBackend_Openssl: return prTlsOpensslCreate(net);
#endif
        default: return nullptr;
    }
}

void prTlsDestroy(PrTls* PR_NULLABLE tls) {
    if (tls != nullptr)
        tls->destroyFn(tls);
}

PrStatus prTlsConnect(PrTls* tls, const PrTlsSessionDesc* desc, PrTlsSession* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(desc->protocols != nullptr || desc->protocolCount == 0);
    PR_ASSERT(desc->anchors != nullptr || desc->anchorCount == 0);
    *out = nullptr;
    PrStatus status = tls->connectFn(tls, desc, out);
    PR_ASSERT(status != PrStatus_Ok || *out != nullptr);
    return status;
}

PrTlsSessionInfo prTlsSessionInfo(const PrTlsSession* session) {
    return session->infoFn(session);
}
