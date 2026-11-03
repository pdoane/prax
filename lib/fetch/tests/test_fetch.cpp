#include "pr/compress_builtin.h"
#include "pr/fetch.h"
#include "pr/fetch_load.h"
#include "pr/file.h"
#include "pr/http.h"
#include "pr/net_null.h"
#include "pr/net_sockets.h"
#include "pr/storage_disk.h"
#include "pr/storage_memory.h"
#include "pr/task_builtin.h"
#include "pr/thread.h"

#include "tools/harness/net_peer.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace harness;

extern "C" int prTestFetchCApi(void);

namespace pr_impl {

namespace {

// The network, the peer and the fetcher of one test, over HTTP without TLS.
struct Fixture {
    PrNet* net;
    PrHttp* http;
    PrCompressCodec* compress;
    PrStorage* PR_NULLABLE cache;
    PrFetchNetwork* fetcher;
    NetPeer peer;
};

// Opens the fixture, whose fetcher caches in `cache` if it is given, which the fixture then owns.
bool open(Fixture* fixture, PrStorage* PR_NULLABLE cache = nullptr) {
    if (!netPeerOpen(&fixture->peer)) {
        FAIL("the tests of lib/fetch need tools/harness/net_peer.py's servers: ./dev.sh test fetch");
        return false;
    }
    fixture->net = prNetSocketsCreate();
    PrHttpDesc http{.net = fixture->net};
    fixture->http = prHttpCreate(PrHttpBackend_Default, &http);
    fixture->compress = prCompressBuiltinCreate();
    fixture->cache = cache;
    PrFetchNetworkDesc desc{
        .net = fixture->net,
        .http = fixture->http,
        .compress = fixture->compress,
        .cache = cache,
        .userAgent = "Prax tests",
    };
    fixture->fetcher = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &desc);
    return true;
}

void closeOnLoop(void* PR_NULLABLE context) {
    Fixture* fixture = static_cast<Fixture*>(context);
    prFetchNetworkDestroy(fixture->fetcher);
    prHttpDestroy(fixture->http);
}

void reset(Fixture* fixture) {
    prNetPostBlocking(fixture->net, closeOnLoop, fixture);
    prNetDestroy(fixture->net);
    prCompressCodecDestroy(fixture->compress);
    prStorageDestroy(fixture->cache);
    netPeerReset(&fixture->peer);
}

// One fetch run from a test's thread, and what it received, which the loop writes before it opens `done`. A test sets
// the request's mode, credentials, origin and referrer before it runs the probe.
struct Probe {
    PrFetchNetwork* fetcher;
    std::string url;
    std::string method;
    std::vector<PrFetchHeader> headers;
    PrFetchRedirect redirect;
    PrFetchMode mode;
    PrFetchCredentials credentials;
    std::string origin;
    std::string referrer;
    PrFetchReferrerPolicy referrerPolicy;
    PrFetch* PR_NULLABLE fetch;
    PrStatus created;
    NetLatch done;
    std::vector<PrFetchEvent> events;
    uint16_t status;
    PrFetchResponseType responseType;
    PrFetchTiming timing;
    std::string responseUrl;
    std::string type;
    std::string received;
    PrNetError error;
};

void probeEvent(void* PR_NULLABLE context, PrFetch* fetch, PrFetchEvent event) {
    Probe* probe = static_cast<Probe*>(context);
    probe->events.push_back(event);
    if (event == PrFetchEvent_Response) {
        probe->status = prFetchStatus(fetch);
        probe->responseType = prFetchResponseType(fetch);
        PrStringView url = prFetchUrl(fetch);
        probe->responseUrl.assign(url.data, url.size);
        PrStringView type = prFetchHeaderValue(fetch, "content-type");
        probe->type.assign(type.data != nullptr ? type.data : "", type.size);
    }
    uint8_t chunk[4096];
    for (size_t got = prFetchRead(fetch, chunk, sizeof(chunk)); got != 0; got =
            prFetchRead(fetch, chunk, sizeof(chunk)))
        probe->received.append(reinterpret_cast<const char*>(chunk), got);
    if (event == PrFetchEvent_End || event == PrFetchEvent_Fail) {
        probe->error = prFetchError(fetch);
        probe->timing = prFetchTiming(fetch);
        prFetchDestroy(fetch);
        probe->fetch = nullptr;
        netLatchOpen(&probe->done);
    }
}

void startProbe(void* PR_NULLABLE context) {
    Probe* probe = static_cast<Probe*>(context);
    PrFetchRequestDesc desc{
        .url = probe->url,
        .method = probe->method.empty() ? PrStringView{} : PrStringView(probe->method),
        .headers = probe->headers.data(),
        .headerCount = uint32_t(probe->headers.size()),
        .mode = probe->mode,
        .credentials = probe->credentials,
        .redirect = probe->redirect,
        .origin = probe->origin.empty() ? PrStringView{} : PrStringView(probe->origin),
        .referrer = probe->referrer.empty() ? PrStringView{} : PrStringView(probe->referrer),
        .referrerPolicy = probe->referrerPolicy,
    };
    probe->created = prFetchCreate(probe->fetcher, &desc, probeEvent, probe, &probe->fetch);
    if (probe->created != PrStatus_Ok)
        netLatchOpen(&probe->done);
}

// Fetches `url` with `method` and `redirect`, and waits until the fetch ends or fails.
void run(Fixture* fixture, Probe* probe, const std::string& url, PrFetchRedirect redirect = PrFetchRedirect_Follow,
        const std::string& method = "") {
    probe->fetcher = fixture->fetcher;
    probe->url = url;
    probe->method = method;
    probe->redirect = redirect;
    netLatchInit(&probe->done);
    prNetPost(fixture->net, startProbe, probe);
    netLatchWait(&probe->done);
    netLatchReset(&probe->done);
}

std::string urlOf(const Fixture& fixture, const std::string& path) {
    return "http://localhost:" + std::to_string(fixture.peer.http) + path;
}

// The peer's origin as the tests' requests name it, and a URL on the same server at another origin, by its address.
std::string originOf(const Fixture& fixture) {
    return "http://localhost:" + std::to_string(fixture.peer.http);
}

std::string otherUrlOf(const Fixture& fixture, const std::string& path) {
    return "http://127.0.0.1:" + std::to_string(fixture.peer.http) + path;
}

// Fetches `url` with a fetcher of `net`, a network that the test runs, and runs the network until the fetch ends.
void runOn(PrNet* net, PrFetchNetwork* fetcher, Probe* probe, const std::string& url) {
    probe->fetcher = fetcher;
    probe->url = url;
    startProbe(probe);
    prNetNullRun(net);
}

// The owner's queue of a test's loads: the tasks that the loop posts, which the test's thread runs.
struct OwnerQueue {
    PrMutex mutex;
    PrCondition posted;
    std::vector<std::pair<PrFetchTaskFn, void*>> tasks;
};

void postToOwner(void* PR_NULLABLE queue, PrFetchTaskFn fn, void* PR_NULLABLE context) {
    OwnerQueue* owner = static_cast<OwnerQueue*>(queue);
    prMutexLock(&owner->mutex);
    owner->tasks.emplace_back(fn, context);
    prConditionSignal(&owner->posted);
    prMutexUnlock(&owner->mutex);
}

// Runs the tasks that the owner's queue holds, after waiting for one if `wait` is true.
void runOwner(OwnerQueue* owner, bool wait) {
    prMutexLock(&owner->mutex);
    while (wait && owner->tasks.empty())
        prConditionWait(&owner->posted, &owner->mutex);
    std::vector<std::pair<PrFetchTaskFn, void*>> tasks;
    tasks.swap(owner->tasks);
    prMutexUnlock(&owner->mutex);
    for (const std::pair<PrFetchTaskFn, void*>& task : tasks)
        task.first(task.second);
}

// What a load delivered, and the load, which `done` destroys.
struct Delivery {
    PrFetchLoad* PR_NULLABLE load;
    uint32_t calls;
    PrStatus status;
    uint16_t responseStatus;
    std::string url;
    std::string type;
    std::string body;
};

void delivered(void* PR_NULLABLE context, const PrFetchResponse* response) {
    Delivery* delivery = static_cast<Delivery*>(context);
    ++delivery->calls;
    delivery->status = response->status;
    delivery->responseStatus = response->responseStatus;
    delivery->url.assign(response->url.data != nullptr ? response->url.data : "", response->url.size);
    for (uint32_t i = 0; i < response->headerCount; ++i) {
        if (response->headers[i].name == "Content-Type" || response->headers[i].name == "content-type")
            delivery->type.assign(response->headers[i].value.data, response->headers[i].value.size);
    }
    delivery->body.assign(reinterpret_cast<const char*>(response->body.data), response->body.size);
    prFetchLoadDestroy(delivery->load);
    delivery->load = nullptr;
}

// Loads `url` for the test's thread and waits for the response, with a body of at most `maxBodySize` bytes.
Delivery load(Fixture* fixture, OwnerQueue* owner, const std::string& url, uint64_t maxBodySize = 0) {
    Delivery delivery{};
    PrFetchRequestDesc request{.url = url};
    PrFetchLoadDesc desc{
        .post = postToOwner,
        .queue = owner,
        .done = delivered,
        .context = &delivery,
        .maxBodySize = maxBodySize,
    };
    delivery.load = prFetchLoadCreate(fixture->fetcher, &request, &desc);
    runOwner(owner, true);
    return delivery;
}

void nothing(void* PR_NULLABLE) {}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("fetch: the public headers compile as C") {
    CHECK(prTestFetchCApi() == 0);
}

TEST_CASE("fetch: a redirect is followed, and the response's URL is the one that answered") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe probe{};
    run(&fixture, &probe, urlOf(fixture, "/redirect#part"));
    CHECK(probe.status == 200);
    CHECK(probe.responseUrl == urlOf(fixture, "/hello"));
    CHECK(probe.received == "hello");
    CHECK(probe.events.back() == PrFetchEvent_End);
    reset(&fixture);
}

