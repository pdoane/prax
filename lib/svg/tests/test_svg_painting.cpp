// Painting (SVG2/painting.html): fills, strokes and their properties, context paint, markers and paint-order.

#include "svg/tests/svg_fixture.h"

namespace pr_impl {

namespace {

// A marker that draws a 2 by 2 square at its origin, whatever the marker's size, with nothing clipped.
constexpr const char* kDot = "<marker id='m' markerWidth='2' markerHeight='2' overflow='visible'>"
    "<rect width='2' height='2'/></marker>";

// Returns the squares that kDot's marker draws, which are the rects of a drawing that strokes nothing.
Array<const Drawn*> dots(const Drawing& drawing) {
    Array<const Drawn*> found;
    for (const Drawn* rect : drawing.of(PrDrawCommandKind_Rect))
        if (!rect->stroked && near(rect->bounds.right - rect->bounds.left, 2 * scaleOf(rect->transform), 1e-2f))
            found.push(rect);
    return found;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Fills and strokes

TEST_CASE("fill none and stroke none paint nothing, and the initial stroke is none") {
    CHECK(Drawing(svgDocument("<rect width='10' height='10' fill='none'/>")).commands.isEmpty());
    Drawing initial(svgDocument("<rect width='10' height='10' stroke-width='4'/>"));
    CHECK(initial.order() == "f");
}

TEST_CASE("fill-opacity and stroke-opacity multiply their paints' alpha, and opacity groups the element") {
    Drawing drawing(svgDocument("<rect width='10' height='10' fill='rgb(0 0 255 / 0.5)' fill-opacity='0.5' "
        "stroke='black' stroke-opacity='0.25' opacity='0.5'/>"));
    REQUIRE(drawing.order() == "fs");
    CHECK(near(drawing.commands[0].color.a, 0.25f));
    CHECK(near(drawing.commands[1].color.a, 0.25f));
    CHECK(near(drawing.commands[0].opacity, 0.5f));
}

TEST_CASE("fill-rule reaches the path that is filled") {
    Drawing drawing(svgDocument("<path d='M 0 0 H 50 V 50 H 0 Z M 10 10 H 40 V 40 H 10 Z' fill-rule='evenodd'/>"));
    REQUIRE(drawing.fills().count() == 1);
    CHECK(drawing.fills()[0]->rule == PrPathFillRule_EvenOdd);
}

TEST_CASE("a paint is currentColor, or the fallback of a reference that names no paint server") {
    Drawing current(svgDocument("<rect width='10' height='10' color='#00ff00' fill='currentColor'/>"));
    REQUIRE(current.fills().count() == 1);
    CHECK(current.fills()[0]->color.g == 1.0f);
    Drawing fallback(svgDocument("<rect width='10' height='10' fill='url(#missing) #ff0000'/>"));
    REQUIRE(fallback.fills().count() == 1);
    CHECK(fallback.fills()[0]->color.r == 1.0f);
    // A reference to nothing, without a fallback, paints nothing (SVG2/painting.html#SpecifyingPaint).
    CHECK(Drawing(svgDocument("<rect width='10' height='10' fill='url(#missing)'/>")).commands.isEmpty());
}

TEST_CASE("a reference to a gradient paints with its brush") {
    Drawing drawing(svgDocument("<linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue'/>"
        "</linearGradient><rect width='10' height='10' fill='url(#g) green'/>"));
    REQUIRE(drawing.fills().count() == 1);
    CHECK(drawing.fills()[0]->brushed);
}

TEST_CASE("stroke-width is a percentage of the normalized diagonal, and zero strokes nothing") {
    Drawing percent(svgDocument("<line x2='10' stroke='black' stroke-width='5%'/>"), PrVec2{300, 100});
    REQUIRE(percent.strokes().count() == 1);
    CHECK(near(percent.strokes()[0]->stroke.width, 5 * prF32Sqrt(5.0f), 1e-2f));
    CHECK(Drawing(svgDocument("<line x2='10' stroke='black' stroke-width='0'/>")).commands.isEmpty());
}

TEST_CASE("stroke-linecap, stroke-linejoin and stroke-miterlimit reach the stroke") {
    Drawing drawing(svgDocument("<polyline points='0 0 10 10 20 0' fill='none' stroke='black' "
        "stroke-linecap='square' stroke-linejoin='round' stroke-miterlimit='7'/>"));
    REQUIRE(drawing.strokes().count() == 1);
    const PrPathStroke& stroke = drawing.strokes()[0]->stroke;
    CHECK(stroke.startCap == PrPathCap_Square);
    CHECK(stroke.endCap == PrPathCap_Square);
    CHECK(stroke.join == PrPathJoin_Round);
    CHECK(stroke.miterLimit == 7.0f);
}

TEST_CASE("stroke-dasharray takes lengths and percentages, and dashes that sum to zero stroke solid") {
    Drawing drawing(svgDocument("<line x2='100' stroke='black' stroke-dasharray='5 10%' stroke-dashoffset='3'/>"));
    REQUIRE(drawing.strokes().count() == 1);
    const Drawn& stroke = *drawing.strokes()[0];
    REQUIRE(stroke.dashes.count() == 2);
    CHECK(near(stroke.dashes[0], 5));
    CHECK(near(stroke.dashes[1], 10));
    CHECK(near(stroke.stroke.dashOffset, 3));
    Drawing solid(svgDocument("<line x2='100' stroke='black' stroke-dasharray='0 0'/>"));
    REQUIRE(solid.strokes().count() == 1);
    CHECK(solid.strokes()[0]->dashes.isEmpty());
}

TEST_CASE("an odd number of dashes repeats, and a negative dash makes the list invalid") {
    // The stroke repeats an odd list itself (PrPathStroke::dashCount).
    Drawing odd(svgDocument("<line x2='100' stroke='black' stroke-dasharray='1 2 3'/>"));
    REQUIRE(odd.strokes().count() == 1);
    CHECK(odd.strokes()[0]->dashes.count() == 3);
    Drawing negative(svgDocument("<line x2='100' stroke='black' stroke-dasharray='1 -2'/>"));
    REQUIRE(negative.strokes().count() == 1);
    CHECK(negative.strokes()[0]->dashes.isEmpty());
}

TEST_CASE("vector-effect non-scaling-stroke strokes in the host's space") {
    Drawing drawing(svgDocument("<line x2='10' stroke='black' vector-effect='non-scaling-stroke'/>"));
    REQUIRE(drawing.strokes().count() == 1);
    CHECK(drawing.strokes()[0]->nonScaling);
}

TEST_CASE("shape-rendering crispEdges paints aliased edges") {
    Drawing crisp(svgDocument("<rect width='10' height='10' shape-rendering='crispEdges'/>"));
    REQUIRE(crisp.fills().count() == 1);
    CHECK_FALSE(crisp.fills()[0]->antialias);
    Drawing smooth(svgDocument("<rect width='10' height='10'/>"));
    CHECK(smooth.fills()[0]->antialias);
}

TEST_CASE("a hidden shape paints nothing, but a visible child of a hidden group paints") {
    CHECK(Drawing(svgDocument("<rect width='10' height='10' visibility='hidden'/>")).commands.isEmpty());
    Drawing drawing(svgDocument("<g visibility='hidden'><rect width='10' height='10'/>"
        "<rect width='5' height='5' visibility='visible'/></g>"));
    REQUIRE(drawing.fills().count() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{0, 0, 5, 5}));
}

// ---------------------------------------------------------------------------------------------------------------------
// paint-order

TEST_CASE("paint-order paints a shape's stroke before its fill") {
    Drawing drawing(svgDocument("<rect width='10' height='10' stroke='black' paint-order='stroke'/>"));
    CHECK(drawing.order() == "sf");
}

TEST_CASE("paint-order places markers among the fill and the stroke in the order written") {
    // markers, then the rest in their usual order: fill, then stroke.
    Drawing drawing(svgDocument(format("{}"
        "<path d='M 10 10 H 50' stroke='black' fill='none' marker-start='url(#m)' paint-order='markers'/>", kDot)));
    REQUIRE(drawing.commands.count() == 2);
    CHECK(drawing.commands[0].kind == PrDrawCommandKind_Rect);
    CHECK(drawing.commands[1].stroked);
    // stroke markers: the fill comes last.
    Drawing late(svgDocument(format("{}<path d='M 10 10 H 50 V 50' stroke='black' marker-start='url(#m)' "
        "paint-order='stroke markers'/>", kDot)));
    REQUIRE(late.commands.count() == 3);
    CHECK(late.commands[0].stroked);
    CHECK(late.commands[1].kind == PrDrawCommandKind_Rect);
    CHECK(late.commands[2].kind == PrDrawCommandKind_Path);
    CHECK_FALSE(late.commands[2].stroked);
}

// ---------------------------------------------------------------------------------------------------------------------
// Context paint

TEST_CASE("context paint without a context element paints nothing") {
    CHECK(Drawing(svgDocument("<rect width='10' height='10' fill='context-fill'/>")).commands.isEmpty());
}

TEST_CASE("a use element's paints are the context paint of its shadow tree") {
    Drawing drawing(svgDocument("<defs><rect id='r' width='10' height='10' fill='context-stroke' "
        "stroke='context-fill'/></defs><use href='#r' fill='#ff0000' stroke='#0000ff'/>"));
    REQUIRE(drawing.order() == "fs");
    CHECK(drawing.commands[0].color.b == 1.0f);
    CHECK(drawing.commands[1].color.r == 1.0f);
}

TEST_CASE("a context paint server paints in the context element's user space") {
    Drawing drawing(svgDocument("<linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue'/>"
        "</linearGradient><defs><rect id='r' width='10' height='10' fill='context-fill'/>"
        "</defs><use href='#r' fill='url(#g)'/>"));
    REQUIRE(drawing.fills().count() == 1);
    CHECK(drawing.fills()[0]->brushed);
}

// ---------------------------------------------------------------------------------------------------------------------
// Markers

TEST_CASE("markers draw at the vertices of a shape's equivalent path, a rect's included") {
    // The rect's vertices are its corners, and its last vertex is its first: a start, three mids, and an end on the
    // start, however many commands its path has.
    Drawing drawing(svgDocument("<marker id='m'><circle cx='1' cy='1' r='1'/></marker>"
        "<rect x='10' y='10' width='50' height='20' fill='none' marker-start='url(#m)' "
        "marker-mid='url(#m)' marker-end='url(#m)'/>"));
    CHECK(drawing.of(PrDrawCommandKind_Ellipse).count() == 5);
}

TEST_CASE("markers draw on a circle, an ellipse and a line") {
    // A circle's path is four arcs that close: a start, three mids, and an end on the start.
    CHECK(dots(Drawing(svgDocument(format("{}<circle cx='50' cy='50' r='20' marker-start='url(#m)' "
        "marker-mid='url(#m)' marker-end='url(#m)'/>", kDot)))).count() == 5);
    CHECK(dots(Drawing(svgDocument(format("{}<line x2='50' stroke='black' marker-start='url(#m)' marker-mid='url(#m)' "
        "marker-end='url(#m)'/>", kDot)))).count() == 2);
}

TEST_CASE("the marker shorthand sets the start, mid and end markers") {
    Drawing drawing(svgDocument(format("{}<polyline points='0 0 10 0 20 0 30 0' fill='none' style='marker: url(#m)'/>", kDot)));
    CHECK(dots(drawing).count() == 4);
}

TEST_CASE("a marker is placed at its vertex by its reference point") {
    Drawing drawing(svgDocument("<marker id='m' markerWidth='10' markerHeight='10' refX='5' refY='5'>"
        "<rect width='10' height='10'/></marker><path d='M 30 40 H 60' fill='none' marker-start='url(#m)'/>"));
    REQUIRE(drawing.of(PrDrawCommandKind_Rect).count() == 1);
    CHECK(near(drawing.of(PrDrawCommandKind_Rect)[0]->bounds, PrRect{25, 35, 35, 45}));
}

TEST_CASE("refX and refY take the keywords left, center, right, top and bottom") {
    Drawing drawing(svgDocument("<marker id='m' markerWidth='10' markerHeight='20' refX='right' refY='center'>"
        "<rect width='10' height='20'/></marker><path d='M 30 40 H 60' fill='none' marker-start='url(#m)'/>"));
    REQUIRE(drawing.of(PrDrawCommandKind_Rect).count() == 1);
    CHECK(near(drawing.of(PrDrawCommandKind_Rect)[0]->bounds, PrRect{20, 30, 30, 50}));
}

TEST_CASE("orient auto turns a marker to the path's direction, and auto-start-reverse turns the start around") {
    const char* m = "<marker id='m' orient='auto' overflow='visible'><rect width='2' height='2'/></marker>"
        "<marker id='r' orient='auto-start-reverse' overflow='visible'><rect width='2' height='2'/></marker>";
    Drawing drawing(svgDocument(format("{}<path d='M 10 10 L 50 50' fill='none' marker-start='url(#m)'/>"
        "<path d='M 10 10 L 50 50' fill='none' marker-start='url(#r)' marker-end='url(#r)'/>", m)));
    REQUIRE(drawing.commands.count() == 3);
    CHECK(near(angleOf(drawing.commands[0].transform), 45, 1e-2f));
    CHECK(near(angleOf(drawing.commands[1].transform), -135, 1e-2f));
    CHECK(near(angleOf(drawing.commands[2].transform), 45, 1e-2f));
}

TEST_CASE("orient takes an angle, which does not follow the path") {
    Drawing drawing(svgDocument("<marker id='m' orient='30deg' overflow='visible'><rect width='2' height='2'/>"
        "</marker><path d='M 10 10 L 50 50' fill='none' marker-start='url(#m)'/>"));
    REQUIRE(drawing.commands.count() == 1);
    CHECK(near(angleOf(drawing.commands[0].transform), 30, 1e-2f));
}

TEST_CASE("a closed subpath's first and last markers face half way between its closing and first segments") {
    // The square runs right, down, left and up to close, so its start faces half way between up and right.
    Drawing drawing(svgDocument("<marker id='m' orient='auto' overflow='visible'><rect width='2' height='2'/>"
        "</marker><path d='M 10 10 H 50 V 50 H 10 Z' fill='none' marker-start='url(#m)' marker-end='url(#m)'/>"));
    REQUIRE(drawing.commands.count() == 2);
    CHECK(near(angleOf(drawing.commands[0].transform), -45, 1e-2f));
    CHECK(near(angleOf(drawing.commands[1].transform), -45, 1e-2f));
}

TEST_CASE("markerUnits strokeWidth scales a marker by the stroke's width, and userSpaceOnUse does not") {
    const char* m = "<marker id='s' overflow='visible'><rect width='2' height='2'/></marker>"
        "<marker id='u' markerUnits='userSpaceOnUse' overflow='visible'><rect width='2' height='2'/></marker>";
    Drawing drawing(svgDocument(format("{}<path d='M 10 10 H 50' fill='none' stroke-width='4' marker-start='url(#s)' "
        "marker-end='url(#u)'/>", m)));
    REQUIRE(drawing.commands.count() == 2);
    CHECK(near(scaleOf(drawing.commands[0].transform), 4));
    CHECK(near(scaleOf(drawing.commands[1].transform), 1));
}

TEST_CASE("a marker on a non-scaling stroke is sized by the stroke's width in the host's space") {
    // The user space is scaled by 2, and the stroke of 3 is 3 wide in the host's space, so the marker's unit is 3.
    Drawing drawing(svgDocument("<marker id='m' overflow='visible'><rect width='2' height='2'/></marker>"
        "<g transform='scale(2)'><path d='M 10 10 H 20' fill='none' stroke-width='3' "
        "vector-effect='non-scaling-stroke' marker-start='url(#m)'/></g>"));
    REQUIRE(drawing.commands.count() == 1);
    CHECK(near(scaleOf(drawing.commands[0].transform), 3));
}

TEST_CASE("a marker's viewBox maps its content into markerWidth by markerHeight") {
    Drawing drawing(svgDocument("<marker id='m' viewBox='0 0 20 20' markerWidth='10' markerHeight='10' "
        "markerUnits='userSpaceOnUse'><rect width='20' height='20'/></marker>"
        "<path d='M 30 30 H 60' fill='none' marker-start='url(#m)'/>"));
    REQUIRE(drawing.of(PrDrawCommandKind_Rect).count() == 1);
    CHECK(near(drawing.of(PrDrawCommandKind_Rect)[0]->bounds, PrRect{30, 30, 40, 40}));
}

TEST_CASE("a marker clips its content to its viewport unless its overflow is visible") {
    Drawing hidden(svgDocument("<marker id='m'><rect width='2' height='2'/></marker>"
        "<path d='M 10 10 H 50' fill='none' marker-start='url(#m)'/>"));
    Drawing visible(svgDocument(format("{}<path d='M 10 10 H 50' fill='none' marker-start='url(#m)'/>", kDot)));
    REQUIRE(hidden.commands.count() == 1);
    REQUIRE(visible.commands.count() == 1);
    // The outermost svg element clips too.
    CHECK(hidden.commands[0].clips == visible.commands[0].clips + 1);
}

TEST_CASE("a marker draws even inside a subtree that is not displayed") {
    Drawing drawing(svgDocument(format("<g style='display: none'>{}</g>"
        "<path d='M 10 10 H 50' fill='none' marker-start='url(#m)'/>", kDot)));
    CHECK(dots(drawing).count() == 1);
}

TEST_CASE("a shape that is not visible draws no markers") {
    Drawing drawing(svgDocument(format("{}<path d='M 10 10 H 50' fill='none' visibility='hidden' "
        "marker-start='url(#m)'/>", kDot)));
    CHECK(drawing.commands.isEmpty());
}

TEST_CASE("a marker with no width or height draws nothing") {
    Drawing drawing(svgDocument("<marker id='m' markerWidth='0' overflow='visible'><rect width='2' height='2'/>"
        "</marker><path d='M 10 10 H 50' fill='none' marker-start='url(#m)'/>"));
    CHECK(drawing.commands.isEmpty());
}

TEST_CASE("a marker paints with its shape's paint as context paint, and does not draw inside itself") {
    // The marker's own content references the marker, which draws once at each vertex rather than without end.
    Drawing drawing(svgDocument("<marker id='m' overflow='visible'>"
        "<circle r='2' fill='context-stroke'/><path d='M 0 0 L 5 0' fill='none' marker-end='url(#m)'/>"
        "</marker><path d='M 10 10 L 50 10' stroke='#ff0000' marker-start='url(#m)' marker-end='url(#m)'/>"));
    Array<const Drawn*> circles = drawing.of(PrDrawCommandKind_Ellipse);
    REQUIRE(circles.count() == 2);
    for (const Drawn* circle : circles) {
        CHECK(circle->color.r == 1.0f);
        CHECK(circle->color.g == 0.0f);
    }
}

TEST_CASE("markers that draw each other nest to a bounded depth") {
    // Each marker draws the other on its content's line, so the nesting stops at a depth rather than without end.
    Drawing drawing(svgDocument("<marker id='a' overflow='visible'><path d='M 0 0 H 1' fill='none' marker-start='url(#b)'/>"
        "</marker><marker id='b' overflow='visible'><path d='M 0 0 H 1' "
        "marker-start='url(#a)'/></marker><path d='M 10 10 H 50' fill='none' marker-start='url(#a)'/>"));
    CHECK(drawing.commands.count() >= 1);
    CHECK(drawing.commands.count() <= 9);
}
