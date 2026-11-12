#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/net_sockets.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/tls.h"
#include "pr/websocket.h"

#include "tools/harness/net_peer.h"

#include "tools/harness/test.h"

using namespace harness;

extern "C" int prTestWebSocketCApi(void);

namespace pr_impl {

namespace {

// The network, the peer and the source of sockets of one test. The source trusts the peer's CA.
struct Fixture {
    PrNet* net;
    PrTls* PR_NULLABLE tls;
    PrWebSocketNetwork* network;
    NetPeer peer;
    PrByteView anchor;
};

bool open(Fixture* fixture) {
    if (!netPeerOpen(&fixture->peer)) {
        FAIL("the tests of lib/websocket need tools/harness/net_peer.py's servers: ./dev.sh test websocket");
        return false;
    }
    fixture->net = prNetSocketsCreate();
    fixture->tls = prTlsCreate(PrTlsBackend_Default, fixture->net);
    fixture->anchor = PrByteView{fixture->peer.ca.data, fixture->peer.ca.size};
    PrWebSocketNetworkDesc desc{
        .net = fixture->net,
        .tls = fixture->tls,
        .anchors = &fixture->anchor,
        .anchorCount = 1,
    };
    fixture->network = prWebSocketNetworkCreate(PrWebSocketNetworkBackend_Default, &desc);
    return true;
}

void reset(Fixture* fixture) {
    prNetPostBlocking(fixture->net, [](void* PR_NULLABLE network) {
        prWebSocketNetworkDestroy(static_cast<PrWebSocketNetwork*>(network));
    }, fixture->network);
    prTlsDestroy(fixture->tls);
    prNetDestroy(fixture->net);
    netPeerReset(&fixture->peer);
}

// A message as a probe records it.
struct Message {
    PrWebSocketMessageKind kind;
    String payload;

