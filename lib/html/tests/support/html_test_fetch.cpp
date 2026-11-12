// What the tests of lib/html host without a script engine (html_test_fetch.h).

#include "html/tests/support/html_test_fetch.h"

#include "pr/string_ops.h"
#include "pr/url.h"

namespace pr_impl {

namespace {

void receive(void* PR_NULLABLE context, PrFetchInterceptRequest* request) {
    TestFetcher* fetcher = static_cast<TestFetcher*>(context);
    // The request waits until it is answered, which takes it out of `held`.
    fetcher->held.push(request);
    if (fetcher->answer)
        fetcher->answer(request);
}

void forget(TestFetcher* fetcher, PrFetchInterceptRequest* request) {
    fetcher->held.removeFirst(request);
}

} // namespace

TestFetcher::TestFetcher() : net(prNetNullCreate()) {
    PrFetchInterceptDesc desc{.net = net, .request = receive, .context = this};
    fetcher = prFetchInterceptCreate(&desc);
}

TestFetcher::~TestFetcher() {
    for (PrFetchInterceptRequest* request : held)
        prFetchInterceptRequestFail(request, PrNetError_Refused);
    held.clear();
    prNetNullRun(net);
    prFetchNetworkDestroy(fetcher);
    prNetDestroy(net);
}

uint32_t TestFetcher::waiting() {
    prNetNullRun(net);
    return held.count();
}

void TestFetcher::settle(PrDomAgent* agent) {
    for (;;) {
        bool ran = prNetNullRun(net);
        while (prDomAgentRunTask(agent))
            ran = true;
        if (!ran)
            return;
    }
}

bool TestFetcher::respond(PrStringView suffix, PrStringView body, uint16_t status, PrStringView contentType) {
    for (PrFetchInterceptRequest* request : held) {
        if (prStringEndsWith(request->desc.url, suffix)) {
            respondTo(request, body, status, contentType);
            return true;
        }
    }
    return false;
}

void TestFetcher::respondTo(PrFetchInterceptRequest* request, PrStringView body, uint16_t status,
        PrStringView contentType, PrStringView url) {
    respondTo(request, prStringBytes(body), status, contentType, url);
}

void TestFetcher::respondTo(PrFetchInterceptRequest* request, PrByteView body, uint16_t status,
        PrStringView contentType, PrStringView url) {
    PrFetchHeader header{"Content-Type", contentType};
    PrFetchInterceptResponse response{
        .status = status,
        .headers = &header,
        .headerCount = contentType.data != nullptr ? 1u : 0u,
        .body = body,
        .url = url,
    };
    forget(this, request);
    prFetchInterceptRequestRespond(request, &response);
}

void TestFetcher::respondWith(PrFetchInterceptRequest* request, const PrFetchInterceptResponse& response) {
    forget(this, request);
    prFetchInterceptRequestRespond(request, &response);
}

void TestFetcher::failTo(PrFetchInterceptRequest* request) {
    forget(this, request);
    prFetchInterceptRequestFail(request, PrNetError_Refused);
}

String TestFetcher::urlOf(const PrFetchInterceptRequest* request) {
    return String(request->desc.url);
}

uint32_t TestModules::parse(const PrHtmlScript* script, PrHtmlModuleRequestFn request, void* PR_NULLABLE requestContext) {
    PrStringView source = script->source;
    PrUrl* base = nullptr;
    if (prUrlCreate(script->baseUrl, nullptr, PrCharset_Utf8, &base) == PrStatus_Ok) {
        size_t start = 0;
        while (start < source.size) {
            PrStringView rest = prStringSlice(source, start, source.size - start);
            size_t end = start + prStringFindByte(rest, '\n');
            PrStringView line = prStringSlice(source, start, end - start);
            PrUrl* imported = nullptr;
            PrStringView keyword = "import ";
            if (prStringStartsWith(line, keyword) &&
                    prUrlCreate(prStringSlice(line, keyword.size, line.size - keyword.size), base, PrCharset_Utf8,
                        &imported) == PrStatus_Ok) {
                PrRawString url = prUrlSerialize(imported, PrUrlSerialization_Whole);
                request(requestContext, prRawStringView(&url));
                prRawStringReset(&url);
            }
            prUrlDestroy(imported);
            start = end + 1;
        }
    }
    prUrlDestroy(base);
    sources.push(String(source));
    return sources.count() - 1;
}

} // namespace pr_impl
