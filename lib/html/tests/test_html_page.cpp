// Browsing-context host callbacks and document URLs.
//
// html - https://html.spec.whatwg.org/multipage/document-sequences.html

#include "pr/compress.h"
#include "pr/html_page.h"
#include "pr/image.h"
#include "pr/string_ops.h"

#include "html/tests/support/html_test_fetch.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A host that answers fetches when the test says, as a network does, and records the scripts that it runs and the
// events that the window sees. A module script's record is the last line of the module that it runs.
struct LaterHost {
    TestFetcher fetcher;
    TestModules modules;
    String log;

    PrHtmlHost host() {
        PrHtmlHost host = prHtmlHostDefault();
        host.context = this;
        host.fetcher = fetcher.fetcher;
        host.execute = [](void* data, PrHtmlPage*, const PrHtmlScript* script) -> PrStatus {
            LaterHost* laterHost = static_cast<LaterHost*>(data);
            if (script->kind == PrHtmlScriptKind_Module) {
                PrStringView source = laterHost->modules.sources[script->module];
                size_t newline = prStringFindLastByte(source, '\n');
                size_t start = newline == source.size ? 0 : newline + 1;
                laterHost->log.append(prStringSlice(source, start, source.size - start));
            } else {
                laterHost->log.append(script->source);
            }
            laterHost->log.append(' ');
            return PrStatus_Ok;
        };
        host.parseModule = [](void* data, PrHtmlPage*, const PrHtmlScript* script,
                PrHtmlModuleRequestFn request, void* PR_NULLABLE requestContext) {
            return static_cast<LaterHost*>(data)->modules.parse(script, request, requestContext);
        };
        return host;
    }

    // Answers the waiting fetch whose URL ends in `name` with `body`, of the content type `type` if one is given.
    void answer(const char* name, const char* body, uint16_t status = 200, PrStringView type = PrStringView{nullptr, 0}) {
        if (!fetcher.respond(name, body, status, type))
            FAIL("no fetch waits for " << name);
    }

    // Answers the waiting fetch whose URL ends in `name` with the JavaScript `body`.
    void answerScript(const char* name, const char* body) {
        answer(name, body, 200, "text/javascript");
    }

    void settle(PrHtmlPage* page) {
        fetcher.settle(prHtmlPageAgent(page));
    }

    // Returns the number of fetches that wait for a URL that ends in `name`.
    size_t waitingFor(const char* name) {
        fetcher.waiting();
        size_t count = 0;
        for (PrFetchInterceptRequest* request : fetcher.held)
            count += prStringEndsWith(TestFetcher::urlOf(request), name);
        return count;
    }
};

void runTasks(PrHtmlPage* page) {
    while (prDomAgentRunTask(prHtmlPageAgent(page))) {
    }
}

PrHtmlDocumentReadyState readiness(PrHtmlPage* page) {
    return prHtmlDocumentReadyState(prHtmlWindowDocument(prHtmlPageWindow(page)));
}

bool has(PrHtmlPage* page, const char* id) {
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    return prDomNodeGetElementById(prDomDocumentAsNode(document), id) != nullptr;
}

// Returns the media text of the document's sheet at `index`.
String sheetMedia(PrHtmlPage* page, uint32_t index) {
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrCssStyleSheet* sheet = prCssStyleSheetListStyleSheetAt(prCssDocumentStyleSheets(document), index);
    REQUIRE(sheet != nullptr);
    PrRawString text = prCssMediaListMediaText(prCssStyleSheetMedia(sheet));
    String out(prRawStringView(&text));
    prRawStringReset(&text);
    return out;
}

// Loads `source` at `url` in a page whose host decodes images with `images`, and returns the URL of its icon, or
// "(none)" for the null string.
String iconUrl(PrImageCodec* PR_NULLABLE images, PrStringView source, PrStringView url) {
    PrHtmlHost host = prHtmlHostDefault();
    host.images = images;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, source, url, nullptr) == PrStatus_Ok);
    PrRawString icon = prHtmlPageIconUrl(page);
    String out = "(none)";
    if (!prRawStringIsNull(&icon))
        out = String(prRawStringView(&icon));
    prRawStringReset(&icon);
    prHtmlPageDestroy(page);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the host loads resolved URLs and executes source with its location") {
    struct State {
        uint32_t loaded = 0;
        uint32_t executed = 0;
        PrHtmlPage* page = nullptr;
    } state;
    TestFetcher fetcher;
    fetcher.answer = [&](PrFetchInterceptRequest* request) {
        const PrFetchRequestDesc* desc = &request->desc;
        CHECK(state.executed == 1);
        CHECK(desc->url == "https://example.test/assets/x%20y.js?x=1");
        CHECK(desc->destination == PrFetchDestination_Script);
        CHECK(desc->mode == PrFetchMode_NoCors);
        CHECK(desc->credentials == PrFetchCredentials_Include);
        CHECK(desc->origin == "https://example.test");
        CHECK(desc->referrer == "https://example.test/tests/page.html");
        CHECK(readiness(state.page) == PrHtmlDocumentReadyState_Loading);
        ++state.loaded;
        fetcher.respondTo(request, "external");
    };
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &state;
    host.fetcher = fetcher.fetcher;
    host.execute = [](void* data, PrHtmlPage* page, const PrHtmlScript* script) -> PrStatus {
        State* state = static_cast<State*>(data);
        if (state->executed == 0) {
            CHECK(script->source == "inline");
            CHECK(script->url == "https://example.test/tests/page.html");
            CHECK(script->line == 2);
            CHECK(script->column == 18);
        } else {
            CHECK(state->loaded == 1);
            CHECK(script->source == "external");
            CHECK(script->url == "https://example.test/assets/x%20y.js?x=1#frag");
            CHECK(script->line == 1);
            CHECK(script->column == 1);
            CHECK(prHtmlDocumentWrite(prHtmlWindowDocument(prHtmlPageWindow(page)), "<p id=written>inserted</p>") ==
                PrStatus_Ok);
        }
        ++state->executed;
        return PrStatus_Ok;
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    state.page = page;
    CHECK(prHtmlPageLoad(page, "<!doctype html><meta charset=utf-8>\r\n<p>\xf0\x9f\x98\x80</p><script>inline</script>"
        "<base href='../assets/'><script src='./x%20y.js?x=1#frag'>ignored</script>",
        "https://example.test/tests/page.html", nullptr) == PrStatus_Ok);
    fetcher.settle(prHtmlPageAgent(page));
    CHECK(state.loaded == 1);
    CHECK(state.executed == 2);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    CHECK(prDomNodeGetElementById(prDomDocumentAsNode(document), "written") != nullptr);
    PrRawString url = prDomDocumentUrl(document);
    PrRawString base = prDomNodeBaseUri(prDomDocumentAsNode(document));
    CHECK(prRawStringView(&url) == "https://example.test/tests/page.html");
    CHECK(prRawStringView(&base) == "https://example.test/assets/");
    prRawStringReset(&url);
    prRawStringReset(&base);
    prHtmlPageDestroy(page);
}

