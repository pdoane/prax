#include "css/src/css_counter.h"
#include "css/src/css_parse.h"
#include "css/src/css_property.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns the representation of `value` in the predefined style `name`.
String represent(const char* name, int32_t value, CssCounterFlow flow = kCssCounterFlowDefault) {
    const CssCounterStyle* style = cssCounterStyleFind(nullptr, name);
    REQUIRE(style != nullptr);
    PrTextBuffer out{};
    cssCounterRepresent(&out, nullptr, style, value, flow);
    return take(&out);
}

// Returns the marker string that a list-style-type, written as CSS, gives the list-item counter `value`, or "none".
String marker(const char* type, int32_t value) {
    CssTestSource source(type);
    CssValuePool pool{};
    CssMatchState match{};
    CssValueSpan span{};
    REQUIRE(cssParsePropertyValue(PrCssProperty_ListStyleType, &source.ts, &pool, &match, &span));
    PrTextBuffer out{};
    bool any = cssCounterAppendMarker(&out, nullptr, cssValuePoolRun(&pool, span), value, kCssCounterFlowDefault);
    cssValuePoolReset(&pool);
    cssMatchStateReset(&match);
    String text = take(&out);
    return any ? text : String("none");
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("each counter system represents a value as css-counter-styles-3 defines it") {
    // Numeric: positional, with zero as the first symbol and a negative sign.
    CHECK(represent("decimal", 0) == "0");
    CHECK(represent("decimal", 1907) == "1907");
    CHECK(represent("decimal", -12) == "-12");
    CHECK(represent("decimal", INT32_MIN) == "-2147483648");
    CHECK(represent("arabic-indic", 10) == "\xD9\xA1\xD9\xA0");
    // Alphabetic: bijective, from 1.
    CHECK(represent("lower-alpha", 1) == "a");
    CHECK(represent("lower-alpha", 26) == "z");
    CHECK(represent("lower-alpha", 27) == "aa");
    CHECK(represent("upper-alpha", 702) == "ZZ");
    // Additive: the greatest weights first.
    CHECK(represent("upper-roman", 1994) == "MCMXCIV");
    CHECK(represent("lower-roman", 4) == "iv");
    CHECK(represent("hebrew", 15) == "\xD7\x98\xD7\x95");
    // Cyclic: one symbol, whatever the value.
    CHECK(represent("disc", 5) == "\xE2\x80\xA2");
    CHECK(represent("square", -2) == "\xE2\x96\xAA");
    // Fixed: each symbol once, from the first value.
    CHECK(represent("cjk-heavenly-stem", 1) == "\xE7\x94\xB2");
    // The disclosure styles' symbols follow the direction.
    CHECK(represent("disclosure-open", 1) == "\xE2\x96\xBE");
    CHECK(represent("disclosure-closed", 1) == "\xE2\x96\xB8");
    CHECK(represent("disclosure-closed", 1, CssCounterFlow{CssCounterSide_Left, CssCounterSide_Down}) == "\xE2\x97\x82");
    // In vertical-rl, closed points down to the inline end, and open left to the block end.
    CssCounterFlow verticalRl{CssCounterSide_Down, CssCounterSide_Left};
    CHECK(represent("disclosure-closed", 1, verticalRl) == "\xE2\x96\xBE");
    CHECK(represent("disclosure-open", 1, verticalRl) == "\xE2\x97\x82");
}

TEST_CASE("a value outside a counter style's range, or that it cannot represent, uses its fallback") {
    // Roman numerals stop at 3999, and lower-alpha starts at 1. Both fall back to decimal.
    CHECK(represent("upper-roman", 4000) == "4000");
    CHECK(represent("upper-roman", 0) == "0");
    CHECK(represent("lower-alpha", 0) == "0");
    CHECK(represent("lower-alpha", -3) == "-3");
    // A pad counts the negative sign, and pads after it.
    CHECK(represent("decimal-leading-zero", 7) == "07");
    CHECK(represent("decimal-leading-zero", 123) == "123");
    CHECK(represent("decimal-leading-zero", -7) == "-7");
    // The Japanese styles have a negative sign of their own.
    CHECK(represent("japanese-informal", -1) == "\xE3\x83\x9E\xE3\x82\xA4\xE3\x83\x8A\xE3\x82\xB9\xE4\xB8\x80");
    // A predefined name is matched however its case is written.
    CHECK(represent("Upper-Roman", 3) == "III");
}

TEST_CASE("a representation is no longer than the limit, past which the fallback represents the value") {
    CHECK(marker("symbols(symbolic '*')", 3) == "*** ");
    CHECK(marker("symbols(symbolic '*')", 61) == "61 ");
    CHECK(marker("symbols(cyclic 'x' 'y')", 4) == "y ");
    CHECK(marker("symbols(fixed 'x' 'y')", 3) == "3 ");
    CHECK(marker("symbols(alphabetic 'a' 'b')", 3) == "aa ");
    CHECK(marker("symbols(numeric '0' '1')", 5) == "101 ");
}

TEST_CASE("a marker string is the counter style's prefix, representation and suffix, or the string that is given") {
    CHECK(marker("decimal", 3) == "3. ");
    CHECK(marker("lower-roman", 9) == "ix. ");
    CHECK(marker("disc", 3) == "\xE2\x80\xA2 ");
    CHECK(marker("cjk-decimal", 3) == "\xE4\xB8\x89\xE3\x80\x81");
    CHECK(marker("'- '", 3) == "- ");
    CHECK(marker("none", 3) == "none");
    // A style that no rule defines is decimal.
    CHECK(marker("no-such-style", 3) == "3. ");
}

TEST_CASE("the Chinese longhand styles and ethiopic-numeric represent values by their algorithms") {
    // The examples of css-counter-styles-3/#limited-chinese.
    CHECK(represent("simp-chinese-informal", 0) == "零");
    CHECK(represent("simp-chinese-informal", 10) == "十");
    CHECK(represent("simp-chinese-informal", 11) == "十一");
    CHECK(represent("simp-chinese-informal", 99) == "九十九");
    CHECK(represent("simp-chinese-informal", 101) == "一百零一");
    CHECK(represent("simp-chinese-informal", 110) == "一百一十");
    CHECK(represent("simp-chinese-informal", 6001) == "六千零一");
    CHECK(represent("simp-chinese-formal", 10) == "壹拾");
    CHECK(represent("simp-chinese-formal", 6001) == "陆仟零壹");
    CHECK(represent("trad-chinese-formal", 6001) == "陸仟零壹");
    CHECK(represent("trad-chinese-informal", -3) == "負三");
    CHECK(represent("cjk-ideographic", 1010) == "一千零一十");
    // Past 9999, the groups of four digits take their markers (css-counter-styles-3/#extended-range-optional).
    CHECK(represent("simp-chinese-informal", 12345) == "一万二千三百四十五");
    CHECK(represent("simp-chinese-informal", 10001) == "一万零一");
    CHECK(represent("simp-chinese-informal", 100000001) == "一亿零一");
    CHECK(represent("simp-chinese-informal", 100100) == "十万零一百");
    CHECK(represent("trad-chinese-informal", 100000000) == "一億");
    CHECK(represent("simp-chinese-informal", INT32_MAX) == "二十一亿四千七百四十八万三千六百四十七");
    CHECK(represent("japanese-informal", 1111) == "千百十一");
    CHECK(represent("japanese-informal", 12345) == "一万二千三百四十五");
    CHECK(represent("japanese-formal", 1000) == "壱阡");
    CHECK(represent("japanese-informal", -5) == "マイナス五");
    CHECK(represent("korean-hangul-formal", 12345) == "일만 이천삼백사십오");
    CHECK(represent("korean-hangul-formal", 100000000) == "일억");
    CHECK(represent("korean-hanja-informal", 10000) == "萬");
    CHECK(represent("korean-hanja-informal", 1111) == "千百十一");
    CHECK(represent("korean-hanja-formal", 10) == "壹拾");
    // hebrew reaches past 10999, as css-counter-styles-3/#simple-numeric allows: the thousands, a geresh and the rest.
    CHECK(represent("hebrew", 10999) == "י׳תתקצט");
    CHECK(represent("hebrew", 11000) == "יא׳");
    CHECK(represent("hebrew", 11001) == "יא׳א");
    CHECK(represent("hebrew", 999999) == "תתקצט׳תתקצט");
    CHECK(represent("hebrew", 1000000) == "1000000");
    // The examples of css-counter-styles-3/#ethiopic-numeric-counter-style.
    CHECK(represent("ethiopic-numeric", 1) == "፩");
    CHECK(represent("ethiopic-numeric", 100) == "፻");
    CHECK(represent("ethiopic-numeric", 78010092) == "፸፰፻፩፼፺፪");
    CHECK(represent("ethiopic-numeric", 0) == "0");
}
