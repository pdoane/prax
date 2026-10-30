// What the tests of lib/html host without a script engine: a fetcher that a test answers, which is an intercepting
// fetcher (`pr/fetch_intercept.h`) on the null network, whose loop runs on the test's thread (`pr/net_null.h`), so that
// the test decides when each response arrives; and the modules that a host parses.

#pragma once

#include "pr/dom.h"
#include "pr/fetch_intercept.h"
#include "pr/html_page.h"
#include "pr/net_null.h"
#include "pr/string_view.h"

#include <functional>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The fetcher and its network, and the requests that wait for the test to answer them.
struct TestFetcher {
    PrNet* net;
    PrFetchNetwork* fetcher;

    // Called on the loop with each request, which it answers or leaves to the test. A request that it leaves, or every
    // request if it is empty, waits in `held` until the test answers it.
    std::function<void(PrFetchInterceptRequest*)> answer;
    std::vector<PrFetchInterceptRequest*> held;

    TestFetcher();

    // Fails the requests that wait, runs what is still posted to the loop, such as the cancels of the fetches that a
    // destroyed browsing context dropped, then frees the fetcher.
    ~TestFetcher();

    TestFetcher(const TestFetcher&) = delete;
    TestFetcher& operator=(const TestFetcher&) = delete;

    // Runs the loop, which hands the host the requests made so far, and returns the number of requests that wait.
    size_t waiting();

    // Runs the loop and the agent's tasks until neither has anything to run now.
    void settle(PrDomAgent* agent);

    // Answers the waiting request whose URL ends in `suffix` with `body`, and returns false if none waits.
    bool respond(const std::string& suffix, const std::string& body, uint16_t status = 200,
        PrStringView contentType = PrStringView{nullptr, 0});

    // Answers `request` with `body`, and takes it out of `held`. A `url` other than the null view is the response's
    // URL, as redirects would leave it.
    void respondTo(PrFetchInterceptRequest* request, const std::string& body, uint16_t status = 200,
        PrStringView contentType = PrStringView{nullptr, 0}, PrStringView url = PrStringView{nullptr, 0});

    // Answers `request` with `response`, and takes it out of `held`.
    void respondWith(PrFetchInterceptRequest* request, const PrFetchInterceptResponse& response);

    // Answers `request` with a network error, and takes it out of `held`.
    void failTo(PrFetchInterceptRequest* request);

    // Returns the URL that `request` asks for.
    static std::string urlOf(const PrFetchInterceptRequest* request);
};

// The modules that a test's host parses (PrHtmlHost::parseModule), by the number that `parse` returns. Each line of a
// module's source that is `import` and a specifier imports the module that the specifier names, resolved against the
// module's base URL.
struct TestModules {
    std::vector<std::string> sources;

    // Parses `script` as the host's parseModule does.
    uint32_t parse(const PrHtmlScript* script, PrHtmlModuleRequestFn request, void* PR_NULLABLE requestContext);
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
