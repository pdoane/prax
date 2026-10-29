// WebSockets: messages exchanged both ways with a server over one connection (rfc 6455), as a client of the WebSocket
// API sees them (websockets/#the-websocket-interface).
//
// A socket follows the contract of its source's network (pr/net.h): its callbacks run on the network's loop, and it is
// used there. A source is an interface, so that a host can bring the WebSockets of a platform that has its own, as a
// browser does.
//
// websockets - https://websockets.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/net.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/tls.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of WebSockets, created by prWebSocketNetworkCreate or by its backend's own constructor.
typedef struct PrWebSocketNetwork PrWebSocketNetwork;

// One WebSocket connection.
typedef struct PrWebSocket PrWebSocket;

// The largest message that a socket receives. A larger message closes the connection with code 1009.
enum { kPrWebSocketMessageMaxBytes = 64 * 1024 * 1024 };

// Where a socket is in its life (websockets/#dom-websocket-readystate).
enum {
    PrWebSocketState_Connecting,
    PrWebSocketState_Open,
    PrWebSocketState_Closing,   // the client or the server began the closing handshake
    PrWebSocketState_Closed,
};
typedef uint8_t PrWebSocketState;

// What a message holds.
enum {
    PrWebSocketMessageKind_Text,    // UTF-8 text
    PrWebSocketMessageKind_Binary,
};
typedef uint8_t PrWebSocketMessageKind;

// What happened to a socket. Open and Close are each delivered at most once, and Close is the last event.
enum {
    PrWebSocketEvent_Open,      // the opening handshake succeeded
    PrWebSocketEvent_Message,   // a whole message arrived, which prWebSocketMessage gives during the callback
    PrWebSocketEvent_Close,     // the connection closed, as prWebSocketCloseInfo says
};
typedef uint8_t PrWebSocketEvent;

// Called on the network's loop with each event of `socket`.
typedef void (*PrWebSocketFn)(void* PR_NULLABLE context, PrWebSocket* socket, PrWebSocketEvent event);

// A message: its kind and its bytes.
typedef struct PrWebSocketMessage {
    PrWebSocketMessageKind kind;
    PrByteView data;
} PrWebSocketMessage;

// How a connection closed (websockets/#closeWebSocket).
typedef struct PrWebSocketCloseInfo {
    // True if the closing handshake completed. False if the connection failed, or the opening handshake did not
    // succeed.
    bool wasClean;

    // The status code and the UTF-8 reason of the Close frame that the server sent. A connection that ended without a
    // Close frame has code 1006, and one whose Close frame had no code has code 1005. Each has an empty reason.
    uint16_t code;
    PrStringView reason;

    PrNetError error;           // why the connection failed, or None for a clean close
} PrWebSocketCloseInfo;

// What a socket connects to.
typedef struct PrWebSocketDesc {
    PrStringView url;           // an absolute ws: or wss: URL, which has no fragment

    // The subprotocols that the client offers (websockets/#dom-websocket-websocket), in order of preference. Each is a
    // token, and none repeats.
    const PrStringView* PR_NULLABLE protocols;
    uint32_t protocolCount;

    // Called with the socket's events, with its context.
    PrWebSocketFn PR_NULLABLE event;
    void* PR_NULLABLE context;
} PrWebSocketDesc;

// The start of every socket's state. `network` is the source that made the socket.
struct PrWebSocket {
    PrWebSocketNetwork* network;
};

// The start of every source's state: the function table of its backend, then its network. Each member of the table
// implements the public function of the same name. Callers use the functions below, never these members.
struct PrWebSocketNetwork {
    void (*destroy)(PrWebSocketNetwork* network);
    PrStatus (*socketCreate)(PrWebSocketNetwork* network, const PrWebSocketDesc* desc,
                             PrWebSocket* PR_NULLABLE* PR_NONNULL out);
    void (*socketDestroy)(PrWebSocketNetwork* network, PrWebSocket* socket);
    PrWebSocketState (*socketState)(const PrWebSocketNetwork* network, const PrWebSocket* socket);
    PrStringView (*socketProtocol)(const PrWebSocketNetwork* network, const PrWebSocket* socket);
    size_t (*socketBufferedAmount)(const PrWebSocketNetwork* network, const PrWebSocket* socket);
    PrStatus (*socketSend)(PrWebSocketNetwork* network, PrWebSocket* socket, PrWebSocketMessageKind kind,
                           PrByteView data);
    void (*socketClose)(PrWebSocketNetwork* network, PrWebSocket* socket, uint16_t code, PrStringView reason);
    PrWebSocketMessage (*socketMessage)(const PrWebSocketNetwork* network, const PrWebSocket* socket);
    PrWebSocketCloseInfo (*socketCloseInfo)(const PrWebSocketNetwork* network, const PrWebSocket* socket);

