#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/string_ops.h"
#include "pr/url.h"

#include "css/src/css_hints.h"
#include "css/src/css_inline_style.h"
#include "css/src/css_object.h"
#include "css/src/css_root.h"
#include "css/src/css_style.h"
#include "css/tests/css_tests.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_xml.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

PrDomElement* styleElement(PrDomDocument* document, PrStringView text) {
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Style, "style");
    domAttach(element, domTextCreate(document, text), nullptr);
    return element;
}

// Two connected style elements with `text`, whose sheets share their data.
struct SharedSheets {
    PrDomDocument* document;
    PrCssStyleSheet* one;
    PrCssStyleSheet* two;

    explicit SharedSheets(PrStringView text) {
        document = domDocumentCreate(DomDocumentKind_Html);
        prCssEnable(document);
        PrDomElement* first = styleElement(document, text);
        PrDomElement* second = styleElement(document, text);
        domInsert(document, first, nullptr);
        domInsert(document, second, nullptr);
        one = cssElementSheet(first);
        two = cssElementSheet(second);
        REQUIRE(one->data == two->data);
    }
    ~SharedSheets() { domNodeRelease(document); }

    SharedSheets(const SharedSheets&) = delete;
    SharedSheets& operator=(const SharedSheets&) = delete;
};

void setColor(PrCssRule* rule, PrStringView value) {
    PrCssDeclarationName color = prCssDeclarationNameFromProperty(PrCssProperty_Color);
    prCssStyleDeclarationSetProperty((&rule->style), &color, value, false);
}

std::string ruleText(const CssRuleData* rule) {
    PrTextBuffer text = {};
    cssSerializeRule(&text, rule);
    std::string out = textOf(prTextBufferView(&text));
    prTextBufferReset(&text);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an element's style data is made on first use and holds its inline style apart from its sheet") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    prCssEnable(document);
    PrDomElement* element = styleElement(document, "a {}");
    CHECK(element->style == nullptr);
    CHECK(cssElementSheet(element) == nullptr);
    CssElementStyle* style = cssElementStyle(element);
    cssDeclarationBlockParse(cssStyleDeclarationWritable(cssElementInlineStyle(element)), "color: red", nullptr,
        CssOrigin_Author);
    domInsert(document, element, nullptr);
    PrCssStyleSheet* sheet = cssElementSheet(element);
    REQUIRE(sheet != nullptr);
    CHECK(element->style == style);
    CHECK(style->inlineStyle->block.count == 1);
    CHECK(sheet->ownerNode == element);
    CHECK(cssDocumentStyleSheets(document)->sheets.data[0] == sheet);
    domNodeRelease(document);
}

TEST_CASE("a document lists its style sheets in the tree order of their owners") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    prCssEnable(document);
    PrDomElement* branch = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domInsert(document, branch, nullptr);
    PrDomElement* first = styleElement(document, "");
    PrDomElement* second = styleElement(document, "");
    PrDomElement* third = styleElement(document, "");
    PrCssStyleSheetList* list = cssDocumentStyleSheets(document);
    domInsert(document, third, nullptr);
    domInsert(branch, second, nullptr);
    domInsert(branch, first, second);
    CHECK(cssDocumentStyleSheets(document) == list);
    REQUIRE(list->sheets.count == 3);
    CHECK(list->sheets.data[0] == cssElementSheet(first));
    CHECK(list->sheets.data[1] == cssElementSheet(second));
    CHECK(list->sheets.data[2] == cssElementSheet(third));
    CHECK(branch->style == nullptr);
    domNodeRelease(document);
}

TEST_CASE("a style element's sheet follows its connection and its text") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    prCssEnable(document);
    PrDomElement* element = styleElement(document, "a {}");
    CHECK(cssElementSheet(element) == nullptr);

    domInsert(document, element, nullptr);
    PrCssStyleSheet* sheet = cssElementSheet(element);
    REQUIRE(sheet != nullptr);
    CHECK(sheet->data->rules.count == 1);

    // A change to the text creates a new sheet. The old one leaves the list, and stays alive while referenced.
    cssStyleSheetRetain(sheet);
    domNodeRetain(element);
    domStringReplaceAll(element, "a {} b {}");
    PrCssStyleSheet* replaced = cssElementSheet(element);
    REQUIRE(replaced != nullptr);
    CHECK(replaced != sheet);
    CHECK(replaced->data->rules.count == 2);
    CHECK(sheet->ownerNode == nullptr);
    CHECK(cssDocumentStyleSheets(document)->sheets.count == 1);
    cssStyleSheetRelease(sheet);

    cssStyleSheetRetain(replaced);
    domRemove(element);
    CHECK(cssElementSheet(element) == nullptr);
    CHECK(replaced->ownerNode == nullptr);
    CHECK(replaced->data->rules.count == 2);
    CHECK(cssDocumentStyleSheets(document)->sheets.count == 0);
    cssStyleSheetRelease(replaced);
    domNodeRelease(element);
    domNodeRelease(document);
}