TEST_CASE("a script whose status is not ok fails, and not the load, and a failed URL keeps the document") {
    struct State { uint32_t executed = 0; uint32_t loads = 0; } state;
    TestFetcher fetcher;
    fetcher.answer = [&](PrFetchInterceptRequest* request) {
        fetcher.respondTo(request, "A body with a status that is not ok is dropped.", 404);
    };
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &state;
    host.fetcher = fetcher.fetcher;
    host.execute = [](void* data, PrHtmlPage*, const PrHtmlScript*) -> PrStatus {
        ++static_cast<State*>(data)->executed;
        return PrStatus_Ok;
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    PrDomEventListener listener{
        [](void* data, PrDomEvent*) { ++static_cast<State*>(data)->loads; },
        &state,
        1,
        nullptr,
        nullptr,
    };
    prDomEventTargetAddEventListener(prHtmlWindowAsEventTarget(prHtmlPageWindow(page)), "load", &listener, nullptr);
    CHECK(prHtmlPageLoad(page, "<script src='missing.js'></script><script>later</script>",
        "https://example.test/", nullptr) == PrStatus_Ok);
    fetcher.settle(prHtmlPageAgent(page));
    CHECK(state.executed == 1);
    CHECK(state.loads == 1);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    CHECK(prHtmlPageLoad(page, "", "relative", nullptr) == PrStatus_Syntax);
    CHECK(prHtmlWindowDocument(prHtmlPageWindow(page)) == document);
    prHtmlPageDestroy(page);
}

TEST_CASE("base URL uses the first href and rejects data and javascript schemes") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    const PrStringView kHrefs[] = {"https://[invalid", "data:text/plain,base", "javascript:base"};
    for (PrStringView href : kHrefs) {
        PrRawString html = prRawStringCopy("<base href='");
        prRawStringAppend(&html, href);
        prRawStringAppend(&html, "'><base href='https://ignored.test/'><script src='unloaded.js'></script>");
        CHECK(prHtmlPageLoad(page, prRawStringView(&html), "https://example.test/page", nullptr) == PrStatus_Ok);
        prRawStringReset(&html);
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
        PrRawString base = prDomNodeBaseUri(prDomDocumentAsNode(document));
        CHECK(prRawStringView(&base) == "https://example.test/page");
        prRawStringReset(&base);
    }
    prHtmlPageDestroy(page);
}

TEST_CASE("a script runs by its type, and data blocks and empty scripts do not") {
    struct State { String sources; } state;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &state;
    host.execute = [](void* data, PrHtmlPage*, const PrHtmlScript* script) -> PrStatus {
        static_cast<State*>(data)->sources.append(script->source);
        return PrStatus_Ok;
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<script>a</script><script type=''>b</script><script type=' Text/JavaScript '>c</script>"
        "<script language=JScript>d</script><script type='text/template'>no</script>"
        "<script type='text/javascript; charset=utf-8'>no</script><script language=vbscript>no</script>"
        "<script type=importmap>no</script><script></script><script src='/no-loader.js'></script>"
        "<script nomodule>no</script>", "https://example.test/", nullptr) == PrStatus_Ok);
    CHECK(state.sources == "abcd");
    prHtmlPageDestroy(page);
}

TEST_CASE("an SVG script in HTML runs as the parser closes it, by its end tag or as it closes itself, by its href") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<svg><script>a</script><rect/><script href='b.js'/>"
        "<script xlink:href='c.js'></script><script type='text/plain'>no</script></svg>"
        "<script>d</script>", "https://example.test/", nullptr) == PrStatus_Ok);
    later.settle(page);
    CHECK(later.log == "a ");
    later.answer("b.js", "b");
    later.settle(page);
    later.answer("c.js", "c");
    later.settle(page);
    CHECK(later.log == "a b c d ");
    prHtmlPageDestroy(page);
}

TEST_CASE("a module script runs once its graph is fetched, which fetches and parses each module once") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<script type=module src=root.js></script><script type=module src=root.js></script>"
        "<script type=module>import shared.js\ninline</script>", "https://example.test/", nullptr) == PrStatus_Ok);
    CHECK(later.waitingFor("root.js") == 1);
    CHECK(later.waitingFor("shared.js") == 1);
    later.answerScript("root.js", "import a.js\nimport b.js\nroot");
    later.settle(page);
    // The modules that a module imports are fetched together.
    CHECK(later.waitingFor("a.js") == 1);
    CHECK(later.waitingFor("b.js") == 1);
    later.answerScript("a.js", "import shared.js\na");
    later.answerScript("b.js", "import a.js\nb");
    later.settle(page);
    CHECK(later.log.isEmpty());
    CHECK(readiness(page) != PrHtmlDocumentReadyState_Complete);
    later.answerScript("shared.js", "shared");
    later.settle(page);
    // The deferred module scripts run in order, and the modules of their graphs are parsed once each.
    CHECK(later.log == "root root inline ");
    CHECK(later.modules.sources.count() == 5);
    CHECK(later.fetcher.waiting() == 0);
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
}

TEST_CASE("a module script whose graph has a module that does not load fires error at its element") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<script id=missing type=module src=root.js></script>"
        "<script id=typed type=module>import text.js\ntyped</script>"
        "<script type=module>after</script>", "https://example.test/", nullptr) == PrStatus_Ok);
    uint32_t errors = 0;
    PrDomEventListener error{
        [](void* data, PrDomEvent*) { ++*static_cast<uint32_t*>(data); },
        &errors,
        1,
        nullptr,
        nullptr,
    };
    PrDomNode* root = prDomDocumentAsNode(prHtmlWindowDocument(prHtmlPageWindow(page)));
    const char* const kIds[] = {"missing", "typed"};
    for (const char* id : kIds)
        prDomEventTargetAddEventListener(prDomElementAsEventTarget(prDomNodeGetElementById(root, id)), "error", &error, nullptr);
    later.settle(page);
    later.answerScript("root.js", "import gone.js\nroot");
    later.settle(page);
    later.answer("gone.js", "", 404);
    // A module script's response must be JavaScript.
    later.answer("text.js", "text", 200, "text/plain");
    later.settle(page);
    CHECK(errors == 2);
    CHECK(later.log == "after ");
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
}

TEST_CASE("an import fetches its graph and reports in a task, and one that waits is lost with its document") {
    struct Imported {
        Array<PrStatus> statuses;
        static void done(void* PR_NULLABLE data, PrStatus status) {
            static_cast<Imported*>(data)->statuses.push(status);
        }
    } imported;
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<p>page</p>", "https://example.test/", nullptr) == PrStatus_Ok);
    prHtmlPageImportModule(page, "https://example.test/a.js", Imported::done, &imported);
    prHtmlPageImportModule(page, "https://example.test/missing.js", Imported::done, &imported);
    later.settle(page);
    later.answerScript("a.js", "import b.js\na");
    later.answer("missing.js", "", 404);
    later.settle(page);
    CHECK(imported.statuses == Array<PrStatus>({PrStatus_NotFound}));
    later.answerScript("b.js", "b");
    later.settle(page);
    CHECK(imported.statuses == Array<PrStatus>({PrStatus_NotFound, PrStatus_Ok}));

    // A graph that is already fetched is reported in a task too.
    prHtmlPageImportModule(page, "https://example.test/a.js", Imported::done, &imported);
    CHECK(imported.statuses.count() == 2);
    later.settle(page);
    CHECK(imported.statuses.last() == PrStatus_Ok);

    prHtmlPageImportModule(page, "https://example.test/later.js", Imported::done, &imported);
    prHtmlPageDestroy(page);
    CHECK(imported.statuses.last() == PrStatus_Lost);
    CHECK(later.modules.sources.count() == 2);
}

TEST_CASE("a module's imports resolve against the URL that redirects left its response at") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<script type=module src=root.js></script>", "https://example.test/", nullptr) ==
        PrStatus_Ok);
    later.settle(page);
    REQUIRE(later.fetcher.held.count() == 1);
    later.fetcher.respondTo(later.fetcher.held[0], "import a.js\nroot", 200, "text/javascript",
        "https://example.test/moved/root.js");
    later.settle(page);
    CHECK(later.waitingFor("/moved/a.js") == 1);
    later.answerScript("a.js", "a");
    later.settle(page);
    CHECK(later.log == "root ");
    prHtmlPageDestroy(page);
}

TEST_CASE("an import's fetches do not delay the load event") {
    struct Imported {
        Array<PrStatus> statuses;
        static void done(void* PR_NULLABLE data, PrStatus status) {
            static_cast<Imported*>(data)->statuses.push(status);
        }
    } imported;
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<img src=image.png>", "https://example.test/", nullptr) == PrStatus_Ok);
    prHtmlPageImportModule(page, "https://example.test/a.js", Imported::done, &imported);
    later.settle(page);
    later.answer("image.png", "");
    later.settle(page);
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    CHECK(later.waitingFor("a.js") == 1);
    CHECK(imported.statuses.isEmpty());
    later.answerScript("a.js", "a");
    later.settle(page);
    CHECK(imported.statuses == Array<PrStatus>({PrStatus_Ok}));
    prHtmlPageDestroy(page);
}

