// An SVG document on its own: the XML parser creates it, and its elements are SVG elements without the members HTML
// adds.

#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/draw_commands.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"

#include "doctest/doctest.h"

#include <cmath>
#include <string>

using namespace pr_impl;

TEST_CASE("an SVG document's elements are SVG elements, each owned by the svg element nearest it") {
    PrXmlError error = {};
    PrDomDocument* document = prDomParseXml("<svg xmlns='http://www.w3.org/2000/svg'><g><svg><rect/></svg></g></svg>",
        "image/svg+xml", &error);
    CHECK(error.message.data == nullptr);
    PrSvgElement* root = prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document)));
    REQUIRE(root != nullptr);
    CHECK(prSvgElementOwnerSvgElement(root) == nullptr);

    PrDomElement* rect = nullptr;
    REQUIRE(prCssNodeQuerySelector(prDomDocumentAsNode(document), "rect", &rect) == PrStatus_Ok);
    PrSvgElement* inner = prSvgElementOwnerSvgElement(prSvgElementQuery(prDomElementAsEventTarget(rect)));
    REQUIRE(inner != nullptr);
    PrSvgElement* outer = prSvgElementOwnerSvgElement(inner);
    CHECK(outer == root);

    PrStringView type = prDomDocumentContentType(document);
    CHECK(type == "image/svg+xml");
    prDomDocumentRelease(document);
}

TEST_CASE("an element in another namespace is not an SVG element") {
    PrDomDocument* document = prDomParseXml("<math xmlns='http://www.w3.org/1998/Math/MathML'/>", "application/xml", nullptr);
    CHECK(prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document))) == nullptr);
    prDomDocumentRelease(document);
}

TEST_CASE("a standalone SVG document has its style sheets, selectors and inline styles") {
    PrDomDocument* document = prDomParseXml("<svg xmlns='http://www.w3.org/2000/svg'><style>rect { fill: green }</style>"
        "<rect id='r' style='stroke: blue'/></svg>", "image/svg+xml", nullptr);
    prCssEnable(document);
    prSvgEnable(document);
    PrCssStyleSheetList* sheets = prCssDocumentStyleSheets(document);
    REQUIRE(prCssStyleSheetListLength(sheets) == 1);
    PrCssRuleList* rules = prCssStyleSheetCssRules(prCssStyleSheetListStyleSheetAt(sheets, 0));
    REQUIRE(prCssRuleListLength(rules) == 1);
    PrRawString text = prCssRuleCssText(prCssRuleListRuleAt(rules, 0));
    CHECK(prRawStringView(&text) == "rect { fill: green; }");
    prRawStringReset(&text);

    PrDomElement* rect = nullptr;
    REQUIRE(prCssNodeQuerySelector(prDomDocumentAsNode(document), "svg > rect#r", &rect) == PrStatus_Ok);
    REQUIRE(rect != nullptr);
    PrCssStyleDeclaration* style = prCssStylePropertiesAsCssStyleDeclaration(prCssElementStyle(rect));
    const PrCssDeclarationName strokeName = prCssDeclarationNameFromProperty(PrCssProperty_Stroke);
    PrRawString stroke = prCssStyleDeclarationGetPropertyValue(style, &strokeName);
    CHECK(prRawStringView(&stroke) == "blue");
    prRawStringReset(&stroke);
    prDomDocumentRelease(document);
}

TEST_CASE("use elements that multiply each other's content draw a bounded number of elements") {
    // Each level uses the one below it ten times, so the last level is a million rectangles.
    std::string source = "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'><defs>"
        "<rect id='l0' width='1' height='1'/>";
    for (int level = 1; level <= 6; ++level) {
        source += "<g id='l" + std::to_string(level) + "'>";
        for (int i = 0; i < 10; ++i)
            source += "<use href='#l" + std::to_string(level - 1) + "'/>";
        source += "</g>";
    }
    source += "</defs><use href='#l6'/></svg>";
    PrDomDocument* document = prDomParseXml(source, "image/svg+xml", nullptr);
    PrSvgElement* root = prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document)));
    PrDrawList* list = prDrawListCreate();
    prSvgElementDraw(root, PrVec2{100, 100}, nullptr, list);
    uint32_t rects = 0;
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i)
        rects += prDrawListCommands(list)[i].kind != PrDrawCommandKind_PushTransformAffine2 &&
            prDrawListCommands(list)[i].kind != PrDrawCommandKind_PopTransform;
    CHECK(rects > 0);
    CHECK(rects <= 100000);
    prDrawListRelease(list);
    prDomDocumentRelease(document);
}

TEST_CASE("a fill resolves for the display's headroom, beyond the sRGB gamut") {
    PrDomDocument* document = prDomParseXml("<svg xmlns='http://www.w3.org/2000/svg'><rect width='10' height='10' "
        "fill='color-hdr(color(rec2100-linear 1 1 1) 0, color(rec2100-linear 4 4 4) 2)'/></svg>", "image/svg+xml", nullptr);
    prSvgEnable(document);
    PrSvgElement* root = prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document)));
    // Returns the green channel of the first painting command, drawn for `headroom` stops.
    auto green = [&](float headroom) {
        PrSvgDrawOptions options = prSvgDrawOptionsDefault();
        options.headroom = headroom;
        PrDrawList* list = prDrawListCreate();
        prSvgElementDraw(root, PrVec2{10, 10}, &options, list);
        float g = -1;
        for (uint32_t i = 0; i < prDrawListCommandCount(list) && g < 0; ++i) {
            const PrDrawCommand& command = prDrawListCommands(list)[i];
            if (command.kind == PrDrawCommandKind_Path || command.kind == PrDrawCommandKind_Rect)
                g = prColorFromColor16(command.color).g;
        }
        prDrawListRelease(list);
        return g;
    };
    CHECK(green(0) == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(green(2) == doctest::Approx(1.055f * std::pow(4.0f, 1 / 2.4f) - 0.055f).epsilon(0.01));
    prDomDocumentRelease(document);
}