TEST_CASE("a style element whose type is not CSS has no sheet") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    prCssEnable(document);
    PrDomElement* element = styleElement(document, "a {}");
    domSetAttribute(element, PrDomNamespace_None, "type", "text/plain");
    domInsert(document, element, nullptr);
    CHECK(cssElementSheet(element) == nullptr);
    domNodeRelease(document);
}

TEST_CASE("an inline style is the style attribute's declarations, and writes the attribute back") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    prCssEnable(document);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domSetAttribute(element, PrDomNamespace_None, "style", "color: red");

    PrCssStyleDeclaration* style = cssElementInlineStyle(element);
    CHECK(cssElementInlineStyle(element) == style);
    REQUIRE(cssStyleDeclarationBlock(style)->count == 1);

    // A change through CSSOM writes the serialization to the attribute.
    PrCssStyleDeclaration* handle = style;
    PrCssDeclarationName width = prCssDeclarationNameFromProperty(PrCssProperty_Width);
    prCssStyleDeclarationSetProperty(handle, &width, "1px", false);
    const PrDomAttr* attribute = domAttribute(element, PrDomNamespace_None, "style");
    REQUIRE(attribute != nullptr);
    CHECK(prRawStringView(&attribute->value) == "color: red; width: 1px;");

    // Setting the attribute sets the declarations.
    domSetAttribute(element, PrDomNamespace_None, "style", "top: 0");
    REQUIRE(cssStyleDeclarationBlock(style)->count == 1);
    CHECK(cssDeclarations(cssStyleDeclarationBlock(style))[0].property == PrCssProperty_Top);

    // Removing nothing, setting a value that the block rejects, and setting the value that it already holds all leave
    // the attribute as written (cssom-1/#set-a-css-declaration).
    PrCssDeclarationName left = prCssDeclarationNameFromProperty(PrCssProperty_Left);
    prCssStyleDeclarationRemoveProperty(handle, &left, nullptr);
    CHECK(prRawStringView(&attribute->value) == "top: 0");
    PrCssDeclarationName top = prCssDeclarationNameFromProperty(PrCssProperty_Top);
    prCssStyleDeclarationSetProperty(handle, &top, "nonesuch", false);
    prCssStyleDeclarationSetProperty(handle, &top, "0", false);
    prCssStyleDeclarationSetProperty(handle, &left, "", false);
    CHECK(prRawStringView(&attribute->value) == "top: 0");
    domNodeRelease(document);
}

TEST_CASE("style elements of the same text share their sheet's data until one is changed") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    prCssEnable(document);
    PrDomElement* first = styleElement(document, "a { color: red } @media screen { b {} }");
    PrDomElement* second = styleElement(document, "a { color: red } @media screen { b {} }");
    domInsert(document, first, nullptr);
    domInsert(document, second, nullptr);
    PrCssStyleSheet* one = cssElementSheet(first);
    PrCssStyleSheet* two = cssElementSheet(second);
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    CHECK(one != two);
    CHECK(one->data == two->data);

    // A change that the data rejects, or one that would write what it already holds, leaves the data shared.
    PrCssRule* rule = cssStyleSheetRule(one, 0);
    PrCssStyleDeclaration* style = &rule->style;
    PrCssDeclarationName color = prCssDeclarationNameFromProperty(PrCssProperty_Color);
    PrCssDeclarationName width = prCssDeclarationNameFromProperty(PrCssProperty_Width);
    PrCssMediaList* media = prCssMediaRuleMedia(prCssMediaRuleQuery(cssStyleSheetRule(one, 1)));
    PrCssStyleSheet* sheet = one;
    prCssStyleDeclarationSetProperty(style, &color, "nonesuch", false);
    prCssStyleDeclarationSetProperty(style, &color, "red", false);
    prCssStyleDeclarationRemoveProperty(style, &width, nullptr);
    prCssStyleRuleSetSelectorText(prCssStyleRuleQuery(rule), "::nonesuch(");
    prCssMediaListAppendMedium(media, "screen");
    CHECK(prCssMediaListDeleteMedium(media, "print") == PrStatus_NotFound);
    CHECK(prCssStyleSheetInsertRule(sheet, "not a rule", 0, nullptr) == PrStatus_Syntax);
    CHECK(prCssStyleSheetDeleteRule(sheet, 5) == PrStatus_OutOfRange);
    CHECK(one->data == two->data);

    // A change that modifies the data copies it first, which leaves the other sheet's data unchanged.
    prCssStyleDeclarationSetProperty(style, &color, "blue", false);
    CHECK(one->data != two->data);
    CHECK(rule->data == one->data->rules.data[0]);
    PrTextBuffer text = {};
    cssSerializeRule(&text, two->data->rules.data[0]);
    CHECK(textOf(prTextBufferView(&text)) == "a { color: red; }");
    prTextBufferClear(&text);
    cssSerializeRule(&text, one->data->rules.data[0]);
    CHECK(textOf(prTextBufferView(&text)) == "a { color: blue; }");
    prTextBufferReset(&text);
    domNodeRelease(document);
}

