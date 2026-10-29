// Clipping and masking of SVG content (css-masking-1): clip-path with clipPath elements, basic shapes and reference
// boxes, and mask layers with mask elements.

#include "svg/tests/svg_fixture.h"

namespace pr_impl {

namespace {

constexpr const char* kBlue = "0 0 255";
constexpr const char* kWhite = "255 255 255";

// Returns a drawing of a blue rectangle that fills a viewport of 100 by 100, with `attributes`, after `defs`.
std::string clipped(const std::string& defs, const std::string& attributes) {
    return svgDocument(defs + "<rect width='100' height='100' fill='#0000ff' " + attributes + "/>");
}

// Returns the number of commands of `kind` in a drawing's list.
uint32_t countOf(const Drawing& drawing, PrDrawCommandKind kind) {
    uint32_t count = 0;
    const PrDrawCommand* all = prDrawListCommands(drawing.list);
    for (uint32_t i = 0; i < prDrawListCommandCount(drawing.list); ++i)
        count += all[i].kind == kind ? 1 : 0;
    return count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// The clipPath element

TEST_CASE("a clipPath of one shape is a clip of the list, in the user space of the element it clips") {
    Drawing drawing(svgDocument("<clipPath id='c'><rect x='10' y='20' width='30' height='40'/></clipPath>"
                                "<g transform='translate(5 5)'><rect width='100' height='100' fill='#0000ff' "
                                "clip-path='url(#c)'/></g>"));
    CHECK(countOf(drawing, PrDrawCommandKind_PushClipPath) == 1);
    CHECK(countOf(drawing, PrDrawCommandKind_PushGroup) == 0);
    CHECK(drawing.pixel(20, 30) == kBlue);
    CHECK(drawing.pixel(12, 22) == kWhite);
    CHECK(drawing.pixel(44, 64) == kBlue);
    CHECK(drawing.pixel(50, 70) == kWhite);
}

TEST_CASE("a clipPath's children are OR'd, and each takes its own clip-rule") {
    Drawing drawing(clipped("<clipPath id='c'><rect width='20' height='20'/>"
                            "<path clip-rule='evenodd' d='M 40 0 h 40 v 40 h -40 Z M 50 10 h 20 v 20 h -20 Z'/>"
                            "</clipPath>",
                            "clip-path='url(#c)'"));
    // Several children are the coverage of their silhouettes, which masks a group.
    CHECK(countOf(drawing, PrDrawCommandKind_PushGroup) == 1);
    CHECK(drawing.pixel(10, 10) == kBlue);
    CHECK(drawing.pixel(45, 5) == kBlue);
    CHECK(drawing.pixel(60, 20) == kWhite);
    CHECK(drawing.pixel(30, 10) == kWhite);
}

TEST_CASE("clip-rule inherits into a clipPath from its ancestors, not from the element that it clips") {
    std::string path = "<path d='M 10 10 h 60 v 60 h -60 Z M 30 30 h 20 v 20 h -20 Z'/>";
    Drawing inherited(clipped("<g clip-rule='evenodd'><clipPath id='c'>" + path + "</clipPath></g>",
                              "clip-path='url(#c)'"));
    CHECK(inherited.pixel(40, 40) == kWhite);
    CHECK(inherited.pixel(20, 20) == kBlue);
    Drawing referencing(clipped("<clipPath id='c'>" + path + "</clipPath>", "clip-rule='evenodd' clip-path='url(#c)'"));
    CHECK(referencing.pixel(40, 40) == kBlue);
}

TEST_CASE("a hidden child of a clipPath does not contribute, and an empty clipPath clips everything away") {
    Drawing hidden(clipped("<clipPath id='c'><rect width='20' height='20'/>"
                           "<rect x='50' width='20' height='20' visibility='hidden'/></clipPath>",
                           "clip-path='url(#c)'"));
    CHECK(hidden.pixel(10, 10) == kBlue);
    CHECK(hidden.pixel(60, 10) == kWhite);
    Drawing empty(clipped("<clipPath id='c'/>", "clip-path='url(#c)'"));
    CHECK(empty.commands.empty());
    Drawing none(clipped("<clipPath id='c'><rect width='20' height='20' display='none'/></clipPath>",
                         "clip-path='url(#c)'"));
    CHECK(none.commands.empty());
}

TEST_CASE("a clipPath inside display none still clips") {
    Drawing drawing(clipped("<g display='none'><clipPath id='c'><rect width='20' height='20'/></clipPath></g>",
                            "clip-path='url(#c)'"));
    CHECK(drawing.pixel(10, 10) == kBlue);
    CHECK(drawing.pixel(50, 50) == kWhite);
}

TEST_CASE("a reference to nothing or to an element that is not a clipPath clips nothing") {
    Drawing missing(clipped("", "clip-path='url(#nothing)'"));
    CHECK(missing.pixel(90, 90) == kBlue);
    Drawing other(clipped("<mask id='m'/>", "clip-path='url(#m)'"));
    CHECK(other.pixel(90, 90) == kBlue);
}

TEST_CASE("clipPathUnits objectBoundingBox places the content in fractions of the bounding box") {
    Drawing drawing(svgDocument("<clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='0.5' height='0.5'/>"
                                "</clipPath><rect x='20' y='20' width='60' height='40' fill='#0000ff' "
                                "clip-path='url(#c)'/>"));
    CHECK(drawing.pixel(30, 30) == kBlue);
    CHECK(drawing.pixel(55, 30) == kWhite);
    CHECK(drawing.pixel(30, 45) == kWhite);
}

TEST_CASE("a clipPath's transform and its own clip-path apply to its content") {
    Drawing transformed(clipped("<clipPath id='c' transform='translate(50 0)'><rect width='20' height='20'/>"
                                "</clipPath>",
                                "clip-path='url(#c)'"));
    CHECK(transformed.pixel(60, 10) == kBlue);
    CHECK(transformed.pixel(10, 10) == kWhite);
    Drawing own(clipped("<clipPath id='cut'><rect width='30' height='100'/></clipPath>"
                        "<clipPath id='c' clip-path='url(#cut)'><rect width='20' height='20'/>"
                        "<rect x='50' width='20' height='20'/></clipPath>",
                        "clip-path='url(#c)'"));
    CHECK(own.pixel(10, 10) == kBlue);
    CHECK(own.pixel(60, 10) == kWhite);
}

TEST_CASE("a clip-path on a child of a clipPath clips that child before the silhouettes are OR'd") {
    Drawing drawing(clipped("<clipPath id='cut'><rect width='10' height='100'/></clipPath>"
                            "<clipPath id='c'><rect width='20' height='20' clip-path='url(#cut)'/>"
                            "<rect x='50' width='20' height='20'/></clipPath>",
                            "clip-path='url(#c)'"));
    CHECK(drawing.pixel(5, 10) == kBlue);
    CHECK(drawing.pixel(15, 10) == kWhite);
    CHECK(drawing.pixel(60, 10) == kBlue);
}

TEST_CASE("a use child of a clipPath contributes only if it references a shape or text directly") {
    Drawing shape(clipped("<defs><rect id='r' width='20' height='20'/></defs>"
                          "<clipPath id='c'><use href='#r' x='10'/></clipPath>",
                          "clip-path='url(#c)'"));
    CHECK(shape.pixel(20, 10) == kBlue);
    CHECK(shape.pixel(5, 10) == kWhite);
    Drawing group(clipped("<defs><g id='g'><rect width='20' height='20'/></g></defs>"
                          "<clipPath id='c'><use href='#g'/></clipPath>",
                          "clip-path='url(#c)'"));
    CHECK(group.commands.empty());
}

TEST_CASE("a clipPath's fill, stroke, opacity and mask do not reach its silhouettes") {
    Drawing drawing(clipped("<mask id='m'/><clipPath id='c'><rect width='20' height='20' fill='none' stroke='red' "
                            "stroke-width='20' opacity='0' mask='url(#m)'/></clipPath>",
                            "clip-path='url(#c)'"));
    CHECK(drawing.pixel(10, 10) == kBlue);
    CHECK(drawing.pixel(25, 10) == kWhite);
}

TEST_CASE("a clipPath that references itself ignores the reference") {
    Drawing drawing(clipped("<clipPath id='c' clip-path='url(#c)'><rect width='20' height='20'/></clipPath>",
                            "clip-path='url(#c)'"));
    CHECK(drawing.pixel(10, 10) == kBlue);
    CHECK(drawing.pixel(50, 50) == kWhite);
    Drawing child(clipped("<clipPath id='c'><rect width='20' height='20' clip-path='url(#c)'/>"
                          "<rect x='50' width='20' height='20'/></clipPath>",
                          "clip-path='url(#c)'"));
    CHECK(child.pixel(10, 10) == kBlue);
    CHECK(child.pixel(60, 10) == kBlue);
}

TEST_CASE("text in a clipPath clips by its glyphs") {
    // A font whose glyph `a` is a square of half an em, set at an em of 40.
    std::string font = "<font horiz-adv-x='1000'><font-face font-family='Squares' units-per-em='1000'/>"
                       "<glyph unicode='a' d='M0 0h500v500z'/></font>";
    Drawing drawing(clipped("<defs>" + font + "</defs><clipPath id='c'><text x='10' y='50' font-family='Squares' "
                            "font-size='40' fill='none'>aa</text></clipPath>",
                            "clip-path='url(#c)'"));
    CHECK(countOf(drawing, PrDrawCommandKind_PushGroup) == 1);
    CHECK(drawing.pixel(20, 40) == kBlue);
    CHECK(drawing.pixel(40, 40) == kWhite);
    CHECK(drawing.pixel(60, 40) == kBlue);
    CHECK(drawing.pixel(20, 20) == kWhite);
}

// ---------------------------------------------------------------------------------------------------------------------
// Basic shapes and reference boxes

TEST_CASE("a basic shape clips in the stroke box by default, and in the box that it names") {
    std::string rect = "<rect x='20' y='20' width='60' height='60' fill='#0000ff' stroke='#0000ff' stroke-width='10' ";
    Drawing border(svgDocument(rect + "style='clip-path: inset(0)'/>"));
    CHECK(border.pixel(17, 50) == kBlue);
    Drawing fill(svgDocument(rect + "style='clip-path: inset(0) fill-box'/>"));
    CHECK(fill.pixel(17, 50) == kWhite);
    CHECK(fill.pixel(22, 50) == kBlue);
    // content-box is fill-box for an element without a CSS box.
    Drawing content(svgDocument(rect + "style='clip-path: inset(0) content-box'/>"));
    CHECK(content.pixel(17, 50) == kWhite);
    Drawing circle(svgDocument(rect + "style='clip-path: circle(50%) fill-box'/>"));
    CHECK(circle.pixel(50, 50) == kBlue);
    CHECK(circle.pixel(23, 23) == kWhite);
}

TEST_CASE("a reference box alone clips to the box, and view-box is the nearest viewport") {
    Drawing drawing(svgDocument("<rect x='20' y='20' width='60' height='60' fill='#0000ff' stroke='#0000ff' "
                                "stroke-width='20' style='clip-path: fill-box'/>"));
    CHECK(drawing.pixel(15, 50) == kWhite);
    CHECK(drawing.pixel(25, 50) == kBlue);
    Drawing view(svgDocument("<rect x='-10' y='-10' width='200' height='200' fill='#0000ff' "
                             "style='clip-path: inset(0 50% 0 0) view-box'/>"));
    CHECK(view.pixel(40, 50) == kBlue);
    CHECK(view.pixel(60, 50) == kWhite);
}

TEST_CASE("the stroke box of a line bounds its stroke's shape") {
    Drawing drawing(svgDocument("<line x1='20' y1='30' x2='80' y2='30' stroke='#0000ff' stroke-width='10' "
                                "stroke-linejoin='round' style='clip-path: inset(0 0 50% 0) stroke-box'/>"));
    CHECK(drawing.pixel(50, 27) == kBlue);
    CHECK(drawing.pixel(50, 32) == kWhite);
}

TEST_CASE("a container's boxes are the union of its children's") {
    Drawing drawing(svgDocument("<g style='clip-path: inset(0 50% 0 0) fill-box'>"
                                "<rect x='10' y='10' width='20' height='20' fill='#0000ff'/>"
                                "<rect x='70' y='10' width='20' height='20' fill='#0000ff'/></g>"));
    CHECK(drawing.pixel(20, 20) == kBlue);
    CHECK(drawing.pixel(80, 20) == kWhite);
}

// ---------------------------------------------------------------------------------------------------------------------
// The mask element

TEST_CASE("a mask is luminance by default, and alpha by mask-type") {
    std::string content = "<rect width='50' height='100' fill='black'/><rect x='50' width='50' height='100' "
                          "fill='white'/>";
    Drawing luminance(clipped("<mask id='m' maskUnits='userSpaceOnUse'>" + content + "</mask>", "mask='url(#m)'"));
    CHECK(luminance.pixel(25, 50) == kWhite);
    CHECK(luminance.pixel(75, 50) == kBlue);
    CHECK(luminance.pixel(50, 50) == kBlue);
    CHECK(luminance.pixel(99, 50) == kBlue);
    CHECK(luminance.pixel(75, 0) == kBlue);
    // A region that does not start on a whole unit, here -3.3, leaves the edges of the masked content sharp.
    Drawing fraction(svgDocument("<mask id='m'><rect width='100%' height='100%' fill='white'/></mask>"
                                 "<rect width='33' height='100' fill='#0000ff' mask='url(#m)'/>"));
    CHECK(fraction.pixel(0, 50) == kBlue);
    CHECK(fraction.pixel(32, 50) == kBlue);
    Drawing alpha(clipped("<mask id='m' maskUnits='userSpaceOnUse' style='mask-type: alpha'>" + content + "</mask>",
                          "mask='url(#m)'"));
    CHECK(alpha.pixel(25, 50) == kBlue);
    CHECK(alpha.pixel(75, 50) == kBlue);
}

TEST_CASE("mask-mode overrides a mask's mask-type") {
    std::string mask = "<mask id='m' maskUnits='userSpaceOnUse'><rect width='50' height='100' fill='black'/></mask>";
    Drawing alpha(svgDocument(mask + "<rect width='100' height='100' fill='#0000ff' "
                                     "style='mask-image: url(#m); mask-mode: alpha'/>"));
    CHECK(alpha.pixel(25, 50) == kBlue);
    CHECK(alpha.pixel(75, 50) == kWhite);
}

TEST_CASE("a mask's region defaults to -10% and 120% of the bounding box, and a width of zero hides the element") {
    std::string content = "<rect x='-1000' y='-1000' width='2000' height='2000' fill='white'/>";
    Drawing whole(svgDocument("<mask id='m'>" + content + "</mask><rect x='20' y='20' width='50' height='50' "
                              "fill='#0000ff' stroke='#0000ff' stroke-width='16' mask='url(#m)'/>"));
    // The region runs from 15 to 80 in each axis, which cuts the stroke's outer edge at 12.
    CHECK(whole.pixel(17, 40) == kBlue);
    CHECK(whole.pixel(13, 40) == kWhite);
    Drawing zero(clipped("<mask id='m' width='0'>" + content + "</mask>", "mask='url(#m)'"));
    CHECK(zero.commands.empty());
}

TEST_CASE("maskContentUnits objectBoundingBox draws the content in fractions of the bounding box") {
    Drawing drawing(svgDocument("<mask id='m' maskContentUnits='objectBoundingBox'><rect width='0.5' height='1' "
                                "fill='white'/></mask><rect x='20' y='20' width='60' height='60' fill='#0000ff' "
                                "mask='url(#m)'/>"));
    CHECK(drawing.pixel(30, 50) == kBlue);
    CHECK(drawing.pixel(70, 50) == kWhite);
}

TEST_CASE("a mask's content resolves currentColor in its own tree, and its opacity and display do not apply") {
    std::string content = "<rect width='50' height='100' fill='currentColor'/>";
    Drawing current(svgDocument("<g color='white'><mask id='m' maskUnits='userSpaceOnUse'>" + content +
                                "</mask></g><rect width='100' height='100' fill='#0000ff' color='black' "
                                "mask='url(#m)'/>"));
    CHECK(current.pixel(25, 50) == kBlue);
    CHECK(current.pixel(75, 50) == kWhite);
    Drawing faded(svgDocument("<mask id='m' maskUnits='userSpaceOnUse' opacity='0' display='none'>"
                              "<rect width='50' height='100' fill='white'/></mask>"
                              "<rect width='100' height='100' fill='#0000ff' mask='url(#m)'/>"));
    CHECK(faded.pixel(25, 50) == kBlue);
}

TEST_CASE("a reference to nothing or to an element that is not a mask masks nothing, and none is transparent") {
    Drawing missing(clipped("", "mask='url(#nothing)'"));
    CHECK(missing.pixel(50, 50) == kBlue);
    Drawing other(clipped("<clipPath id='c'/>", "mask='url(#c)'"));
    CHECK(other.pixel(50, 50) == kBlue);
    // A none layer beside another counts as transparent black, so intersecting with it makes the whole mask
    // transparent.
    Drawing none(svgDocument("<mask id='m' maskUnits='userSpaceOnUse'><rect width='100' height='100' fill='white'/>"
                             "</mask><rect width='100' height='100' fill='#0000ff' style='mask-image: none, url(#m); "
                             "mask-composite: intersect'/>"));
    CHECK(none.pixel(50, 50) == kWhite);
}

TEST_CASE("mask layers composite from the bottom up by mask-composite") {
    std::string defs = "<mask id='left' maskUnits='userSpaceOnUse'><rect width='60' height='100' fill='white'/></mask>"
                       "<mask id='right' maskUnits='userSpaceOnUse'><rect x='40' width='60' height='100' "
                       "fill='white'/></mask>";
    auto drawn = [&](const std::string& composite) {
        return svgDocument(defs + "<rect width='100' height='100' fill='#0000ff' style='mask-image: url(#left), "
                                  "url(#right); mask-composite: " + composite + "'/>");
    };
    Drawing add(drawn("add"));
    CHECK(add.pixel(20, 50) == kBlue);
    CHECK(add.pixel(80, 50) == kBlue);
    Drawing intersect(drawn("intersect"));
    CHECK(intersect.pixel(20, 50) == kWhite);
    CHECK(intersect.pixel(50, 50) == kBlue);
    Drawing subtract(drawn("subtract"));
    CHECK(subtract.pixel(20, 50) == kBlue);
    CHECK(subtract.pixel(50, 50) == kWhite);
    CHECK(subtract.pixel(80, 50) == kWhite);
    Drawing exclude(drawn("exclude"));
    CHECK(exclude.pixel(20, 50) == kBlue);
    CHECK(exclude.pixel(50, 50) == kWhite);
    CHECK(exclude.pixel(80, 50) == kBlue);
}

TEST_CASE("nested masks and clip paths apply inside a mask's content") {
    Drawing drawing(svgDocument("<clipPath id='c'><rect width='30' height='100'/></clipPath>"
                                "<mask id='m' maskUnits='userSpaceOnUse'><rect width='100' height='100' fill='white' "
                                "clip-path='url(#c)'/></mask><rect width='100' height='100' fill='#0000ff' "
                                "mask='url(#m)'/>"));
    CHECK(drawing.pixel(20, 50) == kBlue);
    CHECK(drawing.pixel(50, 50) == kWhite);
}

TEST_CASE("a mask that references itself from its content ignores the reference") {
    Drawing drawing(svgDocument("<mask id='m' maskUnits='userSpaceOnUse'><rect width='50' height='100' fill='white' "
                                "mask='url(#m)'/></mask><rect width='100' height='100' fill='#0000ff' "
                                "mask='url(#m)'/>"));
    CHECK(drawing.pixel(25, 50) == kBlue);
    CHECK(drawing.pixel(75, 50) == kWhite);
}

TEST_CASE("the clip path and the mask apply around the opacity of the element") {
    Drawing drawing(svgDocument("<clipPath id='c'><rect width='50' height='100'/></clipPath>"
                                "<mask id='m' maskUnits='userSpaceOnUse'><rect width='100' height='50' "
                                "fill='white'/></mask><rect width='100' height='100' fill='#0000ff' opacity='0.5' "
                                "clip-path='url(#c)' mask='url(#m)'/>"));
    CHECK(drawing.pixel(25, 25) == "127 127 255");
    CHECK(drawing.pixel(75, 25) == kWhite);
    CHECK(drawing.pixel(25, 75) == kWhite);
}

// ---------------------------------------------------------------------------------------------------------------------
// Mask layers of images

TEST_CASE("a gradient masks by its alpha, over the stroke box by default") {
    Drawing drawing(svgDocument("<rect x='20' y='20' width='60' height='60' fill='#0000ff' stroke='#0000ff' "
                                "stroke-width='20' style='mask-image: linear-gradient(to right, black 50%, "
                                "transparent 50%)'/>"));
    // The layer is placed in the fill box, 20 to 80, and repeats across the stroke box, 10 to 90.
    CHECK(drawing.pixel(30, 50) == kBlue);
    CHECK(drawing.pixel(60, 50) == kWhite);
    CHECK(drawing.pixel(15, 50) == kWhite);
    CHECK(drawing.pixel(85, 50) == kBlue);
}

TEST_CASE("mask-clip and mask-origin take SVG's boxes, and the boxes of CSS map onto them") {
    std::string rect = "<rect x='20' y='20' width='60' height='60' fill='#0000ff' stroke='#0000ff' stroke-width='20' ";
    Drawing content(svgDocument(rect + "style='mask-image: linear-gradient(black, black); mask-clip: content-box'/>"));
    CHECK(content.pixel(15, 50) == kWhite);
    CHECK(content.pixel(25, 50) == kBlue);
    Drawing stroke(svgDocument(rect + "style='mask-image: linear-gradient(black, black); mask-repeat: no-repeat; "
                                      "mask-origin: stroke-box; mask-clip: no-clip'/>"));
    CHECK(stroke.pixel(15, 50) == kBlue);
    Drawing fill(svgDocument(rect + "style='mask-image: linear-gradient(black, black); mask-repeat: no-repeat; "
                                    "mask-clip: no-clip'/>"));
    CHECK(fill.pixel(15, 50) == kWhite);
    CHECK(fill.pixel(25, 50) == kBlue);
}

TEST_CASE("mask-size, mask-position and mask-repeat place an image layer") {
    std::string rect = "<rect width='100' height='100' fill='#0000ff' style='mask-image: linear-gradient(black, black); ";
    Drawing placed(svgDocument(rect + "mask-size: 20px 20px; mask-position: 50% 50%; mask-repeat: no-repeat'/>"));
    CHECK(placed.pixel(50, 50) == kBlue);
    CHECK(placed.pixel(35, 50) == kWhite);
    CHECK(placed.pixel(65, 50) == kWhite);
    Drawing spaced(svgDocument(rect + "mask-size: 30px 100px; mask-repeat: space no-repeat'/>"));
    // Three copies 30 wide across 100, with gaps of 5 between them.
    CHECK(spaced.pixel(15, 50) == kBlue);
    CHECK(spaced.pixel(32, 50) == kWhite);
    CHECK(spaced.pixel(50, 50) == kBlue);
    CHECK(spaced.pixel(67, 50) == kWhite);
    CHECK(spaced.pixel(85, 50) == kBlue);
}

TEST_CASE("an image layer that cannot be displayed masks nothing") {
    Drawing missing(clipped("", "style='mask-image: url(missing.png)'"));
    CHECK(missing.pixel(50, 50) == kBlue);
    Drawing set(clipped("", "style='mask-image: image-set(url(missing.png) 1x)'"));
    CHECK(set.pixel(50, 50) == kBlue);
}
