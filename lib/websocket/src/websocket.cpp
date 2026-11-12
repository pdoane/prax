// WebSockets: messages exchanged both ways with a server over one connection.

#include "pr/websocket.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/websocket_backend.h"

#if defined(PR_WEBSOCKET_HAS_BUILTIN)
#include "pr/websocket_builtin.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrWebSocketNetworkBackend_Count] = {"default", "builtin"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrWebSocketNetworkBackend kBuilt[] = {
#if defined(PR_WEBSOCKET_HAS_BUILTIN)
    PrWebSocketNetworkBackend_Builtin,
#endif
    PrWebSocketNetworkBackend_Default,
};

bool isToken(PrStringView text) {
    if (text.size == 0)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsHttpToken(text.data[i]))
            return false;
    }
    return true;
}

// Returns true if each subprotocol is a token and none repeats (websockets/#dom-websocket-websocket).
bool areProtocols(const PrStringView* PR_NULLABLE protocols, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!isToken(protocols[i]))
            return false;
        for (uint32_t k = 0; k < i; ++k) {
            if (protocols[k] == protocols[i])
                return false;
        }
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrWebSocketNetworkBackend prWebSocketNetworkBackendFromName(PrStringView name) {
    for (PrWebSocketNetworkBackend backend = 0; backend < PrWebSocketNetworkBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrWebSocketNetworkBackend_Count;
}

PrStringView prWebSocketNetworkBackendName(PrWebSocketNetworkBackend backend) {
    return backend < PrWebSocketNetworkBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prWebSocketNetworkBackendIsBuilt(PrWebSocketNetworkBackend backend) {
    for (PrWebSocketNetworkBackend built : kBuilt) {
        if (built != PrWebSocketNetworkBackend_Default &&
                (built == backend || backend == PrWebSocketNetworkBackend_Default))
            return true;
    }
    return false;
}

PrWebSocketNetwork* PR_NULLABLE prWebSocketNetworkCreate(PrWebSocketNetworkBackend backend,
        const PrWebSocketNetworkDesc* desc) {
    switch (backend == PrWebSocketNetworkBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_WEBSOCKET_HAS_BUILTIN)
        case PrWebSocketNetworkBackend_Builtin: return prWebSocketBuiltinCreate(desc);
#endif
        default: return nullptr;
    }
}

void prWebSocketNetworkDestroy(PrWebSocketNetwork* PR_NULLABLE network) {
    if (network != nullptr)
        network->destroyFn(network);
}

PrStatus prWebSocketCreate(PrWebSocketNetwork* network, const PrWebSocketDesc* desc,
        PrWebSocket* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(desc->protocols != nullptr || desc->protocolCount == 0);
    *out = nullptr;
    if (!areProtocols(desc->protocols, desc->protocolCount))
        return PrStatus_InvalidData;
    PrStatus status = network->socketCreateFn(network, desc, out);
    PR_ASSERT(status != PrStatus_Ok || *out != nullptr);
    return status;
}

void prWebSocketDestroy(PrWebSocket* PR_NULLABLE socket) {
    if (socket != nullptr)
        socket->destroyFn(socket);
}

PrWebSocketState prWebSocketState(const PrWebSocket* socket) {
    return socket->stateFn(socket);
}

PrStringView prWebSocketProtocol(const PrWebSocket* socket) {
    return socket->protocolFn(socket);
}

size_t prWebSocketBufferedAmount(const PrWebSocket* socket) {
    return socket->bufferedAmountFn(socket);
}

PrStatus prWebSocketSend(PrWebSocket* socket, PrWebSocketMessageKind kind, PrByteView data) {
    PR_ASSERT(data.data != nullptr || data.size == 0);
    return socket->sendFn(socket, kind, data);
}

void prWebSocketClose(PrWebSocket* socket, uint16_t code, PrStringView reason) {
    PR_ASSERT(code == 1000 || (code >= 3000 && code <= 4999));
    PR_ASSERT(reason.size <= 123);
    socket->closeFn(socket, code, reason);
}

PrWebSocketMessage prWebSocketMessage(const PrWebSocket* socket) {
    return socket->messageFn(socket);
}

PrWebSocketCloseInfo prWebSocketCloseInfo(const PrWebSocket* socket) {
    return socket->closeInfoFn(socket);
}
