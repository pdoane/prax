// What the tests of lib/html host without a script engine (html_test_fetch.h).

#include "html/tests/support/html_test_fetch.h"

#include "pr/url.h"

namespace pr_impl {

namespace {

void receive(void* PR_NULLABLE context, PrFetchInterceptRequest* request) {
    TestFetcher* self = static_cast<TestFetcher*>(context);
    // The request waits until it is answered, which takes it out of `held`.
    self->held.push_back(request);
    if (self->answer)
        self->answer(request);
}

void forget(TestFetcher* self, PrFetchInterceptRequest* request) {
    for (size_t i = 0; i < self->held.size(); ++i) {
        if (self->held[i] == request) {
            self->held.erase(self->held.begin() + ptrdiff_t(i));
            return;
        }
    }
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

size_t TestFetcher::waiting() {
    prNetNullRun(net);
    return held.size();
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

bool TestFetcher::respond(const std::string& suffix, const std::string& body, uint16_t status,
                          PrStringView contentType) {
    for (PrFetchInterceptRequest* request : held) {
        if (urlOf(request).ends_with(suffix)) {
            respondTo(request, body, status, contentType);
            return true;
        }
    }
    return false;
}

void TestFetcher::respondTo(PrFetchInterceptRequest* request, const std::string& body, uint16_t status,
                            PrStringView contentType, PrStringView url) {
    PrFetchHeader header{"Content-Type", contentType};
    PrFetchInterceptResponse response{
        .status = status,
        .headers = &header,
        .headerCount = contentType.data != nullptr ? 1u : 0u,
        .body = PrByteView{reinterpret_cast<const uint8_t*>(body.data()), body.size()},
        .url = url,
    };
    forget(this, request);
    prFetchInterceptRequestRespond(request, &response);
}

void TestFetcher::failTo(PrFetchInterceptRequest* request) {
    forget(this, request);
    prFetchInterceptRequestFail(request, PrNetError_Refused);
}

std::string TestFetcher::urlOf(const PrFetchInterceptRequest* request) {
    PrStringView url = request->desc.url;
    return std::string(url.data, url.size);
}

uint32_t TestModules::parse(const PrHtmlScript* script, PrHtmlModuleRequestFn request,
                            void* PR_NULLABLE requestContext) {
    std::string source(script->source.data, script->source.size);
    PrUrl* base = nullptr;
    if (prUrlCreate(script->baseUrl, nullptr, PrCharset_Utf8, &base) == PrStatus_Ok) {
        size_t start = 0;
        while (start < source.size()) {
            size_t end = std::min(source.find('\n', start), source.size());
            std::string line = source.substr(start, end - start);
            PrUrl* imported = nullptr;
            if (line.starts_with("import ")
                && prUrlCreate(line.substr(7), base, PrCharset_Utf8, &imported) == PrStatus_Ok) {
                PrString url = prUrlSerialize(imported, PrUrlSerialization_Whole);
                request(requestContext, prStringView(&url));
                prStringReset(&url);
            }
            prUrlDestroy(imported);
            start = end + 1;
        }
    }
    prUrlDestroy(base);
    sources.push_back(source);
    return uint32_t(sources.size() - 1);
}

} // namespace pr_impl
