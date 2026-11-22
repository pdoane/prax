// Embedded content (SVG2/embedded.html): the image element, and the conditions that let a foreignObject render. The
// content of a foreignObject is laid out by a presentation, which html's tests cover.

#include "pr/array.h"
#include "pr/css.h"
#include "pr/map.h"
#include "pr/mem.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/texture.h"

#include "svg/tests/svg_fixture.h"
#include "tools/harness/harness_textures.h"

using namespace harness;

namespace pr_impl {

namespace {

// The images that the tests' documents reference. `flag.png` is a 40 by 20 raster. `turned.png` is a raster that its
// orientation transposes. SVG documents are found by name. The fragment of a name selects the SVG image that it names.
struct Images {
    Map<String, String> svgs;
    uint32_t loads = 0;
};

bool loadImage(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url, PrSvgLoadedImage* out) {
    (void)from;
    Images* images = static_cast<Images*>(context);
    ++images->loads;
    size_t hash = prStringFindByte(url, '#');
    PrStringView name = prStringSlice(url, 0, hash);
    PrStringView fragment = hash != url.size ? prStringSlice(url, hash + 1, url.size - hash - 1) : PrStringView("");
    if (name == "flag.png") {
        Array<uint8_t> pixels(40 * 20 * 4);
        prMemFill(pixels.data(), 0xff, pixels.count());
        out->raster = prTextureSourceCreateCopy(PrPixelView{pixels.data(), 40 * 4, {40, 20}}, PrPixelFormat_Rgba8,
            PrTextureAlpha_Straight);
        return true;
    }
    if (name == "turned.png") {
        // The stored texels are 4 by 2: an opaque row over a transparent row. Transposed, the image displays as 2 by 4,
        // with an opaque column to the left of a transparent column.
        uint8_t texels[4 * 2 * 4] = {};
        prMemFill(texels, 0xff, 4 * 4);
        PrTextureDesc desc = prTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{4, 2}, PrTextureAlpha_Straight);
        desc.orientation = PrTextureOrientationFlag_Transpose;
        out->raster = prTextureSourceCreatePixels(&desc, PrPixelView{texels, 4 * 4, {4, 2}});
        return true;
    }
    const String* found = images->svgs.get(name);
    if (found == nullptr)
        return false;
    PrDomDocument* document = prDomParseXml(*found, "image/svg+xml", nullptr);
    prCssEnable(document);
    prSvgEnable(document);
    PrSvgElement* root = prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document)));
    PrSvgDrawOptions options = prSvgDrawOptionsDefault();
    options.loadImage = loadImage;
    options.loadContext = context;
    options.textures = harnessMemoryTextures();
    out->svg = root != nullptr ? prSvgImageCreate(root, fragment, &options) : nullptr;
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
        options.textures = harnessMemoryTextures();
        // The flag 40 by 20 in quarters, with a viewBox of its size.
        images.svgs["flag.svg"] = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20' viewBox='0 0 40 20'>"
            "<rect width='20' height='10' fill='blue'/><rect id='orange' x='20' width='20' "
            "height='10' fill='orange'/><view id='quarter' viewBox='20 0 20 10'/></svg>";
    }

    // Returns the bounds of the commands that `body` draws, in order.
    Array<PrRect> drawn(PrStringView body) {
        Drawing drawing(svgDocument(body), PrVec2{160, 60}, &options);
        Array<PrRect> bounds;
        for (const Drawn& command : drawing.commands)
            bounds.push(command.bounds);
        return bounds;
    }
};