TEST_CASE("fetch: a 303 is followed with a GET") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe probe{};
    run(&fixture, &probe, urlOf(fixture, "/see-other"), PrFetchRedirect_Follow, "POST");
    CHECK(probe.status == 200);
    CHECK(probe.received == "hello");
    reset(&fixture);
}

TEST_CASE("fetch: the redirect modes error and manual stop at the redirect, and a loop of redirects fails") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe error{.origin = originOf(fixture)};
    run(&fixture, &error, urlOf(fixture, "/redirect"), PrFetchRedirect_Error);
    CHECK(error.events.back() == PrFetchEvent_Fail);
    CHECK(error.error == PrNetError_Protocol);
    Probe manual{.origin = originOf(fixture)};
    run(&fixture, &manual, urlOf(fixture, "/redirect"), PrFetchRedirect_Manual);
    CHECK(manual.status == 302);
    CHECK(manual.events.back() == PrFetchEvent_End);
    Probe loop{};
    run(&fixture, &loop, urlOf(fixture, "/loop"));
    CHECK(loop.events.back() == PrFetchEvent_Fail);
    CHECK(loop.status == 0);
    reset(&fixture);
}

TEST_CASE("fetch: a body in a content coding arrives decoded") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe probe{};
    run(&fixture, &probe, urlOf(fixture, "/gzip"));
    std::string expected;
    for (int i = 0; i < 1000; ++i)
        expected += "compressed ";
    CHECK(probe.received == expected);
    reset(&fixture);
}

