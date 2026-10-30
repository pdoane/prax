// Structure and geometry: symbols and their reference points, the geometry properties of svg elements, the limits of
// min and max sizes, and vector-effect.

#include "svg/tests/svg_fixture.h"

namespace pr_impl {

namespace {

// Documents by the URL that references name them by, which a document without a browsing context loads through its
// options.
struct Library {
    std::vector<std::pair<std::string, PrDomDocument*>> documents;
    std::vector<std::string> requests;

    ~Library() {
        for (auto& [url, document] : documents)
            prDomDocumentRelease(document);
    }

    void add(const std::string& url, const std::string& source) {
        PrDomDocument* document = prDomParseXml(source, "image/svg+xml", nullptr);
        prCssEnable(document);
        prSvgEnable(document);
        documents.emplace_back(url, document);
    }

    PrSvgDrawOptions options() {
        PrSvgDrawOptions options = prSvgDrawOptionsDefault();
        options.load = load;
        options.loadContext = this;
        return options;
    }

    static PrDomDocument* PR_NULLABLE load(void* PR_NULLABLE context, PrDomDocument*, PrStringView url) {
        Library* library = static_cast<Library*>(context);
        library->requests.emplace_back(url.data, url.size);
        for (auto& [name, document] : library->documents) {
            if (name == std::string(url.data, url.size))
                return document;
        }
        return nullptr;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Symbols

TEST_CASE("a symbol's x, y, width and height attributes place its instance") {
    Drawing drawing(svgDocument("<symbol id='s' x='80' y='20' width='60' height='30' viewBox='0 0 20 10'>"
        "<rect width='20' height='10'/></symbol><use href='#s'/>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{80, 20, 140, 50}));
}

TEST_CASE("a symbol takes x and y from a style rule, and not width and height") {
    Drawing drawing(svgDocument("<style>symbol { x: 80px; y: 20px; width: 60px; height: 30px }</style>"
        "<symbol id='s' viewBox='0 0 20 10' overflow='visible'>"
        "<rect width='20' height='10'/></symbol><use href='#s'/>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{100, 20, 220, 80}));
}

TEST_CASE("a symbol's refX and refY keywords place that point of its content at the use's x and y") {
    std::string symbol = "<symbol id='s' width='40' height='20' viewBox='0 0 20 10' refX='center' refY='bottom'>"
        "<rect width='20' height='10'/></symbol><use href='#s' x='80' y='30'/>";
    Drawing drawing(svgDocument(symbol), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{60, 10, 100, 30}));
}

TEST_CASE("a symbol's refX of zero is in its viewBox's coordinates, and an unset one places its edge") {
    Drawing zero(svgDocument("<symbol id='s' width='40' height='20' viewBox='10 0 20 10' refX='0' refY='0'>"
        "<rect x='10' width='20' height='10'/></symbol><use href='#s' x='80' y='30'/>"), PrVec2{160, 60});
    REQUIRE(zero.fills().size() == 1);
    CHECK(near(zero.fills()[0]->bounds, PrRect{100, 30, 140, 50}));
    Drawing unset(svgDocument("<symbol id='s' width='40' height='20' viewBox='10 0 20 10'>"
        "<rect x='10' width='20' height='10'/></symbol><use href='#s' x='80' y='30'/>"), PrVec2{160, 60});
    REQUIRE(unset.fills().size() == 1);
    CHECK(near(unset.fills()[0]->bounds, PrRect{80, 30, 120, 50}));
}

TEST_CASE("the instance of a symbol draws nothing if a style rule sets the symbol's display to none") {
    Drawing hidden(svgDocument("<style>.hidden { display: none }</style>"
        "<symbol id='s' class='hidden'><rect width='20' height='10'/></symbol><use href='#s' x='5'/>"));
    CHECK(hidden.fills().empty());
    Drawing shown(svgDocument("<symbol id='s'><rect width='20' height='10'/></symbol><use href='#s' x='5'/>"));
    REQUIRE(shown.fills().size() == 1);
    CHECK(near(shown.fills()[0]->bounds, PrRect{5, 0, 25, 10}));
}

// ---------------------------------------------------------------------------------------------------------------------
// The use element

TEST_CASE("a use element's x and y translate the space of its clip path, mask and filter, which apply once") {
    // The clip is 0 to 40 in the space that x moves by 60, which scale(2 1) makes 120 to 200: the instance, at 130 to
    // 170, is whole.
    Drawing clipped(svgDocument("<clipPath id='c'><rect width='40' height='100'/></clipPath>"
        "<defs><rect id='r' x='5' y='5' width='20' height='10' fill='#0000ff'/></defs>"
        "<use href='#r' x='60' y='10' transform='scale(2 1)' clip-path='url(#c)'/>"), PrVec2{200, 40});
    CHECK(clipped.pixel(132, 20) == "0 0 255");
    CHECK(clipped.pixel(168, 20) == "0 0 255");
    // The mask's region is 0 to 15 in the space that x moves by 60, and so 60 to 75: the instance, at 65 to 85, is cut
    // at 75.
    Drawing masked(svgDocument("<mask id='m' maskUnits='userSpaceOnUse' x='0' y='0' width='15' height='40'>"
        "<rect width='200' height='40' fill='white'/></mask>"
        "<defs><rect id='r' x='5' y='5' width='20' height='10' fill='#0000ff'/></defs>"
        "<use href='#r' x='60' y='10' mask='url(#m)'/>"), PrVec2{200, 40});
    CHECK(masked.pixel(70, 20) == "0 0 255");
    CHECK(masked.pixel(80, 20) == "255 255 255");
    // The offset applies once, not again in the space before the translation.
    Drawing filtered(svgDocument("<filter id='f' x='-1' y='-1' width='3' height='3'><feOffset dx='20'/></filter>"
        "<defs><rect id='r' width='20' height='10' fill='#0000ff'/></defs>"
        "<use href='#r' x='10' y='10' filter='url(#f)'/>"), PrVec2{200, 40});
    CHECK(filtered.pixel(32, 15) == "0 0 255");
    CHECK(filtered.pixel(48, 15) == "0 0 255");
    CHECK(filtered.pixel(52, 15) == "255 255 255");
    CHECK(filtered.pixel(68, 15) == "255 255 255");
}

// ---------------------------------------------------------------------------------------------------------------------
// References into other documents

TEST_CASE("a use instances an element of another document, or its root without a fragment") {
    Library library;
    library.add("defs.svg", svgDocument("<rect id='square' x='5' width='20' height='20' fill='#0000ff'/>"));
    PrSvgDrawOptions options = library.options();
    Drawing element(svgDocument("<use href='defs.svg#square' y='10'/>"), PrVec2{100, 100}, &options);
    REQUIRE(element.fills().size() == 1);
    CHECK(near(element.fills()[0]->bounds, PrRect{5, 10, 25, 30}));
    CHECK(element.fills()[0]->color.b == doctest::Approx(1));
    Drawing root(svgDocument("<use href='defs.svg' x='10'/>"), PrVec2{100, 100}, &options);
    REQUIRE(root.fills().size() == 1);
    CHECK(near(root.fills()[0]->bounds, PrRect{15, 0, 35, 20}));
    // The document is loaded once for the URL that both references name.
    CHECK(library.requests.size() == 2);
}

TEST_CASE("a paint server and a clipPath of another document apply, styled by their own document") {
    Library library;
    library.add("defs.svg", svgDocument("<style>stop { stop-color: #00ff00 }</style>"
        "<linearGradient id='green'><stop offset='0'/><stop offset='1'/></linearGradient>"
        "<clipPath id='half'><rect width='10' height='100'/></clipPath>"));
    PrSvgDrawOptions options = library.options();
    Drawing drawing(svgDocument("<rect width='40' height='40' fill='url(defs.svg#green) red' "
        "clip-path='url(defs.svg#half)'/>"), PrVec2{100, 100}, &options);
    REQUIRE(drawing.fills().size() == 1);
    CHECK(drawing.fills()[0]->brushed);
    CHECK(drawing.pixel(5, 20) == "0 255 0");
    CHECK(drawing.pixel(20, 20) == "255 255 255");
}

TEST_CASE("a reference into a document that does not load is in error") {
    Library library;
    PrSvgDrawOptions options = library.options();
    Drawing drawing(svgDocument("<use href='missing.svg#a'/><rect width='10' height='10' "
        "fill='url(missing.svg#g) #ff0000'/>"), PrVec2{100, 100}, &options);
    REQUIRE(drawing.fills().size() == 1);
    CHECK(drawing.fills()[0]->color.r == doctest::Approx(1));
    CHECK_FALSE(drawing.fills()[0]->brushed);
}

// ---------------------------------------------------------------------------------------------------------------------
// Geometry properties

TEST_CASE("an inner svg element takes its x and y from a style rule") {
    Drawing drawing(svgDocument("<style>svg svg { x: 80px; y: 10px }</style>"
        "<svg x='20' width='40' height='40'><rect width='100%' height='100%'/></svg>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{80, 10, 120, 50}));
}

TEST_CASE("an inner svg element's width and height from a style rule are ignored") {
    Drawing drawing(svgDocument("<style>svg svg { width: 10px; height: 10px }</style>"
        "<svg width='40' height='40'><rect width='100%' height='100%'/></svg>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{0, 0, 40, 40}));
}

TEST_CASE("min-width and max-width limit a rect's width, and the minimum wins") {
    Drawing max(svgDocument("<rect width='120' height='40' style='max-width: 60px'/>"), PrVec2{160, 60});
    REQUIRE(max.fills().size() == 1);
    CHECK(near(max.fills()[0]->bounds, PrRect{0, 0, 60, 40}));
    Drawing min(svgDocument("<rect width='40' height='40' style='min-width: 50%'/>"), PrVec2{160, 60});
    REQUIRE(min.fills().size() == 1);
    CHECK(near(min.fills()[0]->bounds, PrRect{0, 0, 80, 40}));
    Drawing both(svgDocument("<rect width='40' height='10' style='min-height: 30px; max-height: 20px'/>"), PrVec2{160, 60});
    REQUIRE(both.fills().size() == 1);
    CHECK(near(both.fills()[0]->bounds, PrRect{0, 0, 40, 30}));
}

// ---------------------------------------------------------------------------------------------------------------------
// Vector effects

TEST_CASE("non-scaling-size keeps the host's scale at the origin that the CTM gives") {
    Drawing drawing(svgDocument("<g transform='translate(40 30) scale(3)'>"
        "<rect x='5' width='20' height='10' vector-effect='non-scaling-size'/></g>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{45, 30, 65, 40}));
}

TEST_CASE("non-rotation drops the rotation and keeps the scale") {
    Drawing drawing(svgDocument("<g transform='translate(40 30) rotate(90) scale(2)'>"
        "<rect width='20' height='10' vector-effect='non-rotation'/></g>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{40, 30, 80, 50}, 1e-2f));
}

TEST_CASE("fixed-position places the user space's origin at the transform's translation in the host") {
    Drawing drawing(svgDocument("<g transform='translate(60 0) scale(1.5)'>"
        "<rect width='20' height='10' transform='translate(40 30)' "
        "vector-effect='fixed-position non-scaling-size'/></g>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{40, 30, 60, 40}));
}

TEST_CASE("all three vector effects draw the geometry as it is, at the transform's translation") {
    Drawing drawing(svgDocument("<g transform='translate(140 60) rotate(45) scale(3)'>"
        "<rect width='20' height='10' transform='translate(40 30)' "
        "vector-effect='fixed-position non-scaling-size non-rotation'/></g>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{40, 30, 60, 40}, 1e-3f));
}

TEST_CASE("the host of vector effects is the outermost viewport, past a nested svg's viewBox") {
    Drawing drawing(svgDocument("<svg x='20' y='10' width='120' height='40' viewBox='0 0 30 10'>"
        "<rect x='10' y='2.5' width='10' height='5' transform='translate(5 2.5)' "
        "vector-effect='non-scaling-size'/></svg>"), PrVec2{160, 60});
    REQUIRE(drawing.fills().size() == 1);
    CHECK(near(drawing.fills()[0]->bounds, PrRect{50, 22.5f, 60, 27.5f}));
}

TEST_CASE("vector-effect lists its keywords in any order, and non-scaling-stroke stays a stroke's") {
    Drawing drawing(svgDocument("<g transform='scale(2)'><rect width='10' height='10' fill='none' stroke='black' "
        "vector-effect='non-scaling-size non-scaling-stroke'/></g>"));
    REQUIRE(drawing.strokes().size() == 1);
    CHECK(drawing.strokes()[0]->nonScaling);
    CHECK(near(drawing.strokes()[0]->bounds, PrRect{0, 0, 10, 10}));
}