// Returns the bounds of the one command that `body` draws, or an empty rectangle if it draws any other number of
// commands.
PrRect only(Embedded& e, PrStringView body) {
    Array<PrRect> bounds = e.drawn(body);
    CHECK(bounds.count() == 1);
    return bounds.count() == 1 ? bounds[0] : PrRect{};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("preserveAspectRatio fits a raster's ratio into the positioning rectangle") {
    Embedded e;
    const char* image = "<image href='flag.png' x='20' y='10' width='120' height='40' ";
    CHECK(near(only(e, format("{}/>", image)), PrRect{40, 10, 120, 50}));
    CHECK(near(only(e, format("{}preserveAspectRatio='xMinYMin meet'/>", image)), PrRect{20, 10, 100, 50}));
    CHECK(near(only(e, format("{}preserveAspectRatio='none'/>", image)), PrRect{20, 10, 140, 50}));
    CHECK(near(only(e, format("{}preserveAspectRatio='xMidYMid slice'/>", image)), PrRect{20, 0, 140, 60}));
}

// The outermost svg element clips its content to its viewport, which is one clip around any command.
TEST_CASE("an image clips to its positioning rectangle unless its overflow is visible") {
    Embedded e;
    const char* image = "<image href='flag.png' x='20' y='10' width='120' height='40' preserveAspectRatio='xMidYMid slice'";
    Drawing hidden(svgDocument(format("{}/>", image)), PrVec2{160, 60}, &e.options);
    REQUIRE(hidden.commands.count() == 1);
    CHECK(hidden.commands[0].clips == 2);
    Drawing visible(svgDocument(format("{} style='overflow: visible'/>", image)), PrVec2{160, 60}, &e.options);
    REQUIRE(visible.commands.count() == 1);
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
    CHECK(e.drawn("<image href='flag.png' width='0' height='40' style='overflow: visible'/>").isEmpty());
    CHECK(e.drawn("<image href='flag.png' width='120' style='height: 0; overflow: visible'/>").isEmpty());
}

TEST_CASE("object-fit sizes the object, object-position places it, and preserveAspectRatio then has nothing to do") {
    Embedded e;
    const char* image = "<image href='flag.png' x='20' y='10' width='120' height='40' preserveAspectRatio='none' ";
    CHECK(near(only(e, format("{}style='object-fit: contain'/>", image)), PrRect{40, 10, 120, 50}));
    CHECK(near(only(e, format("{}style='object-fit: contain; object-position: right'/>", image)), PrRect{60, 10, 140, 50}));
    CHECK(near(only(e, format("{}style='object-fit: cover; object-position: top'/>", image)), PrRect{20, 10, 140, 70}));
    CHECK(near(only(e, format("{}style='object-fit: none'/>", image)), PrRect{60, 20, 100, 40}));
    CHECK(near(only(e, format("{}style='object-fit: none; object-position: -20px 30px'/>", image)), PrRect{0, 40, 40, 60}));
    CHECK(near(only(e, format("{}style='object-fit: none; object-position: right 10px bottom 25%'/>", image)),
        PrRect{90, 25, 130, 45}));
}

TEST_CASE("an SVG image's viewport is the image-rendering rectangle, and its root's preserveAspectRatio is none") {
    Embedded e;
    const char* image = "<image href='flag.svg' x='20' y='10' width='120' height='40' ";
    // The blue quarter is the first rect: a quarter of what the rendering rectangle covers.
    CHECK(near(e.drawn(format("{}/>", image))[0], PrRect{40, 10, 80, 30}));
    CHECK(near(e.drawn(format("{}preserveAspectRatio='xMaxYMid meet'/>", image))[0], PrRect{60, 10, 100, 30}));
    CHECK(near(e.drawn(format("{}preserveAspectRatio='none'/>", image))[0], PrRect{20, 10, 80, 30}));
    CHECK(near(e.drawn(format("{}preserveAspectRatio='xMidYMax slice'/>", image))[0], PrRect{20, -10, 80, 20}));
}

TEST_CASE("an SVG image with a width and a height but no viewBox scales as if they were its viewBox") {
    Embedded e;
    e.images.svgs["sized.svg"] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'><rect width='20' height='10'/></svg>";
    const char* image = "<image href='sized.svg' x='20' y='10' width='120' height='40' ";
    CHECK(near(only(e, format("{}/>", image)), PrRect{40, 10, 80, 30}));
    CHECK(near(only(e, format("{}preserveAspectRatio='none'/>", image)), PrRect{20, 10, 80, 30}));
}

TEST_CASE("an SVG image without a ratio fills the positioning rectangle, whatever its preserveAspectRatio") {
    Embedded e;
    e.images.svgs["free.svg"] = "<svg xmlns='http://www.w3.org/2000/svg'><rect width='50%' height='100%'/></svg>";
    const char* image = "<image href='free.svg' x='20' y='10' width='120' height='40' ";
    CHECK(near(only(e, format("{}/>", image)), PrRect{20, 10, 80, 50}));
    CHECK(near(only(e, format("{}preserveAspectRatio='xMinYMin meet'/>", image)), PrRect{20, 10, 80, 50}));
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
    REQUIRE(both.commands.count() == 1);
    CHECK(both.commands[0].clips == 1);
    CHECK(near(both.commands[0].bounds, PrRect{0, 10, 160, 50}));
}

TEST_CASE("a fragment names a view of an SVG image, or leaves the whole document") {
    Embedded e;
    const char* place = "' x='40' y='10' width='80' height='40'/>";
    // The orange quarter is the second rect, which covers the whole rectangle in any view of it.
    CHECK(near(e.drawn(format("<image href='flag.svg#svgView(viewBox(20,0,20,10)){}", place))[1], PrRect{40, 10, 120, 50}));
    CHECK(near(e.drawn(format("<image href='flag.svg#xywh=20,0,20,10{}", place))[1], PrRect{40, 10, 120, 50}));
    CHECK(near(e.drawn(format("<image href='flag.svg#xywh=percent:50,0,50,50{}", place))[1], PrRect{40, 10, 120, 50}));
    CHECK(near(e.drawn(format("<image href='flag.svg#quarter{}", place))[1], PrRect{40, 10, 120, 50}));
    CHECK(near(e.drawn(format("<image href='flag.svg#orange{}", place))[1], PrRect{80, 10, 120, 30}));
}

TEST_CASE("svgView's transform() moves the view about its center, after its viewBox") {
    Embedded e;
    const char* place = "' x='40' y='10' width='80' height='40' preserveAspectRatio='none'/>";
    // The blue quarter, from 40, 10 to 80, 30 without the transform, doubled about the center at 80, 30.
    CHECK(near(e.drawn(format("<image href='flag.svg#svgView(transform(scale(2))){}", place))[0], PrRect{0, -10, 80, 30}));
    CHECK(near(e.drawn(format("<image href='flag.svg#svgView(viewBox(0,0,20,10);transform(translate(10))){}",
        place))[0], PrRect{50, 10, 130, 50}));
}

TEST_CASE("the element that an SVG image's fragment names matches :target") {
    Embedded e;
    e.images.svgs["target.svg"] = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'>"
        "<style>#a:target { fill: orange }</style><rect id='a' width='40' height='20'/></svg>";
    Drawing named(svgDocument("<image href='target.svg#a'/>"), PrVec2{160, 60}, &e.options);
    Drawing whole(svgDocument("<image href='target.svg'/>"), PrVec2{160, 60}, &e.options);
    REQUIRE(named.commands.count() == 1);
    REQUIRE(whole.commands.count() == 1);
    CHECK(named.commands[0].color.r == doctest::Approx(1));
    CHECK(whole.commands[0].color.r == doctest::Approx(0));
}

TEST_CASE("a mask image with a transposing orientation repeats along the axes of the displayed image") {
    Embedded e;
    // Each displayed texel of the mask covers ten pixels. The mask repeats horizontally and not vertically.
    Drawing drawing(svgDocument("<rect width='100' height='100' fill='#0000ff' "
        "style='mask-image: url(turned.png); mask-size: 20px 40px; mask-repeat: repeat-x'/>"),
        PrVec2{100, 100}, &e.options);
    Array<uint8_t> bytes(100 * 100 * 4, 255);
    PrDrawRenderer* renderer = harnessMemoryRenderer();
    REQUIRE(prDrawRendererRenderPixels(renderer, drawing.list, PrPixels{bytes.data(), 100 * 4, {100, 100}},
        PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    // The rectangle is blue where the mask is opaque. Where the mask is transparent, the white beneath shows.
    auto masked = [&](uint32_t x, uint32_t y) { return bytes[(y * 100 + x) * 4] > 215; };
    auto shown = [&](uint32_t x, uint32_t y) { return bytes[(y * 100 + x) * 4] < 40; };
    CHECK(shown(4, 20));
    CHECK(masked(15, 20));
    CHECK(shown(24, 20));
    CHECK(masked(35, 20));
    CHECK(shown(84, 35));
    CHECK(masked(4, 60));
    CHECK(masked(24, 60));
}

TEST_CASE("an image that does not load draws nothing, and its href names an image only") {
    Embedded e;
    CHECK(e.drawn("<image href='missing.png' width='40' height='20'/>").isEmpty());
    CHECK(e.drawn("<image width='40' height='20'/>").isEmpty());
    CHECK(e.images.loads == 1);
}

TEST_CASE("an image takes its transform, its opacity and its visibility") {
    Embedded e;
    const char* image = "<image href='flag.png' width='40' height='20' ";
    Drawing moved(svgDocument(format("{}transform='translate(10 5)' opacity='0.5'/>", image)), PrVec2{160, 60},
        &e.options);
    REQUIRE(moved.commands.count() == 1);
    CHECK(near(moved.commands[0].bounds, PrRect{10, 5, 50, 25}));
    CHECK(moved.commands[0].opacity == doctest::Approx(0.5));
    CHECK(e.drawn(format("{}visibility='hidden'/>", image)).isEmpty());
}

TEST_CASE("requiredExtensions passes for the namespaces of HTML and MathML, and fails empty or for any other") {
    auto first = [](const char* extensions) {
        Drawing drawing(svgDocument(format("<switch><rect requiredExtensions='{}' width='10' height='10' fill='lime'/>"
            "<rect width='10' height='10' fill='red'/></switch>", extensions)));
        REQUIRE(drawing.commands.count() == 1);
        return drawing.commands[0].color.g == 1.0f;
    };
    CHECK(first("http://www.w3.org/1999/xhtml"));
    CHECK(first("http://www.w3.org/1998/Math/MathML http://www.w3.org/1999/xhtml"));
    CHECK_FALSE(first(""));
    CHECK_FALSE(first("http://example.com/SVGExtensions/EmbeddedXHTML"));
}