TEST_CASE("fetch: a status that is not ok is a response, not a network error") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe probe{};
    run(&fixture, &probe, urlOf(fixture, "/missing"));
    CHECK(probe.status == 404);
    CHECK(probe.received == "missing");
    CHECK(probe.events.back() == PrFetchEvent_End);
    reset(&fixture);
}

TEST_CASE("fetch: a file: URL is read from the disk, with the type of its extension, and http: needs an HTTP source") {
    std::string directory = std::string(PR_FETCH_TEST_DIR) + "/files";
    REQUIRE(prFileCreateDirectory(directory));
    std::string page = directory + "/page one.html";
    std::string empty = directory + "/empty.css";
    REQUIRE(prFileWrite(page, PrByteView{reinterpret_cast<const uint8_t*>("<p>file"), 7}));
    REQUIRE(prFileWrite(empty, PrByteView{nullptr, 0}));
    PrNet* net = prNetNullCreate();
    PrFetchNetworkDesc desc{.net = net};
    PrFetchNetwork* fetcher = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &desc);
    Probe file{};
    runOn(net, fetcher, &file, "file://" + directory + "/page%20one.html#part");
    CHECK(file.status == 200);
    CHECK(file.type == "text/html");
    CHECK(file.received == "<p>file");
    Probe nothing{};
    runOn(net, fetcher, &nothing, "file://" + empty);
    CHECK(nothing.status == 200);
    CHECK(nothing.type == "text/css");
    CHECK(nothing.received.empty());
    CHECK(nothing.events.back() == PrFetchEvent_End);
    Probe missing{};
    runOn(net, fetcher, &missing, "file://" + directory + "/missing.html");
    CHECK(missing.events.back() == PrFetchEvent_Fail);
    CHECK(missing.error == PrNetError_Io);
    Probe http{};
    runOn(net, fetcher, &http, "http://example.com/");
    CHECK(http.events.back() == PrFetchEvent_Fail);
    prFetchNetworkDestroy(fetcher);
    prNetDestroy(net);
}

