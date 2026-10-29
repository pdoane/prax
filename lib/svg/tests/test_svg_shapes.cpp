// Paths and basic shapes as they draw (SVG2/paths.html, SVG2/shapes.html): their geometry from attributes and from CSS,
// and the cases that draw nothing.

#include "svg/tests/svg_fixture.h"

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Paths

TEST_CASE("d set by a style rule overrides the d attribute, and d: none draws nothing") {
    Drawing ruled(svgDocument("<style>#p { d: path('M 10 10 H 50') }</style>"
                              "<path id='p' d='M 10 10 H 20' stroke='black'/>"));
    REQUIRE(ruled.strokes().size() == 1);
    CHECK(near(ruled.strokes()[0]->bounds, PrRect{10, 10, 50, 10}));
    Drawing none(svgDocument("<path d='M 10 10 H 20' style='d: none' stroke='black'/>"));
    CHECK(none.commands.empty());
}

TEST_CASE("path data in error draws the commands before the error") {
    // The second L has one number. What came before draws (SVG2/paths.html#PathDataErrorHandling).
    Drawing drawing(svgDocument("<path d='M 10 10 L 50 10 L 60' stroke='black'/>"));
    REQUIRE(drawing.strokes().size() == 1);
    CHECK(near(drawing.strokes()[0]->bounds, PrRect{10, 10, 50, 10}));
}

TEST_CASE("pathLength scales the dashes and their offset by the length of the geometry") {
    // The path is 100 long and its author's length is 10, so a dash of 2 is 20 and the offset of 1 is 10.
    Drawing drawing(svgDocument("<path d='M 0 10 H 100' pathLength='10' stroke='black' stroke-dasharray='2 3' "
                                "stroke-dashoffset='1'/>"));
    REQUIRE(drawing.strokes().size() == 1);
    const Drawn& stroke = *drawing.strokes()[0];
    REQUIRE(stroke.dashes.size() == 2);
    CHECK(near(stroke.dashes[0], 20));
    CHECK(near(stroke.dashes[1], 30));
    CHECK(near(stroke.stroke.dashOffset, 10));
}

TEST_CASE("pathLength applies to a basic shape, measured along its equivalent path") {
    // The rect's perimeter is 200, and its author's length 100, so each dash doubles.
    Drawing drawing(svgDocument("<rect width='50' height='50' pathLength='100' fill='none' stroke='black' "
                                "stroke-dasharray='5'/>"));
    REQUIRE(drawing.strokes().size() == 1);
    REQUIRE(drawing.strokes()[0]->dashes.size() == 1);
    CHECK(near(drawing.strokes()[0]->dashes[0], 10, 1e-2f));
}

TEST_CASE("a pathLength of zero takes every dash past the end, and a negative one is ignored") {
    Drawing zero(svgDocument("<path d='M 0 10 H 100' pathLength='0' stroke='black' stroke-dasharray='2 0'/>"));
    REQUIRE(zero.strokes().size() == 1);
    CHECK(zero.strokes()[0]->dashes[0] > 100.0f);
    CHECK(zero.strokes()[0]->dashes[1] == 0.0f);
    Drawing negative(svgDocument("<path d='M 0 10 H 100' pathLength='-5' stroke='black' stroke-dasharray='2'/>"));
    REQUIRE(negative.strokes().size() == 1);
    CHECK(near(negative.strokes()[0]->dashes[0], 2));
}

TEST_CASE("a subpath that only closes strokes, as a segment of zero length") {
    Drawing drawing(svgDocument("<path d='M 20 20 Z' stroke='black' stroke-width='10' stroke-linecap='round'/>"));
    REQUIRE(drawing.strokes().size() == 1);
    const Drawn& dot = *drawing.strokes()[0];
    CHECK(dot.stroke.startCap == PrPathCap_Round);
    CHECK(near(dot.bounds, PrRect{20, 20, 20, 20}));
}

TEST_CASE("an arc that ends where it starts strokes as a segment of zero length") {
    Drawing drawing(svgDocument("<path d='M 20 20 A 10 10 0 0 1 20 20' stroke='black' stroke-width='10' "
                                "stroke-linecap='square'/>"));
    REQUIRE(drawing.strokes().size() == 1);
    CHECK(drawing.strokes()[0]->verbs.size() >= 2);
}

// ---------------------------------------------------------------------------------------------------------------------
// Rectangles

TEST_CASE("a rect is placed by x, y, width and height, which percentages take from the viewport") {
    Drawing drawing(svgDocument("<rect x='10%' y='5' width='50%' height='20'/>"), PrVec2{200, 100});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{20, 5, 120, 25}));
}

TEST_CASE("a rect's geometry properties apply from a style rule") {
    Drawing drawing(svgDocument("<style>rect { x: 10px; width: 30px; height: 20px }</style>"
                                "<rect x='50' width='5' height='5'/>"));
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{10, 0, 40, 20}));
}

TEST_CASE("a rect with no width or height, or a negative one, draws nothing") {
    CHECK(Drawing(svgDocument("<rect width='0' height='10'/>")).commands.empty());
    CHECK(Drawing(svgDocument("<rect width='10'/>")).commands.empty());
    // A negative attribute is the initial value, auto, which is zero for a rect.
    CHECK(Drawing(svgDocument("<rect width='-10' height='10'/>")).commands.empty());
}

