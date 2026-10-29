// Embedded content (SVG2/embedded.html): the image element, and the conditions that let a foreignObject render. The
// content of a foreignObject is laid out by a presentation, which html's tests cover.

#include "pr/css.h"
#include "pr/draw_image.h"
#include "pr/svg.h"

#include "svg/tests/svg_fixture.h"

#include <map>

namespace pr_impl {

namespace {

// The images that the tests' documents name: a raster 40 by 20 named `flag.png`, and SVG documents by name. A name's
// fragment reaches the SVG image that it names.
struct Images {
    std::map<std::string, std::string> svgs;
    uint32_t loads = 0;
};

bool loadImage(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url, PrSvgLoadedImage* out) {
    (void)from;
    Images* images = static_cast<Images*>(context);
    ++images->loads;
    std::string reference(url.data, url.size);
    size_t hash = reference.find('#');
    std::string name = reference.substr(0, hash);
    std::string fragment = hash != std::string::npos ? reference.substr(hash + 1) : std::string();
    if (name == "flag.png") {
        std::vector<uint8_t> pixels(40 * 20 * 4, 0xff);
        PrDrawImageDesc desc{.pixels = {pixels.data(), 40 * 4, {40, 20}},
                             .format = PrPixelFormat_Rgba8,
                             .alpha = PrDrawAlpha_Straight,
                             .space = PrColorSpace_Srgb};
        return prDrawImageCreate(&desc, &out->raster) == PrStatus_Ok;
    }
    std::map<std::string, std::string>::const_iterator found = images->svgs.find(name);
    if (found == images->svgs.end())
        return false;
    PrDomDocument* document = prDomParseXml(found->second, "image/svg+xml", nullptr);
    prCssEnable(document);
    prSvgEnable(document);
    PrSvgElement* root = prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document)));
    PrSvgDrawOptions options = prSvgDrawOptionsDefault();
    options.loadImage = loadImage;
    options.loadContext = context;
    out->svg = root != nullptr
                   ? prSvgImageCreate(root, PrStringView{fragment.data(), fragment.size()}, &options)
                   : nullptr;
    prDomDocumentRelease(document);
    return out->svg != nullptr;
}

// A document drawn with the images above, in a viewport of 160 by 60.
struct Embedded {
    Images images;
    PrSvgDrawOptions options = prSvgDrawOptionsDefault();

    Embedded() {
        options.loadImage = loadImage;
        options.loadContext = &images;
        // The flag 40 by 20 in quarters, with a viewBox of its size.
        images.svgs["flag.svg"] = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20' viewBox='0 0 40 20'>"
                                  "<rect width='20' height='10' fill='blue'/><rect id='orange' x='20' width='20' "
                                  "height='10' fill='orange'/><view id='quarter' viewBox='20 0 20 10'/></svg>";
    }

    // Returns the bounds of the commands that `body` draws, in order.
    std::vector<PrRect> drawn(const std::string& body) {
        Drawing drawing(svgDocument(body), PrVec2{160, 60}, &options);
        std::vector<PrRect> bounds;
        for (const Drawn& command : drawing.commands)
            bounds.push_back(command.bounds);
        return bounds;
    }
};