TEST_CASE("fetch: data: and about:blank are answered by the fetcher, and other schemes are network errors") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe data{};
    run(&fixture, &data, "data:text/plain;base64,aGVsbG8=");
    CHECK(data.status == 200);
    CHECK(data.type == "text/plain");
    CHECK(data.received == "hello");
    Probe blank{};
    run(&fixture, &blank, "about:blank");
    CHECK(blank.status == 200);
    CHECK(blank.received.empty());
    Probe other{};
    run(&fixture, &other, "ftp://localhost/file");
    CHECK(other.events.back() == PrFetchEvent_Fail);
    Probe invalid{};
    run(&fixture, &invalid, "not a url");
    CHECK(invalid.created == PrStatus_InvalidData);
    reset(&fixture);
}

TEST_CASE("fetch: a fresh response is answered from the cache, and one that it must not keep is fetched again") {
    Fixture fixture{};
    if (!open(&fixture, prStorageMemoryCreate(1 << 20, PrStorageEviction_LeastRecentlyUsed)))
        return;
    Probe first{};
    run(&fixture, &first, urlOf(fixture, "/cache/fresh"));
    Probe second{};
    run(&fixture, &second, urlOf(fixture, "/cache/fresh"));
    CHECK(first.status == 200);
    CHECK(second.received == first.received);
    CHECK(second.events.back() == PrFetchEvent_End);
    Probe stored{};
    run(&fixture, &stored, urlOf(fixture, "/cache/no-store"));
    Probe again{};
    run(&fixture, &again, urlOf(fixture, "/cache/no-store"));
    CHECK(again.received != stored.received);
    Probe expired{};
    run(&fixture, &expired, urlOf(fixture, "/cache/expired"));
    Probe refetched{};
    run(&fixture, &refetched, urlOf(fixture, "/cache/expired"));
    CHECK(refetched.received != expired.received);
    // A response without explicit freshness stays fresh for a part of its age since Last-Modified.
    Probe heuristic{};
    run(&fixture, &heuristic, urlOf(fixture, "/cache/heuristic"));
    Probe reused{};
    run(&fixture, &reused, urlOf(fixture, "/cache/heuristic"));
    CHECK(reused.received == heuristic.received);
    reset(&fixture);
}

TEST_CASE("fetch: a response that must be revalidated is answered with its body when the server answers 304") {
    Fixture fixture{};
    if (!open(&fixture, prStorageMemoryCreate(1 << 20, PrStorageEviction_LeastRecentlyUsed)))
        return;
    Probe first{};
    run(&fixture, &first, urlOf(fixture, "/cache/validated"));
    Probe second{};
    run(&fixture, &second, urlOf(fixture, "/cache/validated"));
    CHECK(second.status == 200);
    CHECK(second.received == first.received);
    reset(&fixture);
}

TEST_CASE("fetch: a cached response answers only the requests that match what its Vary names") {
    Fixture fixture{};
    if (!open(&fixture, prStorageMemoryCreate(1 << 20, PrStorageEviction_LeastRecentlyUsed)))
        return;
    Probe a{};
    a.headers.push_back(PrFetchHeader{"X-Variant", "a"});
    run(&fixture, &a, urlOf(fixture, "/cache/variant"));
    Probe sameA{};
    sameA.headers.push_back(PrFetchHeader{"X-Variant", "a"});
    run(&fixture, &sameA, urlOf(fixture, "/cache/variant"));
    Probe b{};
    b.headers.push_back(PrFetchHeader{"X-Variant", "b"});
    run(&fixture, &b, urlOf(fixture, "/cache/variant"));
    CHECK(sameA.received == a.received);
    CHECK(b.received != a.received);
    CHECK(b.received.find(" b ") != std::string::npos);
    reset(&fixture);
}