TEST_CASE("module execution follows parsing and precedes host settlement and load") {
    struct State {
        TestModules modules;
        uint32_t scripts = 0;
        bool settled = false;
        bool loaded = false;
    } state;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &state;
    host.parseModule = [](void* data, PrHtmlPage*, const PrHtmlScript* script,
            PrHtmlModuleRequestFn request, void* PR_NULLABLE requestContext) {
        return static_cast<State*>(data)->modules.parse(script, request, requestContext);
    };
    host.execute = [](void* data, PrHtmlPage* page, const PrHtmlScript* script) -> PrStatus {
        State* state = static_cast<State*>(data);
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
        CHECK_FALSE(state->settled);
        CHECK_FALSE(state->loaded);
        if (script->kind == PrHtmlScriptKind_Classic) {
            CHECK(state->scripts == 0);
            CHECK(prDomNodeGetElementById(prDomDocumentAsNode(document), "later") == nullptr);
        } else {
            CHECK(state->scripts == 1);
            CHECK(prDomNodeGetElementById(prDomDocumentAsNode(document), "later") != nullptr);
            CHECK(prHtmlDocumentWrite(prHtmlWindowDocument(prHtmlPageWindow(page)), "ignored") == PrStatus_Ok);
            CHECK(script->baseUrl == "https://example.test/modules/");
        }
        ++state->scripts;
        return PrStatus_Ok;
    };
    host.settle = [](void* data, PrHtmlPage*) -> PrStatus {
        State* state = static_cast<State*>(data);
        CHECK(state->scripts == 2);
        CHECK_FALSE(state->loaded);
        state->settled = true;
        return PrStatus_Ok;
    };
    PrDomEventListener listener{
        [](void* data, PrDomEvent*) {
            State* state = static_cast<State*>(data);
            CHECK(state->settled);
            state->loaded = true;
        },
        &state,
        1,
        nullptr,
        nullptr,
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    prDomEventTargetAddEventListener(prHtmlWindowAsEventTarget(prHtmlPageWindow(page)), "load", &listener, nullptr);
    CHECK(prHtmlPageLoad(page, "<base href='/modules/'><script type=module>module</script>"
        "<script>classic</script><p id=later>later</p>", "https://example.test/page", nullptr) == PrStatus_Ok);
    CHECK(state.loaded);
    prHtmlPageDestroy(page);
}

TEST_CASE("a style element fires load for each update of its block, and the window's load waits for it") {
    struct State { uint32_t loads = 0; uint32_t errors = 0; uint32_t loadsAtWindowLoad = 0; } state;
    PrDomEventListener load{
        [](void* data, PrDomEvent*) { ++static_cast<State*>(data)->loads; },
        &state,
        1,
        nullptr,
        nullptr,
    };
    PrDomEventListener error{
        [](void* data, PrDomEvent*) { ++static_cast<State*>(data)->errors; },
        &state,
        1,
        nullptr,
        nullptr,
    };
    PrDomEventListener window{
        [](void* data, PrDomEvent*) {
            State* state = static_cast<State*>(data);
            state->loadsAtWindowLoad = state->loads;
        },
        &state,
        1,
        nullptr,
        nullptr,
    };
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrHtmlWindow* view = prHtmlPageWindow(page);
    prDomEventTargetAddEventListener(prHtmlWindowAsEventTarget(view), "load", &window, nullptr);
    // The listeners are added when the style element is created, before its block is updated.
    CHECK(prHtmlPageLoad(page, "<style id=s></style><style id=e></style>", "https://example.test/", nullptr) ==
        PrStatus_Ok);
    PrDomNode* root = prDomDocumentAsNode(prHtmlWindowDocument(view));
    PrDomElement* style = prDomNodeGetElementById(root, "s");
    PrDomElement* failing = prDomNodeGetElementById(root, "e");
    prDomEventTargetAddEventListener(prDomElementAsEventTarget(style), "load", &load, nullptr);
    prDomEventTargetAddEventListener(prDomElementAsEventTarget(failing), "error", &error, nullptr);

    // Each text a script sets is one update for the text it removes and one for the text it inserts. Each update fires,
    // however soon the next one comes, and even though the element leaves the document (html/#update-a-style-block).
    prDomNodeSetTextContent(prDomElementAsNode(style), "a {}");
    prDomNodeSetTextContent(prDomElementAsNode(style), "b {}");
    prDomNodeSetTextContent(prDomElementAsNode(style), "c {}");
    prDomNodeRemove(prDomElementAsNode(style));
    // An import that does not load is an error.
    prDomNodeSetTextContent(prDomElementAsNode(failing), "@import url(missing.css);");
    while (prDomAgentRunTask(prHtmlPageAgent(page))) {
    }
    CHECK(state.loads == 5);
    CHECK(state.errors == 1);
    prHtmlPageDestroy(page);

    // The window's load event waits for the load events that its style elements fire. A listener that captures them at
    // the document, added once the document is parsed, receives them before the window's load.
    state = State{};
    struct Waiting { State* state; PrDomEventListener* load; } waiting = {&state, &load};
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &waiting;
    host.settle = [](void* data, PrHtmlPage* page) -> PrStatus {
        Waiting* waiting = static_cast<Waiting*>(data);
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
        PrDomEventTarget* target = prDomNodeAsEventTarget(prDomDocumentAsNode(document));
        PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(target, "load");
        options.capture = true;
        prDomEventTargetAddEventListener(target, "load", waiting->load, &options);
        return PrStatus_Ok;
    };
    page = prHtmlPageCreate(&host);
    view = prHtmlPageWindow(page);
    prDomEventTargetAddEventListener(prHtmlWindowAsEventTarget(view), "load", &window, nullptr);
    CHECK(prHtmlPageLoad(page, "<style>a {}</style>", "https://example.test/", nullptr) == PrStatus_Ok);
    CHECK(state.loadsAtWindowLoad == 1);
    prHtmlPageDestroy(page);
}

TEST_CASE("a parser failure releases deferred module source without executing it") {
    PrHtmlHost host = prHtmlHostDefault();
    host.execute = [](void*, PrHtmlPage*, const PrHtmlScript* script) -> PrStatus {
        CHECK(script->kind == PrHtmlScriptKind_Classic);
        return PrStatus_InvalidState;
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<script type=module>an owned source string longer than inline storage</script>"
        "<script>fails</script>", "https://example.test/", nullptr) == PrStatus_InvalidState);
    prHtmlPageDestroy(page);
}

TEST_CASE("a media element its window runs may leave for a document that goes first") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<video id=v></video>", "about:blank", nullptr) == PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* video = prDomNodeGetElementById(prDomDocumentAsNode(document), "v");
    PrHtmlMediaElement* media = prHtmlMediaElementQuery(prDomElementAsEventTarget(video));
    REQUIRE(media != nullptr);
    prHtmlMediaElementLoad(media);
    PrDomDocument* other = prDomDocumentCreate();
    CHECK(prDomNodeAppendChild(prDomDocumentAsNode(other), prDomElementAsNode(video)) == PrStatus_Ok);
    prDomDocumentRelease(other);
    prHtmlPageDestroy(page);
}

TEST_CASE("a page is visible until its host hides it, and its document then fires visibilitychange in a task") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<p>text</p>", "about:blank", nullptr) == PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    CHECK(prHtmlDocumentVisibilityState(document) == PrHtmlVisibility_Visible);
    struct Heard {
        int count = 0;
        bool bubbles = false;
    } heard;
    PrDomEventListener listener{
        .invoke = [](void* context, PrDomEvent* event) {
            Heard* h = static_cast<Heard*>(context);
            ++h->count;
            h->bubbles = prDomEventBubbles(event);
        },
        .context = &heard,
    };
    prDomEventTargetAddEventListener(prDomDocumentAsEventTarget(document), "visibilitychange", &listener, nullptr);
    prHtmlPageSetVisibility(page, PrHtmlVisibility_Hidden);
    CHECK(prHtmlDocumentVisibilityState(document) == PrHtmlVisibility_Hidden);
    CHECK(heard.count == 0);
    runTasks(page);
    CHECK(heard.count == 1);
    CHECK(heard.bubbles);
    // Setting the same visibility again changes nothing.
    prHtmlPageSetVisibility(page, PrHtmlVisibility_Hidden);
    runTasks(page);
    CHECK(heard.count == 1);
    // A document that is not its window's has no visibility of its own.
    PrDomDocument* other = prDomDocumentCreate();
    CHECK(prHtmlDocumentVisibilityState(other) == PrHtmlVisibility_Hidden);
    prDomDocumentRelease(other);
    prHtmlPageDestroy(page);
}