// Returns the bounds of the one command that `body` draws, or an empty rectangle if it draws any other number of
// commands.
PrRect only(Embedded& e, const std::string& body) {
    std::vector<PrRect> bounds = e.drawn(body);
    CHECK(bounds.size() == 1);
    return bounds.size() == 1 ? bounds[0] : PrRect{};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("preserveAspectRatio fits a raster's ratio into the positioning rectangle") {
    Embedded e;
    std::string image = "<image href='flag.png' x='20' y='10' width='120' height='40' ";
    CHECK(near(only(e, image + "/>"), PrRect{40, 10, 120, 50}));
    CHECK(near(only(e, image + "preserveAspectRatio='xMinYMin meet'/>"), PrRect{20, 10, 100, 50}));
    CHECK(near(only(e, image + "preserveAspectRatio='none'/>"), PrRect{20, 10, 140, 50}));
    CHECK(near(only(e, image + "preserveAspectRatio='xMidYMid slice'/>"), PrRect{20, 0, 140, 60}));
}

// The outermost svg element clips its content to its viewport, which is one clip around any command.
TEST_CASE("an image clips to its positioning rectangle unless its overflow is visible") {
    Embedded e;
    std::string image = "<image href='flag.png' x='20' y='10' width='120' height='40' "
                        "preserveAspectRatio='xMidYMid slice'";
    Drawing hidden(svgDocument(image + "/>"), PrVec2{160, 60}, &e.options);
    REQUIRE(hidden.commands.size() == 1);
    CHECK(hidden.commands[0].clips == 2);
    Drawing visible(svgDocument(image + " style='overflow: visible'/>"), PrVec2{160, 60}, &e.options);
    REQUIRE(visible.commands.size() == 1);
    CHECK(visible.commands[0].clips == 1);
}

TEST_CASE("an auto width or height comes from the image's natural size and ratio") {
    Embedded e;
    CHECK(near(only(e, "<image href='flag.png' x='20' y='10'/>"), PrRect{20, 10, 60, 30}));
    CHECK(near(only(e, "<image href='flag.png' x='20' y='10' width='80'/>"), PrRect{20, 10, 100, 50}));
    CHECK(near(only(e, "<image href='flag.png' style='width: auto; height: 40px'/>"), PrRect{0, 0, 80, 40}));
}

TEST_CASE("an image without a natural size is 300 by 150, or the largest of its ratio in that") {
    Embedded e;
    e.images.svgs["none.svg"] = "<svg xmlns='http://www.w3.org/2000/svg'><rect width='100%' height='100%'/></svg>";
    e.images.svgs["ratio.svg"] =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 40 20'><rect width='40' height='20'/></svg>";
    CHECK(near(only(e, "<image href='none.svg'/>"), PrRect{0, 0, 300, 150}));
    CHECK(near(only(e, "<image href='ratio.svg'/>"), PrRect{0, 0, 300, 150}));
}

TEST_CASE("a width or height of zero renders nothing, whatever the overflow") {
    Embedded e;
    CHECK(e.drawn("<image href='flag.png' width='0' height='40' style='overflow: visible'/>").empty());
    CHECK(e.drawn("<image href='flag.png' width='120' style='height: 0; overflow: visible'/>").empty());
}

TEST_CASE("object-fit sizes the object, object-position places it, and preserveAspectRatio then has nothing to do") {
    Embedded e;
    std::string image = "<image href='flag.png' x='20' y='10' width='120' height='40' preserveAspectRatio='none' ";
    CHECK(near(only(e, image + "style='object-fit: contain'/>"), PrRect{40, 10, 120, 50}));
    CHECK(near(only(e, image + "style='object-fit: contain; object-position: right'/>"), PrRect{60, 10, 140, 50}));
    CHECK(near(only(e, image + "style='object-fit: cover; object-position: top'/>"), PrRect{20, 10, 140, 70}));
    CHECK(near(only(e, image + "style='object-fit: none'/>"), PrRect{60, 20, 100, 40}));
    CHECK(near(only(e, image + "style='object-fit: none; object-position: -20px 30px'/>"), PrRect{0, 40, 40, 60}));
    CHECK(near(only(e, image + "style='object-fit: none; object-position: right 10px bottom 25%'/>"),
               PrRect{90, 25, 130, 45}));
}

TEST_CASE("an SVG image's viewport is the image-rendering rectangle, and its root's preserveAspectRatio is none") {
    Embedded e;
    std::string image = "<image href='flag.svg' x='20' y='10' width='120' height='40' ";
    // The blue quarter is the first rect: a quarter of what the rendering rectangle covers.
    CHECK(near(e.drawn(image + "/>")[0], PrRect{40, 10, 80, 30}));
    CHECK(near(e.drawn(image + "preserveAspectRatio='xMaxYMid meet'/>")[0], PrRect{60, 10, 100, 30}));
    CHECK(near(e.drawn(image + "preserveAspectRatio='none'/>")[0], PrRect{20, 10, 80, 30}));
    CHECK(near(e.drawn(image + "preserveAspectRatio='xMidYMax slice'/>")[0], PrRect{20, -10, 80, 20}));
}

TEST_CASE("an SVG image with a width and a height but no viewBox scales as if they were its viewBox") {
    Embedded e;
    e.images.svgs["sized.svg"] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'><rect width='20' height='10'/></svg>";
    std::string image = "<image href='sized.svg' x='20' y='10' width='120' height='40' ";
    CHECK(near(only(e, image + "/>"), PrRect{40, 10, 80, 30}));
    CHECK(near(only(e, image + "preserveAspectRatio='none'/>"), PrRect{20, 10, 80, 30}));
}

TEST_CASE("an SVG image without a ratio fills the positioning rectangle, whatever its preserveAspectRatio") {
    Embedded e;
    e.images.svgs["free.svg"] =
        "<svg xmlns='http://www.w3.org/2000/svg'><rect width='50%' height='100%'/></svg>";
    std::string image = "<image href='free.svg' x='20' y='10' width='120' height='40' ";
    CHECK(near(only(e, image + "/>"), PrRect{20, 10, 80, 50}));
    CHECK(near(only(e, image + "preserveAspectRatio='xMinYMin meet'/>"), PrRect{20, 10, 80, 50}));
}

TEST_CASE("an SVG image's root applies its own filter, which no box applies") {
    Embedded e;
    e.images.svgs["inverted.svg"] = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20' "
                                    "style='filter: invert(1)'><rect width='40' height='20' fill='blue'/></svg>";
    Drawing drawing(svgDocument("<image href='inverted.svg' width='40' height='20'/>"), PrVec2{160, 60}, &e.options);
    CHECK(drawing.pixel(10, 10) == "255 255 0");
}

TEST_CASE("an SVG image's width and height give its ratio before its viewBox does") {
    Embedded e;
    e.images.svgs["square.svg"] = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='40' viewBox='0 0 40 20' "
                                  "preserveAspectRatio='xMinYMin meet'><rect width='40' height='20'/></svg>";
    CHECK(near(only(e, "<image href='square.svg' x='60' y='10' width='40' height='40'/>"), PrRect{60, 10, 100, 50}));
}

TEST_CASE("the root's overflow decides whether an SVG image clips to its viewport") {
    Embedded e;
    e.images.svgs["bar.svg"] = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20' viewBox='0 0 40 20' "
                               "style='overflow: visible'><rect x='-20' width='80' height='20'/></svg>";
    Drawing both(svgDocument("<image href='bar.svg' x='20' y='10' width='120' height='40' style='overflow: visible'/>"),
                 PrVec2{160, 60}, &e.options);
    REQUIRE(both.commands.size() == 1);
    CHECK(both.commands[0].clips == 1);
    CHECK(near(both.commands[0].bounds, PrRect{0, 10, 160, 50}));
}

TEST_CASE("a fragment names a view of an SVG image, or leaves the whole document") {
    Embedded e;
    std::string place = "' x='40' y='10' width='80' height='40'/>";
    // The orange quarter is the second rect, which covers the whole rectangle in any view of it.
    CHECK(near(e.drawn("<image href='flag.svg#svgView(viewBox(20,0,20,10))" + place)[1], PrRect{40, 10, 120, 50}));
    CHECK(near(e.drawn("<image href='flag.svg#xywh=20,0,20,10" + place)[1], PrRect{40, 10, 120, 50}));
    CHECK(near(e.drawn("<image href='flag.svg#xywh=percent:50,0,50,50" + place)[1], PrRect{40, 10, 120, 50}));
    CHECK(near(e.drawn("<image href='flag.svg#quarter" + place)[1], PrRect{40, 10, 120, 50}));
    CHECK(near(e.drawn("<image href='flag.svg#orange" + place)[1], PrRect{80, 10, 120, 30}));
}

TEST_CASE("svgView's transform() moves the view about its center, after its viewBox") {
    Embedded e;
    std::string place = "' x='40' y='10' width='80' height='40' preserveAspectRatio='none'/>";
    // The blue quarter, from 40, 10 to 80, 30 without the transform, doubled about the center at 80, 30.
    CHECK(near(e.drawn("<image href='flag.svg#svgView(transform(scale(2)))" + place)[0], PrRect{0, -10, 80, 30}));
    CHECK(near(e.drawn("<image href='flag.svg#svgView(viewBox(0,0,20,10);transform(translate(10)))" + place)[0],
               PrRect{50, 10, 130, 50}));
}

TEST_CASE("the element that an SVG image's fragment names matches :target") {
    Embedded e;
    e.images.svgs["target.svg"] = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'>"
                                  "<style>#a:target { fill: orange }</style><rect id='a' width='40' height='20'/></svg>";
    Drawing named(svgDocument("<image href='target.svg#a'/>"), PrVec2{160, 60}, &e.options);
    Drawing whole(svgDocument("<image href='target.svg'/>"), PrVec2{160, 60}, &e.options);
    REQUIRE(named.commands.size() == 1);
    REQUIRE(whole.commands.size() == 1);
    CHECK(named.commands[0].color.r == doctest::Approx(1));
    CHECK(whole.commands[0].color.r == doctest::Approx(0));
}

TEST_CASE("an image that does not load draws nothing, and its href names an image only") {
    Embedded e;
    CHECK(e.drawn("<image href='missing.png' width='40' height='20'/>").empty());
    CHECK(e.drawn("<image width='40' height='20'/>").empty());
    CHECK(e.images.loads == 1);
}

TEST_CASE("an image takes its transform, its opacity and its visibility") {
    Embedded e;
    std::string image = "<image href='flag.png' width='40' height='20' ";
    Drawing moved(svgDocument(image + "transform='translate(10 5)' opacity='0.5'/>"), PrVec2{160, 60}, &e.options);
    REQUIRE(moved.commands.size() == 1);
    CHECK(near(moved.commands[0].bounds, PrRect{10, 5, 50, 25}));
    CHECK(moved.commands[0].opacity == doctest::Approx(0.5));
    CHECK(e.drawn(image + "visibility='hidden'/>").empty());
}

TEST_CASE("requiredExtensions passes for the namespaces of HTML and MathML, and fails empty or for any other") {
    auto first = [](const char* extensions) {
        Drawing drawing(svgDocument(std::string("<switch><rect requiredExtensions='") + extensions
                                    + "' width='10' height='10' fill='lime'/><rect width='10' height='10' fill='red'/>"
                                      "</switch>"));
        REQUIRE(drawing.commands.size() == 1);
        return drawing.commands[0].color.g == 1.0f;
    };
    CHECK(first("http://www.w3.org/1999/xhtml"));
    CHECK(first("http://www.w3.org/1998/Math/MathML http://www.w3.org/1999/xhtml"));
    CHECK_FALSE(first(""));
    CHECK_FALSE(first("http://example.com/SVGExtensions/EmbeddedXHTML"));
}