TEST_CASE("fetch: a request that may change a resource invalidates what the cache has of it") {
    Fixture fixture{};
    if (!open(&fixture, prStorageMemoryCreate(1 << 20, PrStorageEviction_LeastRecentlyUsed)))
        return;
    Probe first{};
    run(&fixture, &first, urlOf(fixture, "/cache/fresh"));
    Probe post{};
    run(&fixture, &post, urlOf(fixture, "/cache/fresh"), PrFetchRedirect_Follow, "POST");
    Probe after{};
    run(&fixture, &after, urlOf(fixture, "/cache/fresh"));
    CHECK(post.status == 200);
    CHECK(after.received != first.received);
    reset(&fixture);
}

TEST_CASE("fetch: a cache on disk answers a fetcher made after the one that stored the response") {
    std::string directory = std::string(PR_FETCH_TEST_DIR) + "/cache";
    // The pool that writes the store's files, which outlives every store that the test opens.
    struct Pool {
        PrTaskPool* pool = prTaskBuiltinCreate(1);
        ~Pool() { prTaskPoolDestroy(pool); }
    } writer;
    PrStorageDiskDesc desc{.directory = directory, .limitBytes = 1 << 20, .pool = writer.pool};
    PrStorage* store = nullptr;
    REQUIRE(prStorageDiskCreate(&desc, &store) == PrStatus_Ok);
    prStorageClear(store);
    Fixture fixture{};
    if (!open(&fixture, store))
        return;
    Probe first{};
    run(&fixture, &first, urlOf(fixture, "/cache/fresh"));
    reset(&fixture);
    REQUIRE(prStorageDiskCreate(&desc, &store) == PrStatus_Ok);
    Fixture later{};
    if (!open(&later, store))
        return;
    Probe second{};
    run(&later, &second, urlOf(later, "/cache/fresh"));
    CHECK(second.received == first.received);
    reset(&later);
}

TEST_CASE("fetch: a response from another origin is opaque to no-cors, and a same-origin request there fails") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe same{.origin = originOf(fixture)};
    run(&fixture, &same, urlOf(fixture, "/hello"));
    CHECK(same.responseType == PrFetchResponseType_Basic);
    Probe opaque{.origin = originOf(fixture)};
    run(&fixture, &opaque, otherUrlOf(fixture, "/hello"));
    CHECK(opaque.responseType == PrFetchResponseType_Opaque);
    CHECK(opaque.status == 200);
    CHECK(opaque.received == "hello");
    Probe sameOrigin{.mode = PrFetchMode_SameOrigin, .origin = originOf(fixture)};
    run(&fixture, &sameOrigin, otherUrlOf(fixture, "/hello"));
    CHECK(sameOrigin.events.back() == PrFetchEvent_Fail);
    CHECK(sameOrigin.error == PrNetError_Blocked);
    Probe manual{.origin = originOf(fixture)};
    run(&fixture, &manual, otherUrlOf(fixture, "/redirect"), PrFetchRedirect_Manual);
    CHECK(manual.error == PrNetError_Blocked);
    Probe redirect{.origin = originOf(fixture)};
    run(&fixture, &redirect, urlOf(fixture, "/redirect"), PrFetchRedirect_Manual);
    CHECK(redirect.responseType == PrFetchResponseType_OpaqueRedirect);
    reset(&fixture);
}

TEST_CASE("fetch: a CORS response from another origin is received only if it allows the request's origin") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    std::string origin = originOf(fixture);
    Probe allowed{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &allowed, otherUrlOf(fixture, "/cors?origin=" + origin));
    CHECK(allowed.responseType == PrFetchResponseType_Cors);
    CHECK(allowed.received == "GET none");
    Probe any{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &any, otherUrlOf(fixture, "/cors?origin=*"));
    CHECK(any.responseType == PrFetchResponseType_Cors);
    Probe missing{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &missing, otherUrlOf(fixture, "/cors"));
    CHECK(missing.error == PrNetError_Blocked);
    Probe other{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &other, otherUrlOf(fixture, "/cors?origin=http://example.com"));
    CHECK(other.error == PrNetError_Blocked);
    Probe twice{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &twice, otherUrlOf(fixture, "/cors?origin=*&origin=*"));
    CHECK(twice.error == PrNetError_Blocked);
    reset(&fixture);
}