TEST_CASE("a media element keeps its state and track lists without a window, in their initial state") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<video id=v src=a.mp4></video>", "about:blank", nullptr) == PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* video = prDomNodeGetElementById(prDomDocumentAsNode(document), "v");
    PrHtmlMediaElement* media = prHtmlMediaElementQuery(prDomElementAsEventTarget(video));
    REQUIRE(media != nullptr);
    prDomNodeRetain(prDomElementAsNode(video));
    PrHtmlTrackList* texts = prHtmlMediaElementTracks(media, PrHtmlTrackKind_Text);
    CHECK(prHtmlTrackListKind(texts) == PrHtmlTrackKind_Text);
    CHECK(prHtmlTrackListQuery(prHtmlTrackListAsEventTarget(texts)) == texts);
    CHECK(prHtmlMediaElementSetVolume(media, 0.5) == PrStatus_Ok);
    CHECK(prHtmlMediaElementVolume(media) == 0.5);
    // Once the window goes, the element is in its initial state, and its lists are the same.
    prHtmlPageDestroy(page);
    CHECK(prHtmlMediaElementVolume(media) == 1);
    CHECK(prHtmlMediaElementPaused(media));
    CHECK(prHtmlMediaElementNetworkState(media) == PrHtmlMediaNetworkState_Empty);
    CHECK(prHtmlMediaElementTracks(media, PrHtmlTrackKind_Text) == texts);
    prHtmlMediaElementLoad(media);
    CHECK(prHtmlMediaElementNetworkState(media) == PrHtmlMediaNetworkState_Empty);
    prDomNodeRelease(prDomElementAsNode(video));
}

TEST_CASE("the host designates and activates elements, and answers the states it alone knows") {
    struct State { const PrDomElement* known = nullptr; } state;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &state;
    host.elementState =
            [](void* data, PrHtmlPage*, const PrDomElement* element, PrDomHostState wanted) -> PrDomHostState {
        return element == static_cast<State*>(data)->known ? wanted : PrDomHostState(0);
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<div id=outer><label id=label for=name>Name <span id=inner>x</span></label></div>"
        "<input id=name><a id=link href=x>link</a><fieldset disabled><input id=off></fieldset>",
        "about:blank", nullptr) == PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomNode* root = prDomDocumentAsNode(document);
    auto byId = [&](const char* id) {
        return prDomNodeGetElementById(root, id);
    };
    auto matches = [&](const char* id, const char* selectors) {
        bool result = false;
        REQUIRE(prCssElementMatches(byId(id), selectors, &result) == PrStatus_Ok);
        return result;
    };

    // The designated element's flat-tree ancestors, and the labeled control of any label among them.
    prHtmlDocumentSetHover(document, byId("inner"));
    CHECK(matches("inner", ":hover"));
    CHECK(matches("label", ":hover"));
    CHECK(matches("outer", ":hover"));
    CHECK(matches("name", ":hover"));
    CHECK_FALSE(matches("link", ":hover"));
    prHtmlDocumentSetActive(document, byId("name"));
    CHECK(matches("name", ":active"));
    CHECK_FALSE(matches("label", ":active"));

    // A script sees every link as unvisited, whatever the host knows.
    state.known = byId("link");
    CHECK(matches("link", ":link"));
    CHECK_FALSE(matches("link", ":visited"));
    CHECK_FALSE(matches("link", ":autofill"));
    state.known = byId("name");
    CHECK(matches("name", ":autofill"));

    // A removed element is no longer designated.
    PrDomElement* outer = byId("outer");
    PrDomElement* inner = byId("inner");
    prDomNodeRetain(prDomElementAsNode(outer));
    prDomNodeRemove(prDomElementAsNode(outer));
    bool hovered = true;
    REQUIRE(prCssElementMatches(inner, ":hover", &hovered) == PrStatus_Ok);
    CHECK_FALSE(hovered);
    prDomNodeRelease(prDomElementAsNode(outer));
    prHtmlDocumentSetHover(document, nullptr);

    // A control inside a disabled fieldset is disabled, and so not focusable.
    CHECK(matches("off", ":disabled"));
    prHtmlFocus(byId("off"), nullptr);
    CHECK(prHtmlActiveElement(root) != byId("off"));
    prHtmlPageDestroy(page);
}

TEST_CASE("a script the host answers later pauses the parse, and a sheet it answers later blocks scripts and rendering") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<head><link rel=stylesheet href=a.css><script>inline</script>"
        "<script src=b.js></script></head><body><p id=after>after</p>"
        "<script defer src=d.js></script><img src=i.png></body>", "https://example.test/", nullptr) == PrStatus_Ok);
    // The sheet blocks the inline script, and the parse waits for it: the document has no body, and does not render.
    CHECK(later.log.isEmpty());
    CHECK_FALSE(has(page, "after"));
    CHECK(prHtmlPageIsRenderBlocked(page));
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Loading);
    REQUIRE(later.fetcher.waiting() == 1);

    // The sheet arrives: the inline script runs, and the parse goes on to wait for the external script.
    later.answer("a.css", "p { color: red }");
    later.settle(page);
    CHECK(later.log == "inline ");
    CHECK_FALSE(has(page, "after"));
    CHECK(prHtmlPageIsRenderBlocked(page));

    // The script arrives and runs, and the parse goes to its end. The deferred script and the image delay the rest.
    later.answer("b.js", "external");
    later.settle(page);
    CHECK(later.log == "inline external ");
    CHECK(has(page, "after"));
    CHECK_FALSE(prHtmlPageIsRenderBlocked(page));
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Interactive);
    later.answer("d.js", "deferred");
    later.settle(page);
    CHECK(later.log == "inline external deferred ");
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Interactive);
    later.answer("i.png", "not an image");
    later.settle(page);
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
}

TEST_CASE("a media attribute changes its sheet's media in place, and a link's load handler that sets it runs once") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<link id=l rel=stylesheet href=a.css media=print>"
        "<style id=s media=print>p {}</style>", "https://example.test/", nullptr) == PrStatus_Ok);
    PrDomNode* root = prDomDocumentAsNode(prHtmlWindowDocument(prHtmlPageWindow(page)));
    PrDomElement* link = prDomNodeGetElementById(root, "l");
    PrDomElement* style = prDomNodeGetElementById(root, "s");

    // The pattern that loads a sheet without blocking: its load handler sets the media to all, which sets the attribute
    // again even if its value is already all.
    struct State { PrDomElement* link; uint32_t loads = 0; } state{link};
    PrDomEventListener load{
        [](void* data, PrDomEvent*) {
            State* state = static_cast<State*>(data);
            ++state->loads;
            CHECK(prDomElementSetAttribute(state->link, "media", "all") == PrStatus_Ok);
        },
        &state,
        1,
        nullptr,
        nullptr,
    };
    prDomEventTargetAddEventListener(prDomElementAsEventTarget(link), "load", &load, nullptr);
    later.fetcher.waiting();
    later.answer("a.css", "p { color: red }");
    later.settle(page);
    CHECK(state.loads == 1);
    CHECK(later.fetcher.waiting() == 0);
    CHECK(sheetMedia(page, 0) == "all");

    CHECK(sheetMedia(page, 1) == "print");
    CHECK(prDomElementSetAttribute(style, "media", "screen") == PrStatus_Ok);
    CHECK(sheetMedia(page, 1) == "screen");
    prDomElementRemoveAttribute(style, "media");
    CHECK(sheetMedia(page, 1).isEmpty());
    later.settle(page);
    CHECK(state.loads == 1);
    prHtmlPageDestroy(page);
}