TEST_CASE("a change through a shared sheet copies the rules on its way and shares the rest") {
    SharedSheets sheets("a { color: red } @media screen { b { color: red } c { color: red } } d { color: red }");
    PrCssRule* media = cssStyleSheetRule(sheets.one, 1);
    setColor(cssRuleChild(media, 0), "blue");

    const CssSheetData* one = sheets.one->data;
    const CssSheetData* two = sheets.two->data;
    CHECK(one != two);
    CHECK(one->rules.data[0] == two->rules.data[0]);
    CHECK(one->rules.data[2] == two->rules.data[2]);
    CHECK(one->rules.data[1] != two->rules.data[1]);
    CHECK(one->rules.data[1]->children.data[0] != two->rules.data[1]->children.data[0]);
    CHECK(one->rules.data[1]->children.data[1] == two->rules.data[1]->children.data[1]);
    CHECK(media->data == one->rules.data[1]);
    CHECK(ruleText(two->rules.data[1]) == "@media screen {\n  b { color: red; }\n  c { color: red; }\n}");

    // The data that the change made its own is written in place, and a rule beside it is copied alone.
    const CssRuleData* copied = one->rules.data[1];
    setColor(cssRuleChild(media, 1), "green");
    CHECK(sheets.one->data->rules.data[1] == copied);
    CHECK(copied->children.data[1] != two->rules.data[1]->children.data[1]);
    CHECK(ruleText(copied) == "@media screen {\n  b { color: blue; }\n  c { color: green; }\n}");
    CHECK(ruleText(two->rules.data[1]) == "@media screen {\n  b { color: red; }\n  c { color: red; }\n}");
}

TEST_CASE("a rule taken out of a shared sheet is copied as it is changed") {
    SharedSheets sheets("a { color: red }");
    PrCssRule* first = cssStyleSheetRule(sheets.one, 0);
    PrCssRule* second = cssStyleSheetRule(sheets.two, 0);
    prCssRuleRetain(first);
    prCssRuleRetain(second);
    CHECK(prCssStyleSheetDeleteRule(sheets.one, 0) == PrStatus_Ok);
    CHECK(prCssStyleSheetDeleteRule(sheets.two, 0) == PrStatus_Ok);
    CHECK(first->data == second->data);

    setColor(first, "blue");
    CHECK(ruleText(first->data) == "a { color: blue; }");
    CHECK(ruleText(second->data) == "a { color: red; }");
    prCssRuleRelease(first);
    prCssRuleRelease(second);
}

TEST_CASE("a style rule's new selectors change its nested rules' specificity in its own sheet alone") {
    SharedSheets sheets("a { & b { } }");
    prCssStyleRuleSetSelectorText(prCssStyleRuleQuery(cssStyleSheetRule(sheets.one, 0)), "#x");
    const CssRuleData* one = sheets.one->data->rules.data[0]->children.data[0];
    const CssRuleData* two = sheets.two->data->rules.data[0]->children.data[0];
    CHECK(one != two);
    CHECK(one->selectors->complex[0].specificity == cssSpecificity(1, 0, 1));
    CHECK(two->selectors->complex[0].specificity == cssSpecificity(0, 0, 2));
}

