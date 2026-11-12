// A browsing context's fetches on a network whose loop is a thread of its own, as a host's network is.

#include "pr/fetch_intercept.h"
#include "pr/html_page.h"
#include "pr/lock.h"
#include "pr/mutex.h"
#include "pr/net_sockets.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// The host: the loop's thread answers the requests, and the test's thread runs the agent and sleeps until a task is
// posted to it. The loop answers a request for a.js at once, and holds any other request until the test asks the loop
// to fail it. `mutex` guards `posted` and `log`.
struct Threaded {
    PrNet* net = nullptr;
    PrFetchNetwork* fetcher = nullptr;
    PrDomAgent* agent = nullptr;
    Array<PrFetchInterceptRequest*> held;  // reached on the loop alone
    PrMutex mutex;
    PrCondition wake;
    bool posted = false;
    bool loopDone = false;
    String log;

    Threaded() {
        prMutexInit(&mutex);
        prConditionInit(&wake);
    }
    ~Threaded() {
        prConditionReset(&wake);
        prMutexReset(&mutex);
    }
    Threaded(const Threaded&) = delete;
    Threaded& operator=(const Threaded&) = delete;
};

void answer(void* PR_NULLABLE context, PrFetchInterceptRequest* request) {
    Threaded* t = static_cast<Threaded*>(context);
    if (!prStringEndsWith(request->desc.url, "a.js")) {
        t->held.push(request);
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
    Lock lock(&t->mutex);
    t->posted = true;
    prConditionSignal(&t->wake);
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
    Lock lock(&t->mutex);
    t->loopDone = true;
    prConditionSignal(&t->wake);
}

// Runs the agent's tasks, sleeping until one is posted, until `done` returns true or a second passes.
template <typename Done>
bool runUntil(Threaded* t, Done done) {
    double deadline = prTimeMonotonicMilliseconds() + 1000;
    while (!done()) {
        while (prDomAgentRunTask(t->agent)) {
        }
        if (done())
            return true;
        Lock lock(&t->mutex);
        while (!t->posted) {
            if (!prConditionWaitFor(&t->wake, &t->mutex, deadline - prTimeMonotonicMilliseconds()) && !t->posted)
                return false;
        }
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
        Lock lock(&threaded->mutex);
        threaded->log.append(script->source);
        threaded->log.append(' ');
        return PrStatus_Ok;
    };
    return host;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("fetches run on the network's thread and arrive in the agent's tasks, and a dropped one posts nothing") {
    Threaded t;
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
        Lock lock(&t.mutex);
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
        Lock lock(&t.mutex);
        while (!t.loopDone)
            prConditionWait(&t.wake, &t.mutex);
    }
    while (prDomAgentRunTask(t.agent)) {
    }
    CHECK(prDomAgentIsIdle(t.agent));
    prDomAgentDestroy(t.agent);
    prNetDestroy(t.net);
}