TEST_CASE("a corner radius that is auto takes the other's value, and both auto are square corners") {
    Drawing ry(svgDocument("<rect width='40' height='40' ry='5'/>"));
    REQUIRE(ry.of(PrDrawCommandKind_CornerRect).size() == 1);
    CHECK(near(ry.of(PrDrawCommandKind_CornerRect)[0]->radii.x, 5));
    CHECK(near(ry.of(PrDrawCommandKind_CornerRect)[0]->radii.y, 5));
    Drawing square(svgDocument("<rect width='40' height='40'/>"));
    CHECK(square.of(PrDrawCommandKind_Rect).size() == 1);
}

TEST_CASE("a corner radius is clamped to half the width or height, and zero in either is square") {
    Drawing clamped(svgDocument("<rect width='40' height='20' rx='30' ry='4'/>"));
    REQUIRE(clamped.of(PrDrawCommandKind_CornerRect).size() == 1);
    CHECK(near(clamped.of(PrDrawCommandKind_CornerRect)[0]->radii.x, 20));
    CHECK(near(clamped.of(PrDrawCommandKind_CornerRect)[0]->radii.y, 4));
    Drawing zero(svgDocument("<rect width='40' height='20' rx='0' ry='4'/>"));
    CHECK(zero.of(PrDrawCommandKind_Rect).size() == 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// Circles and ellipses

TEST_CASE("a circle's r is a percentage of the viewport's normalized diagonal") {
    // The diagonal of 300 by 100, normalized, is the square root of half the sum of the squares: 100 times root 5.
    Drawing drawing(svgDocument("<circle cx='50' cy='50' r='10%'/>"), PrVec2{300, 100});
    REQUIRE(drawing.of(PrDrawCommandKind_Ellipse).size() == 1);
    CHECK(near(drawing.of(PrDrawCommandKind_Ellipse)[0]->radii.x, 10 * std::sqrt(5.0f), 1e-2f));
}

TEST_CASE("a circle with r of zero, or a negative r, draws nothing") {
    CHECK(Drawing(svgDocument("<circle cx='10' cy='10' r='0'/>")).commands.empty());
    CHECK(Drawing(svgDocument("<circle cx='10' cy='10' r='-5'/>")).commands.empty());
}

TEST_CASE("a negative r in a declaration is ignored, and one from a calculation is clamped to zero") {
    Drawing ignored(svgDocument("<circle r='5' style='r: -10px'/>"));
    REQUIRE(ignored.of(PrDrawCommandKind_Ellipse).size() == 1);
    CHECK(near(ignored.of(PrDrawCommandKind_Ellipse)[0]->radii.x, 5));
    CHECK(Drawing(svgDocument("<circle r='5' style='r: calc(5px - 10px)'/>")).commands.empty());
}

TEST_CASE("an ellipse radius that is auto takes the other's value, and both auto draw nothing") {
    Drawing auto_(svgDocument("<ellipse cx='50' cy='50' ry='10'/>"));
    REQUIRE(auto_.of(PrDrawCommandKind_Ellipse).size() == 1);
    CHECK(near(auto_.of(PrDrawCommandKind_Ellipse)[0]->radii.x, 10));
    CHECK(Drawing(svgDocument("<ellipse cx='50' cy='50'/>")).commands.empty());
}

TEST_CASE("an ellipse's radii and center apply from CSS") {
    Drawing drawing(svgDocument("<ellipse style='cx: 40px; cy: 30px; rx: 20px; ry: 10px'/>"));
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{20, 20, 60, 40}));
}

// ---------------------------------------------------------------------------------------------------------------------
// Lines, polylines and polygons

TEST_CASE("a line is never filled") {
    Drawing drawing(svgDocument("<line x1='10' y1='10' x2='50' y2='30' fill='red' stroke='black'/>"));
    CHECK(drawing.order() == "s");
    CHECK(near(drawing.strokes()[0]->bounds, PrRect{10, 10, 50, 30}));
}

TEST_CASE("a polyline is open and a polygon closes") {
    Drawing polyline(svgDocument("<polyline points='10 10 50 10 50 50' stroke='black'/>"));
    REQUIRE(polyline.strokes().size() == 1);
    CHECK(polyline.strokes()[0]->verbs.back() != PrPathVerb_Close);
    Drawing polygon(svgDocument("<polygon points='10 10 50 10 50 50' stroke='black'/>"));
    REQUIRE(polygon.strokes().size() == 1);
    CHECK(polygon.strokes()[0]->verbs.back() == PrPathVerb_Close);
}

TEST_CASE("points with an odd number of coordinates draw the pairs before the last") {
    Drawing drawing(svgDocument("<polyline points='10 10, 50 10, 50 50, 90' stroke='black'/>"));
    REQUIRE(drawing.strokes().size() == 1);
    CHECK(near(drawing.strokes()[0]->bounds, PrRect{10, 10, 50, 50}));
}

TEST_CASE("points in error draw the pairs before the error") {
    Drawing drawing(svgDocument("<polyline points='10 10 50 10 x 50 50' stroke='black'/>"));
    REQUIRE(drawing.strokes().size() == 1);
    CHECK(near(drawing.strokes()[0]->bounds, PrRect{10, 10, 50, 10}));
}
