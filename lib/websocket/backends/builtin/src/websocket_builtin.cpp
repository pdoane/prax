// The built-in source of WebSockets (websocket_builtin.h).

#include "pr/websocket_builtin.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/base64.h"
#include "pr/checksum.h"
#include "pr/mem.h"
#include "pr/random.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/url.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The longest head of the server's handshake response: its status line and its header fields.
constexpr uint32_t kHeadMaxBytes = 64 * 1024;

// The bytes that one read from a stream takes at most.
constexpr uint32_t kReadChunkBytes = 16 * 1024;

// The GUID that the server appends to the client's key to prove that it read the handshake (rfc 6455 section 1.3).
constexpr PrStringView kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// The status codes of Close frames that the client sends or reports (rfc 6455 section 7.4.1).
constexpr uint16_t kCloseNoStatus = 1005;
constexpr uint16_t kCloseAbnormal = 1006;
constexpr uint16_t kCloseProtocolError = 1002;
constexpr uint16_t kCloseInvalidData = 1007;
constexpr uint16_t kCloseTooBig = 1009;

// The opcodes of frames (rfc 6455 section 5.2).
enum {
    Opcode_Continuation = 0x0,
    Opcode_Text = 0x1,
    Opcode_Binary = 0x2,
    Opcode_Close = 0x8,
    Opcode_Ping = 0x9,
    Opcode_Pong = 0xa,
};
typedef uint8_t Opcode;

struct BuiltinNetwork : PrWebSocketNetwork {
    PrTls* PR_NULLABLE tls;
    const PrByteView* PR_NULLABLE anchors;
    uint32_t anchorCount;
};

struct Socket : PrWebSocket {
    // The callbacks that are running, and the posted tasks that wait, each of which keeps the memory alive after the
    // client destroys the socket.
    uint32_t holds;
    bool dead;

    PrWebSocketFn PR_NULLABLE event;
    void* PR_NULLABLE context;
    PrWebSocketState state;
    PrString host;              // the host to connect to, without an IPv6 address's brackets
    uint16_t port;
    bool secure;
    PrString request;           // the opening handshake's bytes
    PrString accept;            // the Sec-WebSocket-Accept value that the server must answer with
    PrString offered;           // the subprotocols offered, as the Sec-WebSocket-Protocol list sends them
    PrString protocol;          // the subprotocol that the server chose
    PrNetStream* PR_NULLABLE stream;

    // What arrived and was not parsed, from `inputStart`.
    Array<uint8_t> input;
    uint32_t inputStart;
    bool handshaken;

    // The data message being assembled from its fragments.
    Array<uint8_t> message;
    PrWebSocketMessageKind messageKind;
    bool inMessage;
    PrWebSocketMessage delivered;   // the message during the callback that delivers it

    bool closeSent;
    uint16_t closeCode;
    PrString closeReason;
    bool wasClean;
    PrNetError error;
};

PrStringView viewOf(const PrString& string) {
    return prStringView(&string);
}

// Returns true if the comma-separated list `value` has `token`, without regard to ASCII case.
bool listHas(PrStringView value, PrStringView token) {
    size_t start = 0;
    while (start <= value.size) {
        size_t end = start;
        while (end < value.size && value.data[end] != ',')
            ++end;
        if (asciiEqualsFold(stringTrim(stringSlice(value, start, end - start), asciiIsTabOrSpace), token))
            return true;
        start = end + 1;
    }
    return false;
}

void freeSocket(Socket* socket) {
    prStringReset(&socket->host);
    prStringReset(&socket->request);
    prStringReset(&socket->accept);
    prStringReset(&socket->offered);
    prStringReset(&socket->protocol);
    prStringReset(&socket->closeReason);
    socket->input.reset();
    socket->message.reset();
    prMemoryFree(socket);
}

void hold(Socket* socket) {
    ++socket->holds;
}

void release(Socket* socket) {
    PR_ASSERT(socket->holds != 0);
    if (--socket->holds == 0 && socket->dead)
        freeSocket(socket);
}

void deliver(Socket* socket, PrWebSocketEvent event) {
    if (!socket->dead && socket->event != nullptr)
        socket->event(socket->context, socket, event);
}

