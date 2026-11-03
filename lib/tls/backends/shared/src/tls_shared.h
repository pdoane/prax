// What the TLS backends that run an engine over a transport share: a session's state and buffers of plaintext, the
// calls of PrNetStream and PrTlsSession that do not depend on the engine, and the handling of the transport's events.
// Each backend's session derives from TlsSharedSession and names its engine's steps in a TlsSharedEngine.

#pragma once

#include "pr/base.h"
#include "pr/net.h"
#include "pr/raw_array.h"
#include "pr/tls.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The most bytes of plaintext that a session holds that arrived and were not read. A session stops reading from its
// transport while it holds this many.
constexpr uint32_t kTlsSharedReceivedMaxBytes = 256 * 1024;

// The states of a session.
enum {
    TlsSharedState_Handshaking,
    TlsSharedState_Verifying,       // the handshake completed, and the certificate is being verified
    TlsSharedState_Open,
    TlsSharedState_Failed,
};
typedef uint8_t TlsSharedState;

struct TlsSharedSession;

// The steps of a backend's engine that the shared calls take.
struct TlsSharedEngine {
    // Moves what the engine holds: the handshake's next step, and the records that it decrypts into `received`.
    void (*advance)(TlsSharedSession* session);

    // Hands the bytes that the transport holds to the engine, and advances it.
    void (*drainTransport)(TlsSharedSession* session);

    // Writes the close_notify alert, and finishes the transport once it is written.
    void (*sendCloseNotify)(TlsSharedSession* session);

    // Frees the session's memory, with the engine's state.
    void (*free)(TlsSharedSession* session);
};

// A session. `received` from `receivedStart` holds plaintext that arrived and was not read, and `unsent` holds
// plaintext written before the session opened.
//
// The session's memory outlives its destruction while `holds` is not zero: while one of its handlers runs, which a
// client may destroy it from, and while a task that it posted waits.
struct TlsSharedSession : PrTlsSession {
    const TlsSharedEngine* engine;
    PrNetStream* PR_NULLABLE transport;
    TlsSharedState state;
    PrNetError error;
    PrTlsFailure failure;
    PrTlsVersion version;
    PrStringView protocol;          // points into the engine's state, which lives as long as the session
    RawArray<uint8_t> received;
    uint32_t receivedStart;
    RawArray<uint8_t> unsent;
    uint32_t holds;
    bool finishing;
    bool finished;
    bool ended;                     // true once the peer's close_notify arrived
    bool transportEnded;
    bool stalled;                   // true while the session holds as much plaintext as it may
    bool writableOwed;
    bool dead;
};

// Adds or releases a hold on the session's memory. The last release of a destroyed session frees it.
void tlsSharedHold(TlsSharedSession* session);
void tlsSharedRelease(TlsSharedSession* session);

// Delivers `event` to the session's client, unless the session was destroyed.
void tlsSharedDeliver(TlsSharedSession* session, PrNetStreamEvent event);

// Fails the session and delivers Fail, now from one of its handlers, or later from a task for a call of the client's. A
// failed session fails no further.
void tlsSharedFail(TlsSharedSession* session, PrNetError error, PrTlsFailure failure);
void tlsSharedFailLater(TlsSharedSession* session, PrNetError error, PrTlsFailure failure);

// Handles an event of the session's transport, whose context is the session (PrNetStreamFn).
void tlsSharedTransportEvent(void* PR_NULLABLE context, PrNetStream* transport, PrNetStreamEvent event);

// The session's calls of PrNetStream and PrTlsSession that do not depend on the engine.
size_t tlsSharedRead(PrNetStream* stream, uint8_t* output, size_t capacity);
PrStatus tlsSharedFinish(PrNetStream* stream);
size_t tlsSharedQueued(const PrNetStream* stream);
PrNetError tlsSharedError(const PrNetStream* stream);
PrTlsSessionInfo tlsSharedInfo(const PrTlsSession* tlsSession);

// Returns the session that a stream of PrNetStream's calls is.
inline TlsSharedSession* tlsSharedSessionFromStream(PrNetStream* stream) {
    return static_cast<TlsSharedSession*>(reinterpret_cast<PrTlsSession*>(stream));
}
inline const TlsSharedSession* tlsSharedSessionFromStream(const PrNetStream* stream) {
    return static_cast<const TlsSharedSession*>(reinterpret_cast<const PrTlsSession*>(stream));
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
