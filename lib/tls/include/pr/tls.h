// TLS sessions: streams secured by TLS (rfc 8446, rfc 5246), with the peer's certificate verified by the backend.
//
// A session is a stream of its network (pr/net.h), and follows its contract: its callbacks run on the network's loop,
// and it is used there.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/net.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of TLS sessions over a network. Each backend provides its own constructor.
typedef struct PrTls PrTls;

// A TLS session, which is a stream. Its stream delivers Open once the handshake completes and the certificate is
// verified, and End for the peer's close_notify. prNetStreamFinish sends the session's own close_notify if the backend
// can end its sending side alone.
typedef struct PrTlsSession PrTlsSession;

// The versions of TLS that a session negotiates. Nothing older than TLS 1.2 is negotiated.
enum {
    PrTlsVersion_None,          // not negotiated yet
    PrTlsVersion_Tls12,
    PrTlsVersion_Tls13,
};
typedef uint8_t PrTlsVersion;

// Why a session failed, beyond the PrNetError_Protocol that its stream reports.
enum {
    PrTlsFailure_None,
    PrTlsFailure_Untrusted,     // the certificate chain does not end in a trusted root
    PrTlsFailure_Expired,       // a certificate of the chain is outside its validity
    PrTlsFailure_NameMismatch,  // the certificate does not name the server
    PrTlsFailure_Handshake,     // the handshake failed in another way, such as no protocol version in common
    PrTlsFailure_Truncated,     // the transport ended without the peer's close_notify
    PrTlsFailure_Protocol,      // the peer sent a record that is malformed or not allowed, or an alert
};
typedef uint8_t PrTlsFailure;

// What a session connects to and trusts.
typedef struct PrTlsSessionDesc {
    // The stream that the session runs over, which the session takes over once it is made and destroys with itself. If
    // null, the session connects to `host` and `port` itself.
    PrNetStream* PR_NULLABLE transport;
    PrStringView host;
    uint16_t port;

    // The name that the server's certificate must name: a domain name, which is also sent as the server name (rfc
    // 6066), or an IP address. The null view means `host`.
    PrStringView serverName;

    // The application protocols that the session offers (rfc 7301), in order of preference, such as "http/1.1".
    const PrStringView* PR_NULLABLE protocols;
    uint32_t protocolCount;

    // DER certificates that the session trusts as roots, in addition to the platform's.
    const PrByteView* PR_NULLABLE anchors;
    uint32_t anchorCount;

    // Called with the session's events, with its context. The stream that the callback receives is the session's
    // (prTlsSessionAsStream).
    PrNetStreamFn PR_NULLABLE event;
    void* PR_NULLABLE context;
} PrTlsSessionDesc;

// What a session negotiated, and why it failed.
typedef struct PrTlsSessionInfo {
    PrTlsVersion version;
    PrTlsFailure failure;

    // The application protocol that the server chose, or the null view. It is valid while the session lives.
    PrStringView protocol;
} PrTlsSessionInfo;

// Returns the session's stream, which is the same object.
PR_HANDLE_UPCAST(TlsSession, NetStream, Stream)

// The library's backends of TLS, which the build turns on (README.md), and Default, which creates the platform's own if
// the build has it, and otherwise the first that it has.
enum {
    PrTlsBackend_Default,
    PrTlsBackend_Apple,         // Network.framework
    PrTlsBackend_Schannel,
    PrTlsBackend_Openssl,
    PrTlsBackend_Count,
};
typedef uint8_t PrTlsBackend;

// Returns the backend that `name` names, as prTlsBackendName spells it, ignoring case, or Count if it names none.
PrTlsBackend prTlsBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prTlsBackendName(PrTlsBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prTlsBackendIsBuilt(PrTlsBackend backend);

// Creates a source of sessions on `net` by the backend. A backend's own header documents what its sessions do. Returns
// null if the build does not have the backend. `net` must outlive the source.
PrTls* PR_NULLABLE prTlsCreate(PrTlsBackend backend, PrNet* net);

// Frees the source. Its sessions must be destroyed first. Called from a thread other than the network's loop, before
// the network is destroyed.
void prTlsDestroy(PrTls* PR_NULLABLE tls);

// Starts a session as `desc` describes it, and writes it to `*out`. A transport must belong to the source's network.
// Returns NotSupported if the backend cannot run over a transport that it is given, or cannot connect without one, and
// InvalidData for a host or server name that is not a domain name or an address, or an anchor that is not a DER
// certificate. On failure, `*out` is null, and the transport stays the caller's.
PrStatus prTlsConnect(PrTls* tls, const PrTlsSessionDesc* desc, PrTlsSession* PR_NULLABLE* PR_NONNULL out);

// Returns what the session negotiated so far, and why it failed.
PrTlsSessionInfo prTlsSessionInfo(const PrTlsSession* session);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
