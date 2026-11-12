#include "pr/array.h"
#include "pr/http.h"
#include "pr/net_sockets.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/tls.h"

#include "tools/harness/net_peer.h"

#include "tools/harness/test.h"

using namespace harness;

extern "C" int prTestHttpCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

// The network, the peer and the source of exchanges of one test. The source trusts the peer's CA.
struct Fixture {
    PrNet* net;
    PrTls* PR_NULLABLE tls;
    PrHttp* http;
    NetPeer peer;
    PrByteView anchor;
};

bool open(Fixture* fixture, const PrHttpHostRule* PR_NULLABLE rules = nullptr, uint32_t ruleCount = 0) {
    if (!netPeerOpen(&fixture->peer)) {
        FAIL("the tests of lib/http need tools/harness/net_peer.py's servers: ./dev.sh test http");
        return false;
    }
    fixture->net = prNetSocketsCreate();
    fixture->tls = prTlsCreate(PrTlsBackend_Default, fixture->net);
    fixture->anchor = PrByteView{fixture->peer.ca.data, fixture->peer.ca.size};
    PrHttpDesc desc{
        .net = fixture->net,
        .tls = fixture->tls,
        .anchors = &fixture->anchor,
        .anchorCount = 1,
        .hostRules = rules,
        .hostRuleCount = ruleCount,
    };
    fixture->http = prHttpCreate(PrHttpBackend_Default, &desc);
    return true;
}

void reset(Fixture* fixture) {
    prNetPostBlocking(fixture->net, [](void* PR_NULLABLE http) {
        prHttpDestroy(static_cast<PrHttp*>(http));
    }, fixture->http);
    prTlsDestroy(fixture->tls);
    prNetDestroy(fixture->net);
    netPeerReset(&fixture->peer);
}

// A header of a response, as it arrived.
struct Header {
    String name;
    String value;
};

// One exchange run from a test's thread, and what it received, which the loop writes before it opens `done`.
struct Probe {
    PrHttp* http;
    String url;
    String method;
    String body;
    PrHttpExchange* PR_NULLABLE exchange;
    NetLatch done;
    Array<PrHttpEvent> events;
    uint16_t status;
    Array<Header> headers;
    String received;
    PrNetError error;
};

void probeEvent(void* PR_NULLABLE context, PrHttpExchange* exchange, PrHttpEvent event) {
    Probe* probe = static_cast<Probe*>(context);
    probe->events.push(event);
    if (event == PrHttpEvent_Response) {
        probe->status = prHttpExchangeStatus(exchange);
        for (uint32_t i = 0; i < prHttpExchangeHeaderCount(exchange); ++i) {
            PrHttpHeader header = prHttpExchangeHeader(exchange, i);
            probe->headers.push(Header{.name = header.name, .value = header.value});
        }
    }
    uint8_t chunk[4096];
    for (size_t got = prHttpExchangeRead(exchange, chunk, sizeof(chunk)); got != 0;
            got = prHttpExchangeRead(exchange, chunk, sizeof(chunk)))
        probe->received.append(PrStringView{reinterpret_cast<const char*>(chunk), got});
    if (event == PrHttpEvent_End || event == PrHttpEvent_Fail) {
        probe->error = prHttpExchangeError(exchange);
        prHttpExchangeDestroy(exchange);
        probe->exchange = nullptr;
        netLatchOpen(&probe->done);
    }
}

void startProbe(void* PR_NULLABLE context) {
    Probe* probe = static_cast<Probe*>(context);
    PrHttpRequestDesc desc{
        .method = probe->method.isEmpty() ? PrStringView{} : PrStringView(probe->method),
        .url = probe->url,
        .body = prStringBytes(probe->body),
        .event = probeEvent,
        .context = probe,
    };
    if (prHttpExchangeCreate(probe->http, &desc, &probe->exchange) != PrStatus_Ok)
        netLatchOpen(&probe->done);
}

// Runs an exchange for `url` with `method` and `body`, and waits until it ends or fails.
void run(Fixture* fixture, Probe* probe, PrStringView url, PrStringView method = "", PrStringView body = "") {
    probe->http = fixture->http;
    probe->url = url;
    probe->method = method;
    probe->body = body;
    netLatchInit(&probe->done);
    prNetPost(fixture->net, startProbe, probe);
    netLatchWait(&probe->done);
    netLatchReset(&probe->done);
}

String headerOf(const Probe& probe, const char* name) {
    for (const Header& header : probe.headers) {
        if (header.name == name)
            return header.value;
    }
    return "";
}

String urlOf(uint16_t port, const char* path, const char* scheme = "http") {
    return format("{}://localhost:{}{}", scheme, port, path);
}

