#include "css/src/css_parse.h"
#include "css/src/css_property.h"
#include "css/src/css_serialize.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

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
String specified(PrStringView property, PrStringView value) {
    Parsed parsed(property, value);
    if (!parsed.valid)
        return String();
    PrTextBuffer out = {};
    cssSerializeValues(&out, parsed.run());
    return take(&out);
}

String width(const char* value) {
    return specified("width", value);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a math function is a tree of values below one that holds its type") {
    Parsed parsed("width", "calc(1em + 2px)");
    REQUIRE(parsed.valid);
    CssValueRun run = parsed.run();
    REQUIRE(run.count == 4);
    CHECK(run.values[0].kind == PrCssValueKind_Math);
    CHECK(cssValueMathCategory(&run.values[0]) == CssUnitCategory_Length);
    CHECK(run.values[0].next == 4);
    CHECK(run.values[1].kind == PrCssValueKind_Calc);
    CHECK(cssValueCalcOperator(&run.values[1]) == PrCssCalc_Sum);
    CHECK(run.values[2].unit == PrCssUnit_Em);
    CHECK(run.values[3].unit == PrCssUnit_Px);
}

TEST_CASE("a math function converts to a unit where it is one value of a compatible one") {
    Parsed parsed("width", "calc(1in / 2)");
    REQUIRE(parsed.valid);
    double number = 0;
    REQUIRE(cssMathTo(parsed.run(), 0, PrCssUnit_Px, &number));
    CHECK(number == 48);
    CHECK_FALSE(cssMathTo(parsed.run(), 0, PrCssUnit_Deg, &number));

    Parsed mixed("width", "calc(1em + 2px)");
    CHECK_FALSE(cssMathTo(mixed.run(), 0, PrCssUnit_Px, &number));
}

TEST_CASE("a math function is accepted where its type fits the value it stands for") {
    CHECK(width("calc(10px + 5%)") == "calc(5% + 10px)");
    CHECK(width("calc(10px + 5s)").isEmpty());
    CHECK(width("calc(0 + 5px)").isEmpty());          // no unitless zero in a calculation
    CHECK(width("calc(5)").isEmpty());
    CHECK(specified("opacity", "calc(0.25 + 25%)").isEmpty());
    CHECK(specified("opacity", "calc(100% / 4)") == "calc(25%)");
    CHECK(specified("z-index", "calc(1 / 2)") == "calc(0.5)");
    CHECK(specified("border-top-width", "calc(10% + 1px)").isEmpty());
    CHECK(width("calc(1px * 1px / 1px)") == "calc(1px)");
    CHECK(width("calc(1px * 1px)").isEmpty());
    CHECK(width("min(1px * 1px, 2px)").isEmpty());
}

TEST_CASE("+ and - need whitespace on both sides") {
    CHECK(width("calc(1px + 2px)") == "calc(3px)");
    CHECK(width("calc(1px +2px)").isEmpty());
    CHECK(width("calc(1px*2)") == "calc(2px)");
    CHECK(width("calc((1px) - (2px))") == "calc(-1px)");
    CHECK(width("calc((1px)-(2px))").isEmpty());
}

TEST_CASE("a calculation is simplified as a specified value") {
    CHECK(width("calc(20px + 30px)") == "calc(50px)");
    CHECK(width("calc(1in + 4px)") == "calc(100px)");
    CHECK(width("calc(10px + 1vmin + 10%)") == "calc(10% + 10px + 1vmin)");
    CHECK(width("calc(20px + 0%)") == "calc(0% + 20px)");
    CHECK(width("calc(1em - (2px + 1em))") == "calc(0em - 2px)");
    CHECK(width("calc(2 * (1em + 2px))") == "calc(2em + 4px)");
    CHECK(width("calc(calc(2 + 3) * 4px)") == "calc(20px)");
    CHECK(width("calc(1px - -2px)") == "calc(3px)");
    CHECK(width("calc(1em / 2)") == "calc(0.5em)");
    CHECK(width("calc(1em / 1px)").isEmpty());
    CHECK(specified("rotate", "calc(1turn)") == "calc(360deg)");
    CHECK(specified("transition-delay", "calc(100ms)") == "calc(0.1s)");
}

