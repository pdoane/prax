#include "pr/net_sockets.h"
#include "pr/tls.h"

#include "tools/harness/net_peer.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

using namespace harness;

extern "C" int prTestTlsCApi(void);

namespace pr_impl {

namespace {

// A backend under test, and what its header says that it cannot do.
struct Backend {
    PrTlsBackend backend;
    std::string name;
    bool overTransport;         // true if its sessions run over a transport that they are given
    bool finishes;              // true if its sessions end their sending side alone
    bool detectsTruncation;     // true if it tells a transport's end from the peer's close_notify
};

// The backends that the build has. Network.framework connects on its own and has no half-close.
std::vector<Backend> backends() {
    std::vector<Backend> found;
    for (PrTlsBackend backend = PrTlsBackend_Default + 1; backend < PrTlsBackend_Count; ++backend) {
        if (!prTlsBackendIsBuilt(backend))
            continue;
        PrStringView name = prTlsBackendName(backend);
        bool streams = backend != PrTlsBackend_Apple;
        found.push_back(Backend{backend, std::string(name.data, name.size), streams, streams, streams});
    }
    return found;
}

// How a probe's session is made, and what it saw. `info` and `protocol` are copied on the loop when the session opens
// and when it ends or fails.
struct Session {
    PrTls* tls;
    PrTlsSessionDesc desc;
    bool overTransport;         // true to connect TCP first, and run the session over it
    PrTlsSessionInfo info;
    std::string protocol;
};

PrStatus openSession(void* PR_NULLABLE context, PrNet* net, PrNetStreamFn event, void* eventContext,
        PrNetStream* PR_NULLABLE* PR_NONNULL out) {
    Session* session = static_cast<Session*>(context);
    PrTlsSessionDesc desc = session->desc;
    desc.event = event;
    desc.context = eventContext;
    PrNetStream* transport = nullptr;
    if (session->overTransport) {
        PrNetConnectDesc connect{.host = desc.host, .port = desc.port};
        PrStatus status = prNetConnect(net, &connect, &transport);
        if (status != PrStatus_Ok)
            return status;
        desc.transport = transport;
    }
    PrTlsSession* made = nullptr;
    PrStatus status = prTlsConnect(session->tls, &desc, &made);
    if (status != PrStatus_Ok) {
        prNetStreamDestroy(transport);
        return status;
    }
    *out = prTlsSessionAsStream(made);
    return PrStatus_Ok;
}

void inspect(void* PR_NULLABLE context, PrNetStream* stream) {
    Session* session = static_cast<Session*>(context);
    session->info = prTlsSessionInfo(reinterpret_cast<PrTlsSession*>(stream));
    session->protocol.assign(session->info.protocol.data != nullptr ? session->info.protocol.data : "",
        session->info.protocol.size);
}

const PrStringView kProtocols[] = {"h2", "http/1.1"};

// The network, the peer and the source of sessions of one test.
struct Fixture {
    PrNet* net;
    NetPeer peer;
    PrTls* tls;
    PrByteView anchor;
};

bool open(Fixture* fixture, const Backend& backend) {
    if (!netPeerOpen(&fixture->peer)) {
        FAIL("the tests of lib/tls need tools/harness/net_peer.py's servers: ./dev.sh test tls");
        return false;
    }
    fixture->net = prNetSocketsCreate();
    fixture->tls = prTlsCreate(backend.backend, fixture->net);
    fixture->anchor = PrByteView{fixture->peer.ca.data, fixture->peer.ca.size};
    return true;
}

void reset(Fixture* fixture) {
    prTlsDestroy(fixture->tls);
    prNetDestroy(fixture->net);
    netPeerReset(&fixture->peer);
}

// The descriptor of a session to `host` and `port` that offers kProtocols and trusts the peer's CA.
PrTlsSessionDesc trusting(const Fixture& fixture, PrStringView host, uint16_t port) {
    return PrTlsSessionDesc{
        .host = host,
        .port = port,
        .protocols = kProtocols,
        .protocolCount = 2,
        .anchors = &fixture.anchor,
        .anchorCount = 1,
    };
}

// Whether a session finishes after it writes its payload.
enum {
    Ending_Wait,                // the payload asks the peer to end
    Ending_Finish,
};
typedef uint8_t Ending;

// Runs a session that writes `payload`, ends as `ending` says, and reads until it ends or fails.
void run(NetProbe* probe, Fixture* fixture, Session* session, PrByteView payload, Ending ending) {
    NetProbeScript script{
        .open = openSession,
        .context = session,
        .payload = payload,
        .finish = ending == Ending_Finish,
        .inspect = inspect,
    };
    netProbeRun(probe, fixture->net, &script);
}

PrByteView bytesOf(const char* text) {
    size_t size = 0;
    while (text[size] != 0)
        ++size;
    return PrByteView{reinterpret_cast<const uint8_t*>(text), size};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("tls: the public headers compile as C") {
    CHECK(prTestTlsCApi() == 0);
}

TEST_CASE("tls: a session negotiates TLS 1.3 and ALPN, carries bytes, and ends at the peer's close_notify") {
    for (const Backend& backend : backends()) {
        INFO("backend: " << backend.name);
        Fixture fixture{};
        if (!open(&fixture, backend))
            return;
        Session session{.tls = fixture.tls, .desc = trusting(fixture, "localhost", fixture.peer.tlsEcho)};
        NetProbe probe{};
        run(&probe, &fixture, &session, bytesOf("hello<bye>"), Ending_Wait);
        CHECK(probe.status == PrStatus_Ok);
        CHECK(netProbeReceived(&probe, "hello<bye><eof:asked>"));
        REQUIRE(probe.events.count != 0);
        CHECK(probe.events.data[0] == PrNetStreamEvent_Open);
        CHECK(netProbeEventCount(&probe, PrNetStreamEvent_End) == 1);
        CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Fail) == 0);
        CHECK(session.info.version == PrTlsVersion_Tls13);
        CHECK(session.protocol == "http/1.1");
        CHECK(session.info.failure == PrTlsFailure_None);
        netProbeReset(&probe);
        reset(&fixture);
    }
}

TEST_CASE("tls: finishing sends close_notify while the session still reads, where the backend can") {
    for (const Backend& backend : backends()) {
        INFO("backend: " << backend.name);
        Fixture fixture{};
        if (!open(&fixture, backend))
            return;
        Session session{.tls = fixture.tls, .desc = trusting(fixture, "localhost", fixture.peer.tlsEcho)};
        NetProbe probe{};
        if (backend.finishes) {
            run(&probe, &fixture, &session, bytesOf("hello"), Ending_Finish);
            CHECK(probe.finished == PrStatus_Ok);
            CHECK(netProbeReceived(&probe, "hello<eof:clean>"));
            CHECK(netProbeEventCount(&probe, PrNetStreamEvent_End) == 1);
        } else {
            // The payload also asks the peer to end, so the session ends without finishing.
            run(&probe, &fixture, &session, bytesOf("hello<bye>"), Ending_Finish);
            CHECK(probe.finished == PrStatus_NotSupported);
            CHECK(netProbeReceived(&probe, "hello<bye><eof:asked>"));
        }
        netProbeReset(&probe);
        reset(&fixture);
    }
}

TEST_CASE("tls: a server named by its address is verified against the certificate's addresses") {
    for (const Backend& backend : backends()) {
        INFO("backend: " << backend.name);
        Fixture fixture{};
        if (!open(&fixture, backend))
            return;
        Session session{.tls = fixture.tls, .desc = trusting(fixture, "127.0.0.1", fixture.peer.tlsEcho)};
        NetProbe probe{};
        run(&probe, &fixture, &session, bytesOf("ip<bye>"), Ending_Wait);
        CHECK(netProbeReceived(&probe, "ip<bye><eof:asked>"));
        CHECK(session.info.failure == PrTlsFailure_None);
        netProbeReset(&probe);
        reset(&fixture);
    }
}

TEST_CASE("tls: a session runs over a transport that it is given, or the backend says it cannot") {
    for (const Backend& backend : backends()) {
        INFO("backend: " << backend.name);
        Fixture fixture{};
        if (!open(&fixture, backend))
            return;
        Session session{
            .tls = fixture.tls,
            .desc = trusting(fixture, "localhost", fixture.peer.tlsEcho),
            .overTransport = true,
        };
        NetProbe probe{};
        run(&probe, &fixture, &session, bytesOf("layered<bye>"), Ending_Wait);
        if (backend.overTransport) {
            CHECK(probe.status == PrStatus_Ok);
            CHECK(netProbeReceived(&probe, "layered<bye><eof:asked>"));
            CHECK(session.info.version == PrTlsVersion_Tls13);
        } else {
            CHECK(probe.status == PrStatus_NotSupported);
        }
        netProbeReset(&probe);
        reset(&fixture);
    }
}

TEST_CASE("tls: bytes beyond the buffers flow both ways") {
    std::string payload;
    for (uint32_t i = 0; payload.size() < 3 * 1024 * 1024; ++i)
        payload += "record " + std::to_string(i) + "\n";
    payload += "<bye>";
    for (const Backend& backend : backends()) {
        INFO("backend: " << backend.name);
        Fixture fixture{};
        if (!open(&fixture, backend))
            return;
        Session session{.tls = fixture.tls, .desc = trusting(fixture, "localhost", fixture.peer.tlsEcho)};
        NetProbe probe{};
        run(&probe, &fixture, &session, PrByteView{reinterpret_cast<const uint8_t*>(payload.data()), payload.size()},
            Ending_Wait);
        CHECK(netProbeReceived(&probe, payload + "<eof:asked>"));
        CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Writable) >= 1);
        netProbeReset(&probe);
        reset(&fixture);
    }
}

