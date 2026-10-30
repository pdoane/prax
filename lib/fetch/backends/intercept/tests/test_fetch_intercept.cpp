#include "pr/fetch_intercept.h"
#include "pr/net_null.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A host that answers every request with the URL that it asked for, or fails a request for /fail, holds a request for
// /later until the test answers it, passes a request for /next on, and answers a request for /moved as if a redirect
// had moved it to /moved/here.
struct Host {
    std::vector<std::string> asked;
    PrFetchInterceptRequest* PR_NULLABLE held;
};

void answer(void* PR_NULLABLE context, PrFetchInterceptRequest* request) {
    Host* host = static_cast<Host*>(context);
    PrStringView url = request->desc.url;
    std::string location(url.data, url.size);
    host->asked.push_back(location);
    if (location.ends_with("/fail")) {
        prFetchInterceptRequestFail(request, PrNetError_Refused);
    } else if (location.ends_with("/later")) {
        host->held = request;
    } else if (location.ends_with("/next")) {
        prFetchInterceptRequestPass(request);
    } else if (location.ends_with("/moved")) {
        std::string moved = location + "/here";
        PrFetchInterceptResponse response{.status = 200, .url = PrStringView{moved.data(), moved.size()}};
        prFetchInterceptRequestRespond(request, &response);
    } else {
        PrFetchHeader header{"Content-Type", "text/plain"};
        PrFetchInterceptResponse response{
            .status = 200,
            .headers = &header,
            .headerCount = 1,
            .body = PrByteView{reinterpret_cast<const uint8_t*>(location.data()), location.size()},
        };
        prFetchInterceptRequestRespond(request, &response);
    }
}

// The next fetcher's host, which answers with a 404 and an opaque response.
void answerNext(void*, PrFetchInterceptRequest* request) {
    PrFetchInterceptResponse response{.status = 404, .type = PrFetchResponseType_Opaque};
    prFetchInterceptRequestRespond(request, &response);
}

// What one fetch received.
struct Probe {
    std::vector<PrFetchEvent> events;
    uint16_t status;
    PrFetchResponseType type;
    std::string contentType;
    std::string url;
    std::string body;
    PrNetError error;
    PrFetch* PR_NULLABLE fetch;
};

void probeEvent(void* PR_NULLABLE context, PrFetch* fetch, PrFetchEvent event) {
    Probe* probe = static_cast<Probe*>(context);
    probe->events.push_back(event);
    if (event == PrFetchEvent_Response) {
        probe->status = prFetchStatus(fetch);
        probe->type = prFetchResponseType(fetch);
        PrStringView type = prFetchHeaderValue(fetch, "content-type");
        probe->contentType.assign(type.data != nullptr ? type.data : "", type.size);
        PrStringView url = prFetchUrl(fetch);
        probe->url.assign(url.data, url.size);
    }
    uint8_t chunk[64];
    for (size_t got = prFetchRead(fetch, chunk, sizeof(chunk)); got != 0; got =
            prFetchRead(fetch, chunk, sizeof(chunk)))
        probe->body.append(reinterpret_cast<const char*>(chunk), got);
    if (event == PrFetchEvent_End || event == PrFetchEvent_Fail) {
        probe->error = prFetchError(fetch);
        prFetchDestroy(fetch);
        probe->fetch = nullptr;
    }
}

void start(PrFetchNetwork* fetcher, Probe* probe, const char* url) {
    PrFetchRequestDesc desc{.url = url, .event = probeEvent, .context = probe};
    REQUIRE(prFetchCreate(fetcher, &desc, &probe->fetch) == PrStatus_Ok);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an intercepting fetcher asks its host on the loop, and delivers the host's answer") {
    PrNet* net = prNetNullCreate();
    Host host{};
    PrFetchInterceptDesc desc{.net = net, .request = answer, .context = &host};
    PrFetchNetwork* fetcher = prFetchInterceptCreate(&desc);
    Probe ok{};
    start(fetcher, &ok, "http://example.com/page#fragment");
    Probe failed{};
    start(fetcher, &failed, "http://example.com/fail");
    CHECK(host.asked.empty());
    CHECK(prNetNullRun(net));
    CHECK(host.asked == std::vector<std::string>{"http://example.com/page", "http://example.com/fail"});
    CHECK(ok.events == std::vector<PrFetchEvent>{PrFetchEvent_Response, PrFetchEvent_Readable, PrFetchEvent_End});
    CHECK(ok.status == 200);
    CHECK(ok.contentType == "text/plain");
    CHECK(ok.body == "http://example.com/page");
    CHECK(ok.url == "http://example.com/page");
    CHECK(failed.events == std::vector<PrFetchEvent>{PrFetchEvent_Fail});
    CHECK(failed.error == PrNetError_Refused);
    prFetchNetworkDestroy(fetcher);
    prNetDestroy(net);
}

TEST_CASE("an intercepting fetcher delivers an answer that the host gives later, and frees a request whose fetch is gone") {
    PrNet* net = prNetNullCreate();
    Host host{};
    PrFetchInterceptDesc desc{.net = net, .request = answer, .context = &host};
    PrFetchNetwork* fetcher = prFetchInterceptCreate(&desc);
    Probe later{};
    start(fetcher, &later, "http://example.com/later");
    prNetNullRun(net);
    REQUIRE(host.held != nullptr);
    CHECK(later.events.empty());
    prFetchInterceptRequestFail(host.held, PrNetError_TimedOut);
    CHECK(later.error == PrNetError_TimedOut);
    Probe abandoned{};
    start(fetcher, &abandoned, "http://example.com/later");
    prNetNullRun(net);
    prFetchDestroy(abandoned.fetch);
    PrFetchInterceptResponse response{.status = 200};
    prFetchInterceptRequestRespond(host.held, &response);
    CHECK(abandoned.events.empty());
    Probe unasked{};
    start(fetcher, &unasked, "http://example.com/page");
    prFetchDestroy(unasked.fetch);
    prNetNullRun(net);
    CHECK(host.asked.size() == 2);
    prFetchNetworkDestroy(fetcher);
    prNetDestroy(net);
}

TEST_CASE("an intercepting fetcher passes the requests that its host passes on to the next fetcher") {
    PrNet* net = prNetNullCreate();
    PrFetchInterceptDesc nextDesc{.net = net, .request = answerNext};
    PrFetchNetwork* next = prFetchInterceptCreate(&nextDesc);
    Host host{};
    PrFetchInterceptDesc desc{.net = net, .request = answer, .context = &host, .next = next};
    PrFetchNetwork* fetcher = prFetchInterceptCreate(&desc);
    Probe passed{};
    start(fetcher, &passed, "http://example.com/next");
    prNetNullRun(net);
    CHECK(passed.events == std::vector<PrFetchEvent>{PrFetchEvent_Response, PrFetchEvent_End});
    CHECK(passed.status == 404);
    CHECK(passed.type == PrFetchResponseType_Opaque);
    prFetchNetworkDestroy(fetcher);
    prFetchNetworkDestroy(next);
    prNetDestroy(net);
}

TEST_CASE("a host's answer may give the URL that redirects left its response at") {
    PrNet* net = prNetNullCreate();
    Host host{};
    PrFetchInterceptDesc desc{.net = net, .request = answer, .context = &host};
    PrFetchNetwork* fetcher = prFetchInterceptCreate(&desc);
    Probe moved{};
    start(fetcher, &moved, "http://example.com/moved");
    CHECK(prNetNullRun(net));
    CHECK(moved.status == 200);
    CHECK(moved.url == "http://example.com/moved/here");
    prFetchNetworkDestroy(fetcher);
    prNetDestroy(net);
}