TEST_CASE("a preload link fetches its resource and fires load or error, and fetches nothing for an unknown as") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<link id=i rel=preload href=a.png as=image><link id=m rel=preload href=b.png as=image>"
        "<link id=x rel=preload href=c.png as=nothing>", "https://example.test/", nullptr) == PrStatus_Ok);
    PrDomNode* root = prDomDocumentAsNode(prHtmlWindowDocument(prHtmlPageWindow(page)));
    // Each listener writes the type of the event that it receives.
    String log;
    PrDomEventListener listener{
        [](void* data, PrDomEvent* event) {
            PrStringView type = prDomEventType(event);
            static_cast<String*>(data)->append(type).append(" ");
        },
        &log,
        1,
        nullptr,
        nullptr,
    };
    prDomEventTargetAddEventListener(prDomElementAsEventTarget(prDomNodeGetElementById(root, "i")), "load", &listener, nullptr);
    prDomEventTargetAddEventListener(prDomElementAsEventTarget(prDomNodeGetElementById(root, "m")), "error", &listener, nullptr);
    CHECK(later.fetcher.waiting() == 2);
    later.answer("a.png", "png");
    later.answer("b.png", "", 404);
    later.settle(page);
    CHECK(log == "load error ");
    prHtmlPageDestroy(page);
}

TEST_CASE("an element's crossorigin attribute decides its request's mode and credentials") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<img src=none.png><img src=empty.png crossorigin>"
        "<img src=invalid.png crossorigin=other><img src=credentials.png crossorigin=USE-CREDENTIALS>"
        "<link rel=stylesheet href=sheet.css crossorigin=anonymous><script type=module src=module.js></script>"
        "<iframe src=frame.html crossorigin=use-credentials></iframe>", "https://example.test/", nullptr) ==
        PrStatus_Ok);
    // An img's fetch starts as its src is set, and starts again as its crossorigin is set. The last request is current.
    later.fetcher.waiting();
    struct Expected {
        const char* name;
        PrFetchMode mode;
        PrFetchCredentials credentials;
    };
    const Expected expected[] = {
        {"none.png", PrFetchMode_NoCors, PrFetchCredentials_Include},
        {"empty.png", PrFetchMode_Cors, PrFetchCredentials_SameOrigin},
        {"invalid.png", PrFetchMode_Cors, PrFetchCredentials_SameOrigin},
        {"credentials.png", PrFetchMode_Cors, PrFetchCredentials_Include},
        {"sheet.css", PrFetchMode_Cors, PrFetchCredentials_SameOrigin},
        {"module.js", PrFetchMode_Cors, PrFetchCredentials_SameOrigin},
        {"frame.html", PrFetchMode_Navigate, PrFetchCredentials_Include},
    };
    for (const Expected& e : expected) {
        PrFetchInterceptRequest* last = nullptr;
        for (PrFetchInterceptRequest* request : later.fetcher.held) {
            if (prStringEndsWith(TestFetcher::urlOf(request), e.name))
                last = request;
        }
        REQUIRE_MESSAGE(last != nullptr, e.name);
        CHECK_MESSAGE(last->desc.mode == e.mode, e.name);
        CHECK_MESSAGE(last->desc.credentials == e.credentials, e.name);
    }
    prHtmlPageDestroy(page);
}

TEST_CASE("an XML document's parse pauses at an external script until it arrives, and runs it where it stands") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    PrHtmlResponseHeaders headers{.contentType = "application/xhtml+xml"};
    CHECK(prHtmlPageLoad(page, "<html xmlns='http://www.w3.org/1999/xhtml'><body><p id='before'/>"
        "<script src='a.js'/><script>inline</script><p id='after'/>"
        "<script async='' src='b.js'/><p id='last'/></body></html>", "https://example.test/page.xhtml", &headers) ==
        PrStatus_Ok);
    CHECK(has(page, "before"));
    CHECK_FALSE(has(page, "after"));
    REQUIRE(later.fetcher.waiting() == 1);
    later.answer("a.js", "external");
    later.settle(page);
    CHECK(later.log == "external inline ");
    // An async script does not pause the parse.
    CHECK(has(page, "last"));
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Interactive);
    later.answer("b.js", "async");
    later.settle(page);
    CHECK(later.log == "external inline async ");
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
}

TEST_CASE("an XML document's script waits for the style sheets before it that block scripts") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    PrHtmlResponseHeaders headers{.contentType = "application/xhtml+xml"};
    CHECK(prHtmlPageLoad(page, "<html xmlns='http://www.w3.org/1999/xhtml'><head>"
        "<link rel='stylesheet' href='a.css'/></head>"
        "<body><script>inline</script><p id='after'/></body></html>", "https://example.test/page.xhtml", &headers) ==
        PrStatus_Ok);
    CHECK(later.log.isEmpty());
    CHECK_FALSE(has(page, "after"));
    REQUIRE(later.fetcher.waiting() == 1);
    later.answer("a.css", "p { color: green }");
    later.settle(page);
    CHECK(later.log == "inline ");
    CHECK(has(page, "after"));
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
}

TEST_CASE("a streamed XML document parses as its pieces arrive, in the charset that its declaration names") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    PrHtmlResponseHeaders headers{.contentType = "application/xhtml+xml"};
    REQUIRE(prHtmlPageBeginLoad(page, "https://example.test/page.xhtml", &headers) == PrStatus_Ok);
    // The parse waits for the end of the XML declaration, which names the charset.
    prHtmlPageAppendLoad(page, "<?xml version='1.0' encoding='windows-1252'");
    runTasks(page);
    prHtmlPageAppendLoad(page, "?><html xmlns='http://www.w3.org/1999/xhtml'><body><p id='first'>\xE9");
    runTasks(page);
    CHECK(has(page, "first"));
    CHECK_FALSE(has(page, "second"));
    prHtmlPageAppendLoad(page, "</p><p id='sec");
    runTasks(page);
    CHECK_FALSE(has(page, "second"));
    prHtmlPageAppendLoad(page, "ond'/></body></html>");
    runTasks(page);
    CHECK(has(page, "second"));
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Loading);
    prHtmlPageEndLoad(page);
    runTasks(page);
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrRawString text = prDomNodeTextContent(prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), "first")));
    CHECK(prRawStringView(&text) == "\xC3\xA9");
    prRawStringReset(&text);
    prHtmlPageDestroy(page);
}

TEST_CASE("a streamed XML document whose input ends while a script pauses its parse ends once the script runs") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    PrHtmlResponseHeaders headers{.contentType = "application/xhtml+xml"};
    REQUIRE(prHtmlPageBeginLoad(page, "https://example.test/page.xhtml", &headers) == PrStatus_Ok);
    prHtmlPageAppendLoad(page, "<html xmlns='http://www.w3.org/1999/xhtml'><body>"
        "<script src='a.js'/><p id='after'/></body></html>");
    runTasks(page);
    CHECK_FALSE(has(page, "after"));
    prHtmlPageEndLoad(page);
    runTasks(page);
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Loading);
    REQUIRE(later.fetcher.waiting() == 1);
    later.answer("a.js", "external");
    later.settle(page);
    CHECK(later.log == "external ");
    CHECK(has(page, "after"));
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
}

TEST_CASE("a streamed XML document that is not well-formed shows its error once its parse reaches it") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    PrHtmlResponseHeaders headers{.contentType = "application/xml"};
    REQUIRE(prHtmlPageBeginLoad(page, "https://example.test/page.xml", &headers) == PrStatus_Ok);
    prHtmlPageAppendLoad(page, "<r><a id='a'/></b>");
    runTasks(page);
    CHECK_FALSE(has(page, "a"));
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* root = prDomDocumentElement(document);
    REQUIRE(root != nullptr);
    PrStringView name = prDomElementLocalName(root);
    CHECK(name == "parsererror");
    prHtmlPageDestroy(page);
}

TEST_CASE("a fetch the host answers after its browsing context is gone is dropped") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<script src=a.js></script><script async src=b.js></script>",
        "https://example.test/", nullptr) == PrStatus_Ok);
    REQUIRE(later.fetcher.waiting() == 1);
    // An answer whose task has not run yet is dropped with the context too.
    later.answer("a.js", "first");
    prHtmlPageDestroy(page);
    CHECK(later.log.isEmpty());
    // A new load drops the previous load's fetches as well.
    page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<script src=c.js></script>", "https://example.test/", nullptr) == PrStatus_Ok);
    CHECK(prHtmlPageLoad(page, "<p>", "https://example.test/", nullptr) == PrStatus_InvalidState);
    REQUIRE(later.fetcher.waiting() == 1);
    prHtmlPageDestroy(page);
    later.answer("c.js", "late");
    prNetNullRun(later.fetcher.net);
    CHECK(later.log.isEmpty());
}