// Writes a frame of `opcode` with `payload`, masked with a fresh key as every frame from a client is (rfc 6455 section
// 5.3).
void writeFrame(Socket* socket, Opcode opcode, PrByteView first, PrByteView second = {}) {
    if (socket->stream == nullptr)
        return;
    uint64_t size = first.size + second.size;
    Array<uint8_t> frame{};
    frame.reserve(uint32_t(14 + size));
    frame.push(uint8_t(0x80 | opcode));
    if (size < 126) {
        frame.push(uint8_t(0x80 | size));
    } else if (size <= 0xffff) {
        frame.push(0x80 | 126);
        frame.push(uint8_t(size >> 8));
        frame.push(uint8_t(size));
    } else {
        frame.push(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push(uint8_t(size >> shift));
    }
    uint8_t key[4];
    prRandomBytes(key, sizeof(key));
    frame.append(key, 4);
    uint32_t start = frame.count;
    frame.append(first.data, uint32_t(first.size));
    frame.append(second.data, uint32_t(second.size));
    for (uint32_t i = 0; i < uint32_t(size); ++i)
        frame.data[start + i] ^= key[i & 3];
    prNetStreamWrite(socket->stream, PrByteView{frame.data, frame.count});
    frame.reset();
}

void writeClose(Socket* socket, uint16_t code, PrStringView reason) {
    uint8_t status[2] = {uint8_t(code >> 8), uint8_t(code)};
    writeFrame(socket, Opcode_Close, PrByteView{status, 2},
               PrByteView{reinterpret_cast<const uint8_t*>(reason.data), reason.size});
    socket->closeSent = true;
}

// Ends what the client sends, once the bytes written are sent, and keeps the stream until the server ends it.
void finishStream(Socket* socket) {
    if (socket->stream != nullptr && prNetStreamFinish(socket->stream) != PrStatus_Ok) {
        prNetStreamDestroy(socket->stream);
        socket->stream = nullptr;
    }
}

void closeStream(Socket* socket) {
    prNetStreamDestroy(socket->stream);
    socket->stream = nullptr;
}

// Closes the socket and delivers Close, which comes only once.
void closeSocket(Socket* socket, bool wasClean, uint16_t code, PrStringView reason, PrNetError error) {
    if (socket->state == PrWebSocketState_Closed)
        return;
    socket->state = PrWebSocketState_Closed;
    socket->wasClean = wasClean;
    socket->closeCode = code;
    prStringReplace(&socket->closeReason, reason);
    socket->error = error;
    deliver(socket, PrWebSocketEvent_Close);
}

// Fails the connection (rfc 6455 section 7.1.7): sends a Close frame with `code` if the connection is open, and closes
// the socket as unclean.
void failSocket(Socket* socket, uint16_t code, PrNetError error) {
    if (socket->state == PrWebSocketState_Closed)
        return;
    if (socket->handshaken && !socket->closeSent && code != kCloseAbnormal) {
        writeClose(socket, code, "");
        finishStream(socket);
    } else {
        closeStream(socket);
    }
    closeSocket(socket, false, kCloseAbnormal, "", error);
}

// Returns the line that starts at `start` of `text`, without its CRLF, and sets `*next` past it. Returns false if no
// line ending is in `text`.
bool lineAt(PrStringView text, size_t start, PrStringView* line, size_t* next) {
    size_t end = start;
    while (end < text.size && text.data[end] != '\n')
        ++end;
    if (end == text.size)
        return false;
    size_t lineEnd = end > start && text.data[end - 1] == '\r' ? end - 1 : end;
    *line = stringSlice(text, start, lineEnd - start);
    *next = end + 1;
    return true;
}

// Checks the server's handshake response in `head` (rfc 6455 section 4.1), and records the subprotocol that it chose.
bool acceptHead(Socket* socket, PrStringView head) {
    PrStringView line{};
    size_t at = 0;
    if (!lineAt(head, 0, &line, &at) || !stringStartsWith(line, "HTTP/1.1 101"))
        return false;
    if (line.size > 12 && line.data[12] != ' ')
        return false;
    bool upgrade = false;
    bool connection = false;
    bool accepted = false;
    while (lineAt(head, at, &line, &at) && line.size != 0) {
        size_t colon = stringFind(line, ':');
        if (colon == line.size)
            return false;
        PrStringView name = stringSlice(line, 0, colon);
        PrStringView value = stringTrim(stringSlice(line, colon + 1, line.size - colon - 1), asciiIsTabOrSpace);
        if (asciiEqualsFold(name, "upgrade")) {
            upgrade = asciiEqualsFold(value, "websocket");
        } else if (asciiEqualsFold(name, "connection")) {
            connection = listHas(value, "upgrade");
        } else if (asciiEqualsFold(name, "sec-websocket-accept")) {
            accepted = value == viewOf(socket->accept);
        } else if (asciiEqualsFold(name, "sec-websocket-extensions")) {
            // The client offers no extensions, so the server may use none.
            if (value.size != 0)
                return false;
        } else if (asciiEqualsFold(name, "sec-websocket-protocol")) {
            if (value.size == 0 || !listHas(viewOf(socket->offered), value) || stringContains(value, ","))
                return false;
            prStringReplace(&socket->protocol, value);
        }
    }
    return upgrade && connection && accepted;
}

// Reads the server's handshake response once it has all arrived. Returns false if the socket closed or the head has not
// all arrived.
bool readHead(Socket* socket) {
    PrStringView text{reinterpret_cast<const char*>(socket->input.data), socket->input.count};
    size_t end = stringFind(text, "\r\n\r\n");
    if (end == text.size) {
        if (text.size > kHeadMaxBytes)
            failSocket(socket, kCloseAbnormal, PrNetError_Protocol);
        return false;
    }
    if (!acceptHead(socket, stringSlice(text, 0, end + 2))) {
        failSocket(socket, kCloseAbnormal, PrNetError_Protocol);
        return false;
    }
    socket->inputStart = uint32_t(end + 4);
    socket->handshaken = true;
    socket->state = PrWebSocketState_Open;
    deliver(socket, PrWebSocketEvent_Open);
    return !socket->dead && socket->state == PrWebSocketState_Open;
}

bool isValidCloseCode(uint16_t code) {
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) || (code >= 3000 && code <= 4999);
}

