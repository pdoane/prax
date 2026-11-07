// The interface that WebSocket backends implement. Callers use the functions of pr/websocket.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/websocket.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a socket's backend, at the start of its state. Each implements the function of pr/websocket.h that
// it is named for.
struct PrWebSocket {
    void (*destroyFn)(PrWebSocket* socket);
    PrWebSocketState (*stateFn)(const PrWebSocket* socket);
    PrStringView (*protocolFn)(const PrWebSocket* socket);
    size_t (*bufferedAmountFn)(const PrWebSocket* socket);
    PrStatus (*sendFn)(PrWebSocket* socket, PrWebSocketMessageKind kind, PrByteView data);
    void (*closeFn)(PrWebSocket* socket, uint16_t code, PrStringView reason);
    PrWebSocketMessage (*messageFn)(const PrWebSocket* socket);
    PrWebSocketCloseInfo (*closeInfoFn)(const PrWebSocket* socket);
};

// The functions of a source's backend, at the start of its state. Each implements the function of pr/websocket.h that
// it is named for.
struct PrWebSocketNetwork {
    void (*destroyFn)(PrWebSocketNetwork* network);
    PrStatus (*socketCreateFn)(PrWebSocketNetwork* network, const PrWebSocketDesc* desc,
        PrWebSocket* PR_NULLABLE* PR_NONNULL out);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