TEST_CASE("tls: a certificate that fails verification fails the session with its reason") {
    struct Case {
        const char* name;
        PrStringView serverName;
        uint16_t NetPeer::* port;
        bool anchored;
        PrTlsFailure failure;
    };
    const Case cases[] = {
        {"another name", "wrong.test", &NetPeer::tlsEcho, true, PrTlsFailure_NameMismatch},
        {"expired", PrStringView{}, &NetPeer::tlsExpired, true, PrTlsFailure_Expired},
        {"an untrusted issuer", PrStringView{}, &NetPeer::tlsUntrusted, true, PrTlsFailure_Untrusted},
        {"the platform's roots alone", PrStringView{}, &NetPeer::tlsEcho, false, PrTlsFailure_Untrusted},
    };
    for (const Backend& backend : backends()) {
        for (const Case& test : cases) {
            INFO("backend: " << backend.name << ", case: " << std::string(test.name));
            Fixture fixture{};
            if (!open(&fixture, backend))
                return;
            Session session{.tls = fixture.tls, .desc = trusting(fixture, "localhost", fixture.peer.*test.port)};
            session.desc.serverName = test.serverName;
            if (!test.anchored)
                session.desc.anchorCount = 0;
            NetProbe probe{};
            run(&probe, &fixture, &session, bytesOf("secret<bye>"), Ending_Wait);
            CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Open) == 0);
            CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Fail) == 1);
            CHECK(probe.received.count == 0);
            CHECK(probe.error == PrNetError_Protocol);
            CHECK(session.info.failure == test.failure);
            netProbeReset(&probe);
            reset(&fixture);
        }
    }
}