// Handles a Close frame from the server with `payload` (rfc 6455 section 5.5.1).
void receiveClose(Socket* socket, PrByteView payload) {
    uint16_t code = kCloseNoStatus;
    PrStringView reason = "";
    if (payload.size == 1) {
        failSocket(socket, kCloseProtocolError, PrNetError_Protocol);
        return;
    }
    if (payload.size >= 2) {
        code = uint16_t(payload.data[0] << 8 | payload.data[1]);
        reason = PrStringView{reinterpret_cast<const char*>(payload.data) + 2, payload.size - 2};
        if (!isValidCloseCode(code)) {
            failSocket(socket, kCloseProtocolError, PrNetError_Protocol);
            return;
        }
        if (prUtf8Validate(reason) != reason.size) {
            failSocket(socket, kCloseInvalidData, PrNetError_Protocol);
            return;
        }
    }
    // A client that did not begin the closing handshake echoes the server's code.
    if (!socket->closeSent) {
        if (code == kCloseNoStatus)
            writeFrame(socket, Opcode_Close, {});
        else
            writeClose(socket, code, "");
        socket->closeSent = true;
    }
    finishStream(socket);
    closeSocket(socket, true, code, reason, PrNetError_None);
}

// Handles the payload of a whole data frame, and delivers the message once its last fragment arrives.
void receiveData(Socket* socket, Opcode opcode, bool fin, PrByteView payload) {
    if (opcode == Opcode_Continuation ? !socket->inMessage : socket->inMessage) {
        failSocket(socket, kCloseProtocolError, PrNetError_Protocol);
        return;
    }
    if (opcode != Opcode_Continuation) {
        socket->inMessage = true;
        socket->messageKind = opcode == Opcode_Text ? PrWebSocketMessageKind_Text : PrWebSocketMessageKind_Binary;
        socket->message.clear();
    }
    if (payload.size > kPrWebSocketMessageMaxBytes - socket->message.count) {
        failSocket(socket, kCloseTooBig, PrNetError_Protocol);
        return;
    }
    socket->message.append(payload.data, uint32_t(payload.size));
    if (!fin)
        return;
    socket->inMessage = false;
    PrByteView data{socket->message.data, socket->message.count};
    if (socket->messageKind == PrWebSocketMessageKind_Text
        && prUtf8Validate(PrStringView{reinterpret_cast<const char*>(data.data), data.size}) != data.size) {
        failSocket(socket, kCloseInvalidData, PrNetError_Protocol);
        return;
    }
    socket->delivered = PrWebSocketMessage{socket->messageKind, data};
    deliver(socket, PrWebSocketEvent_Message);
    if (!socket->dead) {
        socket->delivered = PrWebSocketMessage{};
        socket->message.clear();
    }
}