TEST_CASE("a location of another document navigates the page from a task, and a fragment does not") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<p id=top>", "https://example.test/start", nullptr) == PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    CHECK(prHtmlWindowSetLocation(window, window, PrUrlComponent_Hash, "top", PrHtmlHistoryHandling_Push) ==
        PrStatus_Ok);
    CHECK(prHtmlWindowSetLocation(window, window, PrUrlComponent_Href, "next?q=1#part", PrHtmlHistoryHandling_Push) ==
        PrStatus_Ok);
    CHECK(later.waitingFor("next?q=1") == 0);
    later.settle(page);
    REQUIRE(later.waitingFor("next?q=1") == 1);
    later.answer("next?q=1", "<p id=next>");
    later.settle(page);
    CHECK(prHtmlPageWindow(page) != window);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    CHECK(prDomNodeGetElementById(prDomDocumentAsNode(document), "next") != nullptr);
    prHtmlPageDestroy(page);

    // Without a fetcher, only a fragment navigation is supported.
    PrHtmlHost bare = prHtmlHostDefault();
    page = prHtmlPageCreate(&bare);
    REQUIRE(prHtmlPageLoad(page, "<p>", "https://example.test/start", nullptr) == PrStatus_Ok);
    CHECK(prHtmlWindowSetLocation(prHtmlPageWindow(page), prHtmlPageWindow(page), PrUrlComponent_Href, "other",
        PrHtmlHistoryHandling_Push) == PrStatus_NotSupported);
    prHtmlPageDestroy(page);
}

TEST_CASE("a navigation that a document starts is fetched from its origin, and the user's and a traversal from none") {
    LaterHost later;
    PrHtmlSessionHistory* history = prHtmlSessionHistoryCreate();
    PrHtmlHost host = later.host();
    host.sessionHistory = history;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    // Answers the navigation that waits for the URL that ends in `name`, and returns the origin that it was fetched
    // from.
    auto originOf = [&](const char* name) {
        later.settle(page);
        REQUIRE(later.waitingFor(name) == 1);
        String origin = "<none>";
        for (PrFetchInterceptRequest* request : later.fetcher.held) {
            if (prStringEndsWith(TestFetcher::urlOf(request), name) && request->desc.origin.data != nullptr)
                origin = String(request->desc.origin);
        }
        later.answer(name, "<p>");
        later.settle(page);
        return origin;
    };
    REQUIRE(prHtmlPageNavigate(page, "https://example.test/a") == PrStatus_Ok);
    CHECK(originOf("/a") == "<none>");
    REQUIRE(prHtmlWindowSetLocation(prHtmlPageWindow(page), prHtmlPageWindow(page), PrUrlComponent_Href, "https://other.test/b",
        PrHtmlHistoryHandling_Push) == PrStatus_Ok);
    CHECK(originOf("/b") == "https://example.test");
    prHtmlPageTraverse(page, -1);
    CHECK(originOf("/a") == "<none>");
    prHtmlPageDestroy(page);
    prHtmlSessionHistoryDestroy(history);
}

TEST_CASE("a hyperlink whose target names no browsing context needs the user's activation for a new one") {
    struct Opened {
        Array<String> urls;
    } opened;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &opened;
    host.openWindow = [](void* context, PrHtmlPage*, PrStringView url) {
        static_cast<Opened*>(context)->urls.emplace(url);
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<a id=blank target=_BLANK href=one></a><a id=unknown target=nowhere href=two></a>"
        "<a id=dangling target=\"x\n<\" href=three></a><a id=script target=_blank href=\"javascript:1\"></a>",
        "https://example.test/", nullptr) == PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomNode* root = prDomDocumentAsNode(prHtmlWindowDocument(window));
    auto click = [&](const char* id) {
        prHtmlElementClick(prHtmlElementQuery(prDomElementAsEventTarget(prDomNodeGetElementById(root, id))));
    };
    click("blank");
    CHECK(opened.urls.isEmpty());
    CHECK(!prHtmlWindowHasStickyActivation(window));
    const char* const kIds[] = {"blank", "unknown", "dangling"};
    for (const char* id : kIds) {
        prHtmlWindowNotifyUserActivation(window);
        CHECK(prHtmlWindowHasTransientActivation(window));
        click(id);
        CHECK(!prHtmlWindowHasTransientActivation(window));
        CHECK(prHtmlWindowHasStickyActivation(window));
    }
    click("blank");
    prHtmlWindowNotifyUserActivation(window);
    click("script");
    CHECK(prHtmlWindowHasTransientActivation(window));
    CHECK(opened.urls == Array<String>({
        "https://example.test/one", "https://example.test/two", "https://example.test/three",
    }));
    prHtmlPageDestroy(page);
}

TEST_CASE("a page's session history keeps each document's entries, and going to another document's navigates") {
    LaterHost later;
    PrHtmlSessionHistory* history = prHtmlSessionHistoryCreate();
    PrHtmlHost host = later.host();
    host.sessionHistory = history;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    auto url = [&](uint32_t index) {
        PrStringView text = prHtmlSessionHistoryUrl(history, index);
        return text.data == nullptr ? String("<none>") : String(text);
    };
    // Answers the navigation that waits for the document at the URL that ends in `name`.
    auto arrive = [&](const char* name) {
        later.settle(page);
        REQUIRE(later.waitingFor(name) == 1);
        later.answer(name, "<p id=x>");
        later.settle(page);
    };
    REQUIRE(prHtmlPageNavigate(page, "https://example.test/a") == PrStatus_Ok);
    arrive("/a");
    REQUIRE(prHtmlPageNavigate(page, "https://example.test/b") == PrStatus_Ok);
    arrive("/b");
    PrHtmlWindow* window = prHtmlPageWindow(page);
    // A fragment navigation and pushState add entries for the same document.
    REQUIRE(prHtmlWindowSetLocation(window, window, PrUrlComponent_Hash, "x", PrHtmlHistoryHandling_Push) ==
        PrStatus_Ok);
    REQUIRE(prHtmlHistoryPushState(window, "state", "c", PrHtmlHistoryHandling_Push) == PrStatus_Ok);
    CHECK(prHtmlSessionHistoryLength(history) == 4);
    CHECK(prHtmlSessionHistoryIndex(history) == 3);
    CHECK(url(3) == "https://example.test/c");
    // A URL of another origin is refused.
    CHECK(prHtmlHistoryPushState(window, PrStringView{}, "https://other.test/", PrHtmlHistoryHandling_Push) ==
        PrStatus_NotAllowed);

    // Going back within the document changes its URL and state in a task, and fetches nothing.
    prHtmlPageTraverse(page, -1);
    later.settle(page);
    CHECK(prHtmlSessionHistoryIndex(history) == 2);
    PrRawString hash = prHtmlWindowLocation(window, PrUrlComponent_Hash);
    CHECK(prRawStringView(&hash) == "#x");
    prRawStringReset(&hash);
    PrStringView state{};
    uint32_t generation = 0;
    REQUIRE(prHtmlHistoryState(window, &state, &generation) == PrStatus_Ok);
    CHECK(state.data == nullptr);
    CHECK(later.fetcher.held.isEmpty());

    // Going back past the document navigates to the entry's URL, and the document that loads takes that entry.
    prHtmlPageTraverse(page, -2);
    arrive("/a");
    CHECK(prHtmlSessionHistoryLength(history) == 4);
    CHECK(prHtmlSessionHistoryIndex(history) == 0);

    // Going forward to the pushed entry loads its document with the entry's state.
    prHtmlPageTraverse(page, 3);
    arrive("/c");
    CHECK(prHtmlSessionHistoryIndex(history) == 3);
    REQUIRE(prHtmlHistoryState(prHtmlPageWindow(page), &state, &generation) == PrStatus_Ok);
    CHECK(String(state) == "state");

    // A navigation that replaces drops nothing, and the document that loads takes the current entry with no state.
    REQUIRE(prHtmlWindowSetLocation(prHtmlPageWindow(page), prHtmlPageWindow(page), PrUrlComponent_Href, "d",
        PrHtmlHistoryHandling_Replace) == PrStatus_Ok);
    arrive("/d");
    CHECK(prHtmlSessionHistoryLength(history) == 4);
    CHECK(url(3) == "https://example.test/d");
    REQUIRE(prHtmlHistoryState(prHtmlPageWindow(page), &state, &generation) == PrStatus_Ok);
    CHECK(state.data == nullptr);

    // A navigation that pushes from an earlier entry drops the entries after it.
    prHtmlPageTraverse(page, -3);
    arrive("/a");
    REQUIRE(prHtmlPageNavigate(page, "https://example.test/e") == PrStatus_Ok);
    arrive("/e");
    CHECK(prHtmlSessionHistoryLength(history) == 2);
    CHECK(url(1) == "https://example.test/e");

    prHtmlPageDestroy(page);
    prHtmlSessionHistoryDestroy(history);
}