    friend bool operator==(const Message& a, const Message& b) = default;
};

// One socket run from a test's thread: what it sends once it opens, when it closes, and what it received, which the
// loop writes before it opens `done`.
struct Probe {
    PrWebSocketNetwork* network;
    String url;
    Array<String> protocols;
    Array<Message> send;
    uint32_t closeAfter;        // the number of messages received after which the client closes
    bool closeWhileConnecting;
    PrWebSocket* PR_NULLABLE socket;
    NetLatch done;
    PrStatus created;
    Array<PrWebSocketEvent> events;
    Array<Message> received;
    String protocol;
    PrWebSocketCloseInfo close;
    String reason;
};

void probeEvent(void* PR_NULLABLE context, PrWebSocket* socket, PrWebSocketEvent event) {
    Probe* probe = static_cast<Probe*>(context);
    probe->events.push(event);
    switch (event) {
        case PrWebSocketEvent_Open: {
            PrStringView protocol = prWebSocketProtocol(socket);
            probe->protocol = protocol;
            for (const Message& message : probe->send) {
                PrByteView data = prStringBytes(message.payload);
                CHECK(prWebSocketSend(socket, message.kind, data) == PrStatus_Ok);
            }
            if (probe->closeAfter == 0)
                prWebSocketClose(socket, 1000, "done");
            break;
        }
        case PrWebSocketEvent_Message: {
            PrWebSocketMessage message = prWebSocketMessage(socket);
            probe->received.push(Message{message.kind, String(prStringFromBytes(message.data))});
            if (probe->received.count() == probe->closeAfter)
                prWebSocketClose(socket, 1000, "done");
            break;
        }
        case PrWebSocketEvent_Close:
            CHECK(prWebSocketState(socket) == PrWebSocketState_Closed);
            CHECK(prWebSocketSend(socket, PrWebSocketMessageKind_Text, {}) == PrStatus_InvalidState);
            probe->close = prWebSocketCloseInfo(socket);
            probe->reason = probe->close.reason;
            prWebSocketDestroy(socket);
            probe->socket = nullptr;
            netLatchOpen(&probe->done);
            break;
    }
}

void startProbe(void* PR_NULLABLE context) {
    Probe* probe = static_cast<Probe*>(context);
    Array<PrStringView> protocols;
    for (const String& protocol : probe->protocols)
        protocols.push(protocol);
    PrWebSocketDesc desc{
        .url = probe->url,
        .protocols = protocols.data(),
        .protocolCount = protocols.count(),
        .event = probeEvent,
        .context = probe,
    };
    probe->created = prWebSocketCreate(probe->network, &desc, &probe->socket);
    if (probe->created != PrStatus_Ok) {
        netLatchOpen(&probe->done);
        return;
    }
    CHECK(prWebSocketState(probe->socket) == PrWebSocketState_Connecting);
    if (probe->closeWhileConnecting)
        prWebSocketClose(probe->socket, 1000, "");
}

// Runs a probe on `url` and waits until its socket closes, or fails to be made.
void run(Fixture* fixture, Probe* probe, PrStringView url) {
    probe->network = fixture->network;
    probe->url = url;
    netLatchInit(&probe->done);
    prNetPost(fixture->net, startProbe, probe);
    netLatchWait(&probe->done);
    netLatchReset(&probe->done);
}

String urlOf(const char* scheme, uint16_t port, const char* path) {
    return format("{}://localhost:{}{}", scheme, port, path);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestWebSocketCApi() == 0);
}

TEST_CASE("messages echo both ways, and the client's close completes the closing handshake") {
    Fixture fixture{};
    REQUIRE(open(&fixture));
    String large;
    for (uint32_t i = 0; i < 100000; ++i)
        large.append('x');
    const char* const schemes[] = {"ws", "wss"};
    for (const char* scheme : schemes) {
        CAPTURE(scheme);
        Probe probe{
            .send = Array<Message>({
                {PrWebSocketMessageKind_Text, "hello \xE2\x9C\x93"},
                {PrWebSocketMessageKind_Binary, PrStringView{"\0\1\2", 3}},
                {PrWebSocketMessageKind_Text, large},
            }),
            .closeAfter = 3,
        };
        run(&fixture, &probe, urlOf(scheme, scheme[2] == 's' ? fixture.peer.wss : fixture.peer.ws, "/echo"));
        REQUIRE(probe.created == PrStatus_Ok);
        REQUIRE(probe.received.count() == 3);
        CHECK(probe.received[0] == Message{PrWebSocketMessageKind_Text, "hello \xE2\x9C\x93"});
        CHECK(probe.received[1] == Message{PrWebSocketMessageKind_Binary, PrStringView{"\0\1\2", 3}});
        CHECK(probe.received[2].payload == large);
        CHECK(probe.events[0] == PrWebSocketEvent_Open);
        CHECK(probe.close.wasClean);
        CHECK(probe.close.code == 1000);
        CHECK(probe.close.error == PrNetError_None);
    }
    reset(&fixture);
}

TEST_CASE("the server chooses a subprotocol among those offered") {
    Fixture fixture{};
    REQUIRE(open(&fixture));
    Probe probe{.protocols = Array<String>({"other", "superchat", "chat"}), .closeAfter = 0};
    run(&fixture, &probe, urlOf("ws", fixture.peer.ws, "/echo"));
    CHECK(probe.protocol == "superchat");
    CHECK(probe.close.wasClean);

    Probe repeated{.protocols = Array<String>({"chat", "chat"})};
    run(&fixture, &repeated, urlOf("ws", fixture.peer.ws, "/echo"));
    CHECK(repeated.created == PrStatus_InvalidData);
    reset(&fixture);
}

TEST_CASE("fragments join into one message, a ping is answered, and a long frame reads whole") {
    Fixture fixture{};
    REQUIRE(open(&fixture));
    Probe probe{.closeAfter = 2};
    run(&fixture, &probe, urlOf("ws", fixture.peer.ws, "/fragmented"));
    REQUIRE(probe.received.count() == 2);
    CHECK(probe.received[0] == Message{PrWebSocketMessageKind_Text, "hello world"});
    CHECK(probe.received[1].kind == PrWebSocketMessageKind_Binary);
    REQUIRE(probe.received[1].payload.count() == 70000);
    CHECK(uint8_t(probe.received[1].payload.data()[69999]) == 69999 % 251);
    CHECK(probe.close.wasClean);
    reset(&fixture);
}

TEST_CASE("the server's close reports its code and reason") {
    Fixture fixture{};
    REQUIRE(open(&fixture));
    Probe probe{.closeAfter = 100};
    run(&fixture, &probe, urlOf("ws", fixture.peer.ws, "/close"));
    CHECK(probe.events == Array<PrWebSocketEvent>({PrWebSocketEvent_Open, PrWebSocketEvent_Close}));
    CHECK(probe.close.wasClean);
    CHECK(probe.close.code == 4000);
    CHECK(probe.reason == "bye");
    reset(&fixture);
}

TEST_CASE("a failed handshake, a broken frame or an abrupt end closes without a clean close") {
    Fixture fixture{};
    REQUIRE(open(&fixture));
    const char* const paths[] = {"/reject", "/bad-accept", "/bad-frame", "/abort"};
    for (const char* path : paths) {
        CAPTURE(path);
        Probe probe{.closeAfter = 100};
        run(&fixture, &probe, urlOf("ws", fixture.peer.ws, path));
        CHECK(!probe.close.wasClean);
        CHECK(probe.close.code == 1006);
        CHECK(probe.close.error != PrNetError_None);
        bool opened = !probe.events.isEmpty() && probe.events[0] == PrWebSocketEvent_Open;
        CHECK(opened == (PrStringView(path) == "/bad-frame" || PrStringView(path) == "/abort"));
    }
    Probe refused{.closeAfter = 100};
    run(&fixture, &refused, urlOf("ws", fixture.peer.closed, "/echo"));
    CHECK(refused.events == Array<PrWebSocketEvent>({PrWebSocketEvent_Close}));
    CHECK(refused.close.error == PrNetError_Refused);
    reset(&fixture);
}

TEST_CASE("closing a connecting socket closes it after the call") {
    Fixture fixture{};
    REQUIRE(open(&fixture));
    Probe probe{.closeAfter = 100, .closeWhileConnecting = true};
    run(&fixture, &probe, urlOf("ws", fixture.peer.ws, "/echo"));
    CHECK(probe.events == Array<PrWebSocketEvent>({PrWebSocketEvent_Close}));
    CHECK(!probe.close.wasClean);
    CHECK(probe.close.code == 1006);
    reset(&fixture);
}

TEST_CASE("a URL that is not a WebSocket URL is refused") {
    Fixture fixture{};
    REQUIRE(open(&fixture));
    Probe http{};
    run(&fixture, &http, "http://localhost/");
    CHECK(http.created == PrStatus_NotSupported);
    Probe fragment{};
    run(&fixture, &fragment, "ws://localhost/#part");
    CHECK(fragment.created == PrStatus_InvalidData);
    Probe malformed{};
    run(&fixture, &malformed, "ws://");
    CHECK(malformed.created == PrStatus_InvalidData);
    reset(&fixture);
}
