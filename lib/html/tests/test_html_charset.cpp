// The character encodings of loaded documents.
//
// html - https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding

#include "pr/array.h"
#include "pr/html_page.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "html/src/html_charset.h"
#include "html/tests/support/html_test_fetch.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

void runTasks(PrHtmlPage* page) {
    while (prDomAgentRunTask(prHtmlPageAgent(page))) {
    }
}

// The text of the element with `id`, or "missing".
String textOf(PrHtmlPage* page, const char* id) {
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
    if (element == nullptr)
        return "missing";
    return String::adopt(prDomNodeTextContent(prDomElementAsNode(element)));
}

PrStringView charsetOf(PrHtmlPage* page) {
    return prDomDocumentCharacterSet(prHtmlWindowDocument(prHtmlPageWindow(page)));
}

// Returns true if `texts` holds `expected`, in order.
bool textsAre(const Array<String>& texts, Span<const char* const> expected) {
    if (texts.count() != expected.count())
        return false;
    for (uint32_t i = 0; i < expected.count(); ++i) {
        if (texts[i] != expected[i])
            return false;
    }
    return true;
}

// The padding that puts what follows it past the prescan's first kilobyte.
String padding() {
    String out("<!--");
    out.append(1100, 'x');
    out.append("-->");
    return out;
}

// A browsing context that has loaded `source` whole, with the Content-Type `type`, if any.
PrHtmlPage* loaded(PrStringView source, const char* type = nullptr) {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrHtmlResponseHeaders headers = {.contentType = type != nullptr ? PrStringView(type) : PrStringView{nullptr, 0}};
    CHECK(prHtmlPageLoad(page, source, "https://charset.test/", &headers) == PrStatus_Ok);
    runTasks(page);
    return page;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a loaded document has the charset that html5lib's encoding tests expect") {
    uint32_t cases = 0;
    const char* const names[] = {"tests1.dat", "tests2.dat", "test-yahoo-jp.dat"};
    for (const char* name : names) {
        String file = harness::readText(format("{}/encoding/{}", PR_HTML5LIB_DIR, name));
        PrStringView text = file;
        for (size_t at = prStringFind(text, "#data\n"); at != text.size; at = prStringFindFrom(text, "#data\n", at)) {
            at += 6;
            size_t encoding = prStringFindFrom(text, "\n#encoding\n", at);
            REQUIRE(encoding != text.size);
            PrStringView data = prStringSlice(text, at, encoding - at);
            size_t start = encoding + 11;
            PrStringView expected = prStringSlice(text, start, prStringFindFrom(text, "\n", start) - start);
            CAPTURE(data);
            // These cases expect a meta element in the text of a script or style element, or one that script writes, to
            // declare a charset, as the byte prescan of html/#prescan-a-byte-stream-to-determine-its-encoding reads
            // them. The engine follows whatwg/html#6962 and WPT's html/syntax/charset, where neither declares one.
            if (prStringContains(data, "<script>document.write('<meta") ||
                    prStringContains(data, "<script type=\"text/plain\"><meta") ||
                    prStringContains(data, "<style type=\"text/plain\"><meta"))
                continue;
            PrHtmlPage* page = loaded(data);
            PrStringView want = prCharsetName(prCharsetFromLabel(expected));
            CHECK(charsetOf(page) == want);
            prHtmlPageDestroy(page);
            ++cases;
        }
    }
    CHECK(cases > 50);
}

TEST_CASE("a meta element's content names a charset after the word charset") {
    CHECK(htmlCharsetFromMetaContent("text/html; charset=koi8-r") == PrCharset_Koi8R);
    CHECK(htmlCharsetFromMetaContent("text/html;CHARSET = 'utf-8' ") == PrCharset_Utf8);
    CHECK(htmlCharsetFromMetaContent("charsetx; charset=\"latin1\"") == PrCharset_Windows1252);
    CHECK(htmlCharsetFromMetaContent("charset=\"utf-8") == PrCharset_None);
    CHECK(htmlCharsetFromMetaContent("text/html") == PrCharset_None);
    CHECK(htmlCharsetFromMetaContent("charset=") == PrCharset_None);
}

TEST_CASE("a document without a declaration decodes in the default charset") {
    PrHtmlPage* page = loaded("<p id=p>caf\xE9</p>");
    CHECK(charsetOf(page) == "windows-1252");
    CHECK(textOf(page, "p") == "caf\xC3\xA9");
    prHtmlPageDestroy(page);

    page = prHtmlPageCreate(nullptr);
    prHtmlPageSetDefaultCharset(page, PrCharset_Utf8);
    CHECK(prHtmlPageLoad(page, "<p id=p>caf\xC3\xA9</p>", "https://charset.test/", nullptr) == PrStatus_Ok);
    CHECK(charsetOf(page) == "UTF-8");
    CHECK(textOf(page, "p") == "caf\xC3\xA9");
    prHtmlPageDestroy(page);
}

