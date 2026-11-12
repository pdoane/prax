// Filters and blending of SVG content (filter-effects-1, compositing-1): filter elements and their primitives, filter
// functions, mix-blend-mode and isolation.

#include "svg/tests/svg_fixture.h"

namespace pr_impl {

namespace {

constexpr const char* kBlue = "0 0 255";
constexpr const char* kGreen = "0 128 0";
constexpr const char* kWhite = "255 255 255";

// Returns a drawing of a blue rectangle from 20 to 80 on each axis of a viewport of 100 by 100, with `attributes`,
// after `defs`.
String filtered(PrStringView defs, PrStringView attributes) {
    return svgDocument(format("{}<rect x='20' y='20' width='60' height='60' fill='#0000ff' {}/>", defs, attributes));
}

// Returns the groups of a drawing's list, in order.
Array<const PrDrawGroup*> groupsOf(const Drawing& drawing) {
    Array<const PrDrawGroup*> groups;
    const PrDrawCommand* all = prDrawListCommands(drawing.list);
    for (uint32_t i = 0; i < prDrawListCommandCount(drawing.list); ++i) {
        if (all[i].kind == PrDrawCommandKind_PushGroup)
            groups.push(all[i].group);
    }
    return groups;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// The filter element and its region

TEST_CASE("a filter region defaults to 10% of the bounding box past each side, and clips the result") {
    Drawing drawing(filtered("<filter id='f'><feFlood flood-color='green'/></filter>", "filter='url(#f)'"));
    CHECK(drawing.pixel(50, 50) == kGreen);
    CHECK(drawing.pixel(15, 15) == kGreen);
    CHECK(drawing.pixel(11, 50) == kWhite);
    CHECK(drawing.pixel(88, 88) == kWhite);
    REQUIRE(groupsOf(drawing).count() == 1);
    // The group's bounds are the filter region.
    CHECK(near(groupsOf(drawing)[0]->bounds, PrRect{14, 14, 86, 86}));
}

TEST_CASE("a filter region in user space, and primitive subregions in fractions of the bounding box") {
    Drawing user(filtered("<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='50' height='100'>"
        "<feFlood flood-color='green'/></filter>", "filter='url(#f)'"));
    CHECK(user.pixel(5, 90) == kGreen);
    CHECK(user.pixel(60, 50) == kWhite);
    Drawing box(filtered("<filter id='f' primitiveUnits='objectBoundingBox'>"
        "<feFlood flood-color='green' x='0.5' width='50%'/></filter>", "filter='url(#f)'"));
    // The flood starts half way across the box, and takes the filter region's y and height.
    CHECK(box.pixel(40, 50) == kWhite);
    CHECK(box.pixel(60, 50) == kGreen);
    CHECK(box.pixel(60, 16) == kGreen);
}

TEST_CASE("a filter without primitives, or with a region of no area, hides the element") {
    Drawing empty(filtered("<filter id='f'/>", "filter='url(#f)'"));
    CHECK(empty.commands.isEmpty());
    Drawing flat(filtered("<filter id='f' width='0'><feFlood/></filter>", "filter='url(#f)'"));
    CHECK(flat.commands.isEmpty());
}

TEST_CASE("a URL that names no filter element ignores the whole filter list") {
    Drawing missing(filtered("", "filter='url(#missing)'"));
    CHECK(missing.pixel(50, 50) == kBlue);
    CHECK(groupsOf(missing).isEmpty());
    Drawing other(filtered("<linearGradient id='g'/>", "style='filter: grayscale(1) url(#g)'"));
    CHECK(other.pixel(50, 50) == kBlue);
}

TEST_CASE("a filter takes its templates' attributes and primitives through href") {
    Drawing drawing(filtered("<filter id='a' filterUnits='userSpaceOnUse' x='0' y='0' width='100' height='100'>"
        "<feFlood flood-color='green'/></filter><filter id='b' href='#a'/>", "filter='url(#b)'"));
    CHECK(drawing.pixel(5, 5) == kGreen);
}

// ---------------------------------------------------------------------------------------------------------------------
// Primitives

TEST_CASE("an input names the closest preceding result, and a name of no preceding result is the implicit input") {
    const char* floods = "<feFlood flood-color='#ff0000' result='a'/><feFlood flood-color='green' result='a'/>";
    Drawing closest(filtered(format("<filter id='f'>{}<feOffset in='a'/></filter>", floods), "filter='url(#f)'"));
    CHECK(closest.pixel(50, 50) == kGreen);
    // A forward reference is no reference: the offset reads the flood before it.
    Drawing forward(filtered("<filter id='f'><feFlood flood-color='green'/><feOffset in='later'/>"
        "<feFlood result='later'/></filter><filter id='g'><feFlood flood-color='green'/>"
        "<feOffset in='later'/></filter>", "filter='url(#g)'"));
    CHECK(forward.pixel(50, 50) == kGreen);
    // On the first primitive, the implicit input is SourceGraphic.
    Drawing first(filtered("<filter id='f'><feOffset in='nothing' dx='10'/></filter>", "filter='url(#f)'"));
    CHECK(first.pixel(85, 50) == kBlue);
    CHECK(first.pixel(25, 50) == kWhite);
}

TEST_CASE("a primitive's subregion defaults to its inputs', and the filter region clips SourceGraphic") {
    // The offset's subregion is the flood's, so the moved flood is cut at x 40.
    Drawing drawing(filtered("<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='100' height='100'>"
        "<feFlood flood-color='green' x='20' width='20'/><feOffset dx='10'/></filter>", "filter='url(#f)'"));
    CHECK(drawing.pixel(35, 50) == kGreen);
    CHECK(drawing.pixel(45, 50) == kWhite);
    // A region of x 0 to 50 cuts the rectangle before the offset moves it.
    Drawing cut(filtered("<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='50' height='100'>"
        "<feOffset dx='-30'/></filter>", "filter='url(#f)'"));
    CHECK(cut.pixel(5, 50) == kBlue);
    CHECK(cut.pixel(25, 50) == kWhite);
}

TEST_CASE("flood-color resolves currentColor on the primitive, and inherits from the filter's ancestors") {
    Drawing drawing(filtered("<g color='green' fill='orange'><filter id='f'><feFlood flood-color='currentColor'/>"
        "</filter></g>", "color='#ff0000' filter='url(#f)'"));
    CHECK(drawing.pixel(50, 50) == kGreen);
    Drawing opacity(filtered("<filter id='f'><feFlood flood-color='#0000ff' flood-opacity='0.5'/></filter>",
        "filter='url(#f)'"));
    CHECK(opacity.pixel(50, 50) == "127 127 255");
}

TEST_CASE("primitives compute in linearRGB unless color-interpolation-filters says sRGB") {
    const char* half = "<feComponentTransfer><feFuncR type='linear' slope='0.5'/><feFuncG type='linear' slope='0.5'/>"
        "<feFuncB type='linear' slope='0.5'/></feComponentTransfer>";
    Drawing linear(svgDocument(format("<filter id='f'>{}"
        "</filter><rect width='100' height='100' fill='white' filter='url(#f)'/>", half)));
    CHECK(linear.pixel(50, 50) == "188 188 188");
    Drawing srgb(svgDocument(format("<filter id='f' color-interpolation-filters='sRGB'>{}"
        "</filter><rect width='100' height='100' fill='white' filter='url(#f)'/>", half)));
    CHECK(srgb.pixel(50, 50) == "128 128 128");
}

TEST_CASE("a primitive in error passes its input through, or draws transparent black") {
    // feColorMatrix with five values for a matrix passes its input through.
    Drawing matrix(filtered("<filter id='f'><feColorMatrix values='1 0 0 0 0'/></filter>", "filter='url(#f)'"));
    CHECK(matrix.pixel(50, 50) == kBlue);
    // A kernel of the wrong length, and lighting without a light, are transparent black.
    Drawing kernel(filtered("<filter id='f'><feConvolveMatrix kernelMatrix='1 1 1 1'/></filter>", "filter='url(#f)'"));
    CHECK(kernel.pixel(50, 50) == kWhite);
    Drawing light(filtered("<filter id='f'><feDiffuseLighting/></filter>", "filter='url(#f)'"));
    CHECK(light.pixel(50, 50) == kWhite);
    // A negative deviation is zero on its axis: no blur at all here.
    Drawing blur(filtered("<filter id='f'><feGaussianBlur stdDeviation='-4'/></filter>", "filter='url(#f)'"));
    CHECK(blur.pixel(21, 50) == kBlue);
    CHECK(blur.pixel(19, 50) == kWhite);
}

TEST_CASE("feImage draws an element moved by its subregion's origin, and a filter inside its own image hides") {
    Drawing image(filtered("<rect id='dot' width='10' height='10' fill='green'/>"
        "<filter id='f' filterUnits='userSpaceOnUse' x='50' y='50' width='50' height='50'>"
        "<feImage href='#dot'/></filter>", "filter='url(#f)'"));
    CHECK(image.pixel(55, 55) == kGreen);
    CHECK(image.pixel(65, 55) == kWhite);
    Drawing selfReferencing(svgDocument("<filter id='f'><feImage href='#r'/></filter>"
        "<rect id='r' x='20' y='20' width='60' height='60' fill='green' filter='url(#f)'/>"));
    CHECK(selfReferencing.pixel(50, 50) == kWhite);
}

TEST_CASE("FillPaint is the element's fill over the whole filter region") {
    Drawing drawing(svgDocument("<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='100' height='100'>"
        "<feMerge><feMergeNode in='FillPaint'/></feMerge></filter>"
        "<rect x='40' y='40' width='20' height='20' fill='green' filter='url(#f)'/>"));
    CHECK(drawing.pixel(5, 5) == kGreen);
}

TEST_CASE("BackgroundImage is what lies beneath in the nearest isolated group") {
    Drawing drawing(svgDocument("<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='100' height='100'>"
        "<feOffset in='BackgroundImage' dx='50'/></filter><g style='isolation: isolate'><rect width='40' height='40' fill='green'/>"
        "<rect width='1' height='1' fill='#0000ff' filter='url(#f)'/></g>"));
    CHECK(drawing.pixel(20, 20) == kGreen);
    CHECK(drawing.pixel(70, 20) == kGreen);
    CHECK(drawing.pixel(70, 60) == kWhite);
}

// ---------------------------------------------------------------------------------------------------------------------
// Filter functions and lists

TEST_CASE("filter functions apply to SVG content in sRGB, each reading the result of the item before it") {
    Drawing gray(filtered("", "style='filter: grayscale(1)'"));
    CHECK(gray.pixel(50, 50) == "18 18 18");
    Drawing chain(svgDocument("<filter id='f'><feFlood flood-color='#ff0000'/></filter>"
        "<rect x='20' y='20' width='60' height='60' fill='#0000ff' style='filter: url(#f) hue-rotate(180deg) invert(1)'/>"));
    CHECK(chain.pixel(50, 50) != kWhite);
    CHECK(chain.pixel(50, 50) != "255 0 0");
}

TEST_CASE("a filter applies before the clip path, so a blur is cut sharply") {
    Drawing drawing(filtered("<clipPath id='c'><rect x='20' y='20' width='60' height='60'/></clipPath>"
        "<filter id='f'><feGaussianBlur stdDeviation='4'/></filter>", "filter='url(#f)' clip-path='url(#c)'"));
    CHECK(drawing.pixel(18, 50) == kWhite);
    CHECK(drawing.pixel(21, 50) != kBlue);
    CHECK(drawing.pixel(21, 50) != kWhite);
}

// ---------------------------------------------------------------------------------------------------------------------
// Blending and isolation

TEST_CASE("mix-blend-mode blends an element with what lies beneath it, up to an isolated group") {
    const char* band = "<rect width='100' height='100' fill='#ff0000'/>";
    const char* multiply = "<rect x='20' y='20' width='60' height='60' fill='#00ff00' style='mix-blend-mode: multiply'/>";
    Drawing blended(svgDocument(format("{}{}", band, multiply)));
    CHECK(blended.pixel(50, 50) == "0 0 0");
    Drawing isolated(svgDocument(format("{}<g style='isolation: isolate'>{}</g>", band, multiply)));
    CHECK(isolated.pixel(50, 50) == "0 255 0");
    Drawing opacity(svgDocument(format("{}<g opacity='0.999'>{}</g>", band, multiply)));
    CHECK(opacity.pixel(50, 50) == "0 255 0");
    // An inner svg that clips is a stacking context, and isolates; one whose overflow is visible does not.
    Drawing inner(svgDocument(format("{}<svg width='100' height='100'>{}</svg>", band, multiply)));
    CHECK(inner.pixel(50, 50) == "0 255 0");
    Drawing visible(svgDocument(format("{}<svg width='100' height='100' overflow='visible'>{}</svg>", band, multiply)));
    CHECK(visible.pixel(50, 50) == "0 0 0");
}

TEST_CASE("a 3D transform function isolates its group, though its matrix is 2D, and a 2D transform does not") {
    const char* band = "<rect width='100' height='100' fill='#ff0000'/>";
    const char* multiply = "<rect x='20' y='20' width='60' height='60' fill='#00ff00' style='mix-blend-mode: multiply'/>";
    const char* const transforms[] = {"translateZ(0)", "rotateZ(0deg)", "scale3d(1, 1, 1)", "perspective(100px)"};
    for (const char* transform : transforms) {
        Drawing isolated(svgDocument(format("{}<g style='transform: {}'>{}</g>", band, transform, multiply)));
        CHECK(isolated.pixel(50, 50) == "0 255 0");
    }
    Drawing flat(svgDocument(format("{}<g style='transform: translate(0)'>{}</g>", band, multiply)));
    CHECK(flat.pixel(50, 50) == "0 0 0");
}

TEST_CASE("the root is an isolated group only when something blends or reads the backdrop") {
    Drawing plain(filtered("", ""));
    CHECK(groupsOf(plain).isEmpty());
    Drawing blended(filtered("", "style='mix-blend-mode: screen'"));
    Array<const PrDrawGroup*> groups = groupsOf(blended);
    REQUIRE(groups.count() == 2);
    CHECK(groups[0]->isolated);
    CHECK(groups[1]->blendMode == PrDrawBlendMode_Screen);
}