TEST_CASE("a script's fetch sends what it asks, and receives the response filtered as its type says") {
    struct Responses {
        Array<PrStatus> statuses;
        Array<String> received;
        static void done(void* PR_NULLABLE data, const PrFetchResponse* response) {
            Responses* responses = static_cast<Responses*>(data);
            responses->statuses.push(response->status);
            PrStringView url = response->url.data != nullptr ? response->url : PrStringView{""};
            String text = format("{} {} ", response->responseStatus, url);
            for (uint32_t i = 0; i < response->headerCount; ++i)
                formatTo(text, "{}={} ", response->headers[i].name, response->headers[i].value);
            if (response->body.size != 0)
                text.append(prStringFromBytes(response->body));
            responses->received.push(text);
        }
    } responses;
    LaterHost later;
    String sent;
    later.fetcher.answer = [&](PrFetchInterceptRequest* request) {
        const PrFetchRequestDesc* desc = &request->desc;
        String url = TestFetcher::urlOf(request);
        if (prStringEndsWith(url, "/wait"))
            return;
        if (prStringEndsWith(url, "/post")) {
            sent = format("{} {} {} {}", desc->method, desc->destination, desc->mode, prStringFromBytes(desc->body));
            for (uint32_t i = 0; i < desc->headerCount; ++i)
                formatTo(sent, " {}", desc->headers[i].name);
        }
        const PrFetchHeader headers[] = {
            {"Content-Type", "text/plain"}, {"Set-Cookie", "a=b"},
            {"X-Exposed", "1"}, {"X-Hidden", "2"},
            {"Access-Control-Expose-Headers", "X-Exposed"},
        };
        PrFetchResponseType type = PrFetchResponseType_Basic;
        if (prStringEndsWith(url, "/cors"))
            type = PrFetchResponseType_Cors;
        else if (prStringEndsWith(url, "/opaque"))
            type = PrFetchResponseType_Opaque;
        PrFetchInterceptResponse response{
            .status = 201,
            .type = type,
            .headers = headers,
            .headerCount = 5,
            .body = PrByteView{reinterpret_cast<const uint8_t*>("body"), 4},
        };
        later.fetcher.respondWith(request, response);
    };
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<base href=/dir/><p>", "https://example.test/page", nullptr) == PrStatus_Ok);
    later.settle(page);

    const PrFetchHeader asked[] = {{"X-Asked", "yes"}};
    PrFetchClientRequest post{
        .url = "post#fragment",
        .method = "POST",
        .headers = asked,
        .headerCount = 1,
        .body = PrByteView{reinterpret_cast<const uint8_t*>("sent"), 4},
        .mode = PrFetchMode_Cors,
        .credentials = PrFetchCredentials_SameOrigin,
    };
    CHECK(prHtmlPageFetch(page, &post, Responses::done, &responses) == PrStatus_Ok);
    PrFetchClientRequest cors{.url = "https://other.test/cors", .mode = PrFetchMode_Cors};
    CHECK(prHtmlPageFetch(page, &cors, Responses::done, &responses) == PrStatus_Ok);
    PrFetchClientRequest opaque{.url = "https://other.test/opaque", .mode = PrFetchMode_NoCors};
    CHECK(prHtmlPageFetch(page, &opaque, Responses::done, &responses) == PrStatus_Ok);
    PrFetchClientRequest data{.url = "data:,inline", .mode = PrFetchMode_Cors};
    CHECK(prHtmlPageFetch(page, &data, Responses::done, &responses) == PrStatus_Ok);
    PrFetchClientRequest invalid{.url = "https://user@example.test/", .mode = PrFetchMode_Cors};
    CHECK(prHtmlPageFetch(page, &invalid, Responses::done, &responses) == PrStatus_InvalidData);
    // Nothing is answered during the calls that ask, a data: URL included.
    CHECK(responses.statuses.isEmpty());
    later.settle(page);
    CHECK(sent == "POST 0 1 sent X-Asked");
    CHECK(responses.statuses == Array<PrStatus>({PrStatus_Ok, PrStatus_Ok, PrStatus_Ok, PrStatus_Ok}));
    REQUIRE(responses.received.count() == 4);
    // The data: URL is answered in a task of its own, before the network's answers arrive. A basic response hides
    // Set-Cookie, a CORS one shows only what it exposes, and an opaque one shows nothing.
    CHECK(responses.received[0] == "200 data:,inline Content-Type=text/plain;charset=US-ASCII inline");
    CHECK(responses.received[1] == "201 https://example.test/dir/post Content-Type=text/plain X-Exposed=1 X-Hidden=2 "
        "Access-Control-Expose-Headers=X-Exposed body");
    CHECK(responses.received[2] == "201 https://other.test/cors Content-Type=text/plain X-Exposed=1 body");
    CHECK(responses.received[3] == "0  ");

    // A script's fetch does not delay the load event, and one that waits is lost with its document.
    PrFetchClientRequest waits{.url = "wait", .mode = PrFetchMode_Cors};
    CHECK(prHtmlPageFetch(page, &waits, Responses::done, &responses) == PrStatus_Ok);
    later.settle(page);
    CHECK(later.waitingFor("/wait") == 1);
    CHECK(readiness(page) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
    CHECK(responses.statuses.last() == PrStatus_Lost);
}

TEST_CASE("document.open() on an iframe's initial about:blank stops the load of its source") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<iframe id=frame src=frame.html></iframe>", "https://example.test/", nullptr) ==
        PrStatus_Ok);
    later.settle(page);
    REQUIRE(later.waitingFor("frame.html") == 1);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrHtmlIFrameElement* frame = prHtmlIFrameElementQuery(prDomNodeAsEventTarget(
        prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), "frame"))));
    REQUIRE(frame != nullptr);
    PrDomDocument* blank = prHtmlWindowDocument(prHtmlIFrameElementContentWindow(frame));
    CHECK(prHtmlDocumentOpen(blank) == PrStatus_Ok);
    CHECK(prHtmlDocumentWrite(blank, "<p id=opened>") == PrStatus_Ok);
    CHECK(prHtmlDocumentClose(blank) == PrStatus_Ok);
    // The source arrives, and does not replace the document that was opened.
    later.answer("frame.html", "<p id=loaded>");
    later.settle(page);
    PrDomDocument* shown = prHtmlWindowDocument(prHtmlIFrameElementContentWindow(frame));
    CHECK(shown == blank);
    CHECK(prDomNodeGetElementById(prDomDocumentAsNode(shown), "opened") != nullptr);
    CHECK(prDomNodeGetElementById(prDomDocumentAsNode(shown), "loaded") == nullptr);
    prHtmlPageDestroy(page);
}