    PrNet* net;                 // the network whose loop runs the sockets' callbacks
};

// The library's backends of sources, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrWebSocketNetworkBackend_Default,
    PrWebSocketNetworkBackend_Builtin,
    PrWebSocketNetworkBackend_Count,
};
typedef uint8_t PrWebSocketNetworkBackend;

// Returns the backend that `name` names, as prWebSocketNetworkBackendName spells it, ignoring case, or Count if it
// names none.
PrWebSocketNetworkBackend prWebSocketNetworkBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prWebSocketNetworkBackendName(PrWebSocketNetworkBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prWebSocketNetworkBackendIsBuilt(PrWebSocketNetworkBackend backend);

// What a source runs on. Every backend takes the same description, so that an application creates its sockets with the
// same code on every platform.
typedef struct PrWebSocketNetworkDesc {
    PrNet* net;                 // the network whose loop runs the sockets' callbacks

    // The source of TLS sessions, and DER certificates that the sessions trust as roots in addition to the platform's,
    // which must outlive the source. A backend that secures its connections itself uses them, and one whose platform
    // secures them, as a browser does, ignores them. Without a TLS source, such a backend fails every wss: socket.
    PrTls* PR_NULLABLE tls;
    const PrByteView* PR_NULLABLE anchors;
    uint32_t anchorCount;
} PrWebSocketNetworkDesc;

// Creates a source by the backend, as `desc` describes. A backend's own header documents how it connects. Returns null
// if the build does not have the backend. `desc`'s network and TLS source must outlive the source.
PrWebSocketNetwork* PR_NULLABLE prWebSocketNetworkCreate(PrWebSocketNetworkBackend backend,
                                                         const PrWebSocketNetworkDesc* desc);

// Frees the source. Its sockets must be destroyed first. Called on the network's loop, outside the callbacks of its
// sockets.
void prWebSocketNetworkDestroy(PrWebSocketNetwork* PR_NULLABLE network);

// Starts connecting to the server that `desc` names, and writes the socket to `*out`. The socket delivers Open once the
// opening handshake succeeds, or Close if it fails. Its first event comes after this call. Returns InvalidData for a
// URL that does not parse, has a fragment, or offers a subprotocol that is not a token or repeats, and NotSupported for
// a URL whose scheme is not ws or wss. On failure, `*out` is null.
PrStatus prWebSocketCreate(PrWebSocketNetwork* network, const PrWebSocketDesc* desc,
                           PrWebSocket* PR_NULLABLE* PR_NONNULL out);

// Closes the connection at once if it is not closed, and frees the socket. Its callback is not called again. A socket
// may be destroyed during its own callback.
void prWebSocketDestroy(PrWebSocket* PR_NULLABLE socket);

// Returns where the socket is in its life.
PrWebSocketState prWebSocketState(const PrWebSocket* socket);

// Returns the subprotocol that the server chose, or the empty view before Open or if it chose none. The view is valid
// while the socket lives.
PrStringView prWebSocketProtocol(const PrWebSocket* socket);

// Returns the number of bytes of sent messages that the socket has not yet handed to its transport.
size_t prWebSocketBufferedAmount(const PrWebSocket* socket);

// Sends `data` as one message of `kind`, after the messages sent before it. A Text message's data must be UTF-8.
// Returns InvalidState, and sends nothing, if the socket is not open.
PrStatus prWebSocketSend(PrWebSocket* socket, PrWebSocketMessageKind kind, PrByteView data);

// Begins the closing handshake with `code` and the UTF-8 `reason`, or closes a connection that is still connecting.
// `code` is 1000, or 3000 through 4999, and the reason is at most 123 bytes. The socket delivers Close once the server
// answers. Closing a socket that is closing or closed does nothing.
void prWebSocketClose(PrWebSocket* socket, uint16_t code, PrStringView reason);

// Returns the message that arrived, during the callback that delivers Message. The bytes are valid only during that
// callback.
PrWebSocketMessage prWebSocketMessage(const PrWebSocket* socket);

// Returns how the connection closed, once it delivered Close. The reason is valid while the socket lives.
PrWebSocketCloseInfo prWebSocketCloseInfo(const PrWebSocket* socket);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
