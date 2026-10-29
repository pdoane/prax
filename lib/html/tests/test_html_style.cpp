#include "pr/html_page.h"

#include "doctest/doctest.h"

TEST_CASE("a style element's sheet is in the document's list before the next script runs") {
    struct State {
        PrCssStyleSheetList* list = nullptr;
        PrCssStyleSheet* first = nullptr;
        uint32_t calls = 0;
    } state;
    PrHtmlHost scripting = prHtmlHostDefault();
    scripting.context = &state;
    scripting.execute = [](void* data, PrHtmlPage* page,
                           const PrHtmlScript*) -> PrStatus {
        State* state = static_cast<State*>(data);
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
        CHECK(prCssDocumentStyleSheets(document) == state->list);
        CHECK(prCssStyleSheetListLength(state->list) == state->calls);
        CHECK(prCssStyleSheetListStyleSheetAt(state->list, state->calls) == nullptr);
        if (state->calls == 1) {
            state->first = prCssStyleSheetListStyleSheetAt(state->list, 0);
            CHECK(prCssRuleListLength(prCssStyleSheetCssRules(state->first)) == 1);
            CHECK(prCssStyleSheetReplaceSync(state->first, "") == PrStatus_NotAllowed);
            CHECK(prCssStyleSheetInsertRule(state->first, "b { color: red; }", 1, nullptr) == PrStatus_Ok);
            prCssStyleSheetSetDisabled(state->first, true);
        } else if (state->calls == 2) {
            CHECK(prCssStyleSheetListStyleSheetAt(state->list, 0) == state->first);
            CHECK(prCssRuleListLength(prCssStyleSheetCssRules(state->first)) == 2);
        }
        ++state->calls;
        return PrStatus_Ok;
    };
    PrHtmlPage* page = prHtmlPageCreate(&scripting);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    state.list = prCssDocumentStyleSheets(prHtmlWindowDocument(window));
    CHECK(prHtmlPageLoad(page,
                                    "<script>0</script><style>a { color: blue; }</style>"
                                          "<script>0</script><style></style><script>0</script>",
                                    "about:blank", nullptr)
          == PrStatus_Ok);
    CHECK(state.calls == 3);
    CHECK(prHtmlPageLoad(page, "<p>replacement", "about:blank",
                         nullptr)
          == PrStatus_Ok);
    CHECK(prCssStyleSheetListLength(prCssDocumentStyleSheets(prHtmlWindowDocument(window))) == 0);
    prHtmlPageDestroy(page);
}

TEST_CASE("an SVG style element in an HTML document makes a sheet, with its end tag or without one") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page,
                                    "<svg><style>rect { fill: blue; }</style><rect/></svg>"
                                    "<svg><style>circle { fill: red; } circle { r: 1px; }<p>",
                                    "about:blank", nullptr)
          == PrStatus_Ok);
    PrCssStyleSheetList* list = prCssDocumentStyleSheets(prHtmlWindowDocument(prHtmlPageWindow(page)));
    REQUIRE(prCssStyleSheetListLength(list) == 2);
    CHECK(prCssRuleListLength(prCssStyleSheetCssRules(prCssStyleSheetListStyleSheetAt(list, 0))) == 1);
    CHECK(prCssRuleListLength(prCssStyleSheetCssRules(prCssStyleSheetListStyleSheetAt(list, 1))) == 2);
    prHtmlPageDestroy(page);
}

TEST_CASE("a style element outside a template makes a sheet when its type is empty or text/css") {
    PrStringView source = "<style type='text/plain'>a {}</style>"
        "<style type='text/css; charset=utf-8'>b {}</style>"
        "<style type=' TEXT/CSS '>c {}</style>"
        "<template><style>d {}</style></template>"
        "<link rel='stylesheet' href='external.css'>"
        "<style type='TEXT/CSS'>e {}</style>"
        "<style type=''>f {}</style>"
        "<style>g {}";
    auto checkSheets = [](PrDomDocument* document) {
        PrCssStyleSheetList* list = prCssDocumentStyleSheets(document);
        CHECK(prCssStyleSheetListLength(list) == 3);
        for (uint32_t i = 0; i < 3; ++i) {
            PrCssStyleSheet* sheet = prCssStyleSheetListStyleSheetAt(list, i);
            REQUIRE(sheet != nullptr);
            CHECK(prCssRuleListLength(prCssStyleSheetCssRules(sheet)) == 1);
        }
        CHECK(prCssStyleSheetListStyleSheetAt(list, 3) == nullptr);
    };
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, source, "about:blank", nullptr) == PrStatus_Ok);
    checkSheets(prHtmlWindowDocument(prHtmlPageWindow(page)));
    prHtmlPageDestroy(page);

    PrDomDocument* inert =
        prHtmlDomParserParseFromString(source, PrHtmlDomParserSupportedType_TextHtml);
    checkSheets(inert);
    prDomDocumentRelease(inert);
}