TEST_CASE("a script's fetch that is cancelled stops, and is never answered") {
    LaterHost later;
    later.fetcher.answer = [](PrFetchInterceptRequest*) {};
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<p>", "https://example.test/page", nullptr) == PrStatus_Ok);
    later.settle(page);
    uint32_t waited = 0;
    PrFetchResponseFn count = [](void* PR_NULLABLE data, const PrFetchResponse*) {
        ++*static_cast<uint32_t*>(data);
    };
    // One waits for the network, and the answer of the other, a data: URL, waits for its task.
    PrFetchClientRequest waits{.url = "wait", .mode = PrFetchMode_Cors};
    CHECK(prHtmlPageFetch(page, &waits, count, &waited) == PrStatus_Ok);
    later.settle(page);
    REQUIRE(later.waitingFor("/wait") == 1);
    uint32_t inlined = 0;
    PrFetchClientRequest data{.url = "data:,inline", .mode = PrFetchMode_Cors};
    CHECK(prHtmlPageFetch(page, &data, count, &inlined) == PrStatus_Ok);
    prHtmlPageCancelFetch(page, &waited);
    prHtmlPageCancelFetch(page, &inlined);
    later.settle(page);
    CHECK_FALSE(prHtmlPageIsFetching(page));
    // An answer that the network gives after all is not passed on.
    later.answer("/wait", "late");
    later.settle(page);
    prHtmlPageDestroy(page);
    CHECK(waited == 0);
    CHECK(inlined == 0);
}

TEST_CASE("a navigation stops a document that is still loading, unless its URL does not parse") {
    LaterHost later;
    PrHtmlHost host = later.host();
    host.execute = nullptr;
    host.parseModule = nullptr;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    PrHtmlResponseHeaders headers{.contentType = "text/html;charset=utf-8"};
    REQUIRE(prHtmlPageBeginLoad(page, "https://example.test/long.html", &headers) == PrStatus_Ok);
    prHtmlPageAppendLoad(page, "<!doctype html><p id=first>");
    later.settle(page);
    CHECK(has(page, "first"));

    CHECK(prHtmlPageNavigate(page, "not a url") != PrStatus_Ok);
    prHtmlPageAppendLoad(page, "<p id=second>");
    later.settle(page);
    CHECK(has(page, "second"));

    REQUIRE(prHtmlPageNavigate(page, "https://example.test/next.html") == PrStatus_Ok);
    later.settle(page);
    later.answer("/next.html", "<!doctype html><p id=next>", 200, "text/html");
    later.settle(page);
    CHECK(has(page, "next"));
    CHECK_FALSE(has(page, "first"));
    prHtmlPageDestroy(page);
}

TEST_CASE("a navigation fetches its document, keeps the current one until the response, and loads it at the URL") {
    LaterHost later;
    PrStatus loaded = PrStatus_InvalidState;
    PrHtmlHost host = later.host();
    host.context = &loaded;
    host.execute = nullptr;
    host.parseModule = nullptr;
    host.loaded = [](void* data, PrHtmlPage*, PrStatus status) { *static_cast<PrStatus*>(data) = status; };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    auto url = [&] {
        PrRawString text = prDomDocumentUrl(prHtmlWindowDocument(prHtmlPageWindow(page)));
        String out(prRawStringView(&text));
        prRawStringReset(&text);
        return out;
    };

    CHECK(prHtmlPageNavigate(page, "not a url") != PrStatus_Ok);
    REQUIRE(prHtmlPageNavigate(page, "https://example.test/page.html#middle") == PrStatus_Ok);
    CHECK(prHtmlPageIsFetching(page));
    CHECK(prHtmlPageIsNavigating(page));
    later.settle(page);
    CHECK(later.waitingFor("/page.html") == 1);
    CHECK(url() == "about:blank");

    later.answer("/page.html", "<!doctype html><p id=arrived>", 200, "text/html");
    later.settle(page);
    CHECK_FALSE(prHtmlPageIsNavigating(page));
    CHECK(has(page, "arrived"));
    CHECK(loaded == PrStatus_Ok);
    CHECK_FALSE(prHtmlPageIsFetching(page));
    CHECK(url() == "https://example.test/page.html#middle");
    prHtmlPageDestroy(page);
}

TEST_CASE("a navigation that fails loads a document that says why, and reports the failure") {
    LaterHost later;
    PrStatus loaded = PrStatus_Ok;
    PrHtmlHost host = later.host();
    host.context = &loaded;
    host.execute = nullptr;
    host.parseModule = nullptr;
    host.loaded = [](void* data, PrHtmlPage*, PrStatus status) { *static_cast<PrStatus*>(data) = status; };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageNavigate(page, "https://example.test/gone.html") == PrStatus_Ok);
    later.settle(page);
    REQUIRE(later.fetcher.held.count() == 1);
    later.fetcher.failTo(later.fetcher.held[0]);
    later.settle(page);
    CHECK(loaded == PrStatus_Io);
    CHECK_FALSE(prHtmlPageIsFetching(page));
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrRawString text = prDomNodeTextContent(prDomElementAsNode(prDomDocumentElement(document)));
    CHECK(prStringContains(prRawStringView(&text), "gone.html"));
    prRawStringReset(&text);
    prHtmlPageDestroy(page);
}

TEST_CASE("a navigation replaces one in flight, and a page destroyed during one drops it") {
    LaterHost later;
    PrHtmlHost host = later.host();
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageNavigate(page, "https://example.test/first.html") == PrStatus_Ok);
    later.settle(page);
    REQUIRE(prHtmlPageNavigate(page, "https://example.test/second.html") == PrStatus_Ok);
    later.settle(page);
    // The first navigation's response arrives after the second replaced it, and is dropped.
    later.answer("/first.html", "<!doctype html><p id=first>", 200, "text/html");
    later.settle(page);
    CHECK_FALSE(has(page, "first"));
    later.answer("/second.html", "<!doctype html><p id=second>", 200, "text/html");
    later.settle(page);
    CHECK(has(page, "second"));
    CHECK_FALSE(has(page, "first"));

    REQUIRE(prHtmlPageNavigate(page, "https://example.test/third.html") == PrStatus_Ok);
    later.settle(page);
    CHECK(later.waitingFor("/third.html") == 1);
    prHtmlPageDestroy(page);
    later.answer("/third.html", "<!doctype html><p id=third>", 200, "text/html");
    CHECK(later.fetcher.waiting() == 0);
}

TEST_CASE("a document's icon is its last icon link, resolved against its base URL") {
    CHECK(iconUrl(nullptr, "<link rel=icon href=a.png><link rel='shortcut ICON' href=/b.ico>",
        "https://example.test/dir/page") == "https://example.test/b.ico");
    CHECK(iconUrl(nullptr, "<base href=https://cdn.test/x/><link rel=icon href=icon.png>",
        "https://example.test/page") == "https://cdn.test/x/icon.png");
    CHECK(iconUrl(nullptr, "<link rel=icon href=a.png><link rel=apple-touch-icon href=b.png><link rel=icon href=''>",
        "https://example.test/") == "https://example.test/a.png");
    CHECK(iconUrl(nullptr, "<body><link rel=icon href=late.png>", "file:///dir/page.html") == "file:///dir/late.png");
}

TEST_CASE("an icon link whose type the host cannot decode names no icon") {
    PrCompressCodec* compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    PrImageCodec* images = prImageCodecCreate(PrImageCodecBackend_Default, compress);
    REQUIRE(images != nullptr);
    PrStringView source = "<link rel=icon type=image/png href=a.png><link rel=icon type='Image/SVG+XML' href=b.svg>";
    CHECK(iconUrl(images, source, "https://example.test/") == "https://example.test/a.png");
    CHECK(iconUrl(nullptr, source, "https://example.test/") == "https://example.test/b.svg");
    CHECK(iconUrl(images, "<link rel=icon type='image/x-icon; charset=x' href=c.ico>", "https://example.test/") ==
        "https://example.test/c.ico");
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("a document without an icon link has the favicon of its HTTP origin, and none elsewhere") {
    CHECK(iconUrl(nullptr, "<p>no icon", "https://example.test:8443/a/b?c") == "https://example.test:8443/favicon.ico");
    CHECK(iconUrl(nullptr, "<p>no icon", "http://example.test/") == "http://example.test/favicon.ico");
    CHECK(iconUrl(nullptr, "<p>no icon", "file:///dir/page.html") == "(none)");
    CHECK(iconUrl(nullptr, "<p>no icon", "about:blank") == "(none)");
}
