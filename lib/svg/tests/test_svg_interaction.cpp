// SVGGraphicsElement's and SVGGeometryElement's queries, the SVG DOM's lengths and strings, and the elements'
// interfaces, measured in documents that have no presentation.

#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_types.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// Returns the text of a view. The null view reads as the empty string.
String textOf(PrStringView text) {
    return String(text.size != 0 ? text : PrStringView(""));
}

// A document of `body` in an outermost svg element 200 by 100, which a document without a presentation draws at that
// size.
struct Document {
    PrDomDocument* document;

    explicit Document(const char* body) {
        String source = format("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
            "width='200' height='100'>{}</svg>", body);
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

    String attribute(const char* id, const char* name) {
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

String stringOf(PrRawString value) {
    String text = textOf(prRawStringView(&value));
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
        "<clipPath id='c'><rect width='40' height='100'/></clipPath>"
        "<clipPath id='half'><rect width='15' height='100'/></clipPath></defs>"
        "<use id='u' href='#r' x='60' y='10' transform='scale(2 1)' clip-path='url(#c)'/>"
        "<use id='cut' href='#r' x='60' y='10' clip-path='url(#half)'/>");
    PrSvgElement* use = d.byId("u");
    checkRect(prSvgGraphicsElementGetBBox(use, nullptr), 65, 15, 20, 10);
    // The clip path is in the space that x and y move, where it holds the whole instance, or cuts it.
    checkRect(boxOf(use, PrSvgBoundingBoxOptions{.fill = true, .clipped = true}), 65, 15, 20, 10);
    checkRect(boxOf(d.byId("cut"), PrSvgBoundingBoxOptions{.fill = true, .clipped = true}), 65, 15, 10, 10);
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

TEST_CASE("getIntersectionList finds what pointer events hit in the rectangle, cut by clips, and use elements") {
    Document d("<clipPath id='lower'><rect y='50' width='200' height='50'/></clipPath>"
        "<g id='group'><rect id='box' width='40' height='40'/><circle id='round' cx='100' cy='20' r='20'/>"
        "<rect id='clipped' width='200' height='100' clip-path='url(#lower)'/></g>"
        "<rect id='ghost' width='40' height='40' pointer-events='none'/>"
        "<defs><circle id='dot' r='5'/></defs><use id='use' href='#dot' x='150' y='20'/>");
    PrSvgElement* svg = prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(d.document)));
    auto ids = [&](double x, double y, double width, double height, bool enclosure) {
        PrDomNodeList* list = prDomNodeListCreate();
        prSvgSvgElementGetHitList(svg, PrDomRect{x, y, width, height}, nullptr, enclosure, list);
        String text;
        for (uint32_t i = 0; i < prDomNodeListLength(list); ++i) {
            PrDomElement* element = prDomElementQuery(prDomNodeAsEventTarget(prDomNodeListNodeAt(list, i)));
            text.append(format("{}{}", i != 0 ? " " : "", textOf(prDomElementId(element))));
        }
        prDomNodeListDestroy(list);
        return text;
    };
    // The corner of the circle's bounding box holds none of the circle, and the clip leaves nothing above y = 50.
    CHECK(ids(30, 0, 52, 4, false) == "box");
    CHECK(ids(0, 0, 200, 49, false) == "box round use");
    CHECK(ids(0, 0, 200, 60, false) == "box round clipped use");
    CHECK(ids(0, 0, 121, 41, true) == "box round");
    // A container intersects if every graphics element that it holds does.
    CHECK_FALSE(prSvgSvgElementCheckHit(svg, d.byId("group"), PrDomRect{0, 0, 200, 40}, false));
    CHECK(prSvgSvgElementCheckHit(svg, d.byId("group"), PrDomRect{0, 0, 200, 60}, false));
    CHECK(prSvgSvgElementCheckHit(svg, d.byId("round"), PrDomRect{90, 10, 4, 4}, false));
    CHECK_FALSE(prSvgSvgElementCheckHit(svg, d.byId("ghost"), PrDomRect{0, 0, 200, 100}, false));
}

TEST_CASE("a geometry element measures the length of its equivalent path and the point along it") {
    Document d("<circle id='circle' cx='50' cy='50' r='20'/><line id='line' x1='0' y1='10' x2='100' y2='10'/>"
        "<rect id='gone' width='5' height='5' display='none'/><rect id='empty'/>");
    CHECK(prSvgGeometryElementTotalLength(d.byId("circle")) == doctest::Approx(2 * 3.14159265 * 20).epsilon(0.001));
    PrDomPoint point{};
    REQUIRE(prSvgGeometryElementGetPointAtLength(d.byId("line"), 30, &point) == PrStatus_Ok);
    CHECK(point.x == doctest::Approx(30));
    CHECK(point.y == doctest::Approx(10));
    REQUIRE(prSvgGeometryElementGetPointAtLength(d.byId("line"), 500, &point) == PrStatus_Ok);
    CHECK(point.x == doctest::Approx(100));
    // A shape that is not displayed is measured as if it were drawn. An empty path has a length of 0, and no point
    // along it.
    CHECK(prSvgGeometryElementTotalLength(d.byId("gone")) == doctest::Approx(20));
    CHECK(prSvgGeometryElementTotalLength(d.byId("empty")) == 0);
    CHECK(prSvgGeometryElementGetPointAtLength(d.byId("empty"), 0, &point) == PrStatus_InvalidState);
}

TEST_CASE("each element has the interface that its tag names") {
    Document d("<g id='g'/><rect id='rect'/><a id='a'/><use id='use'/><switch id='switch'/><symbol id='symbol'/>"
        "<script id='script'/><view id='view'/><animate id='animate'/><set id='set'/><text id='text'/><desc id='desc'/>");
    CHECK(prSvgElementInterface(d.byId("g")) == PrSvgInterface_G);
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
    CHECK(prSvgElementInterface(d.byId("text")) == PrSvgInterface_Text);
    CHECK(prSvgElementInterface(d.byId("desc")) == PrSvgInterface_Desc);
}

TEST_CASE("a length of an element that is not drawn resolves against the viewport that it would be drawn in") {
    // The inner svg's content is 80 by 40, which the rects in its symbol and its hidden group take their percentages
    // of.
    Document d("<svg width='80' height='40'><symbol width='10' height='10'><rect id='kept' width='50%'/></symbol>"
        "<g display='none'><rect id='hidden' height='50%'/></g></svg>");
    PrSvgLength length{};
    REQUIRE(prSvgElementGetLength(d.byId("kept"), PrSvgAttribute_Width, PrSvgLayer_Base, &length));
    CHECK(prSvgLengthResolve(length, prSvgElementLengthBasis(d.byId("kept"), PrSvgAttribute_Width)) ==
        doctest::Approx(40));
    REQUIRE(prSvgElementGetLength(d.byId("hidden"), PrSvgAttribute_Height, PrSvgLayer_Base, &length));
    CHECK(prSvgLengthResolve(length, prSvgElementLengthBasis(d.byId("hidden"), PrSvgAttribute_Height)) ==
        doctest::Approx(20));
}

TEST_CASE("an element reads a length attribute as a number of a unit, which resolves against its viewport and font") {
    Document d("<rect id='r' x='10%' width='2em' height='3mm' style='font-size: 20px'/><svg id='inner' width='50%' height='10'/>");
    PrSvgElement* rect = d.byId("r");
    PrSvgLength x{};
    REQUIRE(prSvgElementGetLength(rect, PrSvgAttribute_X, PrSvgLayer_Base, &x));
    CHECK(x.unit == PrCssUnit_Percent);
    CHECK(x.value == doctest::Approx(10));
    CHECK(prSvgLengthResolve(x, prSvgElementLengthBasis(rect, PrSvgAttribute_X)) == doctest::Approx(20));
    CHECK(stringOf(prSvgLengthSerialize(x)) == "10%");
    PrSvgLength width{};
    REQUIRE(prSvgElementGetLength(rect, PrSvgAttribute_Width, PrSvgLayer_Base, &width));
    CHECK(width.unit == PrCssUnit_Em);
    CHECK(prSvgLengthResolve(width, prSvgElementLengthBasis(rect, PrSvgAttribute_Width)) == doctest::Approx(40));
    PrSvgLength inner{};
    REQUIRE(prSvgElementGetLength(d.byId("inner"), PrSvgAttribute_Width, PrSvgLayer_Base, &inner));
    CHECK(prSvgLengthResolve(inner, prSvgElementLengthBasis(d.byId("inner"), PrSvgAttribute_Width)) ==
        doctest::Approx(100));

    // A rect has no cx, and its x is not a list.
    PrSvgLength none{};
    CHECK_FALSE(prSvgElementGetLength(rect, PrSvgAttribute_Cx, PrSvgLayer_Base, &none));
    CHECK(prSvgElementAttributeType(rect, PrSvgAttribute_X) == PrSvgValueType_Length);
    CHECK(prSvgElementGetLengthList(rect, PrSvgAttribute_X, PrSvgLayer_Base, nullptr, 0) == 0);
}

TEST_CASE("setting a length writes its attribute, and a missing or negative size reads as its initial value") {
    Document d("<rect id='r' x='10' width='-5'/><circle id='c'/>");
    PrSvgElement* rect = d.byId("r");
    CHECK(prSvgElementSetLength(rect, PrSvgAttribute_X, PrSvgLength{25, PrCssUnit_Number}) == PrStatus_Ok);
    CHECK(d.attribute("r", "x") == "25");
    PrSvgLength animated{};
    REQUIRE(prSvgElementGetLength(rect, PrSvgAttribute_X, PrSvgLayer_Animated, &animated));
    CHECK(animated.value == doctest::Approx(25));
    PrSvgLength inches = prSvgLengthConvert(animated, PrCssUnit_In, prSvgLengthBasisDefault());
    CHECK(inches.unit == PrCssUnit_In);
    CHECK(prSvgLengthResolve(inches, prSvgLengthBasisDefault()) == doctest::Approx(25));
    CHECK(prSvgElementSetLength(rect, PrSvgAttribute_X, PrSvgLength{2, PrCssUnit_Pc}) == PrStatus_Ok);
    CHECK(d.attribute("r", "x") == "2pc");
    CHECK(prSvgElementSetLength(rect, PrSvgAttribute_X, PrSvgLength{2, PrCssUnit_Deg}) == PrStatus_InvalidData);
    CHECK(prSvgElementSetLength(rect, PrSvgAttribute_Points, PrSvgLength{2, PrCssUnit_Px}) == PrStatus_WrongType);
    PrSvgLength width{};
    REQUIRE(prSvgElementGetLength(rect, PrSvgAttribute_Width, PrSvgLayer_Base, &width));
    CHECK(width.value == doctest::Approx(0));
    PrSvgLength radius{};
    REQUIRE(prSvgElementGetLength(d.byId("c"), PrSvgAttribute_R, PrSvgLayer_Base, &radius));
    CHECK(radius.unit == PrCssUnit_Number);
    CHECK(radius.value == doctest::Approx(0));

    PrSvgLength parsed{};
    CHECK_FALSE(prSvgLengthParse("nonsense", &parsed));
    REQUIRE(prSvgLengthParse("4q", &parsed));
    CHECK(parsed.unit == PrCssUnit_Q);
    CHECK(stringOf(prSvgLengthSerialize(parsed)) == "4q");
}

TEST_CASE("an enumeration reads as its keyword's value, and an unknown keyword as its initial value") {
    Document d("<linearGradient id='g' gradientUnits='userSpaceOnUse' spreadMethod='bogus'/>"
        "<marker id='m' orient='auto-start-reverse'/><marker id='n' orient='45deg'/>");
    uint8_t value = 0;
    REQUIRE(prSvgElementGetEnumeration(d.byId("g"), PrSvgAttribute_GradientUnits, PrSvgLayer_Base, &value));
    CHECK(value == PrSvgUnits_UserSpaceOnUse);
    REQUIRE(prSvgElementGetEnumeration(d.byId("g"), PrSvgAttribute_SpreadMethod, PrSvgLayer_Base, &value));
    CHECK(value == PrSvgSpreadMethod_Pad);
    CHECK(prSvgElementSetEnumeration(d.byId("g"), PrSvgAttribute_SpreadMethod, PrSvgSpreadMethod_Repeat) ==
        PrStatus_Ok);
    CHECK(d.attribute("g", "spreadMethod") == "repeat");
    CHECK(prSvgElementSetEnumeration(d.byId("g"), PrSvgAttribute_SpreadMethod, 9) == PrStatus_InvalidData);

    // A marker's orient is a keyword or an angle.
    REQUIRE(prSvgElementGetEnumeration(d.byId("m"), PrSvgAttribute_Orient, PrSvgLayer_Base, &value));
    CHECK(value == PrSvgOrient_AutoStartReverse);
    REQUIRE(prSvgElementGetEnumeration(d.byId("n"), PrSvgAttribute_Orient, PrSvgLayer_Base, &value));
    CHECK(value == PrSvgOrient_Angle);
    PrSvgAngle angle{};
    REQUIRE(prSvgElementGetAngle(d.byId("n"), PrSvgAttribute_Orient, PrSvgLayer_Base, &angle));
    CHECK(angle.unit == PrCssUnit_Deg);
    CHECK(prSvgAngleDegrees(angle) == doctest::Approx(45));
    CHECK(prSvgElementSetEnumeration(d.byId("m"), PrSvgAttribute_Orient, PrSvgOrient_Angle) == PrStatus_Ok);
    CHECK(d.attribute("m", "orient") == "0");
}

TEST_CASE("a value outside an attribute's range reads as its initial value, and a string reads as its text") {
    // order's integers are greater than zero once truncated, and an error in either reads the whole pair as 3 3.
    Document d("<filter><feConvolveMatrix id='half' order='2.7'/><feConvolveMatrix id='zero' order='0.5'/>"
        "<feConvolveMatrix id='one' order='4 -2'/></filter>"
        "<path id='p' d='M0 0h10'/><text><textPath id='t' href='#p' side='right'>x</textPath></text><g id='g' class=' a  b '/>");
    int32_t pair[2] = {};
    REQUIRE(prSvgElementGetIntegerPair(d.byId("half"), PrSvgAttribute_Order, PrSvgLayer_Base, pair));
    CHECK((pair[0] == 2 && pair[1] == 2));
    REQUIRE(prSvgElementGetIntegerPair(d.byId("zero"), PrSvgAttribute_Order, PrSvgLayer_Base, pair));
    CHECK((pair[0] == 3 && pair[1] == 3));
    REQUIRE(prSvgElementGetIntegerPair(d.byId("one"), PrSvgAttribute_Order, PrSvgLayer_Base, pair));
    CHECK((pair[0] == 3 && pair[1] == 3));
    uint8_t side = 0;
    REQUIRE(prSvgElementGetEnumeration(d.byId("t"), PrSvgAttribute_Side, PrSvgLayer_Base, &side));
    CHECK(side == PrSvgTextPathSide_Right);
    CHECK(textOf(prSvgElementGetString(d.byId("g"), PrSvgAttribute_Class, PrSvgLayer_Base)) == " a  b ");
    CHECK(textOf(prSvgElementGetString(d.byId("g"), PrSvgAttribute_Class, PrSvgLayer_Animated)) == " a  b ");
}

TEST_CASE("a pair reads its second number as its first where it has one, and writes one where they are equal") {
    Document d("<filter><feGaussianBlur id='b' stdDeviation='2'/><feConvolveMatrix id='c' order='3 5'/></filter>");
    float pair[2] = {};
    REQUIRE(prSvgElementGetNumberPair(d.byId("b"), PrSvgAttribute_StdDeviation, PrSvgLayer_Base, pair));
    CHECK(pair[0] == doctest::Approx(2));
    CHECK(pair[1] == doctest::Approx(2));
    int32_t order[2] = {};
    REQUIRE(prSvgElementGetIntegerPair(d.byId("c"), PrSvgAttribute_Order, PrSvgLayer_Base, order));
    CHECK(order[0] == 3);
    CHECK(order[1] == 5);
    CHECK(prSvgElementSetNumberPair(d.byId("b"), PrSvgAttribute_StdDeviation, 4, 4) == PrStatus_Ok);
    CHECK(d.attribute("b", "stdDeviation") == "4");
    CHECK(prSvgElementSetNumberPair(d.byId("b"), PrSvgAttribute_StdDeviation, 4, 1.5f) == PrStatus_Ok);
    CHECK(d.attribute("b", "stdDeviation") == "4 1.5");
}

TEST_CASE("a list reads as its items, a list that does not parse is empty, and a write replaces the attribute") {
    Document d("<text id='t' x='1 2em 3%' rotate='bad'/><polygon id='p' points='0,0 10,5'/>"
        "<g id='g' transform='translate(5) rotate(90 1 2)' systemLanguage='en, fr'/>");
    PrSvgLength lengths[4] = {};
    REQUIRE(prSvgElementGetLengthList(d.byId("t"), PrSvgAttribute_X, PrSvgLayer_Base, lengths, 4) == 3);
    CHECK(lengths[1].unit == PrCssUnit_Em);
    CHECK(lengths[2].unit == PrCssUnit_Percent);
    CHECK(prSvgElementGetNumberList(d.byId("t"), PrSvgAttribute_Rotate, PrSvgLayer_Base, nullptr, 0) == 0);
    PrDomPoint points[2] = {};
    REQUIRE(prSvgElementGetPointList(d.byId("p"), PrSvgAttribute_Points, PrSvgLayer_Base, points, 2) == 2);
    CHECK(points[1].x == doctest::Approx(10));
    CHECK(points[1].y == doctest::Approx(5));
    PrSvgTransform transforms[2] = {};
    REQUIRE(prSvgElementGetTransformList(d.byId("g"), PrSvgAttribute_Transform, PrSvgLayer_Base, transforms, 2) == 2);
    CHECK(transforms[0].type == PrSvgTransformType_Translate);
    CHECK(transforms[0].values[1] == doctest::Approx(0));
    CHECK(transforms[1].type == PrSvgTransformType_Rotate);
    CHECK(prSvgTransformAngle(&transforms[1]) == doctest::Approx(90));
    PrDomMatrix rotation = prSvgTransformMatrix(&transforms[1]);
    CHECK(rotation.m[12] == doctest::Approx(3));
    CHECK(rotation.m[13] == doctest::Approx(1));
    PrStringView languages[2] = {};
    REQUIRE(prSvgElementGetStringList(d.byId("g"), PrSvgAttribute_SystemLanguage, languages, 2) == 2);
    CHECK(textOf(languages[1]) == "fr");

    float numbers[] = {1, 2.5f};
    CHECK(prSvgElementSetNumberList(d.byId("t"), PrSvgAttribute_Rotate, numbers, 2) == PrStatus_Ok);
    CHECK(d.attribute("t", "rotate") == "1 2.5");
    PrSvgTransform scale = {.type = PrSvgTransformType_Scale, .values = {2, 3}};
    CHECK(prSvgElementSetTransformList(d.byId("g"), PrSvgAttribute_Transform, &scale, 1) == PrStatus_Ok);
    CHECK(d.attribute("g", "transform") == "scale(2 3)");
}

TEST_CASE("an href reads the href without a namespace before XLink's, and sets the one that it reads") {
    Document d("<a id='both' href='#one' xlink:href='#two'/><use id='xlink' xlink:href='#three'/><rect id='r'/>");
    CHECK(textOf(prSvgElementGetString(d.byId("both"), PrSvgAttribute_Href, PrSvgLayer_Base)) == "#one");
    CHECK(textOf(prSvgElementGetString(d.byId("xlink"), PrSvgAttribute_Href, PrSvgLayer_Base)) == "#three");
    CHECK(prSvgElementGetString(d.byId("r"), PrSvgAttribute_Href, PrSvgLayer_Base).data == nullptr);
    CHECK(prSvgElementSetString(d.byId("xlink"), PrSvgAttribute_Href, "#four") == PrStatus_Ok);
    CHECK(d.attribute("xlink", "href").isEmpty());
    CHECK(prSvgElementSetString(d.byId("both"), PrSvgAttribute_Href, "#five") == PrStatus_Ok);
    CHECK(d.attribute("both", "href") == "#five");
    CHECK(textOf(prSvgElementGetString(d.byId("xlink"), PrSvgAttribute_Href, PrSvgLayer_Base)) == "#four");
}