TEST_CASE("a byte order mark decides the charset and is not text") {
    PrHtmlPage* page = loaded("\xEF\xBB\xBF<meta charset=koi8-r><p id=p>\xC3\xA9</p>",
        "text/html; charset=windows-1252");
    CHECK(charsetOf(page) == "UTF-8");
    CHECK(textOf(page, "p") == "\xC3\xA9");
    PrDomElement* body = prDomDocumentBody(prHtmlWindowDocument(prHtmlPageWindow(page)));
    PrRawString text = prDomNodeTextContent(prDomElementAsNode(body));
    CHECK(prRawStringView(&text) == "\xC3\xA9");
    prRawStringReset(&text);
    prHtmlPageDestroy(page);

    PrStringView utf16("\xFF\xFE<\0p\0 \0i\0d\0=\0p\0>\0\x30\x04", 20);
    page = loaded(utf16);
    CHECK(charsetOf(page) == "UTF-16LE");
    CHECK(textOf(page, "p") == "\xD0\xB0");
    prHtmlPageDestroy(page);
}

TEST_CASE("the Content-Type's charset is certain, and a meta element does not change it") {
    PrHtmlPage* page = loaded("<meta charset=koi8-r><p id=p>\xE0</p>", "text/html; charset=\"windows-1251\"");
    CHECK(charsetOf(page) == "windows-1251");
    CHECK(textOf(page, "p") == "\xD0\xB0");
    prHtmlPageDestroy(page);
}

TEST_CASE("a meta element in the prescan decides the charset") {
    PrHtmlPage* page = loaded("<meta http-equiv=content-type content='text/html; charset=koi8-r'><p id=p>\xC1</p>");
    CHECK(charsetOf(page) == "KOI8-R");
    CHECK(textOf(page, "p") == "\xD0\xB0");
    prHtmlPageDestroy(page);

    // A declaration of UTF-16 in ASCII means UTF-8.
    page = loaded("<meta charset=utf-16><p id=p>\xC3\xA9</p>");
    CHECK(charsetOf(page) == "UTF-8");
    prHtmlPageDestroy(page);
}

TEST_CASE("a meta element in the head past the prescan parses the document again in its charset") {
    PrHtmlPage* page =
        loaded(format("<!doctype html><title id=t>\xE9</title>{}<meta charset=windows-1251><p id=q>\xE0</p>",
            padding()));
    CHECK(charsetOf(page) == "windows-1251");
    CHECK(textOf(page, "t") == "\xD0\xB9");
    CHECK(textOf(page, "q") == "\xD0\xB0");
    prHtmlPageDestroy(page);

    // Past the prescan, a meta element outside the head, in a template, or in text declares nothing.
    const char* const lates[] = {
        "</head><meta charset=windows-1251>",
        "<template><meta charset=windows-1251></template>", "<title><meta charset=windows-1251></title>",
        "<body><meta charset=windows-1251>",
    };
    for (const char* late : lates) {
        CAPTURE(late);
        page = loaded(format("<!doctype html><head>{}{}<p id=q>\xE0</p>", padding(), late));
        CHECK(charsetOf(page) == "windows-1252");
        prHtmlPageDestroy(page);
    }
}

TEST_CASE("the prescan reads the text of a title, a script or a style as text") {
    const char* const elements[] = {"title", "script", "style", "textarea"};
    for (const char* element : elements) {
        String source = format("<{}><meta charset=koi8-r></{}><p>", element, element);
        CAPTURE(source);
        PrCharset charset = PrCharset_Count;
        CHECK(htmlPrescan(source, true, &charset));
        CHECK(charset == PrCharset_None);
    }
    PrCharset charset = PrCharset_None;
    CHECK(htmlPrescan("<template><meta charset=koi8-r></template>", true, &charset));
    CHECK(charset == PrCharset_Koi8R);
    // A partial input that ends before the first kilobyte does waits for more of it.
    CHECK_FALSE(htmlPrescan("<title>t</title>", false, &charset));
    CHECK(htmlPrescan("<title>t</title>", true, &charset));
    CHECK(charset == PrCharset_None);
}

