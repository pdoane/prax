// Basic shapes (css-shapes-1) and the values of the masking properties (css-masking-1).

#include "pr/path.h"

#include "css/src/css_parse.h"
#include "css/src/css_property.h"
#include "css/src/css_serialize.h"
#include "css/src/css_shape.h"
#include "css/src/css_style.h"
#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

#include <cmath>

namespace pr_impl {

namespace {

// A value of a property, parsed.
struct Parsed : CssTestSource {
    CssValuePool pool = {};
    CssMatchState match = {};
    CssValueSpan span = {};
    bool valid;

    Parsed(PrStringView property, PrStringView value) : CssTestSource(value) {
        valid = cssParsePropertyValue(cssPropertyFromText(property), &ts, &pool, &match, &span);
    }

    ~Parsed() {
        cssValuePoolReset(&pool);
        cssMatchStateReset(&match);
    }

    CssValueRun run() const { return cssValuePoolRun(&pool, span); }
};

// Returns the serialization of `value` as a value of `property`, or the empty string if it is not one.
std::string specified(PrStringView property, PrStringView value) {
    Parsed parsed(property, value);
    if (!parsed.valid)
        return std::string();
    PrTextBuffer out = {};
    cssSerializeValues(&out, parsed.run());
    return take(&out);
}

// The outline of a clip-path's basic shape in a reference box.
struct Outline {
    bool drawn = false;
    PrPathFillRule rule = PrPathFillRule_NonZero;
    PrRect bounds{};
    uint32_t verbs = 0;
};

// Returns the outline of the basic shape `value`, the whole value of clip-path, in `box`.
Outline outlineOf(PrStringView value, PrRect box) {
    Parsed parsed("clip-path", value);
    REQUIRE(parsed.valid);
    PrPath* path = prPathCreate();
    Outline outline{};
    outline.drawn = cssShapePath(parsed.run(), 0, box, path, &outline.rule);
    outline.verbs = prPathVerbCount(path);
    outline.bounds = outline.verbs != 0 ? prPathTightBounds(path) : PrRect{};
    prPathDestroy(path);
    return outline;
}

bool near(PrRect a, PrRect b) {
    bool same = std::fabs(a.left - b.left) < 1e-3f && std::fabs(a.top - b.top) < 1e-3f &&
        std::fabs(a.right - b.right) < 1e-3f && std::fabs(a.bottom - b.bottom) < 1e-3f;
    if (!same)
        MESSAGE("{" << a.left << ", " << a.top << ", " << a.right << ", " << a.bottom << "}");
    return same;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a circle's radius may be one percentage, and two sizes are invalid") {
    CHECK(specified("clip-path", "circle(40%) border-box") == "circle(40%) border-box");
    CHECK(specified("clip-path", "circle(closest-corner at 20px 50px)") == "circle(closest-corner at 20px 50px)");
    CHECK(specified("clip-path", "circle(4% 20%)").empty());
    CHECK(specified("clip-path", "circle(-10%)").empty());
    CHECK(specified("clip-path", "ellipse(closest-side 20%)") == "ellipse(closest-side 20%)");
    CHECK(specified("clip-path", "ellipse(10px)").empty());
}

TEST_CASE("clip's rect() separates its edges with commas or with spaces, but not both") {
    CHECK(specified("clip", "rect(0, 40px, 20px, 0)") == "rect(0px, 40px, 20px, 0px)");
    CHECK(specified("clip", "rect(10px, -20px, auto, auto)") == "rect(10px, -20px, auto, auto)");
    CHECK(!specified("clip", "rect(0 40px 20px 0)").empty());
    CHECK(specified("clip", "rect(10px 20px, 30px 40px)").empty());
    CHECK(specified("clip", "rect(10%, 20px, 30px, 40px)").empty());
}

TEST_CASE("clip paths, mask layers, filters and blending are computed in a group of their own, which does not inherit") {
    for (PrCssProperty property : {PrCssProperty_ClipPath, PrCssProperty_Clip, PrCssProperty_MaskImage,
        PrCssProperty_MaskMode, PrCssProperty_MaskRepeat, PrCssProperty_MaskPosition,
                PrCssProperty_MaskSize, PrCssProperty_MaskOrigin, PrCssProperty_MaskClip,
                PrCssProperty_MaskComposite, PrCssProperty_Filter, PrCssProperty_BackdropFilter,
                PrCssProperty_MixBlendMode, PrCssProperty_Isolation})
        CHECK(cssStyleSlot(property).group == CssStyleGroup_Compositing);
    CHECK(!cssStyleGroupInherited(CssStyleGroup_Compositing));
}

TEST_CASE("inset() moves each side in, and reduces a pair of insets that is larger than the box") {
    PrRect box{10, 20, 110, 70};
    CHECK(near(outlineOf("inset(10px 20%)", box).bounds, PrRect{30, 30, 90, 60}));
    // A pair of 75% insets sums to 150% of the width, and is reduced to 50% each, which encloses nothing.
    CHECK(outlineOf("inset(0 75% 0 75%)", box).verbs == 0);
    CHECK(near(outlineOf("inset(10px round 5px)", box).bounds, PrRect{20, 30, 100, 60}));
}

TEST_CASE("circle() and ellipse() take radii from lengths, percentages and extents") {
    PrRect box{0, 0, 100, 60};
    // A percentage of a circle is of the box's normalized diagonal.
    float diagonal = std::sqrt((100.0f * 100 + 60.0f * 60) / 2);
    float r = diagonal * 0.4f;
    CHECK(near(outlineOf("circle(40%)", box).bounds, PrRect{50 - r, 30 - r, 50 + r, 30 + r}));
    // closest-side is the default, from the center.
    CHECK(near(outlineOf("circle()", box).bounds, PrRect{20, 0, 80, 60}));
    CHECK(near(outlineOf("circle(10px at left 20px top 30%)", box).bounds, PrRect{10, 8, 30, 28}));
    CHECK(near(outlineOf("circle(farthest-side at 20px 20px)", box).bounds, PrRect{-60, -60, 100, 100}));
    CHECK(near(outlineOf("ellipse(50% 10px at 50% 50%)", box).bounds, PrRect{0, 20, 100, 40}));
}

TEST_CASE("polygon() and path() take their fill rule, and their points from the box's top left") {
    PrRect box{10, 10, 110, 60};
    Outline polygon = outlineOf("polygon(evenodd, 0 0, 100% 0, 50% 100%)", box);
    CHECK(polygon.rule == PrPathFillRule_EvenOdd);
    CHECK(near(polygon.bounds, PrRect{10, 10, 110, 60}));
    CHECK(outlineOf("polygon(0 0, 10px 0, 0 10px)", box).rule == PrPathFillRule_NonZero);
    Outline path = outlineOf("path(evenodd, 'M 0 0 h 20 v 20 Z')", box);
    CHECK(path.rule == PrPathFillRule_EvenOdd);
    CHECK(near(path.bounds, PrRect{10, 10, 30, 30}));
}
