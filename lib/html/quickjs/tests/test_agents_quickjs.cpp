// Agents on threads of their own, in one process.

#include "pr/css_quickjs.h"
#include "pr/dom.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/html_page.h"
#include "pr/mathml_quickjs.h"
#include "pr/quickjs_binding.h"
#include "pr/svg_quickjs.h"
#include "pr/url_quickjs.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

#include "doctest/doctest.h"

#include <thread>
#include <vector>

namespace pr_impl {

namespace {

constexpr uint32_t kAgents = 4;
constexpr uint32_t kRounds = 20;

// The work of one agent: its own runtime and document, which a script queries and changes, and the shared selector
// list, which is queried on the document through C.
bool runAgent(const PrCssSelectors* shared) {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsUrlBind(ctx);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    prJsSvgBind(ctx);
    prJsMathMlBind(ctx);
    jsHtmlBind(ctx);
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    bool ok = prHtmlPageLoad(
                  page,
                  "<style>p { color: red }</style><ul><li class=a>1<li>2<li class=a>3</ul>",
                  "about:blank", nullptr)
                  == PrStatus_Ok
              && jsHtmlSetGlobal(ctx, prHtmlPageWindow(page));

    const char source[] = R"JS((function() {
        const before = document.querySelectorAll('li.a').length;
        for (let i = 0; i < 20; ++i) {
            const item = document.createElement('li');
            item.className = i % 2 ? 'a' : 'b';
            document.querySelector('ul').append(item);
        }
        if (document.querySelectorAll('li.a').length !== before + 10 || !document.querySelector('ul').matches(':has(> .b)'))
            throw new Error('selectors');
        const rule = document.styleSheets[0].cssRules[0];
        rule.style.setProperty('margin', '1px 2px');
        if (rule.style.marginLeft !== '2px' || new URL('a/b', 'https://x.test/c/').href !== 'https://x.test/c/a/b')
            throw new Error('style and URLs');
    })())JS";
    for (uint32_t round = 0; ok && round < kRounds; ++round) {
        JSValue result = JS_Eval(ctx, source, sizeof(source) - 1, "agent", JS_EVAL_TYPE_GLOBAL);
        ok = !JS_IsException(result);
        JS_FreeValue(ctx, result);
    }

    PrDomNode* document =
        prDomDocumentAsNode(prHtmlWindowDocument(prHtmlPageWindow(page)));
    PrDomNodeList* found = prDomNodeListCreate();
    prCssNodeQuerySelectorAllCompiled(document, shared, found);
    ok = ok && prDomNodeListLength(found) == 2 + kRounds * 10;
    prDomNodeListDestroy(found);

    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
    prHtmlPageDestroy(page);
    return ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("agents run at once on threads of their own, sharing a selector list") {
    PrCssSelectors* shared = nullptr;
    REQUIRE(prCssSelectorsCreate("ul > li.a", &shared) == PrStatus_Ok);
    std::vector<char> ok(kAgents, 0);
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < kAgents; ++i)
        threads.emplace_back([&ok, shared, i] { ok[i] = runAgent(shared) ? 1 : 0; });
    for (std::thread& thread : threads)
        thread.join();
    for (uint32_t i = 0; i < kAgents; ++i)
        CHECK(ok[i] == 1);
    prCssSelectorsDestroy(shared);
}