TEST_CASE("a calculation that cannot be simplified keeps its operators") {
    CHECK(width("calc(1px * 1em / 1px)") == "calc(1em * 1px / 1px)");
    CHECK(width("calc(-1 * (1em + 1vw))") == "calc(-1em - 1vw)");
    CHECK(width("calc(1em - min(1px, 1vw))") == "calc(1em - min(1px, 1vw))");
    CHECK(width("calc(2 / min(1em, 1px) * 1px * 1px)") == "calc(2 * 1px * 1px / min(1em, 1px))");
}

TEST_CASE("a comparison function combines the arguments it can compare") {
    CHECK(width("min(10px, 20px)") == "calc(10px)");
    CHECK(width("min(10px, 20px, 1em)") == "min(10px, 1em)");
    CHECK(width("max(1px + 1%)") == "calc(1% + 1px)");
    CHECK(width("clamp(10px, 35px, 30px)") == "calc(30px)");
    CHECK(width("calc(clamp(10px, 35px, 30px))") == "calc(30px)");
    CHECK(width("clamp(none, 2px, 3em)") == "min(2px, 3em)");
    CHECK(width("clamp(1em, 2px, none)") == "max(1em, 2px)");
    CHECK(width("clamp(none, 1em, none)") == "calc(1em)");
    CHECK(width("clamp( none , 5px , none )") == "calc(5px)");
}

TEST_CASE("the other math functions compute where their arguments are numbers in one unit") {
    CHECK(width("round(15px, 10px)") == "calc(20px)");
    CHECK(width("round(down, 15px, 10px)") == "calc(10px)");
    CHECK(width("round(to-zero, -15px, 10px)") == "calc(-10px)");
    CHECK(width("round(1.5em, 1px)") == "round(1.5em, 1px)");
    CHECK(width("round(15px)").isEmpty());         // B may be left out for a number only
    CHECK(specified("z-index", "round(2.5)") == "calc(3)");
    CHECK(width("mod(-18px, 5px)") == "calc(2px)");
    CHECK(width("rem(-18px, 5px)") == "calc(-3px)");
    CHECK(width("hypot(3px, 4px)") == "calc(5px)");
    CHECK(width("abs(-2em)") == "calc(2em)");
    CHECK(width("calc(1px * sign(-2px))") == "calc(-1px)");
    // A number from a relative length or a percentage waits for the context that resolves it.
    CHECK(width("calc(1px * sign(-2em))") == "calc(1px * sign(-2em))");
    CHECK(specified("rotate", "atan2(1em, -1em)") == "atan2(1em, -1em)");
    CHECK(width("calc(1px * pow(2, 3) + sqrt(4) * 1px)") == "calc(10px)");
    CHECK(width("calc(1px * log(e))") == "calc(1px)");
    CHECK(width("calc(1px * exp(0))") == "calc(1px)");
    CHECK(specified("rotate", "acos(0)") == "calc(90deg)");
    CHECK(specified("rotate", "atan2(1px, -1px)") == "calc(135deg)");
    CHECK(specified("scale", "sin(90deg)") == "calc(1)");
    CHECK(specified("opacity", "progress(5px, 0px, 10px)") == "calc(0.5)");
    CHECK(width("pow(2px, 2)").isEmpty());
    CHECK(specified("rotate", "asin(1deg)").isEmpty());
}

TEST_CASE("an infinite or NaN value is its keyword times one of its unit") {
    CHECK(width("calc(1px * infinity)") == "calc(infinity * 1px)");
    CHECK(width("calc(1in * NaN)") == "calc(NaN * 1px)");
    CHECK(width("calc(1% * -infinity)") == "calc(-infinity * 1%)");
    CHECK(width("calc(1px * 0 / 0)") == "calc(NaN * 1px)");
    CHECK(width("calc(1 * min(NaN * 2px, NaN * 4em))") == "calc(1 * min(NaN * 1px, NaN * 1em))");
    CHECK(specified("scale", "calc(1 / 0)") == "calc(infinity)");
    CHECK(width("calc(1px * iNfInItY)") == "calc(infinity * 1px)");
}

TEST_CASE("a math function nests to the limit and no further") {
    String deep("calc(1px");
    for (uint32_t i = 0; i < kCssMathMaxDepth + 1; ++i)
        deep = format("calc({})", deep);
    for (uint32_t i = 0; i < kCssMathMaxDepth + 2; ++i)
        deep.append(")");
    CHECK(specified("width", deep).isEmpty());
    CHECK(width("calc(calc(calc(1px)))") == "calc(1px)");
}