TEST_CASE("fetch: a CORS response allows credentials only by naming the origin and allowing them") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    std::string origin = originOf(fixture);
    Probe any{.mode = PrFetchMode_Cors, .credentials = PrFetchCredentials_Include, .origin = origin};
    run(&fixture, &any, otherUrlOf(fixture, "/cors?origin=*&credentials=true"));
    CHECK(any.error == PrNetError_Blocked);
    Probe named{.mode = PrFetchMode_Cors, .credentials = PrFetchCredentials_Include, .origin = origin};
    run(&fixture, &named, otherUrlOf(fixture, "/cors?origin=" + origin));
    CHECK(named.error == PrNetError_Blocked);
    Probe allowed{.mode = PrFetchMode_Cors, .credentials = PrFetchCredentials_Include, .origin = origin};
    run(&fixture, &allowed, otherUrlOf(fixture, "/cors?credentials=true&origin=" + origin));
    CHECK(allowed.responseType == PrFetchResponseType_Cors);
    reset(&fixture);
}

TEST_CASE("fetch: a CORS request that is not simple is sent only if its preflight allows its method and headers") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    std::string origin = originOf(fixture);
    Probe simple{.headers = {{"Accept", "text/plain"}}, .mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &simple, otherUrlOf(fixture, "/cors?origin=" + origin), PrFetchRedirect_Follow, "POST");
    CHECK(simple.received == "POST none");
    std::vector<PrFetchHeader> headers = {{"X-Probe", "1"}, {"Accept", "text/plain"}};
    Probe allowed{.headers = headers, .mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &allowed, otherUrlOf(fixture, "/cors?methods=PUT&headers=x-probe&origin=" + origin),
        PrFetchRedirect_Follow, "PUT");
    CHECK(allowed.status == 200);
    CHECK(allowed.received == "PUT PUT;x-probe");
    Probe wildcard{.headers = headers, .mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &wildcard, otherUrlOf(fixture, "/cors?methods=*&headers=*&origin=" + origin), PrFetchRedirect_Follow, "PUT");
    CHECK(wildcard.status == 200);
    Probe method{.headers = headers, .mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &method, otherUrlOf(fixture, "/cors?headers=x-probe&origin=" + origin), PrFetchRedirect_Follow, "PUT");
    CHECK(method.error == PrNetError_Blocked);
    Probe header{.headers = headers, .mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &header, otherUrlOf(fixture, "/cors?methods=PUT&origin=" + origin), PrFetchRedirect_Follow, "PUT");
    CHECK(header.error == PrNetError_Blocked);
    reset(&fixture);
}

TEST_CASE("fetch: a CORS request redirected through another origin sends its origin as null") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    std::string origin = originOf(fixture);
    std::string back = urlOf(fixture, "/cors?origin=" + origin);
    std::string backToNull = urlOf(fixture, "/cors?origin=null");
    Probe named{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &named, otherUrlOf(fixture, "/cors?origin=" + origin + "&to=" + back));
    CHECK(named.error == PrNetError_Blocked);
    Probe tainted{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &tainted, otherUrlOf(fixture, "/cors?origin=" + origin + "&to=" + backToNull));
    CHECK(tainted.responseType == PrFetchResponseType_Cors);
    CHECK(tainted.received == "GET none");
    reset(&fixture);
}

TEST_CASE("fetch: a request sends as much of its referrer as its policy allows, and Origin as its mode says") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    std::string origin = originOf(fixture);
    std::string page = urlOf(fixture, "/page?query#fragment");
    std::string pageSent = urlOf(fixture, "/page?query");
    Probe same{.origin = origin, .referrer = page};
    run(&fixture, &same, urlOf(fixture, "/headers"));
    CHECK(same.received == "Origin: -\nReferer: " + pageSent + "\n");
    Probe cross{.origin = origin, .referrer = page};
    run(&fixture, &cross, otherUrlOf(fixture, "/headers"));
    CHECK(cross.received == "Origin: -\nReferer: " + origin + "/\n");
    Probe none{.origin = origin, .referrer = page, .referrerPolicy = PrFetchReferrerPolicy_NoReferrer};
    run(&fixture, &none, urlOf(fixture, "/headers"));
    CHECK(none.received == "Origin: -\nReferer: -\n");
    Probe unsafe{.origin = origin, .referrer = page, .referrerPolicy = PrFetchReferrerPolicy_UnsafeUrl};
    run(&fixture, &unsafe, otherUrlOf(fixture, "/headers"));
    CHECK(unsafe.received == "Origin: -\nReferer: " + pageSent + "\n");
    Probe post{.origin = origin};
    run(&fixture, &post, urlOf(fixture, "/headers"), PrFetchRedirect_Follow, "POST");
    CHECK(post.received == "Origin: " + origin + "\nReferer: -\n");
    Probe sameCors{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &sameCors, urlOf(fixture, "/headers"));
    CHECK(sameCors.received == "Origin: -\nReferer: -\n");
    Probe cors{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &cors, otherUrlOf(fixture, "/headers"));
    CHECK(cors.received == "Origin: " + origin + "\nReferer: -\n");
    // A loopback host is potentially trustworthy, so the request is no downgrade, and sends its referrer's origin. The
    // Origin header compares schemes alone, and withholds an https origin from an http URL.
    Probe secure{.origin = "https://example.com", .referrer = "https://example.com/page"};
    run(&fixture, &secure, urlOf(fixture, "/headers"), PrFetchRedirect_Follow, "POST");
    CHECK(secure.received == "Origin: null\nReferer: https://example.com/\n");
    reset(&fixture);
}

