// A browsing context's fetches on a network whose loop is a thread of its own, as a host's network is.

#include "pr/fetch_intercept.h"
#include "pr/html_page.h"
#include "pr/net_sockets.h"

#include "tools/harness/test.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// The host: the loop's thread answers the requests, and the test's thread runs the agent and sleeps until a task is
// posted to it. The loop answers a request for a.js at once, and holds any other request until the test asks the loop
// to fail it. `mutex` guards `posted` and `log`.
struct Threaded {
    PrNet* net;
    PrFetchNetwork* fetcher;
    PrDomAgent* agent;
    std::vector<PrFetchInterceptRequest*> held;  // reached on the loop alone
    std::mutex mutex;
    std::condition_variable wake;
    bool posted = false;
    bool loopDone = false;
    std::string log;
};

void answer(void* PR_NULLABLE context, PrFetchInterceptRequest* request) {
    Threaded* t = static_cast<Threaded*>(context);
    std::string url(request->desc.url.data, request->desc.url.size);
    if (!url.ends_with("a.js")) {
        t->held.push_back(request);
        return;
    }
    PrFetchInterceptResponse response{
        .status = 200,
        .body = PrByteView{reinterpret_cast<const uint8_t*>("external"), 8},
    };
    prFetchInterceptRequestRespond(request, &response);
}

void woken(void* PR_NULLABLE context) {
    Threaded* t = static_cast<Threaded*>(context);
    std::lock_guard<std::mutex> lock(t->mutex);
    t->posted = true;
    t->wake.notify_one();
}

// Fails the requests that the loop holds, on the loop.
void failHeld(void* PR_NULLABLE context) {
    Threaded* t = static_cast<Threaded*>(context);
    for (PrFetchInterceptRequest* request : t->held)
        prFetchInterceptRequestFail(request, PrNetError_Refused);
    t->held.clear();
}

// Frees the fetcher on the loop, after the tasks posted before, and tells the test's thread.
void closeOnLoop(void* PR_NULLABLE context) {
    Threaded* t = static_cast<Threaded*>(context);
    prFetchNetworkDestroy(t->fetcher);
    std::lock_guard<std::mutex> lock(t->mutex);
    t->loopDone = true;
    t->wake.notify_one();
}

// Runs the agent's tasks, sleeping until one is posted, until `done` returns true or a second passes.
template <typename Done>
bool runUntil(Threaded* t, Done done) {
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!done()) {
        while (prDomAgentRunTask(t->agent)) {
        }
        if (done())
            return true;
        std::unique_lock<std::mutex> lock(t->mutex);
        if (!t->wake.wait_until(lock, deadline, [&] { return t->posted; }))
            return false;
        t->posted = false;
    }
    return true;
}

PrHtmlHost hostOf(Threaded* t) {
    PrHtmlHost host = prHtmlHostDefault();
    host.context = t;
    host.agent = t->agent;
    host.fetcher = t->fetcher;
    host.execute = [](void* data, PrHtmlPage*, const PrHtmlScript* script) -> PrStatus {
        Threaded* threaded = static_cast<Threaded*>(data);
        std::lock_guard<std::mutex> lock(threaded->mutex);
        threaded->log += std::string(script->source.data, script->source.size) + " ";
        return PrStatus_Ok;
    };
    return host;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("fetches run on the network's thread and arrive in the agent's tasks, and a dropped one posts nothing") {
    Threaded t{};
    t.net = prNetSocketsCreate();
    PrFetchInterceptDesc desc{.net = t.net, .request = answer, .context = &t};
    t.fetcher = prFetchInterceptCreate(&desc);
    t.agent = prDomAgentCreate(nullptr);
    prDomAgentSetWake(t.agent, woken, &t);
    PrHtmlHost host = hostOf(&t);

    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<script src=a.js></script><p id=after>", "https://example.test/", nullptr) ==
        PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    CHECK(runUntil(&t, [&] {
        return prHtmlDocumentReadyState(document) == PrHtmlDocumentReadyState_Complete;
    }));
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        CHECK(t.log == "external ");
    }
    prHtmlPageDestroy(page);

    // A context destroyed while its fetch waits drops the fetch, whose answer then posts nothing to the agent.
    page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<script src=b.js></script>", "https://example.test/", nullptr) == PrStatus_Ok);
    prHtmlPageDestroy(page);
    prNetPost(t.net, failHeld, &t);

    prNetPost(t.net, closeOnLoop, &t);
    {
        std::unique_lock<std::mutex> lock(t.mutex);
        t.wake.wait(lock, [&] { return t.loopDone; });
    }
    while (prDomAgentRunTask(t.agent)) {
    }
    CHECK(prDomAgentIsIdle(t.agent));
    prDomAgentDestroy(t.agent);
    prNetDestroy(t.net);
}
