#include "css/src/css_parse.h"
#include "css/src/css_serialize.h"
#include "css/src/css_style.h"
#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// Returns the serialization of `value` parsed as the color property, or "invalid".
std::string color(PrStringView value) {
    CssTestSource source(value);
    CssValuePool pool = {};
    CssMatchState match = {};
    CssValueSpan span;
    std::string result = "invalid";
    if (cssParsePropertyValue(PrCssProperty_Color, &source.ts, &pool, &match, &span)) {
        PrTextBuffer out = {};
        cssSerializeValues(&out, cssValuePoolRun(&pool, span));
        result = textOf(prTextBufferView(&out));
        prTextBufferReset(&out);
    }
    cssValuePoolReset(&pool);
    cssMatchStateReset(&match);
    return result;
}

// Resolves `value`, parsed as the color property, in `context`. Returns false if it does not parse or resolve.
bool resolved(PrStringView value, const CssColorContext& context, PrColor* out) {
    CssTestSource source(value);
    CssValuePool pool = {};
    CssMatchState match = {};
    CssValueSpan span;
    bool ok = cssParsePropertyValue(PrCssProperty_Color, &source.ts, &pool, &match, &span) &&
        cssResolveColor(cssValuePoolRun(&pool, span), 0, context, out);
    cssValuePoolReset(&pool);
    cssMatchStateReset(&match);
    return ok;
}

bool near(PrColor a, PrColor b) {
    auto close = [](float x, float y) { return x - y < 0.01f && y - x < 0.01f; };
    return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b) && close(a.a, b.a);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a custom color space parses and does not resolve, and device-cmyk() converts naively") {
    PrColor out{};
    CHECK(color("color(--swap 1 0 0)") != "invalid");
    CHECK_FALSE(resolved("color(--swap 1 0 0)", CssColorContext{}, &out));
    CHECK_FALSE(resolved("color-mix(in --swap, red, blue)", CssColorContext{}, &out));
    CHECK_FALSE(resolved("color(from red --swap r g b)", CssColorContext{}, &out));
    REQUIRE(resolved("device-cmyk(0 1 1 0)", CssColorContext{}, &out));
    CHECK(near(out, PrColor{1, 0, 0, 1}));
    REQUIRE(resolved("device-cmyk(0.2 0 0 0.5 / 0.5)", CssColorContext{}, &out));
    CHECK(near(out, PrColor{0.4f, 0.5f, 0.5f, 0.5f}));
}

TEST_CASE("an sRGB color serializes in the legacy form, rounded to 8 bits") {
    CHECK(color("#234") == "rgb(34, 51, 68)");
    CHECK(color("rgb(40%, 20%, 0%)") == "rgb(102, 51, 0)");
    CHECK(color("rgba(2 3 4 / 50%)") == "rgba(2, 3, 4, 0.5)");
    CHECK(color("rgb(-2 300 none)") == "rgb(0, 255, 0)");
    CHECK(color("hsl(120 30% 50%)") == "rgb(89, 166, 89)");
    CHECK(color("hwb(0 100% 50% / 0)") == "rgba(170, 170, 170, 0)");
}

TEST_CASE("hsl() and hwb() with a missing component keep their own function") {
    CHECK(color("hsla(120 80% none)") == "hsl(120 80 none)");
    CHECK(color("hwb(480 30% 50% / none)") == "hwb(120 30 50 / none)");
}

TEST_CASE("a color in another space serializes its components as numbers") {
    CHECK(color("lab(50 -20% -20%)") == "lab(50 -25 -25)");
    CHECK(color("lab(400 0 10/50%)") == "lab(100 0 10 / 0.5)");
    CHECK(color("oklab(0.5 -20% -20%)") == "oklab(0.5 -0.08 -0.08)");
    CHECK(color("lch(10 20 1.28rad)") == "lch(10 20 73.3386)");
    CHECK(color("lch(0.5 -20% -20)") == "lch(0.5 0 340)");
    CHECK(color("color(xyz 20% none 10 / 300%)") == "color(xyz-d65 0.2 none 10)");
    CHECK(color("color(display-p3-linear 1 0 0)") == "color(display-p3-linear 1 0 0)");
}

TEST_CASE("a relative color keeps its arguments") {
    CHECK(color("rgba(from rgb(20%, 40%, 60%, 80%) b alpha r / g)") == "rgb(from rgba(51, 102, 153, 0.8) b alpha r / g)");
    CHECK(color("color(from red xyz x y z / alpha)") == "color(from red xyz-d65 x y z / alpha)");
    CHECK(color("alpha(from currentcolor / alpha)") == "alpha(from currentcolor / alpha)");
    CHECK(color("lab(from red r a b)") == "invalid");
    CHECK(color("rgb(r g b)") == "invalid");
}

TEST_CASE("a relative color's components are numbers in its math functions") {
    CHECK(color("rgb(from red calc(r) g b)") == "rgb(from red calc(r) g b)");
    CHECK(color("rgb(from red calc(r * 2) 10 10)") == "rgb(from red calc(2 * r) 10 10)");
    CHECK(color("hsl(from red h calc(s / 2) l / calc(alpha))") == "hsl(from red h calc(0.5 * s) l / calc(alpha))");
    // A component keyword is a number only inside a relative color.
    CHECK(color("rgb(calc(r) 0 0)") == "invalid");
}

TEST_CASE("an origin written with rgb(), hsl() or hwb() has zero for a missing component") {
    CHECK(color("hsl(from hsl(none none none) h s l)") == "hsl(from rgb(0, 0, 0) h s l)");
    CHECK(color("rgb(from hwb(120deg none 50% / .5) r g b)") == "rgb(from rgba(0, 128, 0, 0.5) r g b)");
    CHECK(color("color(from color(srgb none 0 0) srgb r g b)") == "color(from color(srgb none 0 0) srgb r g b)");
}

TEST_CASE("color-mix() takes the canonical order and percentages") {
    CHECK(color("color-mix(in srgb, 70% red, 50% blue)") == "color-mix(in srgb, red 70%, blue 50%)");
    CHECK(color("color-mix(in hsl shorter hue, red 50%, blue)") == "color-mix(in hsl, red, blue)");
    CHECK(color("color-mix(in oklab, teal, peru 40%)") == "color-mix(teal 60%, peru 40%)");
    CHECK(color("color-mix(in lch longer hue, red, green, blue)") == "color-mix(in lch longer hue, red, green, blue)");
    CHECK(color("color-mix(red 50%, green, blue)") == "color-mix(red 50%, green 25%, blue 25%)");
}

TEST_CASE("color-layers() and contrast-color() are colors") {
    CHECK(color("color-layers(red, blue)") == "color-layers(red, blue)");
    CHECK(color("color-layers(normal, red)") == "color-layers(red)");
    CHECK(color("contrast-color(white)") == "contrast-color(white)");
}