TEST_CASE("tls: a transport that ends without close_notify truncates the session, where the backend can tell") {
    for (const Backend& backend : backends()) {
        INFO("backend: " << backend.name);
        Fixture fixture{};
        if (!open(&fixture, backend))
            return;
        Session session{.tls = fixture.tls, .desc = trusting(fixture, "localhost", fixture.peer.tlsTruncating)};
        NetProbe probe{};
        run(&probe, &fixture, &session, PrByteView{}, Ending_Wait);
        CHECK(netProbeReceived(&probe, "bye"));
        if (backend.detectsTruncation) {
            CHECK(netProbeEventCount(&probe, PrNetStreamEvent_End) == 0);
            CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Fail) == 1);
            CHECK(probe.error == PrNetError_Protocol);
            CHECK(session.info.failure == PrTlsFailure_Truncated);
        } else {
            CHECK(netProbeEventCount(&probe, PrNetStreamEvent_End) == 1);
        }
        netProbeReset(&probe);
        reset(&fixture);
    }
}

TEST_CASE("tls: a refused connection fails with the transport's reason") {
    for (const Backend& backend : backends()) {
        INFO("backend: " << backend.name);
        Fixture fixture{};
        if (!open(&fixture, backend))
            return;
        Session session{.tls = fixture.tls, .desc = trusting(fixture, "127.0.0.1", fixture.peer.closed)};
        NetProbe probe{};
        run(&probe, &fixture, &session, PrByteView{}, Ending_Wait);
        CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Fail) == 1);
        CHECK(probe.error == PrNetError_Refused);
        CHECK(session.info.failure == PrTlsFailure_None);
        netProbeReset(&probe);
        reset(&fixture);
    }
}
