// Paint servers (SVG2/pservers.html): patterns and their templates, how gradients mix their stops, and
// color-interpolation.

#include "svg/tests/svg_fixture.h"

namespace pr_impl {

namespace {

// Returns the brush of the only fill of a drawing, whose source is the list of a pattern's tile.
const PrDrawBrush* tileBrush(const Drawing& drawing) {
    REQUIRE(drawing.fills().size() == 1);
    const PrDrawBrush* brush = drawing.fills()[0]->brush;
    REQUIRE(brush != nullptr);
    REQUIRE(brush->kind == PrDrawBrushKind_Image);
    REQUIRE(brush->image.source.kind == PrDrawSourceKind_List);
    return brush;
}

// Returns the painting commands of the tile that a drawing's only fill repeats.
std::vector<Drawn> tileOf(const Drawing& drawing) {
    return Drawing::commandsOf(tileBrush(drawing)->image.source.list);
}

// Returns the red, green and blue of a pixel as numbers.
std::vector<int> rgb(const std::string& pixel) {
    std::vector<int> out;
    size_t at = 0;
    while (at < pixel.size()) {
        size_t end = pixel.find(' ', at);
        out.push_back(std::stoi(pixel.substr(at, end - at)));
        at = end == std::string::npos ? pixel.size() : end + 1;
    }
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Patterns

TEST_CASE("a pattern paints a tile of its content, repeated from x and y in user space") {
    Drawing drawing(svgDocument("<pattern id='p' x='5' y='5' width='20' height='10' patternUnits='userSpaceOnUse'>"
        "<rect width='10' height='10' fill='#0000ff'/></pattern><rect width='100' height='100' fill='url(#p) red'/>"));
    const PrDrawBrush* brush = tileBrush(drawing);
    CHECK(near(brush->image.tiles.tile, PrRect{0, 0, 20, 10}));
    CHECK(brush->image.tiles.x == PrDrawExtend_Repeat);
    CHECK(brush->image.tiles.y == PrDrawExtend_Repeat);
    CHECK(near(brush->transform.e, 5));
    CHECK(near(brush->transform.f, 5));
    // The content is clipped to the tile, and paints in its own color.
    std::vector<Drawn> tile = tileOf(drawing);
    REQUIRE(tile.size() == 1);
    CHECK(tile[0].clips == 1);
    CHECK(tile[0].color.b == 1.0f);
    CHECK(drawing.pixel(10, 10) == "0 0 255");
    CHECK(drawing.pixel(20, 10) == "255 255 255");
    CHECK(drawing.pixel(30, 10) == "0 0 255");
}

TEST_CASE("objectBoundingBox tiles are fractions of the bounding box, and the pattern's transform applies after") {
    Drawing drawing(svgDocument("<pattern id='p' x='0.5' width='25%' height='0.5' patternTransform='rotate(90)'>"
        "<rect width='5' height='5'/></pattern><rect x='20' y='10' width='40' height='20' fill='url(#p)'/>"));
    const PrDrawBrush* brush = tileBrush(drawing);
    CHECK(near(brush->image.tiles.tile, PrRect{0, 0, 10, 10}));
    // The tile's origin is at 40, 10 in user space, which the rotation turns about the user space's origin.
    PrVec2 origin = prAffine2MapPoint(brush->transform, PrVec2{0, 0});
    CHECK(near(origin.x, -10));
    CHECK(near(origin.y, 40));
    CHECK(near(angleOf(brush->transform), 90));
}

TEST_CASE("a style's transform on a pattern overrides patternTransform, of which it is the presentation attribute") {
    Drawing drawing(svgDocument("<style>#p { transform: scale(3) }</style>"
        "<pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse' "
        "patternTransform='scale(2)'><rect width='5' height='5'/></pattern>"
        "<rect width='100' height='100' fill='url(#p)'/>"));
    CHECK(near(scaleOf(tileBrush(drawing)->transform), 3));
}

TEST_CASE("a patternTransform that does not parse leaves the transform to the templates") {
    Drawing drawing(svgDocument("<pattern id='t' patternTransform='scale(2)'/>"
        "<pattern id='p' href='#t' patternTransform='' width='10' height='10' "
        "patternUnits='userSpaceOnUse'><rect width='5' height='5'/></pattern>"
        "<rect width='100' height='100' fill='url(#p)'/>"));
    CHECK(near(scaleOf(tileBrush(drawing)->transform), 2));
}

TEST_CASE("a paint server whose transform cannot be inverted paints the fallback") {
    Drawing pattern(svgDocument("<pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse' "
        "patternTransform='scale(0)'><rect width='5' height='5'/></pattern><rect width='100' height='100' fill='url(#p) #00ff00'/>"));
    REQUIRE(pattern.fills().size() == 1);
    CHECK(!pattern.fills()[0]->brushed);
    CHECK(pattern.fills()[0]->color.g == 1.0f);
    Drawing gradient(svgDocument("<linearGradient id='g' gradientTransform='matrix(0 0 0 0 0 0)'>"
        "<stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
        "<rect width='100' height='100' fill='url(#g) #00ff00'/>"));
    REQUIRE(gradient.fills().size() == 1);
    CHECK(gradient.fills()[0]->color.g == 1.0f);
}

TEST_CASE("patternContentUnits objectBoundingBox scales the content by the box, unless a viewBox fits it") {
    Drawing box(svgDocument("<pattern id='p' width='0.5' height='0.5' patternContentUnits='objectBoundingBox'>"
        "<rect width='0.25' height='0.25'/></pattern><rect width='40' height='20' fill='url(#p)'/>"));
    std::vector<Drawn> tile = tileOf(box);
    REQUIRE(tile.size() == 1);
    CHECK(near(tile[0].bounds, PrRect{0, 0, 10, 5}));
    Drawing fitted(svgDocument("<pattern id='p' width='0.5' height='0.5' patternContentUnits='objectBoundingBox' "
        "viewBox='0 0 10 10' preserveAspectRatio='none'><rect width='5' height='5'/></pattern>"
        "<rect width='40' height='20' fill='url(#p)'/>"));
    tile = tileOf(fitted);
    REQUIRE(tile.size() == 1);
    CHECK(near(tile[0].bounds, PrRect{0, 0, 10, 5}));
}

TEST_CASE("a pattern's viewBox and preserveAspectRatio fit its content into the tile") {
    Drawing drawing(svgDocument("<pattern id='p' width='40' height='20' patternUnits='userSpaceOnUse' "
        "viewBox='0 0 10 10'><rect width='10' height='10'/></pattern><rect width='100' height='100' fill='url(#p)'/>"));
    std::vector<Drawn> tile = tileOf(drawing);
    REQUIRE(tile.size() == 1);
    CHECK(near(tile[0].bounds, PrRect{10, 0, 30, 20}));
}

TEST_CASE("a pattern of no width or height, or with no content, paints nothing, not even its fallback") {
    CHECK(Drawing(svgDocument("<pattern id='p' height='10' patternUnits='userSpaceOnUse'><rect width='5' height='5'/>"
        "</pattern><rect width='100' height='100' fill='url(#p) red'/>")) .commands.empty());
    CHECK(Drawing(svgDocument("<pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse'><title>t</title>"
        "</pattern><rect width='100' height='100' fill='url(#p) red'/>")) .commands.empty());
}

TEST_CASE("a radial gradient whose start circle holds its end circle paints nothing, not even its fallback") {
    std::string stops = "<stop offset='0' stop-color='orange'/><stop offset='1' stop-color='blue'/></radialGradient>";
    CHECK(Drawing(svgDocument("<radialGradient id='g' fr='0.2' r='0.1'>" + stops +
        "<rect width='100' height='100' fill='url(#g) red'/>")) .commands.empty());
    CHECK(Drawing(svgDocument("<radialGradient id='g' fx='0.6' fr='0.25' r='0.1'>" + stops +
        "<rect width='100' height='100' fill='url(#g) red'/>")) .commands.empty());
    // A start circle that reaches past the end circle without holding it draws the cone that both touch.
    CHECK(Drawing(svgDocument("<radialGradient id='g' fx='0.75' fr='0.2' r='0.1'>" + stops +
        "<rect width='100' height='100' fill='url(#g) red'/>")) .commands.size() == 1);
}

TEST_CASE("a paint server in objectBoundingBox units on a bounding box of no height paints the fallback") {
    const char* servers = "<pattern id='p' width='1' height='1'><rect width='5' height='5'/></pattern>"
        "<linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>";
    Drawing pattern(svgDocument(std::string(servers) + "<line x2='50' stroke='url(#p) #00ff00'/>"));
    REQUIRE(pattern.strokes().size() == 1);
    CHECK(!pattern.strokes()[0]->brushed);
    CHECK(pattern.strokes()[0]->color.g == 1.0f);
    Drawing gradient(svgDocument(std::string(servers) + "<line x2='50' stroke='url(#g) #00ff00'/>"));
    REQUIRE(gradient.strokes().size() == 1);
    CHECK(gradient.strokes()[0]->color.g == 1.0f);
    CHECK(Drawing(svgDocument(std::string(servers) + "<line x2='50' stroke='url(#p)'/>")).commands.empty());
}

TEST_CASE("a pattern's content inherits from the pattern, not from what it paints") {
    Drawing drawing(svgDocument("<g fill='#0000ff'><pattern id='p' width='10' height='10' "
        "patternUnits='userSpaceOnUse'><rect width='5' height='5'/></pattern></g>"
        "<rect width='100' height='100' fill='url(#p)' color='red'/>"));
    std::vector<Drawn> tile = tileOf(drawing);
    REQUIRE(tile.size() == 1);
    CHECK(tile[0].color.b == 1.0f);
    CHECK(tile[0].color.r == 0.0f);
}

TEST_CASE("a pattern takes the attributes and the content that it lacks from its templates, the content inheriting from the pattern") {
    Drawing drawing(svgDocument("<pattern id='t' x='5' width='30' height='20' patternUnits='userSpaceOnUse' "
        "fill='#0000ff'><rect width='10' height='10'/></pattern>"
        "<pattern id='h' href='#t' fill='#00ff00'><desc>only a description</desc></pattern>"
        "<rect width='100' height='100' fill='url(#h)'/>"));
    const PrDrawBrush* brush = tileBrush(drawing);
    CHECK(near(brush->image.tiles.tile, PrRect{0, 0, 30, 20}));
    CHECK(near(brush->transform.e, 5));
    std::vector<Drawn> tile = tileOf(drawing);
    REQUIRE(tile.size() == 1);
    CHECK(tile[0].color.g == 1.0f);
    CHECK(tile[0].color.b == 0.0f);
    // A pattern with content of its own keeps it, and takes only the attributes.
    Drawing own(svgDocument("<pattern id='t' width='30' height='20' patternUnits='userSpaceOnUse'>"
        "<rect width='10' height='10' fill='red'/></pattern>"
        "<pattern id='h' href='#t'><circle r='5' fill='blue'/></pattern>"
        "<rect width='100' height='100' fill='url(#h)'/>"));
    tile = tileOf(own);
    REQUIRE(tile.size() == 1);
    CHECK(tile[0].kind == PrDrawCommandKind_Ellipse);
}

TEST_CASE("a pattern inside its own tile paints its fallback, and patterns that paint each other nest to a bounded depth") {
    Drawing selfReferencing(svgDocument("<pattern id='p' width='20' height='20' patternUnits='userSpaceOnUse'>"
        "<rect width='10' height='10' fill='url(#p) #ff0000'/></pattern>"
        "<rect width='100' height='100' fill='url(#p)'/>"));
    std::vector<Drawn> tile = tileOf(selfReferencing);
    REQUIRE(tile.size() == 1);
    CHECK(!tile[0].brushed);
    CHECK(tile[0].color.r == 1.0f);
    // Each draws the other, which then paints the first's fallback.
    Drawing mutual(svgDocument("<pattern id='a' width='20' height='20' patternUnits='userSpaceOnUse'>"
        "<rect width='20' height='20' fill='url(#b) red'/></pattern>"
        "<pattern id='b' width='10' height='10' patternUnits='userSpaceOnUse'>"
        "<rect width='5' height='5' fill='url(#a) #00ff00'/></pattern>"
        "<rect width='100' height='100' fill='url(#a)'/>"));
    tile = tileOf(mutual);
    REQUIRE(tile.size() == 1);
    REQUIRE(tile[0].brushed);
    std::vector<Drawn> inner = Drawing::commandsOf(tile[0].brush->image.source.list);
    REQUIRE(inner.size() == 1);
    CHECK(inner[0].color.g == 1.0f);
}

TEST_CASE("a pattern strokes, paints text, and paints as a context element's paint") {
    const char* pattern = "<pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse'>"
        "<rect width='5' height='10' fill='blue'/></pattern>";
    Drawing stroked(svgDocument(std::string(pattern) +
        "<circle cx='50' cy='50' r='30' fill='none' stroke='url(#p)' stroke-width='12'/>"));
    REQUIRE(stroked.strokes().size() == 1);
    CHECK(stroked.strokes()[0]->brush->kind == PrDrawBrushKind_Image);
    // Text in an SVG font, whose glyph is a square path.
    Drawing text(svgDocument(std::string(pattern) +
        "<font horiz-adv-x='500'><font-face font-family='Squares' units-per-em='1000'/>"
        "<glyph unicode='a' d='M0 0h1000v1000z'/></font>"
        "<text y='50' font-family='Squares' font-size='20' fill='url(#p)'>a</text>"));
    REQUIRE(text.fills().size() == 1);
    CHECK(text.fills()[0]->brush->kind == PrDrawBrushKind_Image);
    Drawing context(svgDocument(std::string(pattern) +
        "<defs><rect id='r' width='20' height='20' fill='context-fill'/></defs>"
        "<use href='#r' x='30' fill='url(#p)'/>"));
    REQUIRE(context.fills().size() == 1);
    CHECK(context.fills()[0]->brush->kind == PrDrawBrushKind_Image);
}

TEST_CASE("shapes that a pattern paints in the same space share its tile") {
    Drawing drawing(svgDocument("<pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse'>"
        "<rect width='5' height='5'/></pattern><rect width='40' height='40' fill='url(#p)'/><rect x='50' width='40' height='40' "
        "fill='url(#p)'/><rect y='50' width='40' height='40' fill='url(#p)' transform='scale(2)'/>"));
    std::vector<const Drawn*> fills = drawing.fills();
    REQUIRE(fills.size() == 3);
    CHECK(fills[0]->brush->image.source.list == fills[1]->brush->image.source.list);
    CHECK(fills[0]->brush->image.source.list != fills[2]->brush->image.source.list);
}

TEST_CASE("percentages in a pattern's content are of the viewport, not of the tile") {
    Drawing drawing(svgDocument("<pattern id='p' width='40' height='20' patternUnits='userSpaceOnUse'>"
        "<rect width='50%' height='10'/></pattern><rect width='100' height='100' fill='url(#p)'/>"));
    std::vector<Drawn> tile = tileOf(drawing);
    REQUIRE(tile.size() == 1);
    CHECK(near(tile[0].bounds, PrRect{0, 0, 50, 10}));
}

// ---------------------------------------------------------------------------------------------------------------------
// Gradients' stops

TEST_CASE("a gradient's stops toward transparent mix toward black, as their colors mix apart from their alpha") {
    Drawing drawing(svgDocument("<linearGradient id='g'><stop stop-color='#ff8000'/>"
        "<stop offset='1' stop-color='transparent'/></linearGradient>"
        "<rect width='100' height='10' fill='url(#g)'/>"), PrVec2{100, 10});
    // Half way, orange half toward black at half alpha, over white.
    std::vector<int> mid = rgb(drawing.pixel(50, 5));
    CHECK(std::abs(mid[0] - 191) <= 3);
    CHECK(std::abs(mid[1] - 160) <= 3);
    CHECK(std::abs(mid[2] - 128) <= 3);
}

TEST_CASE("the stops that a gradient takes from a template inherit from the gradient") {
    Drawing drawing(svgDocument("<linearGradient id='t' color='#0000ff'><stop stop-color='currentColor'/>"
        "</linearGradient><linearGradient id='g' href='#t' color='#ff0000'/>"
        "<rect width='10' height='10' fill='url(#g)'/>"), PrVec2{10, 10});
    CHECK(drawing.pixel(5, 5) == "255 0 0");
}

// ---------------------------------------------------------------------------------------------------------------------
// color-interpolation

TEST_CASE("color-interpolation linearRGB mixes a gradient's stops in linear light, the referenced gradient's value deciding") {
    const char* stops = "<stop stop-color='black'/><stop offset='1' stop-color='white'/>";
    Drawing srgb(svgDocument(std::string("<linearGradient id='g'>") + stops +
        "</linearGradient><rect width='101' height='10' fill='url(#g)'/>"), PrVec2{101, 10});
    CHECK(std::abs(rgb(srgb.pixel(50, 5))[0] - 128) <= 2);
    Drawing linear(svgDocument(std::string("<linearGradient id='g' color-interpolation='linearRGB'>") + stops +
        "</linearGradient><rect width='101' height='10' fill='url(#g)'/>"), PrVec2{101, 10});
    CHECK(std::abs(rgb(linear.pixel(50, 5))[0] - 188) <= 2);
    Drawing referring(svgDocument(std::string("<linearGradient id='t'>") + stops +
        "</linearGradient><linearGradient id='g' href='#t' "
        "color-interpolation='linearRGB'/><rect width='101' height='10' fill='url(#g)'/>"), PrVec2{101, 10});
    CHECK(std::abs(rgb(referring.pixel(50, 5))[0] - 188) <= 2);
}

TEST_CASE("color-interpolation linearRGB composites an element onto what lies beneath in linear light") {
    const char* black = "<rect width='10' height='10' fill='black'/>";
    Drawing srgb(svgDocument(std::string(black) + "<rect width='10' height='10' fill='white' fill-opacity='0.5'/>"),
        PrVec2{10, 10});
    CHECK(srgb.pixel(5, 5) == "128 128 128");
    Drawing linear(svgDocument(std::string(black) + "<rect width='10' height='10' fill='white' fill-opacity='0.5' "
        "color-interpolation='linearRGB'/>"), PrVec2{10, 10});
    CHECK(linear.pixel(5, 5) == "188 188 188");
    // A group's opacity composites its result in its own space.
    Drawing group(svgDocument(std::string(black) + "<g opacity='0.5' color-interpolation='linearRGB'>"
        "<rect width='10' height='10' fill='white'/></g>"), PrVec2{10, 10});
    CHECK(group.pixel(5, 5) == "188 188 188");
}
