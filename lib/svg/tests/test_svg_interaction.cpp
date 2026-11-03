// SVGGraphicsElement's and SVGGeometryElement's queries, the SVG DOM's lengths and strings, and the elements'
// interfaces, measured in documents that have no presentation.

#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_length.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

// Returns the text of a view. The null view reads as the empty string.
std::string textOf(PrStringView text) {
    return std::string(text.size != 0 ? text.data : "", text.size);
}

// A document of `body` in an outermost svg element 200 by 100, which a document without a presentation draws at that
// size.
struct Document {
    PrDomDocument* document;

    explicit Document(const char* body) {
        std::string source = std::string("<svg xmlns='http://www.w3.org/2000/svg' "
            "xmlns:xlink='http://www.w3.org/1999/xlink' width='200' height='100'>") + body + "</svg>";
        document = prDomParseXml(source, "image/svg+xml", nullptr);
        prCssEnable(document);
        prSvgEnable(document);
        REQUIRE(document != nullptr);
    }

    ~Document() { prDomDocumentRelease(document); }

    PrSvgElement* byId(const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        return prSvgElementQuery(prDomElementAsEventTarget(element));
    }

    std::string attribute(const char* id, const char* name) {
        return textOf(prDomElementGetAttribute(prSvgElementAsElement(byId(id)), name));
    }
};

void checkRect(PrDomRect rect, double x, double y, double width, double height) {
    CHECK(rect.x == doctest::Approx(x));
    CHECK(rect.y == doctest::Approx(y));
    CHECK(rect.width == doctest::Approx(width));
    CHECK(rect.height == doctest::Approx(height));
}

PrDomRect boxOf(PrSvgElement* element, PrSvgBoundingBoxOptions options) {
    return prSvgGraphicsElementGetBBox(element, &options);
}