TEST_CASE("a meta element that script writes declares nothing") {
    PrHtmlHost host = prHtmlHostDefault();
    host.execute = [](void*, PrHtmlPage* page, const PrHtmlScript*) -> PrStatus {
        return prHtmlDocumentWrite(prHtmlWindowDocument(prHtmlPageWindow(page)), "<meta charset=windows-1251>");
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    String source = format("<!doctype html><head>{}<script>w</script></head><p id=q>\xE0</p>", padding());
    CHECK(prHtmlPageLoad(page, source, "https://charset.test/", nullptr) == PrStatus_Ok);
    CHECK(charsetOf(page) == "windows-1252");
    prHtmlPageDestroy(page);
}

TEST_CASE("a streamed meta element changes the decoder for the bytes that follow") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    REQUIRE(prHtmlPageBeginLoad(page, "https://charset.test/", nullptr) == PrStatus_Ok);
    String head = format("<!doctype html><title>ascii so far</title>{}", padding());
    prHtmlPageAppendLoad(page, head);
    runTasks(page);
    CHECK(charsetOf(page) == "windows-1252");
    prHtmlPageAppendLoad(page, "<meta charset=windows-1251><p id=p>\xE0");
    // A sequence split between pieces decodes as one.
    prHtmlPageAppendLoad(page, "</p>");
    prHtmlPageEndLoad(page);
    runTasks(page);
    CHECK(charsetOf(page) == "windows-1251");
    CHECK(textOf(page, "p") == "\xD0\xB0");
    prHtmlPageDestroy(page);

    page = prHtmlPageCreate(nullptr);
    PrHtmlResponseHeaders headers = {.contentType = "text/html;charset=utf-8"};
    REQUIRE(prHtmlPageBeginLoad(page, "https://charset.test/", &headers) == PrStatus_Ok);
    prHtmlPageAppendLoad(page, "<p id=p>\xE2\x82");
    runTasks(page);
    prHtmlPageAppendLoad(page, "\xAC</p>");
    prHtmlPageEndLoad(page);
    runTasks(page);
    CHECK(textOf(page, "p") == "\xE2\x82\xAC");
    prHtmlPageDestroy(page);
}

TEST_CASE("an XML document decodes in the charset of its declaration") {
    PrHtmlPage* page = loaded("<?xml version='1.0' encoding='windows-1251'?><r id='p'>\xE0</r>", "application/xml");
    CHECK(charsetOf(page) == "windows-1251");
    CHECK(textOf(page, "p") == "\xD0\xB0");
    prHtmlPageDestroy(page);

    PrStringView utf16("<\0?\0x\0m\0l\0 \0v\0e\0r\0s\0i\0o\0n\0=\0'\0\x31\0.\0\x30\0'\0?\0>\0<\0r\0/\0>\0", 46);
    page = loaded(utf16, "application/xml");
    CHECK(charsetOf(page) == "UTF-16LE");
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    CHECK(prDomDocumentElement(document) != nullptr);
    prHtmlPageDestroy(page);

    page = loaded("<r id='p'>\xC3\xA9</r>", "application/xml");
    CHECK(charsetOf(page) == "UTF-8");
    CHECK(textOf(page, "p") == "\xC3\xA9");
    prHtmlPageDestroy(page);
}

TEST_CASE("an external classic script decodes in its document's charset unless it names another") {
    struct State {
        TestModules modules;
        Array<String> sources;
    } state;
    TestFetcher fetcher;
    fetcher.answer = [&](PrFetchInterceptRequest* request) {
        String url = TestFetcher::urlOf(request);
        if (prStringEndsWith(url, "response.js"))
            fetcher.respondTo(request, "\xC1", 200, "text/javascript; charset=koi8-r");
        else if (prStringEndsWith(url, "mark.js"))
            fetcher.respondTo(request, "\xEF\xBB\xBF\xC3\xA9");
        else if (prStringEndsWith(url, "module.js"))
            fetcher.respondTo(request, "\xE9", 200, "text/javascript");
        else
            fetcher.respondTo(request, "\xE9");
    };
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &state;
    host.fetcher = fetcher.fetcher;
    host.execute = [](void* data, PrHtmlPage*, const PrHtmlScript* script) -> PrStatus {
        State* state = static_cast<State*>(data);
        if (script->kind == PrHtmlScriptKind_Module)
            state->sources.push(state->modules.sources[script->module]);
        else
            state->sources.emplace(script->source);
        return PrStatus_Ok;
    };
    host.parseModule = [](void* data, PrHtmlPage*, const PrHtmlScript* script,
            PrHtmlModuleRequestFn request, void* PR_NULLABLE requestContext) {
        return static_cast<State*>(data)->modules.parse(script, request, requestContext);
    };
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<meta charset=windows-1252><script src=document.js></script>"
        "<script src=element.js charset=windows-1251></script><script src=response.js charset=windows-1251></script>"
        "<script src=mark.js></script><script type=module src=module.js></script>", "https://charset.test/", nullptr) ==
        PrStatus_Ok);
    fetcher.settle(prHtmlPageAgent(page));
    const char* const sources[] = {"\xC3\xA9", "\xD0\xB9", "\xD0\xB0", "\xC3\xA9", "\xEF\xBF\xBD"};
    CHECK(textsAre(state.sources, sources));
    prHtmlPageDestroy(page);
}
