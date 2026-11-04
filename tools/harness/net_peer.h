// The peer that tools/harness/net_peer.py runs for the tests of lib/net, lib/tls and the protocols over them, and a
// probe that drives one stream from a test's thread.

#pragma once

#include "pr/byte_view.h"
#include "pr/file.h"
#include "pr/net.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/thread.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// The ports of the peer's servers, and the certificate of the CA that issued the certificates of the servers that tests
// trust.
struct NetPeer {
    uint16_t tcpEcho;
    uint16_t tlsEcho;
    uint16_t tlsTruncating;
    uint16_t tlsExpired;
    uint16_t tlsUntrusted;
    uint16_t http;              // HTTP over TCP
    uint16_t https;             // HTTP over TLS
    uint16_t ws;                // WebSocket over TCP
    uint16_t wss;               // WebSocket over TLS
    uint16_t comfy;             // a fake ComfyUI server, over TCP
    uint16_t closed;            // a port that nothing listens on
    PrFileMapping ca;           // DER
};

// Reads the peer from the directory that PR_NET_PEER names. Returns false if the tests run without the peer.
bool netPeerOpen(NetPeer* out);

// Frees what the peer holds.
void netPeerReset(NetPeer* peer);

// A gate that a test's thread waits at until the network's loop opens it.
struct NetLatch {
    PrMutex mutex;
    PrCondition changed;
    bool open;
};

// Initializes the latch closed, or frees what it holds once no thread waits at it.
void netLatchInit(NetLatch* latch);
void netLatchReset(NetLatch* latch);

// Opens the latch, from any thread.
void netLatchOpen(NetLatch* latch);

// Waits until the latch is open, then closes it again.
void netLatchWait(NetLatch* latch);

// Makes the stream that a probe drives, on the loop, with the probe's callback and context.
typedef PrStatus (*NetProbeOpenFn)(void* PR_NULLABLE context, PrNet* net, PrNetStreamFn event, void* eventContext,
    PrNetStream* PR_NULLABLE* PR_NONNULL out);

// Called on the loop with the probe's stream, once it opens and once it ends or fails.
typedef void (*NetProbeInspectFn)(void* PR_NULLABLE context, PrNetStream* stream);

// What a probe does with its stream: it writes `payload` as soon as the stream is made, finishes it if `finish`, and
// reads until the stream ends or fails.
struct NetProbeScript {
    NetProbeOpenFn open;
    void* PR_NULLABLE context;
    PrByteView payload;
    bool finish;
    NetProbeInspectFn PR_NULLABLE inspect;
};

// One stream driven on its network's loop, and what it delivered. `mutex` guards the fields after it.
struct NetProbe {
    PrNet* net;
    NetProbeScript script;
    PrNetStream* PR_NULLABLE stream;
    PrMutex mutex;
    PrCondition changed;
    PrStatus status;            // what the open function returned
    PrStatus finished;          // what prNetStreamFinish returned, if the script finishes
    RawArray<PrNetStreamEvent> events;
    RawArray<uint8_t> received;
    PrNetError error;
    bool done;                  // true once the stream ended or failed, or could not be made
    bool destroyed;
};

// Runs `script` on a stream of `net`, and waits until the stream ends or fails. The probe keeps the stream until
// netProbeReset.
void netProbeRun(NetProbe* probe, PrNet* net, const NetProbeScript* script);

// Returns true if the probe received exactly `text`.
bool netProbeReceived(const NetProbe* probe, PrStringView text);

// Returns the number of times the probe's stream delivered `event`.
uint32_t netProbeEventCount(const NetProbe* probe, PrNetStreamEvent event);

// Destroys the probe's stream on the loop, waits, and frees what the probe holds.
void netProbeReset(NetProbe* probe);

} // namespace harness

PR_ASSUME_NONNULL_END
