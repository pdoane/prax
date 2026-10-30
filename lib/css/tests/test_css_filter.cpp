// Filter functions (filter-effects-1), as layout takes them, and blend modes (compositing-1), as lib/draw takes them.

#include "css/src/css_filter.h"
#include "css/src/css_parse.h"
#include "css/src/css_property.h"
#include "css/src/css_style.h"
#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// The items of a filter value, computed for an element whose color is blue and whose font size is 10px.
struct Filters : CssTestSource {
    CssValuePool parsed = {};
    CssValuePool computed = {};
    CssMatchState match = {};
    Array<CssFilterFunction> items = {};

    explicit Filters(PrStringView value) : CssTestSource(value) {
        CssValueSpan span{};
        REQUIRE(cssParsePropertyValue(PrCssProperty_Filter, &ts, &parsed, &match, &span));
        CssStyleContext context{
            .font = cssFontBases(10, 12),
            .root = kCssInitialFontBases,
            .parentWeight = 400,
            .parentColor = PrColor{0, 0, 1, 1},
            .viewport = CssViewport{800, 600},
        };
        cssStyleComputeValue(&computed, PrCssProperty_Filter, cssValuePoolRun(&parsed, span), context);
        CssValueRun run = cssValuePoolRun(&computed, CssValueSpan{0, computed.values.count});
        cssFilterFunctions(run, CssColorContext{.current = PrColor{0, 0, 1, 1}}, &items);
    }

    ~Filters() {
        items.reset();
        cssValuePoolReset(&parsed);
        cssValuePoolReset(&computed);
        cssMatchStateReset(&match);
    }

    const PrLayoutFilter& operator[](uint32_t i) const { return items.data[i].filter; }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("filter functions take their defaults for omitted arguments, and clamp the amounts that have no meaning past 1") {
    Filters f("grayscale() invert(150%) brightness(150%) opacity(2) blur() hue-rotate(0.5turn) saturate(0)");
    REQUIRE(f.items.count == 7);
    CHECK(f[0].kind == PrLayoutFilterKind_Grayscale);
    CHECK(f[0].amount == 1);
    CHECK(f[1].amount == 1);
    CHECK(f[2].amount == doctest::Approx(1.5f));
    CHECK(f[3].amount == 1);
    CHECK(f[4].kind == PrLayoutFilterKind_Blur);
    CHECK(f[4].amount == 0);
    CHECK(f[5].amount == doctest::Approx(3.14159f));
    CHECK(f[6].amount == 0);
}

TEST_CASE("drop-shadow() takes its color before or after its lengths, and the element's color without one") {
    Filters f("drop-shadow(2px 3px) drop-shadow(1em 5px 6px red) drop-shadow(rgb(0 255 0) -1px 1px)");
    REQUIRE(f.items.count == 3);
    CHECK(f[0].offset.x == 2);
    CHECK(f[0].offset.y == 3);
    CHECK(f[0].sigma == 0);
    CHECK(prFloat16ToFloat(f[0].color.b) == 1);
    CHECK(f[1].offset.x == 10);
    CHECK(f[1].sigma == 6);
    CHECK(prFloat16ToFloat(f[1].color.r) == 1);
    CHECK(prFloat16ToFloat(f[1].color.b) == 0);
    CHECK(f[2].offset.x == -1);
    CHECK(prFloat16ToFloat(f[2].color.g) == 1);
}

TEST_CASE("a filter list keeps its URLs in order among its functions") {
    Filters f("blur(2px) url(#shadow) sepia(0.5)");
    REQUIRE(f.items.count == 3);
    CHECK(f[1].kind == PrLayoutFilterKind_Source);
    CHECK(f.items.data[1].url == "#shadow");
    CHECK(f[2].kind == PrLayoutFilterKind_Sepia);
}

TEST_CASE("blend-mode keywords name lib/draw's blend modes") {
    PrDrawBlendMode mode = PrDrawBlendMode_Normal;
    CHECK(cssBlendModeFromKeyword(PrCssKeyword_Multiply, &mode));
    CHECK(mode == PrDrawBlendMode_Multiply);
    CHECK(cssBlendModeFromKeyword(PrCssKeyword_Luminosity, &mode));
    CHECK(mode == PrDrawBlendMode_Luminosity);
    CHECK(!cssBlendModeFromKeyword(PrCssKeyword_PlusLighter, &mode));
}