std::string stringOf(PrRawString value) {
    std::string text(prRawStringView(&value).data, prRawStringView(&value).size);
    prRawStringReset(&value);
    return text;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("getBBox measures the fill, adds the stroke if asked, and cuts it by the clip path if asked") {
    Document d("<clipPath id='half'><rect width='50' height='100'/></clipPath>"
        "<line id='line' x1='10' y1='20' x2='90' y2='20' stroke='black' stroke-width='10' stroke-linecap='square'/>"
        "<rect id='clipped' width='100' height='10' clip-path='url(#half)'/>");
    PrSvgElement* line = d.byId("line");
    checkRect(prSvgGraphicsElementGetBBox(line, nullptr), 10, 20, 80, 0);
    checkRect(boxOf(line, PrSvgBoundingBoxOptions{.fill = true, .stroke = true}), 5, 15, 90, 10);
    checkRect(boxOf(line, PrSvgBoundingBoxOptions{.fill = true, .clipped = true}), 10, 20, 80, 0);
    checkRect(prSvgGraphicsElementGetBBox(d.byId("clipped"), nullptr), 0, 0, 100, 10);
    checkRect(boxOf(d.byId("clipped"), PrSvgBoundingBoxOptions{.fill = true, .clipped = true}), 0, 0, 50, 10);
}

TEST_CASE("a container's bounding box holds a child without area, and a hidden child, but not a child that displays none") {
    Document d("<g id='g'><line x1='10' y1='40' x2='90' y2='40' stroke='black'/>"
        "<rect x='100' y='0' width='20' height='20' visibility='hidden'/>"
        "<rect x='300' y='300' width='20' height='20' display='none'/></g><g id='empty'/>");
    checkRect(prSvgGraphicsElementGetBBox(d.byId("g"), nullptr), 10, 0, 110, 40);
    checkRect(prSvgGraphicsElementGetBBox(d.byId("empty"), nullptr), 0, 0, 0, 0);
}

TEST_CASE("getCTM maps to the coordinates that the nearest viewport is placed in, through its viewBox") {
    Document d("<svg id='inner' x='10' y='10' width='20' height='20' viewBox='0 0 10 10'>"
        "<rect id='in' x='1' y='1' width='2' height='2'/></svg>"
        "<rect id='moved' width='5' height='5' transform='translate(3 4)'/>");
    PrDomMatrix m = prDomMatrixIdentity();
    REQUIRE(prSvgGraphicsElementGetCtm(d.byId("moved"), &m));
    CHECK(m.m[0] == doctest::Approx(1));
    CHECK(m.m[12] == doctest::Approx(3));
    CHECK(m.m[13] == doctest::Approx(4));
    REQUIRE(prSvgGraphicsElementGetCtm(d.byId("inner"), &m));
    CHECK(m.m[0] == doctest::Approx(2));
    CHECK(m.m[12] == doctest::Approx(10));
    REQUIRE(prSvgGraphicsElementGetCtm(d.byId("in"), &m));
    CHECK(m.m[0] == doctest::Approx(2));
    CHECK(m.m[5] == doctest::Approx(2));
    CHECK(m.m[12] == doctest::Approx(10));
    CHECK(m.m[13] == doctest::Approx(10));
}

TEST_CASE("getBBox's markers option adds the bounds of the markers' content, placed at each vertex") {
    Document d("<marker id='m' markerWidth='10' markerHeight='10' refX='5' refY='5' markerUnits='userSpaceOnUse'>"
        "<rect width='10' height='10'/></marker>"
        "<g id='group'><line id='line' x1='20' y1='50' x2='80' y2='50' stroke='black' marker-start='url(#m)' "
        "marker-end='url(#m)'/></g>");
    PrSvgElement* line = d.byId("line");
    checkRect(prSvgGraphicsElementGetBBox(line, nullptr), 20, 50, 60, 0);
    checkRect(boxOf(line, PrSvgBoundingBoxOptions{.fill = true, .markers = true}), 15, 45, 70, 10);
    checkRect(boxOf(d.byId("group"), PrSvgBoundingBoxOptions{.fill = true, .markers = true}), 15, 45, 70, 10);
}

TEST_CASE("a use element's bounding box holds its x and y, and its CTM leaves them out") {
    Document d("<defs><rect id='r' x='5' y='5' width='20' height='10'/>"
        "<clipPath id='c'><rect width='40' height='100'/></clipPath></defs>"
        "<use id='u' href='#r' x='60' y='10' transform='scale(2 1)' clip-path='url(#c)'/>");
    PrSvgElement* use = d.byId("u");
    checkRect(prSvgGraphicsElementGetBBox(use, nullptr), 65, 15, 20, 10);
    // The clip path is in the space that x and y move, where it holds the whole instance.
    checkRect(boxOf(use, PrSvgBoundingBoxOptions{.fill = true, .clipped = true}), 65, 15, 20, 10);
    PrDomMatrix m = prDomMatrixIdentity();
    REQUIRE(prSvgGraphicsElementGetCtm(use, &m));
    CHECK(m.m[0] == doctest::Approx(2));
    CHECK(m.m[12] == doctest::Approx(0));
    CHECK(m.m[13] == doctest::Approx(0));
}

TEST_CASE("getCTM and a container's bounding box take the vector effects of the element and its children") {
    Document d("<g id='group' transform='translate(40 30) scale(3)'>"
        "<rect id='sized' x='5' width='20' height='10' vector-effect='non-scaling-size'/></g>");
    PrDomMatrix m = prDomMatrixIdentity();
    REQUIRE(prSvgGraphicsElementGetCtm(d.byId("sized"), &m));
    CHECK(m.m[0] == doctest::Approx(1));
    CHECK(m.m[5] == doctest::Approx(1));
    CHECK(m.m[12] == doctest::Approx(40));
    CHECK(m.m[13] == doctest::Approx(30));
    checkRect(prSvgGraphicsElementGetBBox(d.byId("sized"), nullptr), 5, 0, 20, 10);
    // In the group's user space, which scale(3) maps to the host, the rect is a third of its size.
    checkRect(prSvgGraphicsElementGetBBox(d.byId("group"), nullptr), 5.0 / 3, 0, 20.0 / 3, 10.0 / 3);
}

TEST_CASE("an element that no tree draws has no CTM and no screen CTM, and a document without a presentation no screen CTM") {
    Document d("<rect id='gone' width='5' height='5' display='none'/><rect id='shown' width='5' height='5'/>");
    PrDomMatrix m = prDomMatrixIdentity();
    CHECK_FALSE(prSvgGraphicsElementGetCtm(d.byId("gone"), &m));
    CHECK_FALSE(prSvgGraphicsElementGetScreenCtm(d.byId("shown"), &m));
}

TEST_CASE("isPointInStroke takes the stroke's dashes, and isPointInFill the interior by fill-rule") {
    Document d("<line id='dashed' x1='0' y1='10' x2='100' y2='10' stroke='black' stroke-width='10' stroke-dasharray='10 10'/>"
        "<circle id='ring' cx='150' cy='50' r='20' fill='none' stroke='black' stroke-width='4'/>"
        "<path id='hole' d='M0 0h60v60h-60z M20 20h20v20h-20z' fill-rule='evenodd'/>");
    bool inside = false;
    REQUIRE(prSvgGeometryElementIsPointInStroke(d.byId("dashed"), PrDomPoint{5, 10, 0, 1}, &inside) == PrStatus_Ok);
    CHECK(inside);
    REQUIRE(prSvgGeometryElementIsPointInStroke(d.byId("dashed"), PrDomPoint{15, 10, 0, 1}, &inside) == PrStatus_Ok);
    CHECK_FALSE(inside);
    REQUIRE(prSvgGeometryElementIsPointInFill(d.byId("ring"), PrDomPoint{150, 50, 0, 1}, &inside) == PrStatus_Ok);
    CHECK(inside);
    REQUIRE(prSvgGeometryElementIsPointInStroke(d.byId("ring"), PrDomPoint{150, 31, 0, 1}, &inside) == PrStatus_Ok);
    CHECK(inside);
    REQUIRE(prSvgGeometryElementIsPointInFill(d.byId("hole"), PrDomPoint{30, 30, 0, 1}, &inside) == PrStatus_Ok);
    CHECK_FALSE(inside);
    REQUIRE(prSvgGeometryElementIsPointInFill(d.byId("hole"), PrDomPoint{10, 10, 0, 1}, &inside) == PrStatus_Ok);
    CHECK(inside);
}

TEST_CASE("a geometry element measures the length of its equivalent path and the point along it") {
    Document d("<circle id='circle' cx='50' cy='50' r='20'/><line id='line' x1='0' y1='10' x2='100' y2='10'/>"
        "<rect id='gone' width='5' height='5' display='none'/>");
    float length = 0;
    REQUIRE(prSvgGeometryElementGetTotalLength(d.byId("circle"), &length) == PrStatus_Ok);
    CHECK(length == doctest::Approx(2 * 3.14159265 * 20).epsilon(0.001));
    PrDomPoint point{};
    REQUIRE(prSvgGeometryElementGetPointAtLength(d.byId("line"), 30, &point) == PrStatus_Ok);
    CHECK(point.x == doctest::Approx(30));
    CHECK(point.y == doctest::Approx(10));
    REQUIRE(prSvgGeometryElementGetPointAtLength(d.byId("line"), 500, &point) == PrStatus_Ok);
    CHECK(point.x == doctest::Approx(100));
    CHECK(prSvgGeometryElementGetTotalLength(d.byId("gone"), &length) == PrStatus_InvalidState);
}

TEST_CASE("each element has the interface that its tag names") {
    Document d("<g id='g'/><rect id='rect'/><a id='a'/><use id='use'/><switch id='switch'/><symbol id='symbol'/>"
        "<script id='script'/><view id='view'/><animate id='animate'/><set id='set'/><text id='text'/><desc id='desc'/>");
    CHECK(prSvgElementInterface(d.byId("g")) == PrSvgInterface_Graphics);
    CHECK(prSvgElementInterface(d.byId("rect")) == PrSvgInterface_Rect);
    CHECK(prSvgInterfaceIsGeometry(PrSvgInterface_Rect));
    CHECK(prSvgInterfaceIsGeometry(PrSvgInterface_Polygon));
    CHECK_FALSE(prSvgInterfaceIsGeometry(PrSvgInterface_Svg));
    CHECK(prSvgElementInterface(d.byId("a")) == PrSvgInterface_A);
    CHECK(prSvgElementInterface(d.byId("use")) == PrSvgInterface_Use);
    CHECK(prSvgElementInterface(d.byId("switch")) == PrSvgInterface_Switch);
    CHECK(prSvgElementInterface(d.byId("symbol")) == PrSvgInterface_Symbol);
    CHECK(prSvgElementInterface(d.byId("script")) == PrSvgInterface_Script);
    CHECK(prSvgElementInterface(d.byId("view")) == PrSvgInterface_View);
    CHECK(prSvgElementInterface(d.byId("animate")) == PrSvgInterface_Animate);
    CHECK(prSvgElementInterface(d.byId("set")) == PrSvgInterface_Set);
    CHECK(prSvgElementInterface(d.byId("text")) == PrSvgInterface_TextContent);
    CHECK(prSvgElementInterface(d.byId("desc")) == PrSvgInterface_Element);
}

TEST_CASE("a length reflects its attribute: its unit, its value in user units, and its string") {
    Document d("<rect id='r' x='10%' width='2em' height='3mm' style='font-size: 20px'/><svg id='inner' width='50%' height='10'/>");
    PrSvgAnimatedLength* x = prSvgAnimatedLengthCreate(d.byId("r"), "x");
    PrSvgAnimatedLength* width = prSvgAnimatedLengthCreate(d.byId("r"), "width");
    REQUIRE(x != nullptr);
    REQUIRE(width != nullptr);
    CHECK(prSvgAnimatedLengthCreate(d.byId("r"), "cx") == nullptr);
    PrSvgLength* xBase = prSvgAnimatedLengthBaseVal(x);
    CHECK(prSvgLengthUnitType(xBase) == PrSvgLengthType_Percentage);
    CHECK(prSvgLengthValueInSpecifiedUnits(xBase) == doctest::Approx(10));
    CHECK(prSvgLengthValue(xBase) == doctest::Approx(20));
    CHECK(stringOf(prSvgLengthValueAsString(xBase)) == "10%");
    PrSvgLength* widthBase = prSvgAnimatedLengthBaseVal(width);
    CHECK(prSvgLengthUnitType(widthBase) == PrSvgLengthType_Ems);
    CHECK(prSvgLengthValue(widthBase) == doctest::Approx(40));
    PrSvgAnimatedLength* inner = prSvgAnimatedLengthCreate(d.byId("inner"), "width");
    PrSvgLength* innerBase = prSvgAnimatedLengthBaseVal(inner);
    CHECK(prSvgLengthValue(innerBase) == doctest::Approx(100));
    prSvgLengthDestroy(innerBase);
    prSvgAnimatedLengthDestroy(inner);
    prSvgLengthDestroy(widthBase);
    prSvgLengthDestroy(xBase);
    prSvgAnimatedLengthDestroy(width);
    prSvgAnimatedLengthDestroy(x);
}

TEST_CASE("setting a length writes its attribute again, and its animated value is read only") {
    Document d("<rect id='r' x='10'/><circle id='c'/>");
    PrSvgAnimatedLength* x = prSvgAnimatedLengthCreate(d.byId("r"), "x");
    PrSvgLength* base = prSvgAnimatedLengthBaseVal(x);
    PrSvgLength* animated = prSvgAnimatedLengthAnimVal(x);
    CHECK(prSvgLengthSetValue(base, 25) == PrStatus_Ok);
    CHECK(d.attribute("r", "x") == "25");
    CHECK(prSvgLengthValue(animated) == doctest::Approx(25));
    CHECK(prSvgLengthSetValue(animated, 1) == PrStatus_ReadOnly);
    CHECK(prSvgLengthConvertToSpecifiedUnits(base, PrSvgLengthType_In) == PrStatus_Ok);
    CHECK(prSvgLengthUnitType(base) == PrSvgLengthType_In);
    CHECK(prSvgLengthValue(base) == doctest::Approx(25));
    CHECK(prSvgLengthNewValueSpecifiedUnits(base, PrSvgLengthType_Pc, 2) == PrStatus_Ok);
    CHECK(d.attribute("r", "x") == "2pc");
    CHECK(prSvgLengthNewValueSpecifiedUnits(base, PrSvgLengthType_Unknown, 2) == PrStatus_NotSupported);
    CHECK(prSvgLengthSetValueAsString(base, "nonsense") == PrStatus_Syntax);
    CHECK(prSvgLengthSetValueAsString(base, "4q") == PrStatus_Ok);
    CHECK(prSvgLengthUnitType(base) == PrSvgLengthType_Unknown);
    CHECK(prSvgLengthValueInSpecifiedUnits(base) == doctest::Approx(0));
    prSvgLengthDestroy(animated);
    prSvgLengthDestroy(base);
    prSvgAnimatedLengthDestroy(x);

    // A missing attribute reflects its initial value.
    PrSvgAnimatedLength* r = prSvgAnimatedLengthCreate(d.byId("c"), "r");
    PrSvgLength* radius = prSvgAnimatedLengthBaseVal(r);
    CHECK(prSvgLengthUnitType(radius) == PrSvgLengthType_Number);
    CHECK(prSvgLengthValue(radius) == doctest::Approx(0));
    prSvgLengthDestroy(radius);
    prSvgAnimatedLengthDestroy(r);
}

TEST_CASE("a detached length holds its own value, with percentages of 100") {
    PrSvgLength* length = prSvgLengthCreate();
    CHECK(prSvgLengthUnitType(length) == PrSvgLengthType_Number);
    CHECK(prSvgLengthNewValueSpecifiedUnits(length, PrSvgLengthType_Percentage, 30) == PrStatus_Ok);
    CHECK(prSvgLengthValue(length) == doctest::Approx(30));
    CHECK(stringOf(prSvgLengthValueAsString(length)) == "30%");
    prSvgLengthDestroy(length);
}

TEST_CASE("an href reflects the href without a namespace before XLink's, and sets the one without") {
    Document d("<a id='both' href='#one' xlink:href='#two'/><use id='xlink' xlink:href='#three'/><rect id='r'/>");
    PrSvgAnimatedString* both = prSvgAnimatedStringCreate(d.byId("both"), "href");
    PrSvgAnimatedString* xlink = prSvgAnimatedStringCreate(d.byId("xlink"), "href");
    CHECK(prSvgAnimatedStringCreate(d.byId("r"), "href") == nullptr);
    CHECK(textOf(prSvgAnimatedStringBaseVal(both)) == "#one");
    CHECK(textOf(prSvgAnimatedStringBaseVal(xlink)) == "#three");
    prSvgAnimatedStringSetBaseVal(xlink, "#four");
    CHECK(d.attribute("xlink", "href") == "#four");
    CHECK(textOf(prSvgAnimatedStringBaseVal(xlink)) == "#four");
    prSvgAnimatedStringDestroy(xlink);
    prSvgAnimatedStringDestroy(both);
}