// Parses the frames that arrived, while the socket is open or closing. Returns once the input holds no whole frame, or
// the socket closed or was destroyed.
void readFrames(Socket* socket) {
    while (!socket->dead
           && (socket->state == PrWebSocketState_Open || socket->state == PrWebSocketState_Closing)) {
        const uint8_t* bytes = socket->input.data + socket->inputStart;
        uint64_t available = socket->input.count - socket->inputStart;
        if (available < 2)
            return;
        bool fin = (bytes[0] & 0x80) != 0;
        Opcode opcode = bytes[0] & 0x0f;
        uint64_t size = bytes[1] & 0x7f;
        uint32_t header = 2;
        if (size == 126) {
            header = 4;
            if (available < header)
                return;
            size = uint64_t(bytes[2]) << 8 | bytes[3];
        } else if (size == 127) {
            header = 10;
            if (available < header)
                return;
            size = 0;
            for (uint32_t i = 2; i < 10; ++i)
                size = size << 8 | bytes[i];
        }
        // A server masks nothing, and sets no reserved bit without an extension (rfc 6455 section 5.2).
        bool control = (opcode & 0x8) != 0;
        bool known = opcode <= Opcode_Binary || (opcode >= Opcode_Close && opcode <= Opcode_Pong);
        if ((bytes[0] & 0x70) != 0 || (bytes[1] & 0x80) != 0 || !known || (control && (!fin || size > 125))) {
            failSocket(socket, kCloseProtocolError, PrNetError_Protocol);
            return;
        }
        if (size > kPrWebSocketMessageMaxBytes) {
            failSocket(socket, kCloseTooBig, PrNetError_Protocol);
            return;
        }
        if (available - header < size)
            return;
        PrByteView payload{bytes + header, size_t(size)};
        socket->inputStart += header + uint32_t(size);
        switch (opcode) {
            case Opcode_Close:
                receiveClose(socket, payload);
                break;
            case Opcode_Ping:
                if (!socket->closeSent)
                    writeFrame(socket, Opcode_Pong, payload);
                break;
            case Opcode_Pong:
                break;
            default:
                // Data that arrives after the client began the closing handshake is discarded.
                if (socket->state == PrWebSocketState_Open)
                    receiveData(socket, opcode, fin, payload);
                break;
        }
    }
}

// Reads what arrived on the stream, then parses it.
void readStream(Socket* socket) {
    while (socket->stream != nullptr) {
        uint32_t start = socket->input.count;
        socket->input.resize(start + kReadChunkBytes);
        size_t got = prNetStreamRead(socket->stream, socket->input.data + start, kReadChunkBytes);
        socket->input.resize(start + uint32_t(got));
        if (got == 0)
            break;
    }
    if (!socket->handshaken && !readHead(socket))
        return;
    readFrames(socket);
    if (socket->dead)
        return;
    // The parsed bytes are dropped, so that the input holds only what is still to be parsed.
    if (socket->inputStart == socket->input.count) {
        socket->input.clear();
        socket->inputStart = 0;
    } else if (socket->inputStart > socket->input.count / 2) {
        socket->input.erase(0, socket->inputStart);
        socket->inputStart = 0;
    }
}

void streamEvent(void* PR_NULLABLE context, PrNetStream* stream, PrNetStreamEvent event) {
    Socket* socket = static_cast<Socket*>(context);
    hold(socket);
    switch (event) {
        case PrNetStreamEvent_Open:
        case PrNetStreamEvent_Writable:
            break;
        case PrNetStreamEvent_Readable:
            readStream(socket);
            break;
        case PrNetStreamEvent_End:
            readStream(socket);
            // The server closed the connection, after the closing handshake or without one.
            if (!socket->dead) {
                closeStream(socket);
                closeSocket(socket, false, kCloseAbnormal, "", PrNetError_Reset);
            }
            break;
        case PrNetStreamEvent_Fail:
            if (!socket->dead) {
                PrNetError error = prNetStreamError(stream);
                closeStream(socket);
                closeSocket(socket, false, kCloseAbnormal, "", error);
            }
            break;
    }
    release(socket);
}