// Exchanges made at once on the loop, each destroyed as its response arrives, as a cache that revalidates does with a
// 304. The loop writes the results before it opens `done`.
struct Abandoned {
    PrHttp* http;
    String url;
    uint32_t count;
    uint32_t answered;
    uint32_t reused;                // the responses that a connection carried after another
    NetLatch done;
};

void abandonedEvent(void* PR_NULLABLE context, PrHttpExchange* exchange, PrHttpEvent event) {
    Abandoned* batch = static_cast<Abandoned*>(context);
    if (event == PrHttpEvent_Response) {
        for (uint32_t i = 0; i < prHttpExchangeHeaderCount(exchange); ++i) {
            PrHttpHeader header = prHttpExchangeHeader(exchange, i);
            if (header.name == "X-Requests" && header.value != "1")
                ++batch->reused;
        }
    }
    prHttpExchangeDestroy(exchange);
    if (++batch->answered == batch->count)
        netLatchOpen(&batch->done);
}

void startAbandoned(void* PR_NULLABLE context) {
    Abandoned* batch = static_cast<Abandoned*>(context);
    for (uint32_t i = 0; i < batch->count; ++i) {
        PrHttpRequestDesc desc{.url = batch->url, .event = abandonedEvent, .context = batch};
        PrHttpExchange* exchange = nullptr;
        REQUIRE(prHttpExchangeCreate(batch->http, &desc, &exchange) == PrStatus_Ok);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("http: the public headers compile as C") {
    CHECK(prTestHttpCApi() == 0);
}

TEST_CASE("http: a response framed by its Content-Length arrives with its status and headers") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe probe{};
    run(&fixture, &probe, urlOf(fixture.peer.http, "/hello"));
    CHECK(probe.status == 200);
    CHECK(headerOf(probe, "Content-Type") == "text/plain");
    CHECK(probe.received == "hello");
    REQUIRE(!probe.events.isEmpty());
    CHECK(probe.events[0] == PrHttpEvent_Response);
    CHECK(probe.events.last() == PrHttpEvent_End);
    reset(&fixture);
}

TEST_CASE("http: chunked and connection-delimited content arrive whole") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe chunked{};
    run(&fixture, &chunked, urlOf(fixture.peer.http, "/chunked"));
    CHECK(chunked.received == "chunked body");
    Probe closed{};
    run(&fixture, &closed, urlOf(fixture.peer.http, "/close"));
    CHECK(closed.received == "closed");
    CHECK(closed.events.last() == PrHttpEvent_End);
    reset(&fixture);
}

TEST_CASE("http: a connection that the server keeps open carries the next request to its origin") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe first{};
    run(&fixture, &first, urlOf(fixture.peer.http, "/hello"));
    Probe second{};
    run(&fixture, &second, urlOf(fixture.peer.http, "/hello"));
    CHECK(headerOf(first, "X-Requests") == "1");
    CHECK(headerOf(second, "X-Requests") == "2");
    // A response that ends with its connection leaves no connection to reuse.
    Probe closed{};
    run(&fixture, &closed, urlOf(fixture.peer.http, "/close"));
    Probe after{};
    run(&fixture, &after, urlOf(fixture.peer.http, "/hello"));
    CHECK(headerOf(after, "X-Requests") == "1");
    reset(&fixture);
}

TEST_CASE("http: a 204 that declares content leaves its connection to no other request") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe empty{};
    run(&fixture, &empty, urlOf(fixture.peer.http, "/no-content-sent"));
    CHECK(empty.status == 204);
    CHECK(empty.received.isEmpty());
    // The content that the server sends after the head would otherwise be taken for the head of this response.
    Probe after{};
    run(&fixture, &after, urlOf(fixture.peer.http, "/hello"));
    CHECK(after.status == 200);
    CHECK(after.received == "hello");
    CHECK(headerOf(after, "X-Requests") == "1");
    reset(&fixture);
}

TEST_CASE("http: exchanges destroyed as their 304s arrive leave their connections to the exchanges waiting for them") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    // More exchanges than an origin's connections, so most wait for a connection that an earlier one leaves.
    Abandoned batch{.http = fixture.http, .url = urlOf(fixture.peer.http, "/not-modified"), .count = 24};
    netLatchInit(&batch.done);
    prNetPost(fixture.net, startAbandoned, &batch);
    netLatchWait(&batch.done);
    netLatchReset(&batch.done);
    CHECK(batch.answered == batch.count);
    // A 304 has no content, so its connection carries the next request.
    CHECK(batch.reused != 0);
    reset(&fixture);
}