TEST_CASE("fetch: a fetcher measures a fetch in order, and a body's size") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    Probe probe{.origin = originOf(fixture)};
    run(&fixture, &probe, urlOf(fixture, "/cors?origin=null"));
    PrFetchTiming timing = probe.timing;
    CHECK(timing.startTime > 0);
    CHECK(timing.postRedirectStart >= timing.startTime);
    CHECK(timing.requestStart >= timing.postRedirectStart);
    CHECK(timing.responseStart >= timing.requestStart);
    CHECK(timing.endTime >= timing.responseStart);
    CHECK(timing.redirectCount == 0);
    CHECK(timing.redirectStart == 0);
    CHECK(timing.cacheState == PrFetchCacheState_None);
    CHECK(timing.timingAllowPassed);
    CHECK_FALSE(timing.crossOriginRedirect);
    CHECK(timing.nextHopProtocol == "http/1.1");
    CHECK(timing.decodedBodySize == probe.received.size());
    reset(&fixture);
}

TEST_CASE("fetch: a redirect to another origin is measured, and fails the TAO check") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    std::string origin = originOf(fixture);
    Probe probe{.mode = PrFetchMode_Cors, .origin = origin};
    run(&fixture, &probe, urlOf(fixture, "/cors?origin=" + origin + "&to=" + otherUrlOf(fixture, "/cors?origin=null")));
    CHECK(probe.timing.redirectCount == 1);
    CHECK(probe.timing.redirectStart == probe.timing.startTime);
    CHECK(probe.timing.redirectEnd >= probe.timing.redirectStart);
    CHECK(probe.timing.postRedirectStart == probe.timing.redirectEnd);
    CHECK(probe.timing.crossOriginRedirect);
    CHECK_FALSE(probe.timing.timingAllowPassed);
    reset(&fixture);
}

TEST_CASE("fetch: methods and headers are classified as the Fetch standard names them") {
    CHECK(prFetchIsMethod("PATCH"));
    CHECK_FALSE(prFetchIsMethod(""));
    CHECK_FALSE(prFetchIsMethod("GET POST"));
    CHECK(prFetchIsForbiddenMethod("connect"));
    CHECK(prFetchIsForbiddenMethod("TRACK"));
    CHECK_FALSE(prFetchIsForbiddenMethod("PATCH"));
    CHECK(prFetchIsCorsSafelistedMethod("HEAD"));
    CHECK_FALSE(prFetchIsCorsSafelistedMethod("get"));
    CHECK(prFetchNormalizeMethod("pOsT") == "POST");
    CHECK(prFetchNormalizeMethod("patch") == "patch");

    CHECK(prFetchIsHeaderName("X-Custom"));
    CHECK_FALSE(prFetchIsHeaderName("X Custom"));
    CHECK_FALSE(prFetchIsHeaderName(""));
    CHECK(prFetchIsHeaderValue(""));
    CHECK(prFetchIsHeaderValue("a  b"));
    CHECK_FALSE(prFetchIsHeaderValue(" a"));
    CHECK_FALSE(prFetchIsHeaderValue("a\r\nb"));
    CHECK(prFetchNormalizeHeaderValue("\t a b \r\n") == "a b");

    CHECK(prFetchIsForbiddenRequestHeader("Cookie", "a=b"));
    CHECK(prFetchIsForbiddenRequestHeader("sec-fetch-mode", "cors"));
    CHECK(prFetchIsForbiddenRequestHeader("Proxy-Authorization", "x"));
    // A quoted string is one item, read without its quotes.
    CHECK(prFetchIsForbiddenRequestHeader("X-HTTP-Method-Override", "GET, \"x,y\", trace"));
    CHECK(prFetchIsForbiddenRequestHeader("X-Method-Override", "GET, \"tra\\ce\""));
    CHECK_FALSE(prFetchIsForbiddenRequestHeader("X-HTTP-Method-Override", "GET, \"trace,x\""));
    CHECK_FALSE(prFetchIsForbiddenRequestHeader("X-Custom", "trace"));
    CHECK(prFetchIsForbiddenResponseHeaderName("Set-Cookie2"));
    CHECK_FALSE(prFetchIsForbiddenResponseHeaderName("Cookie"));

    CHECK(prFetchIsNoCorsSafelistedRequestHeader("Content-Type", "text/plain; charset=utf-8"));
    CHECK_FALSE(prFetchIsNoCorsSafelistedRequestHeader("Content-Type", "application/json"));
    CHECK_FALSE(prFetchIsNoCorsSafelistedRequestHeader("Range", "bytes=0-1"));
    CHECK(prFetchIsCorsSafelistedRequestHeader("Range", "bytes=0-1"));

    CHECK(prFetchIsCorsExposedHeader("Content-Type", "", PrFetchCredentials_Include));
    CHECK_FALSE(prFetchIsCorsExposedHeader("X-Secret", "", PrFetchCredentials_SameOrigin));
    CHECK(prFetchIsCorsExposedHeader("X-Secret", "x-other, X-SECRET", PrFetchCredentials_SameOrigin));
    CHECK(prFetchIsCorsExposedHeader("X-Secret", " * ", PrFetchCredentials_SameOrigin));
    CHECK_FALSE(prFetchIsCorsExposedHeader("X-Secret", "*", PrFetchCredentials_Include));
    CHECK_FALSE(prFetchIsCorsExposedHeader("Set-Cookie", "set-cookie, *", PrFetchCredentials_Omit));
}