// Connects the socket's stream, from a task, so that a failure is delivered after the call that made the socket.
void connect(void* PR_NULLABLE context) {
    Socket* socket = static_cast<Socket*>(context);
    if (!socket->dead && socket->state == PrWebSocketState_Connecting) {
        BuiltinNetwork* network = static_cast<BuiltinNetwork*>(socket->network);
        PrStatus status = PrStatus_NotSupported;
        if (socket->secure && network->tls != nullptr) {
            constexpr PrStringView kProtocols[] = {"http/1.1"};
            PrTlsSessionDesc desc{
                .host = viewOf(socket->host),
                .port = socket->port,
                .protocols = kProtocols,
                .protocolCount = 1,
                .anchors = network->anchors,
                .anchorCount = network->anchorCount,
                .event = streamEvent,
                .context = socket,
            };
            PrTlsSession* session = nullptr;
            status = prTlsConnect(network->tls, &desc, &session);
            socket->stream = prTlsSessionAsStream(session);
        } else if (!socket->secure) {
            PrNetConnectDesc desc{
                .host = viewOf(socket->host),
                .port = socket->port,
                .event = streamEvent,
                .context = socket,
            };
            status = prNetConnect(network->net, &desc, &socket->stream);
        }
        if (status == PrStatus_Ok) {
            PrStringView request = viewOf(socket->request);
            prNetStreamWrite(socket->stream, PrByteView{reinterpret_cast<const uint8_t*>(request.data), request.size});
        } else {
            closeSocket(socket, false, kCloseAbnormal, "", socket->secure ? PrNetError_Protocol : PrNetError_Io);
        }
    }
    release(socket);
}

// Delivers Close for a socket that the client closed while it was connecting, from a task.
void closeConnecting(void* PR_NULLABLE context) {
    Socket* socket = static_cast<Socket*>(context);
    if (!socket->dead)
        closeSocket(socket, false, kCloseAbnormal, "", PrNetError_None);
    release(socket);
}

