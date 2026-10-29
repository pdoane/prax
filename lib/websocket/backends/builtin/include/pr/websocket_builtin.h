// The built-in source of WebSockets: the WebSocket protocol (rfc 6455) over the streams of a network and of a TLS
// backend.

#pragma once

#include "pr/base.h"
#include "pr/websocket.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a source that opens a connection of its own for each socket, and negotiates no extensions. A ws: socket
// connects over TCP, and a wss: socket over TLS. The client masks every frame that it sends with a key from
// prRandomBytes. `desc`'s network and TLS source must outlive the source.
PrWebSocketNetwork* prWebSocketBuiltinCreate(const PrWebSocketNetworkDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
