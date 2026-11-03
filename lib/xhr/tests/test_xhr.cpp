#include "pr/xhr.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" int prTestXhrCApi(void);

namespace pr_impl {

namespace {

// A client that a test drives: it records each fetch and each timer, and the test answers the fetches and fires the
// timers.
struct TestClient {
    struct Request {
        std::string url;
        std::string method;
        std::vector<std::string> headers;   // each as "name: value"
        std::string body;
        PrFetchCredentials credentials;
        PrFetchResponseFn done;
        void* PR_NULLABLE context;
        bool cancelled;
    };
    struct Timer {
        PrXhrTask task;
        uint32_t milliseconds;
        bool cleared;
    };

    std::vector<Request> fetches;
    std::vector<Timer> timers;
    bool refuses = false;               // true if the client cannot fetch

    PrXhrClient client() {
        return PrXhrClient{
            .context = this,
            .baseUrl = [](void*) { return prRawStringCopy("https://example.com/dir/page.html"); },
            .fetch =
                [](void* context, const PrFetchClientRequest* request, PrFetchResponseFn done, void* doneContext) -> PrStatus {
                    TestClient* client = static_cast<TestClient*>(context);
                    if (client->refuses)
                        return PrStatus_NotSupported;
                    Request fetch{
                        .url = std::string(request->url.data, request->url.size),
                        .method = "GET",
                        .body = std::string(reinterpret_cast<const char*>(request->body.data), request->body.size),
                        .credentials = request->credentials,
                        .done = done,
                        .context = doneContext,
                    };
                    if (request->method.data != nullptr)
                        fetch.method = std::string(request->method.data, request->method.size);
                    for (uint32_t i = 0; i < request->headerCount; ++i)
                        fetch.headers.push_back(std::string(request->headers[i].name.data,
                            request->headers[i].name.size) + ": " +
                            std::string(request->headers[i].value.data, request->headers[i].value.size));
                    client->fetches.push_back(fetch);
                    return PrStatus_Ok;
                },
            .cancelFetch = [](void* context, void* doneContext) {
                for (Request& fetch : static_cast<TestClient*>(context)->fetches) {
                    if (fetch.context == doneContext)
                        fetch.cancelled = true;
                }
            },
            .setTimeout = [](void* context, const PrXhrTask* task, uint32_t milliseconds) {
                TestClient* client = static_cast<TestClient*>(context);
                if (task->retain != nullptr)
                    task->retain(task->context);
                client->timers.push_back(Timer{*task, milliseconds, false});
                return int32_t(client->timers.size());
            },
            .clearTimeout = [](void* context, int32_t id) {
                Timer& timer = static_cast<TestClient*>(context)->timers[size_t(id - 1)];
                if (timer.cleared)
                    return;
                timer.cleared = true;
                if (timer.task.release != nullptr)
                    timer.task.release(timer.task.context);
            },
        };
    }

    // Fires the timers that are set, in order.
    void fireTimers() {
        for (size_t i = 0; i < timers.size(); ++i) {
            Timer timer = timers[i];
            if (timer.cleared)
                continue;
            timers[i].cleared = true;
            timer.task.invoke(timer.task.context);
            if (timer.task.release != nullptr)
                timer.task.release(timer.task.context);
        }
    }

    // Answers the last fetch with `body`, `status` and `headers`.
    void respond(const std::string& body, uint16_t status, std::vector<PrFetchHeader> headers) {
        Request fetch = fetches.back();
        PrFetchResponse response{
            .status = PrStatus_Ok,
            .responseStatus = status,
            .url = "https://example.com/final",
            .headers = headers.data(),
            .headerCount = uint32_t(headers.size()),
            .body = PrByteView{reinterpret_cast<const uint8_t*>(body.data()), body.size()},
        };
        fetch.done(fetch.context, &response);
    }