// Writes the opening handshake (rfc 6455 section 4.1) for `url`, with a fresh key, and records the accept value that
// the key asks for.
void writeRequest(Socket* socket, const PrUrl* url, const PrWebSocketDesc* desc) {
    uint8_t nonce[16];
    prRandomBytes(nonce, sizeof(nonce));
    char key[24];
    base64Encode(PrStringView{reinterpret_cast<const char*>(nonce), sizeof(nonce)}, key, sizeof(key));
    TextBuffer keyed{};
    textAppend(&keyed, PrStringView{key, sizeof(key)});
    textAppend(&keyed, kAcceptGuid);
    uint8_t digest[kSha1Bytes];
    PrStringView keyedView = textView(&keyed);
    sha1Digest(PrByteView{reinterpret_cast<const uint8_t*>(keyedView.data), keyedView.size}, digest);
    textReset(&keyed);
    char accept[28];
    base64Encode(PrStringView{reinterpret_cast<const char*>(digest), sizeof(digest)}, accept, sizeof(accept));
    socket->accept = prStringCopy(PrStringView{accept, sizeof(accept)});

    TextBuffer offered{};
    for (uint32_t i = 0; i < desc->protocolCount; ++i) {
        if (i != 0)
            textAppend(&offered, ", ");
        textAppend(&offered, desc->protocols[i]);
    }
    socket->offered = textCopy(&offered);
    textReset(&offered);

    TextBuffer out{};
    textAppend(&out, "GET ");
    PrStringView path = prUrlPath(url);
    textAppend(&out, path.size != 0 ? path : PrStringView{"/"});
    PrStringView query = prUrlQuery(url);
    if (query.data != nullptr) {
        textAppendChar(&out, '?');
        textAppend(&out, query);
    }
    textAppend(&out, " HTTP/1.1\r\nHost: ");
    textAppend(&out, prUrlHost(url));
    if (prUrlPort(url) >= 0) {
        textAppendChar(&out, ':');
        textAppendU64(&out, uint64_t(prUrlPort(url)));
    }
    textAppend(&out, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    textAppend(&out, PrStringView{key, sizeof(key)});
    textAppend(&out, "\r\nSec-WebSocket-Version: 13\r\n");
    if (desc->protocolCount != 0) {
        textAppend(&out, "Sec-WebSocket-Protocol: ");
        textAppend(&out, viewOf(socket->offered));
        textAppend(&out, "\r\n");
    }
    textAppend(&out, "\r\n");
    socket->request = textCopy(&out);
    textReset(&out);
}

// Source functions

void destroyNetwork(PrWebSocketNetwork* base) {
    prMemoryFree(static_cast<BuiltinNetwork*>(base));
}

PrStatus createSocket(PrWebSocketNetwork* base, const PrWebSocketDesc* desc, PrWebSocket* PR_NULLABLE* PR_NONNULL out) {
    PrUrl* url = nullptr;
    if (prUrlCreate(desc->url, nullptr, PrCharset_Utf8, &url) != PrStatus_Ok)
        return PrStatus_InvalidData;
    PrStringView scheme = prUrlScheme(url);
    bool secure = scheme == "wss";
    PrStringView host = prUrlHost(url);
    PrStatus status = PrStatus_Ok;
    if (!secure && scheme != "ws")
        status = PrStatus_NotSupported;
    else if (prUrlFragment(url).data != nullptr || host.size == 0)
        status = PrStatus_InvalidData;
    if (status != PrStatus_Ok) {
        prUrlDestroy(url);
        return status;
    }
    Socket* socket = allocZeroed<Socket>();
    socket->network = base;
    socket->event = desc->event;
    socket->context = desc->context;
    socket->secure = secure;
    int32_t port = prUrlPort(url);
    socket->port = uint16_t(port >= 0 ? port : secure ? 443 : 80);
    // An IPv6 address is serialized in brackets, which a connection does not take.
    if (host.size > 2 && host.data[0] == '[')
        host = stringSlice(host, 1, host.size - 2);
    socket->host = prStringCopy(host);
    socket->protocol = prStringEmpty();
    writeRequest(socket, url, desc);
    prUrlDestroy(url);
    hold(socket);
    prNetPost(base->net, connect, socket);
    *out = socket;
    return PrStatus_Ok;
}

void destroySocket(PrWebSocketNetwork*, PrWebSocket* handle) {
    Socket* socket = static_cast<Socket*>(handle);
    PR_ASSERT(!socket->dead);
    socket->dead = true;
    closeStream(socket);
    if (socket->holds == 0)
        freeSocket(socket);
}

PrWebSocketState socketState(const PrWebSocketNetwork*, const PrWebSocket* handle) {
    return static_cast<const Socket*>(handle)->state;
}

PrStringView socketProtocol(const PrWebSocketNetwork*, const PrWebSocket* handle) {
    return viewOf(static_cast<const Socket*>(handle)->protocol);
}

size_t socketBufferedAmount(const PrWebSocketNetwork*, const PrWebSocket* handle) {
    const Socket* socket = static_cast<const Socket*>(handle);
    return socket->stream != nullptr ? prNetStreamQueued(socket->stream) : 0;
}

PrStatus socketSend(PrWebSocketNetwork*, PrWebSocket* handle, PrWebSocketMessageKind kind, PrByteView data) {
    Socket* socket = static_cast<Socket*>(handle);
    if (socket->state != PrWebSocketState_Open)
        return PrStatus_InvalidState;
    writeFrame(socket, kind == PrWebSocketMessageKind_Text ? Opcode_Text : Opcode_Binary, data);
    return PrStatus_Ok;
}

void socketClose(PrWebSocketNetwork* base, PrWebSocket* handle, uint16_t code, PrStringView reason) {
    Socket* socket = static_cast<Socket*>(handle);
    if (socket->state == PrWebSocketState_Connecting) {
        // A connection that has not opened is failed (websockets/#dom-websocket-close).
        closeStream(socket);
        socket->state = PrWebSocketState_Closing;
        hold(socket);
        prNetPost(base->net, closeConnecting, socket);
        return;
    }
    if (socket->state != PrWebSocketState_Open)
        return;
    socket->state = PrWebSocketState_Closing;
    writeClose(socket, code, reason);
}

PrWebSocketMessage socketMessage(const PrWebSocketNetwork*, const PrWebSocket* handle) {
    return static_cast<const Socket*>(handle)->delivered;
}

PrWebSocketCloseInfo socketCloseInfo(const PrWebSocketNetwork*, const PrWebSocket* handle) {
    const Socket* socket = static_cast<const Socket*>(handle);
    return PrWebSocketCloseInfo{.wasClean = socket->wasClean,
                                .code = socket->closeCode,
                                .reason = prStringIsNull(&socket->closeReason) ? PrStringView{""}
                                                                               : viewOf(socket->closeReason),
                                .error = socket->error};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrWebSocketNetwork* prWebSocketBuiltinCreate(const PrWebSocketNetworkDesc* desc) {
    BuiltinNetwork* network = allocZeroed<BuiltinNetwork>();
    network->destroy = destroyNetwork;
    network->socketCreate = createSocket;
    network->socketDestroy = destroySocket;
    network->socketState = socketState;
    network->socketProtocol = socketProtocol;
    network->socketBufferedAmount = socketBufferedAmount;
    network->socketSend = socketSend;
    network->socketClose = socketClose;
    network->socketMessage = socketMessage;
    network->socketCloseInfo = socketCloseInfo;
    network->net = desc->net;
    network->tls = desc->tls;
    network->anchors = desc->anchors;
    network->anchorCount = desc->anchorCount;
    return network;
}