TEST_CASE("fetch: a load delivers a whole response on its owner's thread") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    OwnerQueue owner{};
    prMutexInit(&owner.mutex);
    prConditionInit(&owner.posted);
    Delivery hello = load(&fixture, &owner, urlOf(fixture, "/redirect"));
    CHECK(hello.calls == 1);
    CHECK(hello.status == PrStatus_Ok);
    CHECK(hello.responseStatus == 200);
    CHECK(hello.url == urlOf(fixture, "/hello"));
    CHECK(hello.type == "text/plain");
    CHECK(hello.body == "hello");
    Delivery missing = load(&fixture, &owner, urlOf(fixture, "/missing"));
    CHECK(missing.status == PrStatus_Ok);
    CHECK_FALSE(prFetchIsOkStatus(missing.responseStatus));
    CHECK(missing.body == "missing");
    CHECK(load(&fixture, &owner, "not a url").status == PrStatus_InvalidData);
    CHECK(load(&fixture, &owner, "ftp://localhost/file").status == PrStatus_Io);
    CHECK(load(&fixture, &owner, urlOf(fixture, "/large"), 1024).status == PrStatus_Limit);
    prConditionReset(&owner.posted);
    prMutexReset(&owner.mutex);
    reset(&fixture);
}

TEST_CASE("fetch: a load destroyed before its response arrives delivers nothing") {
    Fixture fixture{};
    if (!open(&fixture))
        return;
    OwnerQueue owner{};
    prMutexInit(&owner.mutex);
    prConditionInit(&owner.posted);
    Delivery early{};
    Delivery late{};
    PrFetchRequestDesc request{.url = urlOf(fixture, "/large")};
    PrFetchLoadDesc desc{.post = postToOwner, .queue = &owner, .done = delivered, .context = &early};
    prFetchLoadDestroy(prFetchLoadCreate(fixture.fetcher, &request, &desc));
    // A load whose response is posted to the owner before the owner destroys it.
    PrFetchRequestDesc hello{.url = urlOf(fixture, "/hello")};
    desc.context = &late;
    late.load = prFetchLoadCreate(fixture.fetcher, &hello, &desc);
    prMutexLock(&owner.mutex);
    while (owner.tasks.empty())
        prConditionWait(&owner.posted, &owner.mutex);
    prMutexUnlock(&owner.mutex);
    prFetchLoadDestroy(late.load);
    prNetPostBlocking(fixture.net, nothing, nullptr);
    runOwner(&owner, false);
    CHECK(early.calls == 0);
    CHECK(late.calls == 0);
    prConditionReset(&owner.posted);
    prMutexReset(&owner.mutex);
    reset(&fixture);
}
