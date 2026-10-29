#include "pr/net_sockets.h"

#include "tools/harness/net_peer.h"

#include "doctest/doctest.h"

#include <string>

using namespace harness;

extern "C" int prTestNetCApi(void);

namespace pr_impl {

namespace {

// Where a probe connects.
struct Target {
    PrStringView host;
    uint16_t port;
};

PrStatus openTcp(void* PR_NULLABLE context, PrNet* net, PrNetStreamFn event, void* eventContext,
                 PrNetStream* PR_NULLABLE* PR_NONNULL out) {
    const Target* target = static_cast<const Target*>(context);
    PrNetConnectDesc desc{
        .host = target->host,
        .port = target->port,
        .event = event,
        .context = eventContext,
    };
    return prNetConnect(net, &desc, out);
}

// The network and the peer of one test.
struct Fixture {
    PrNet* net;
    NetPeer peer;
};

bool open(Fixture* fixture) {
    if (!netPeerOpen(&fixture->peer)) {
        FAIL("the tests of lib/net need tools/harness/net_peer.py's servers: ./dev.sh test net");
        return false;
    }
    fixture->net = prNetSocketsCreate();
    return true;
}

void reset(Fixture* fixture) {
    prNetDestroy(fixture->net);
    netPeerReset(&fixture->peer);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("net: the public headers compile as C") {
    CHECK(prTestNetCApi() == 0);
}

TEST_CASE("net: a stream sends, finishes, and reads to the peer's end") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Target target{"127.0.0.1", fixture.peer.tcpEcho};
    NetProbe probe{};
    NetProbeScript script{.open = openTcp, .context = &target, .payload = PrByteView{
        reinterpret_cast<const uint8_t*>("hello"), 5}, .finish = true};
    netProbeRun(&probe, fixture.net, &script);
    CHECK(probe.status == PrStatus_Ok);
    CHECK(netProbeReceived(&probe, "hello<eof:clean>"));
    REQUIRE(probe.events.count != 0);
    CHECK(probe.events.data[0] == PrNetStreamEvent_Open);
    CHECK(netProbeEventCount(&probe, PrNetStreamEvent_End) == 1);
    CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Fail) == 0);
    CHECK(probe.error == PrNetError_None);
    netProbeReset(&probe);
    reset(&fixture);
}

TEST_CASE("net: a name that resolves to several addresses connects to the one that accepts") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    // The peer listens on 127.0.0.1 alone, so a resolver that gives ::1 first is refused there.
    Target target{"localhost", fixture.peer.tcpEcho};
    NetProbe probe{};
    NetProbeScript script{.open = openTcp, .context = &target, .payload = PrByteView{
        reinterpret_cast<const uint8_t*>("x"), 1}, .finish = true};
    netProbeRun(&probe, fixture.net, &script);
    CHECK(netProbeReceived(&probe, "x<eof:clean>"));
    CHECK(probe.error == PrNetError_None);
    netProbeReset(&probe);
    reset(&fixture);
}

TEST_CASE("net: bytes beyond the buffers flow both ways") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    std::string payload;
    for (uint32_t i = 0; payload.size() < 3 * 1024 * 1024; ++i)
        payload += "line " + std::to_string(i) + "\n";
    Target target{"127.0.0.1", fixture.peer.tcpEcho};
    NetProbe probe{};
    NetProbeScript script{.open = openTcp, .context = &target, .payload = PrByteView{
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size()}, .finish = true};
    netProbeRun(&probe, fixture.net, &script);
    CHECK(netProbeReceived(&probe, payload + "<eof:clean>"));
    CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Writable) >= 1);
    netProbeReset(&probe);
    reset(&fixture);
}

TEST_CASE("net: a refused connection fails") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Target target{"127.0.0.1", fixture.peer.closed};
    NetProbe probe{};
    NetProbeScript script{.open = openTcp, .context = &target};
    netProbeRun(&probe, fixture.net, &script);
    CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Open) == 0);
    CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Fail) == 1);
    CHECK(probe.error == PrNetError_Refused);
    netProbeReset(&probe);
    reset(&fixture);
}

TEST_CASE("net: a name that does not resolve fails") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    // The .invalid domain never resolves (rfc 6761).
    Target target{"pr-net-test.invalid", 80};
    NetProbe probe{};
    NetProbeScript script{.open = openTcp, .context = &target};
    netProbeRun(&probe, fixture.net, &script);
    CHECK(netProbeEventCount(&probe, PrNetStreamEvent_Fail) == 1);
    CHECK(probe.error == PrNetError_NameNotResolved);
    netProbeReset(&probe);
    reset(&fixture);
}

TEST_CASE("net: a host that is neither a name nor an address is rejected") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Target target{"two words", 80};
    NetProbe probe{};
    NetProbeScript script{.open = openTcp, .context = &target};
    netProbeRun(&probe, fixture.net, &script);
    CHECK(probe.status == PrStatus_InvalidData);
    netProbeReset(&probe);
    reset(&fixture);
}