TEST_CASE("an anchor function writes its anchor name first") {
    CHECK(specified("left", "anchor(inside --foo)") == "anchor(--foo inside)");
    CHECK(specified("left", "anchor(inside --foo, 1px)") == "anchor(--foo inside, 1px)");
    CHECK(specified("left", "anchor(--foo 50%, 0)") == "anchor(--foo 50%, 0px)");
    CHECK(specified("left", "anchor(min(50%, 100%))") == "anchor(calc(50%))");
    CHECK(width("anchor-size(width --foo, 50%)") == "anchor-size(--foo width, 50%)");
    CHECK(width("anchor-size()") == "anchor-size()");
    CHECK(width("anchor-size(10px)") == "anchor-size(10px)");
}

TEST_CASE("an anchor function is valid only in the properties its spec names") {
    CHECK(specified("top", "anchor(--foo top)") == "anchor(--foo top)");
    CHECK(specified("inset-block-start", "anchor(top)") == "anchor(top)");
    CHECK(width("anchor(--foo top)").isEmpty());
    CHECK(specified("margin-left", "anchor-size(width)") == "anchor-size(width)");
    CHECK(specified("padding-left", "anchor-size(width)").isEmpty());
    CHECK(specified("flex-basis", "anchor-size(width)").isEmpty());
}

TEST_CASE("an anchor function is a <length> inside a calculation") {
    CHECK(specified("top", "calc((anchor(--foo top) + anchor(--bar bottom)) / 2)") ==
        "calc(0.5 * (anchor(--foo top) + anchor(--bar bottom)))");
    CHECK(specified("top", "calc(anchor(--foo left, 1px) + 10%)") == "calc(10% + anchor(--foo left, 1px))");
    CHECK(specified("top", "calc(anchor(--foo left, 0))") == "anchor(--foo left, 0px)");
    CHECK(specified("top", "anchor(--foo top, calc(0.5 * anchor(--bar bottom)))") ==
        "anchor(--foo top, calc(0.5 * anchor(--bar bottom)))");
    CHECK(specified("top", "min(100px, 10%, anchor(--foo top))") == "min(100px, 10%, anchor(--foo top))");
}

TEST_CASE("a percentage that resolves against another type is not compared before it is") {
    CHECK(specified("text-indent", "min(1%, 2%, 3%)") == "min(1%, 2%, 3%)");
    CHECK(specified("text-indent", "calc(1% + 2%)") == "calc(3%)");
    CHECK(specified("opacity", "min(10%, 20%)") == "calc(10%)");
}

TEST_CASE("a random function's key is written as its canonical parts") {
    CHECK(width("random(0px, 100px)") == "random(element-scoped ua-width-1, 0px, 100px)");
    CHECK(width("random(auto, 0px, 100px)") == "random(element-scoped ua-width-1, 0px, 100px)");
    CHECK(width("random(property-scoped element-scoped --foo, 0px, 100px)") == "random(--foo element-scoped ua-width, 0px, 100px)");
    CHECK(width("random(--foo, 1em, 2em)") == "random(--foo, 1em, 2em)");
    CHECK(width("calc(random(1px, 2em) + random(1px, 2em))") ==
        "calc(random(element-scoped ua-width-1, 1px, 2em) + random(element-scoped ua-width-2, 1px, 2em))");
    CHECK(width("random(foo, 1px, 2px)").isEmpty());
    CHECK(width("random(--foo --bar, 1px, 2px)").isEmpty());
}

TEST_CASE("a random function with a fixed base is its value") {
    CHECK(width("random(fixed 0.5, 0px, 100px)") == "calc(50px)");
    CHECK(width("random(fixed 0.5, 0px, 100px, 3px)") == "calc(51px)");
    CHECK(width("random(fixed calc(2 / 4), 0%, 100%)") == "random(fixed calc(0.5), 0%, 100%)");
}

TEST_CASE("a tree-counting function is an integer, and a leaf of a calculation") {
    CHECK(specified("z-index", "sibling-index()") == "sibling-index()");
    CHECK(specified("z-index", "calc(sibling-count( ))") == "sibling-count()");
    CHECK(specified("left", "calc(1px * sibling-index())") == "calc(1px * sibling-index())");
    CHECK(specified("rotate", "calc(sibling-index() * 2rad * pi)") == "calc(360deg * sibling-index())");
    CHECK(specified("z-index", "sibling-index(1)").isEmpty());
    CHECK(specified("left", "calc(10 * sibling-index())").isEmpty());
}