    // Fails the last fetch with `status`, a network error or Lost.
    void fail(PrStatus status) {
        Request fetch = fetches.back();
        PrFetchResponse response{.status = status};
        fetch.done(fetch.context, &response);
    }
};

// The events that reach the targets of a request, in order, each as its type and the request's state when it fired.
struct Recorded {
    PrXhr* request;
    std::vector<std::string> events;
};

void record(void* PR_NULLABLE context, PrDomEvent* event) {
    Recorded* recorded = static_cast<Recorded*>(context);
    PrStringView type = prDomEventType(event);
    recorded->events.push_back(std::string(type.data, type.size) + ":" +
        std::to_string(prXhrReadyState(recorded->request)));
}

void listen(Recorded* recorded, PrDomEventTarget* target, std::initializer_list<const char*> types) {
    PrDomEventListener listener{.invoke = record, .context = recorded, .identity = 1};
    for (const char* type : types)
        prDomEventTargetAddEventListener(target, type, &listener, nullptr);
}

// Tracks the activity that a request reports.
struct Activity {
    std::vector<bool> changes;
};

PrXhr* createRequest(TestClient* client, Activity* activity = nullptr) {
    PrXhrDesc desc{.client = client->client()};
    PrXhr* request = prXhrCreate(&desc);
    if (activity != nullptr) {
        prXhrSetActivity(request,
            [](void* context, PrXhr*, bool active) { static_cast<Activity*>(context)->changes.push_back(active); }, activity);
    }
    return request;
}

std::string take(PrRawString text) {
    PrStringView view = prRawStringView(&text);
    std::string result = view.data != nullptr ? std::string(view.data, view.size) : std::string("(null)");
    prRawStringReset(&text);
    return result;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestXhrCApi() == 0);
}

TEST_CASE("a progress event reports the values that it was created with, and nothing by default") {
    PrXhrProgressEventInit init = prXhrProgressEventInitDefault();
    init.lengthComputable = true;
    init.loaded = 3;
    init.total = 10;
    PrXhrProgressEvent* event = prXhrProgressEventCreate("progress", &init);
    CHECK(prXhrProgressEventQuery(prXhrProgressEventAsEvent(event)) == event);
    CHECK(prXhrProgressEventLengthComputable(event));
    CHECK(prXhrProgressEventLoaded(event) == 3);
    CHECK(prXhrProgressEventTotal(event) == 10);
    prDomEventRelease(prXhrProgressEventAsEvent(event));

    PrXhrProgressEvent* empty = prXhrProgressEventCreate("load", nullptr);
    CHECK(!prXhrProgressEventLengthComputable(empty));
    CHECK(prXhrProgressEventLoaded(empty) == 0);
    prDomEventRelease(prXhrProgressEventAsEvent(empty));
    CHECK(prXhrProgressEventQuery(nullptr) == nullptr);
}

TEST_CASE("open checks its method and URL, and only the first open of a request fires readystatechange") {
    TestClient client;
    PrXhr* request = createRequest(&client);
    Recorded recorded{request, {}};
    listen(&recorded, prXhrAsEventTarget(request), {"readystatechange"});
    CHECK(prXhrQuery(prXhrAsEventTarget(request)) == request);
    CHECK(prXhrUploadQuery(prXhrUploadAsEventTarget(prXhrUpload(request))) == prXhrUpload(request));
    CHECK(prXhrReadyState(request) == PrXhrState_Unsent);

    CHECK(prXhrOpen(request, "GE T", "a", true) == PrStatus_Syntax);
    CHECK(prXhrOpen(request, "TRACE", "a", true) == PrStatus_NotAllowed);
    CHECK(prXhrOpen(request, "GET", "https://exa mple.com/", true) == PrStatus_Syntax);
    CHECK(prXhrOpen(request, "GET", "a", false) == PrStatus_NotSupported);
    CHECK(recorded.events.empty());

    CHECK(prXhrOpen(request, "get", "a", true) == PrStatus_Ok);
    CHECK(prXhrOpen(request, "POST", "a", true) == PrStatus_Ok);
    CHECK(recorded.events == std::vector<std::string>{"readystatechange:1"});
    prXhrRelease(request);
}

TEST_CASE("a request sends its method, its URL resolved against the client's base URL, its headers and its body") {
    TestClient client;
    PrXhr* request = createRequest(&client);
    REQUIRE(prXhrOpen(request, "post", "data.json#fragment", true) == PrStatus_Ok);
    CHECK(prXhrSetRequestHeader(request, "X Test", "1") == PrStatus_Syntax);
    CHECK(prXhrSetRequestHeader(request, "X-Test", "a\nb") == PrStatus_Syntax);
    CHECK(prXhrSetRequestHeader(request, "X-Test", " 1 ") == PrStatus_Ok);
    CHECK(prXhrSetRequestHeader(request, "x-test", "2") == PrStatus_Ok);
    CHECK(prXhrSetRequestHeader(request, "Cookie", "a=b") == PrStatus_Ok);
    CHECK(prXhrSetWithCredentials(request, true) == PrStatus_Ok);
    const uint8_t body[] = {'h', 'i'};
    REQUIRE(prXhrSend(request, PrByteView{body, 2}, "text/plain;charset=UTF-8") == PrStatus_Ok);
    CHECK(prXhrSetRequestHeader(request, "X-Late", "1") == PrStatus_InvalidState);
    CHECK(prXhrSetWithCredentials(request, false) == PrStatus_InvalidState);
    REQUIRE(client.fetches.size() == 1);
    const TestClient::Request& fetch = client.fetches[0];
    CHECK(fetch.url == "https://example.com/dir/data.json#fragment");
    CHECK(fetch.method == "POST");
    CHECK(fetch.headers == std::vector<std::string>{"X-Test: 1, 2", "Content-Type: text/plain;charset=UTF-8"});
    CHECK(fetch.body == "hi");
    CHECK(fetch.credentials == PrFetchCredentials_Include);
    client.respond("", 204, {});
    prXhrRelease(request);
}

TEST_CASE("a response fires its events in order, and is read as its headers, its text and its body") {
    TestClient client;
    Activity activity;
    PrXhr* request = createRequest(&client, &activity);
    Recorded recorded{request, {}};
    listen(&recorded, prXhrAsEventTarget(request), {"readystatechange", "loadstart", "progress", "load", "loadend", "error"});
    REQUIRE(prXhrOpen(request, "GET", "a", true) == PrStatus_Ok);
    REQUIRE(prXhrSend(request, PrByteView{nullptr, 0}, PrStringView{nullptr, 0}) == PrStatus_Ok);
    CHECK(activity.changes == std::vector<bool>{true});
    CHECK(prXhrSend(request, PrByteView{nullptr, 0}, PrStringView{nullptr, 0}) == PrStatus_InvalidState);
    client.respond("caf\xe9", 200,
        {{"Content-Type", "text/plain; charset=windows-1252"}, {"X-B", "1"}, {"x-a", "2"}, {"X-B", "3"}});
    CHECK(recorded.events == std::vector<std::string>{
        "readystatechange:1", "loadstart:1", "readystatechange:2", "readystatechange:3",
        "progress:3", "readystatechange:4", "load:4", "loadend:4",
    });
    CHECK(activity.changes == std::vector<bool>{true, false});
    CHECK(prXhrStatus(request) == 200);
    CHECK(prXhrResponseUrl(request) == "https://example.com/final");
    CHECK(take(prXhrResponseHeader(request, "x-b")) == "1, 3");
    CHECK(take(prXhrResponseHeader(request, "X-Missing")) == "(null)");
    CHECK(take(prXhrResponseHeaders(request)) == "content-type: text/plain; charset=windows-1252\r\nx-a: 2\r\nx-b: 1, 3\r\n");
    CHECK(take(prXhrResponseText(request)) == "caf\xc3\xa9");
    CHECK(prXhrResponseBody(request).size == 4);
    CHECK(prXhrSetResponseType(request, PrXhrResponseType_Json) == PrStatus_InvalidState);
    REQUIRE(prXhrOverrideMimeType(request, "text/plain") == PrStatus_InvalidState);
    prXhrRelease(request);
}

TEST_CASE("an overridden MIME type decides the charset of the response's text") {
    TestClient client;
    PrXhr* request = createRequest(&client);
    REQUIRE(prXhrOpen(request, "GET", "a", true) == PrStatus_Ok);
    REQUIRE(prXhrOverrideMimeType(request, "text/plain;charset=windows-1252") == PrStatus_Ok);
    REQUIRE(prXhrSend(request, PrByteView{nullptr, 0}, PrStringView{nullptr, 0}) == PrStatus_Ok);
    client.respond("\xe9", 200, {{"Content-Type", "text/plain;charset=utf-8"}});
    CHECK(take(prXhrResponseText(request)) == "\xc3\xa9");
    prXhrRelease(request);
}

TEST_CASE("a network error, and a client that cannot fetch, fire error and loadend at the request and its upload") {
    for (bool refuses : {false, true}) {
        CAPTURE(refuses);
        TestClient client;
        client.refuses = refuses;
        PrXhr* request = createRequest(&client);
        Recorded recorded{request, {}};
        for (PrDomEventTarget* target : {prXhrAsEventTarget(request), prXhrUploadAsEventTarget(prXhrUpload(request))})
            listen(&recorded, target, {"readystatechange", "loadstart", "error", "load", "loadend"});
        REQUIRE(prXhrOpen(request, "POST", "a", true) == PrStatus_Ok);
        const uint8_t body[] = {'a', 'b'};
        REQUIRE(prXhrSend(request, PrByteView{body, 2}, "text/plain") == PrStatus_Ok);
        CHECK(recorded.events == std::vector<std::string>{"readystatechange:1", "loadstart:1", "loadstart:1"});
        recorded.events.clear();
        // The error of a fetch that did not start comes later than send(), from the client's timer.
        if (refuses)
            client.fireTimers();
        else
            client.fail(PrStatus_Io);
        CHECK(recorded.events == std::vector<std::string>{"readystatechange:4", "error:4", "loadend:4", "error:4", "loadend:4"});
        CHECK(prXhrStatus(request) == 0);
        prXhrRelease(request);
    }
}

TEST_CASE("a request times out once its timeout passes, and a client that goes away ends the request quietly") {
    TestClient client;
    Activity activity;
    PrXhr* request = createRequest(&client, &activity);
    Recorded recorded{request, {}};
    listen(&recorded, prXhrAsEventTarget(request), {"timeout", "loadend", "error"});
    REQUIRE(prXhrOpen(request, "GET", "a", true) == PrStatus_Ok);
    prXhrSetTimeout(request, 50);
    REQUIRE(prXhrSend(request, PrByteView{nullptr, 0}, PrStringView{nullptr, 0}) == PrStatus_Ok);
    REQUIRE(client.timers.size() == 1);
    CHECK(client.timers[0].milliseconds == 50);
    client.fireTimers();
    CHECK(client.fetches[0].cancelled);
    CHECK(recorded.events == std::vector<std::string>{"timeout:4", "loadend:4"});
    CHECK(activity.changes == std::vector<bool>{true, false});

    recorded.events.clear();
    REQUIRE(prXhrOpen(request, "GET", "a", true) == PrStatus_Ok);
    REQUIRE(prXhrSend(request, PrByteView{nullptr, 0}, PrStringView{nullptr, 0}) == PrStatus_Ok);
    client.fail(PrStatus_Lost);
    CHECK(recorded.events.empty());
    CHECK(client.timers[1].cleared);
    CHECK(activity.changes == std::vector<bool>{true, false, true, false});
    prXhrRelease(request);
}

TEST_CASE("an active request keeps itself alive until it fires loadend") {
    TestClient client;
    Activity activity;
    PrXhr* request = createRequest(&client, &activity);
    Recorded recorded{request, {}};
    listen(&recorded, prXhrAsEventTarget(request), {"load", "loadend"});
    REQUIRE(prXhrOpen(request, "GET", "a", true) == PrStatus_Ok);
    REQUIRE(prXhrSend(request, PrByteView{nullptr, 0}, PrStringView{nullptr, 0}) == PrStatus_Ok);
    prXhrRelease(request);
    client.respond("body", 200, {});
    CHECK(recorded.events == std::vector<std::string>{"load:4", "loadend:4"});
    CHECK(activity.changes == std::vector<bool>{true, false});
}

TEST_CASE("abort ends a request in flight with abort and loadend, and leaves it unsent") {
    TestClient client;
    PrXhr* request = createRequest(&client);
    Recorded recorded{request, {}};
    listen(&recorded, prXhrAsEventTarget(request), {"readystatechange", "abort", "loadend", "error"});
    REQUIRE(prXhrOpen(request, "GET", "a", true) == PrStatus_Ok);
    REQUIRE(prXhrSend(request, PrByteView{nullptr, 0}, PrStringView{nullptr, 0}) == PrStatus_Ok);
    recorded.events.clear();
    prXhrAbort(request);
    CHECK(client.fetches[0].cancelled);
    CHECK(recorded.events == std::vector<std::string>{"readystatechange:4", "abort:4", "loadend:4"});
    CHECK(prXhrReadyState(request) == PrXhrState_Unsent);
    prXhrRelease(request);
}