TEST_CASE("a flow-relative longhand maps to the physical one on its side, axis or corner in each writing mode") {
    constexpr PrCssKeyword kLtr = PrCssKeyword_Ltr;
    constexpr PrCssKeyword kRtl = PrCssKeyword_Rtl;
    CHECK(cssPhysicalProperty(PrCssProperty_PaddingInlineEnd, PrCssKeyword_HorizontalTb, kLtr) ==
        PrCssProperty_PaddingRight);
    CHECK(cssPhysicalProperty(PrCssProperty_PaddingInlineEnd, PrCssKeyword_HorizontalTb, kRtl) ==
        PrCssProperty_PaddingLeft);
    CHECK(cssPhysicalProperty(PrCssProperty_InsetBlockStart, PrCssKeyword_VerticalRl, kLtr) == PrCssProperty_Right);
    CHECK(cssPhysicalProperty(PrCssProperty_InsetBlockStart, PrCssKeyword_SidewaysLr, kLtr) == PrCssProperty_Left);
    CHECK(cssPhysicalProperty(PrCssProperty_InsetInlineStart, PrCssKeyword_SidewaysLr, kLtr) == PrCssProperty_Bottom);
    CHECK(cssPhysicalProperty(PrCssProperty_InsetInlineStart, PrCssKeyword_SidewaysRl, kRtl) == PrCssProperty_Bottom);
    CHECK(cssPhysicalProperty(PrCssProperty_BlockSize, PrCssKeyword_VerticalLr, kLtr) == PrCssProperty_Width);
    CHECK(cssPhysicalProperty(PrCssProperty_OverflowInline, PrCssKeyword_VerticalRl, kLtr) == PrCssProperty_OverflowY);
    // A corner is named by its block-axis side, then its inline-axis side.
    CHECK(cssPhysicalProperty(PrCssProperty_BorderStartStartRadius, PrCssKeyword_HorizontalTb, kRtl) ==
        PrCssProperty_BorderTopRightRadius);
    CHECK(cssPhysicalProperty(PrCssProperty_BorderStartEndRadius, PrCssKeyword_VerticalRl, kLtr) ==
        PrCssProperty_BorderBottomRightRadius);
    CHECK(cssPhysicalProperty(PrCssProperty_BorderEndStartRadius, PrCssKeyword_VerticalLr, kLtr) ==
        PrCssProperty_BorderTopRightRadius);
    // A physical property maps to itself.
    CHECK(!cssIsFlowRelative(PrCssProperty_MarginLeft));
    CHECK(cssPhysicalProperty(PrCssProperty_MarginLeft, PrCssKeyword_VerticalRl, kRtl) == PrCssProperty_MarginLeft);
}

TEST_CASE("an xml-stylesheet instruction in the prolog fetches its CSS sheet, which leaves with the instruction") {
    struct HeldFetch {
        std::string url;
        DomFetchFn done;
        void* data;
    };
    std::vector<HeldFetch> fetches;
    DomDocumentHost host{};
    host.context = &fetches;
    host.fetch = [](void* context, const DomFetchRequest* request, DomFetchFn done, void* data, void (*)(void*)) {
        static_cast<std::vector<HeldFetch>*>(context)->push_back(
            HeldFetch{std::string(request->url.data, request->url.size), done, data});
    };
    PrDomDocument* document = prDomDocumentCreate();
    prCssEnable(document);
    document->kind = DomDocumentKind_Xml;
    prUrlDestroy(document->url);
    document->url = nullptr;
    REQUIRE(prUrlCreate("https://example.test/dir/a.svg", nullptr, PrCharset_Utf8, &document->url) == PrStatus_Ok);
    document->host = &host;
    REQUIRE(domXmlParse(document, "<?xml-stylesheet href='blue.css' type='text/css' media='print' title='t'?>"
        "<?xml-stylesheet href='x.xsl' type='text/xsl'?><?xml-stylesheet href='alt.css' "
        "alternate='yes'?><svg xmlns='http://www.w3.org/2000/svg'/><?xml-stylesheet href='late.css'?>",
        {}, nullptr, nullptr) == PrStatus_Ok);
    REQUIRE(fetches.size() == 1);
    CHECK(fetches[0].url == "https://example.test/dir/blue.css");
    DomFetchResult result{.status = PrStatus_Ok, .responseStatus = 200, .body = "rect { fill: blue }"};
    fetches[0].done(fetches[0].data, &result);
    PrCssStyleSheetList* sheets = prCssDocumentStyleSheets(document);
    REQUIRE(prCssStyleSheetListLength(sheets) == 1);
    PrCssStyleSheet* sheet = prCssStyleSheetListStyleSheetAt(sheets, 0);
    CHECK(sheet->ownerNode == document->first);
    CHECK(prRawStringView(&sheet->title) == "t");
    CHECK(cssMediaListLength(&sheet->media) == 1);
    domRemove(document->first);
    CHECK(prCssStyleSheetListLength(sheets) == 0);
    document->host = nullptr;
    prDomDocumentRelease(document);
}