TEST_CASE("http: content beyond the buffers arrives whole") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe probe{};
    run(&fixture, &probe, urlOf(fixture.peer.http, "/large"));
    String expected;
    for (uint32_t i = 0; i < 400000; ++i)
        formatTo(expected, "line {}\n", i);
    CHECK(probe.received.count() == expected.count());
    CHECK(probe.received == expected);
    reset(&fixture);
}

TEST_CASE("http: a redirect and an error status are responses, and content codings arrive as they were sent") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe redirect{};
    run(&fixture, &redirect, urlOf(fixture.peer.http, "/redirect"));
    CHECK(redirect.status == 302);
    CHECK(headerOf(redirect, "Location") == "/hello");
    Probe missing{};
    run(&fixture, &missing, urlOf(fixture.peer.http, "/missing"));
    CHECK(missing.status == 404);
    CHECK(missing.received == "missing");
    Probe encoded{};
    run(&fixture, &encoded, urlOf(fixture.peer.http, "/gzip"));
    CHECK(headerOf(encoded, "Content-Encoding") == "gzip");
    CHECK(encoded.received.count() > 2);
    CHECK(prStringSlice(encoded.received, 0, 2) == "\x1f\x8b");
    reset(&fixture);
}

TEST_CASE("http: HEAD has no content, and POST sends its body") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe head{};
    run(&fixture, &head, urlOf(fixture.peer.http, "/hello"), "HEAD");
    CHECK(head.status == 200);
    CHECK(head.received.isEmpty());
    CHECK(head.events.last() == PrHttpEvent_End);
    Probe post{};
    run(&fixture, &post, urlOf(fixture.peer.http, "/echo"), "POST", "posted body");
    CHECK(post.status == 200);
    CHECK(post.received == "posted body");
    reset(&fixture);
}

TEST_CASE("http: https runs over the TLS source's sessions") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    if (fixture.tls == nullptr)
        return;
    Probe first{};
    run(&fixture, &first, urlOf(fixture.peer.https, "/chunked", "https"));
    CHECK(first.status == 200);
    CHECK(first.received == "chunked body");
    Probe second{};
    run(&fixture, &second, urlOf(fixture.peer.https, "/hello", "https"));
    CHECK(headerOf(second, "X-Requests") == "2");
    reset(&fixture);
}

TEST_CASE("http: a host rule connects a host elsewhere, and the request keeps its host") {
    // The peer's ports are known once it runs, so the rules are made from a first fixture's peer.
    Fixture probe{};
    if (!open(&probe))
        return;
    uint16_t http = probe.peer.http;
    uint16_t https = probe.peer.https;
    reset(&probe);
    const PrHttpHostRule rules[] = {
        {.host = "PEER.test", .port = 80, .target = "127.0.0.1", .targetPort = http},
        {.host = "*.any.test", .target = "localhost"},
        {.host = "secure.test", .target = "localhost", .targetPort = https},
    };
    Fixture fixture{};
    if (!open(&fixture, rules, 3))
        return;
    Probe mapped{};
    run(&fixture, &mapped, "http://peer.test/headers");
    CHECK(mapped.status == 200);
    CHECK(headerOf(mapped, "X-Host") == "peer.test");
    // A rule without a port keeps the request's port, and a wildcard matches each subdomain of its name.
    Probe kept{};
    run(&fixture, &kept, urlOf(fixture.peer.http, "/headers").replace(7, 9, "www.Any.test"));
    CHECK(kept.status == 200);
    CHECK(headerOf(kept, "X-Host") == format("www.any.test:{}", fixture.peer.http));
    // A rule's port applies only to that port, a wildcard does not match its own name, and a name without a rule has no
    // address.
    Probe other{};
    run(&fixture, &other, "http://peer.test:81/headers");
    CHECK(other.events.last() == PrHttpEvent_Fail);
    Probe bare{};
    run(&fixture, &bare, urlOf(fixture.peer.http, "/headers").replace(7, 9, "any.test"));
    CHECK(bare.events.last() == PrHttpEvent_Fail);
    // TLS verifies the request's host, which the peer's certificate does not name.
    if (fixture.tls != nullptr) {
        Probe secure{};
        run(&fixture, &secure, "https://secure.test/hello");
        CHECK(secure.events.last() == PrHttpEvent_Fail);
        CHECK(secure.error == PrNetError_Protocol);
    }
    reset(&fixture);
}

TEST_CASE("http: a refused connection fails, and a URL that is not http is not supported") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe refused{};
    run(&fixture, &refused, urlOf(fixture.peer.closed, "/"));
    CHECK(refused.events.count() == 1);
    CHECK(refused.events.last() == PrHttpEvent_Fail);
    CHECK(refused.error == PrNetError_Refused);
    Probe other{};
    run(&fixture, &other, "ftp://localhost/");
    CHECK(other.events.isEmpty());
    reset(&fixture);
}
