// SVG's own grammars, and the styles that the cascade computes from presentation attributes.

#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/vec.h"

#include "dom/src/dom_node.h"
#include "svg/src/svg_cascade.h"
#include "svg/src/svg_parse.h"
#include "svg/src/svg_style.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

bool near(PrVec2 a, PrVec2 b) {
    return a.x - b.x < 1e-3f && b.x - a.x < 1e-3f && a.y - b.y < 1e-3f && b.y - a.y < 1e-3f;
}

// A standalone SVG document and the cascade that styles it.
struct Styled {
    PrDomDocument* document;
    SvgCascade cascade{};

    explicit Styled(const char* source) {
        document = prDomParseXml(source, "image/svg+xml", nullptr);
        prCssEnable(document);
        prSvgEnable(document);
        svgCascadeInit(&cascade, document, prSvgDrawOptionsDefault(), PrVec2{100, 100});
    }

    ~Styled() {
        svgCascadeReset(&cascade);
        prDomDocumentRelease(document);
    }

    // Returns the style of the element with the ID `id`.
    const CssComputedStyle* style(const char* id) {
        PrDomElement* element = domElementById(document, id);
        REQUIRE(element != nullptr);
        return svgCascadeStyle(&cascade, element);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a viewBox maps into the viewport as preserveAspectRatio says") {
    PrRect box{};
    REQUIRE(svgParseViewBox("0 0 100 50", &box));
    CHECK_FALSE(svgParseViewBox("0 0 -1 5", &box));
    SvgAspect aspect;
    REQUIRE(svgParseAspect("xMinYMax slice", &aspect));
    CHECK(aspect.slice);
    PrRect viewBox{0, 0, 100, 50};
    SvgAspect meet;
    PrAffine2 t = svgViewBoxTransform(viewBox, meet, 200, 200);
    CHECK(near(prAffine2MapPoint(t, PrVec2{0, 0}), PrVec2{0, 50}));
    SvgAspect none;
    REQUIRE(svgParseAspect("none", &none));
    t = svgViewBoxTransform(viewBox, none, 200, 200);
    CHECK(near(prAffine2MapPoint(t, PrVec2{100, 50}), PrVec2{200, 200}));
}

TEST_CASE("a transform list in SVG's syntax parses to the matrix that applies its functions from left to right") {
    PrAffine2 t{};
    REQUIRE(svgParseTransform("translate(10,20) scale(2)", &t));
    CHECK(near(prAffine2MapPoint(t, PrVec2{1, 1}), PrVec2{12, 22}));
    REQUIRE(svgParseTransform(" rotate(90 10 10)", &t));
    CHECK(near(prAffine2MapPoint(t, PrVec2{20, 10}), PrVec2{10, 20}));
    REQUIRE(svgParseTransform("matrix(1 0 0 1 5 6), skewX(0)", &t));
    CHECK(near(prAffine2MapPoint(t, PrVec2{0, 0}), PrVec2{5, 6}));
    CHECK_FALSE(svgParseTransform("scale(2) bogus(1)", &t));
    CHECK_FALSE(svgParseTransform("rotate(1, 2)", &t));
    CHECK_FALSE(svgParseTransform("", &t));
}

TEST_CASE("a transform attribute in SVG's syntax is the transform property, and one in CSS's is no transform") {
    Styled doc("<svg xmlns='http://www.w3.org/2000/svg'><rect id='list' transform='translate(10,20) scale(2)'/>"
        "<rect id='center' transform='rotate(90 10 10)'/><rect id='css' transform='translate(5px) rotate(90deg)'/>"
        "<rect id='bad' transform='scale(2) bogus(1)'/></svg>");
    PrAffine2 list = svgTransformMatrix(svgStyleTransform(doc.style("list")), PrRect{});
    CHECK(near(prAffine2MapPoint(list, PrVec2{1, 1}), PrVec2{12, 22}));
    PrAffine2 center = svgTransformMatrix(svgStyleTransform(doc.style("center")), PrRect{});
    CHECK(near(prAffine2MapPoint(center, PrVec2{20, 10}), PrVec2{10, 20}));
    CHECK_FALSE(svgStyleTransform(doc.style("css")).any);
    CHECK_FALSE(svgStyleTransform(doc.style("bad")).any);
}

TEST_CASE("presentation attributes compute, inherit and take currentColor") {
    Styled doc("<svg xmlns='http://www.w3.org/2000/svg' id='svg' fill='red' color='blue' stroke-width='2em'"
        " font-size='10'><rect id='rect' stroke='currentColor' fill='inherit' opacity='0.5' stroke-dasharray='1,2 3'/></svg>");
    const CssComputedStyle* style = doc.style("rect");
    SvgPaintStyle fill = svgStylePaint(style, PrCssProperty_Fill, kCssDefaultColorEnvironment);
    CHECK(fill.kind == SvgPaintKind_Color);
    CHECK(fill.color.r == 1.0f);
    SvgPaintStyle stroke = svgStylePaint(style, PrCssProperty_Stroke, kCssDefaultColorEnvironment);
    CHECK(stroke.kind == SvgPaintKind_Color);
    CHECK(stroke.color.b == 1.0f);
    SvgStrokeStyle shape = svgStyleStroke(style);
    CHECK(shape.width.px == 20.0f);
    CHECK(shape.dashCount == 3);
    CHECK(svgStyleNumber(style, PrCssProperty_Opacity, 1) == 0.5f);
    CHECK(svgStyleNumber(doc.style("svg"), PrCssProperty_Opacity, 1) == 1.0f);
}

TEST_CASE("a presentation attribute that holds more than one value declares nothing") {
    Styled doc("<svg xmlns='http://www.w3.org/2000/svg'><rect id='semicolon' fill='red;display:none'/>"
        "<rect id='important' fill='red !important' style='fill: green'/>"
        "<rect id='quoted' font-family='\"a;b\"'/></svg>");
    const CssComputedStyle* semicolon = doc.style("semicolon");
    CHECK_FALSE(svgStyleIs(semicolon, PrCssProperty_Display, PrCssKeyword_None));
    CHECK(svgStylePaint(semicolon, PrCssProperty_Fill, kCssDefaultColorEnvironment).color.r == 0.0f);
    SvgPaintStyle important = svgStylePaint(doc.style("important"), PrCssProperty_Fill, kCssDefaultColorEnvironment);
    CHECK(important.color.g > 0.4f);
    CssValueRun family = cssComputedValue(doc.style("quoted"), PrCssProperty_FontFamily);
    REQUIRE(family.count == 1);
    CHECK(cssValueText(family, family.values) == "a;b");
}

TEST_CASE("a path's d attribute is the path data of the d property, which computes it in absolute coordinates") {
    Styled doc("<svg xmlns='http://www.w3.org/2000/svg'><path id='p' d='m 5 5 l 10 10 \"10'/>"
        "<path id='q' d='m 5 5 l 10 10' style='d: path(\"M 1 2 h 3 z\")'/></svg>");
    // The data is computed up to its first error, which a quote in the attribute is.
    RawArray<PrPathDataCommand> commands;
    svgStylePathCommands(doc.style("p"), &commands);
    REQUIRE(commands.count == 2);
    CHECK(commands.data[1].letter == 'L');
    CHECK(commands.data[1].arguments[0] == 15.0f);
    // A style sheet's path() takes the place of the attribute's.
    commands.clear();
    svgStylePathCommands(doc.style("q"), &commands);
    REQUIRE(commands.count == 3);
    CHECK(commands.data[2].letter == 'Z');
    CHECK(commands.data[1].arguments[0] == 4.0f);
    commands.reset();
}
