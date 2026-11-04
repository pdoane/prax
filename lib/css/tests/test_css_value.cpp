#include "css/src/css_color.h"
#include "css/src/css_keyword.h"
#include "css/src/css_parse.h"
#include "css/src/css_property.h"
#include "css/src/css_serialize.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A source and a pool for the values that parsing it produces.
struct Source : CssTestSource {
    CssValuePool pool = {};
    CssMatchState match = {};

    explicit Source(PrStringView source) : CssTestSource(source) {}

    ~Source() {
        cssValuePoolReset(&pool);
        cssMatchStateReset(&match);
    }

    // Parses the whole source as `def` and reads back the first value that it added.
    bool parse(CssValueDef def, CssValueNode* out) {
        CssValueSpan span;
        if (!cssParseValue(kCssValueDefGrammarRoot[def], &ts, &pool, &match, &span))
            return false;
        *out = pool.values.data[span.start];
        return true;
    }

    const CssValueNode& at(uint32_t index) const { return pool.values.data[index]; }

    std::string text(const CssValueNode& value) const {
        return textOf(PrStringView{pool.text.data + value.text.start, value.text.length});
    }

    bool parseProperty(PrStringView name, CssValueSpan* out) {
        PrCssProperty property = cssPropertyFromText(name);
        REQUIRE(property != PrCssProperty_Count);
        return cssParsePropertyValue(property, &ts, &pool, &match, out);
    }
};

// Returns true if a property matches a value, for the cases with nothing else to check.
bool matches(PrStringView property, PrStringView value) {
    Source s(value);
    CssValueSpan span;
    return s.parseProperty(property, &span);
}

// Returns a value of a property as it serializes once parsed, or the empty string if it does not parse.
std::string serialized(PrStringView property, PrStringView value) {
    Source s(value);
    CssValueSpan span;
    if (!s.parseProperty(property, &span))
        return std::string();
    PrTextBuffer out = {};
    cssSerializeValues(&out, cssValuePoolRun(&s.pool, span));
    return take(&out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a number takes any spelling, an integer only the ones without a fraction") {
    Source integer("12");
    CssValueNode value;
    REQUIRE(integer.parse(CssValueDef_Integer, &value));
    CHECK(value.kind == PrCssValueKind_Integer);
    CHECK(value.number == 12);

    Source fraction("1.5");
    CHECK_FALSE(fraction.parse(CssValueDef_Integer, &value));
    REQUIRE(fraction.parse(CssValueDef_Number, &value));
    CHECK(value.kind == PrCssValueKind_Number);
    CHECK(value.number == 1.5);
}

TEST_CASE("a failed terminal leaves the stream where it was") {
    Source s("12px");
    CssValueNode value;
    CHECK_FALSE(s.parse(CssValueDef_Angle, &value));
    CHECK(s.ts.pos == 0);
    REQUIRE(s.parse(CssValueDef_Length, &value));
    CHECK(s.ts.pos == 1);
}

TEST_CASE("a dimension parses as the type its unit measures") {
    struct Case {
        PrStringView text;
        CssValueDef def;
        PrCssUnit unit;
    };
    for (const Case& test : {Case{"3px", CssValueDef_Length, PrCssUnit_Px},
        Case{"45deg", CssValueDef_Angle, PrCssUnit_Deg}, Case{"2s", CssValueDef_Time, PrCssUnit_S},
                Case{"50Hz", CssValueDef_Frequency, PrCssUnit_Hz},
                Case{"2dppx", CssValueDef_Resolution, PrCssUnit_Dppx},
                Case{"1fr", CssValueDef_Flex, PrCssUnit_Fr}, Case{"-6dB", CssValueDef_Decibel, PrCssUnit_DB},
                Case{"2st", CssValueDef_Semitones, PrCssUnit_St}}) {
        INFO(test.text);
        Source s(test.text);
        CssValueNode value;
        REQUIRE(s.parse(test.def, &value));
        CHECK(value.kind == PrCssValueKind_Dimension);
        CHECK(value.unit == test.unit);
    }
}

TEST_CASE("a unit belongs to one type only") {
    Source s("3px");
    CssValueNode value;
    CHECK_FALSE(s.parse(CssValueDef_Time, &value));
    CHECK_FALSE(s.parse(CssValueDef_Angle, &value));
    CHECK_FALSE(s.parse(CssValueDef_Number, &value));
}

TEST_CASE("only a length may drop the unit from a zero") {
    Source length("0");
    CssValueNode value;
    REQUIRE(length.parse(CssValueDef_Length, &value));
    CHECK(value.kind == PrCssValueKind_Dimension);
    CHECK(value.unit == PrCssUnit_Px);
    CHECK(value.number == 0);

    for (CssValueDef def : {CssValueDef_Angle, CssValueDef_Time, CssValueDef_Flex}) {
        Source s("0");
        CHECK_FALSE(s.parse(def, &value));
    }
}

TEST_CASE("a zero is the literal number, whatever the sign") {
    Source zero("+0");
    CssValueNode value;
    REQUIRE(zero.parse(CssValueDef_Zero, &value));
    CHECK(value.number == 0);

    Source other("1");
    CHECK_FALSE(other.parse(CssValueDef_Zero, &value));
}

TEST_CASE("an unrecognized unit is a dimension and nothing narrower") {
    Source s("3quux");
    CssValueNode value;
    CHECK_FALSE(s.parse(CssValueDef_Length, &value));
    REQUIRE(s.parse(CssValueDef_Dimension, &value));
    CHECK(value.unit == PrCssUnit_Unknown);
    CHECK(value.number == 3);
}

TEST_CASE("a percentage is its own type") {
    Source s("50%");
    CssValueNode value;
    REQUIRE(s.parse(CssValueDef_Percentage, &value));
    CHECK(value.kind == PrCssValueKind_Percentage);
    CHECK(value.number == 50);
}

TEST_CASE("an ident value is its decoded text, not the text it was written as") {
    Source s("re\\64 ");
    CssValueNode value;
    REQUIRE(s.parse(CssValueDef_Ident, &value));
    CHECK(value.kind == PrCssValueKind_Ident);
    CHECK(s.text(value) == "red");
}

TEST_CASE("a string value drops its quotes") {
    Source s("\"a\\\"b\"");
    CssValueNode value;
    REQUIRE(s.parse(CssValueDef_String, &value));
    CHECK(value.kind == PrCssValueKind_String);
    CHECK(s.text(value) == "a\"b");
}

TEST_CASE("a custom ident is any ident but the CSS-wide keywords") {
    for (PrStringView text : {"inherit", "INITIAL", "unset",
        "revert", "revert-layer", "default"}) {
        INFO(textOf(text));
        Source s(text);
        CssValueNode value;
        CHECK_FALSE(s.parse(CssValueDef_CustomIdent, &value));
        CHECK(s.ts.pos == 0);
    }

    Source s("inheritance");
    CssValueNode value;
    REQUIRE(s.parse(CssValueDef_CustomIdent, &value));
    CHECK(s.text(value) == "inheritance");
}

TEST_CASE("a custom ident excludes the keywords its property's prose names") {
    // css-grid-2/#typedef-grid-row-start-grid-line: `span` is the keyword, not a line name.
    CHECK(serialized("grid-row-start", "span 2") == "span 2");
    CHECK(serialized("grid-row-start", "2 span") == "span 2");
    CHECK(serialized("grid-row-start", "span calc(-2)") == "span calc(-2)");
    CHECK_FALSE(matches("grid-row-start", "span"));
    CHECK_FALSE(matches("grid-row-start", "2 auto"));
    CHECK(matches("grid-template-areas", "\"span auto\""));
}

TEST_CASE("a dashed ident needs the two hyphens, however they are escaped") {
    for (PrStringView text : {"--x", "\\--x", "-\\-x"}) {
        INFO(textOf(text));
        Source s(text);
        CssValueNode value;
        REQUIRE(s.parse(CssValueDef_DashedIdent, &value));
        CHECK(s.text(value) == "--x");
    }

    Source s("-x");
    CssValueNode value;
    CHECK_FALSE(s.parse(CssValueDef_DashedIdent, &value));
}

TEST_CASE("a declaration value keeps the source text of the run it covered") {
    // css-variables-1/#serializing-custom-props: comments and whitespace are kept as written.
    Source s("1px  so\\6c id /* x */ red");
    CssValueNode value;
    REQUIRE(s.parse(CssValueDef_DeclarationValue, &value));
    CHECK(value.kind == PrCssValueKind_Tokens);
    CHECK(s.text(value) == "1px  so\\6c id /* x */ red");
}

TEST_CASE("a declaration value's last token is written as it was read where the input ends in it") {
    // css-syntax-3/#consume-escaped-code-point: an escape at the end of the input is U+FFFD, and a string or url that
    // the input ended inside is closed.
    struct Case {
        PrStringView written;
        const char* kept;
    };
    for (const Case& test : {Case{"a  foo\\", "a  foo\uFFFD"},
        Case{"url(foo\\", "url(foo\uFFFD)"}, Case{"\"foo\\", "\"foo\""}, Case{"a  b", "a  b"}}) {
        INFO(textOf(test.written));
        Source s(test.written);
        CssValueNode value;
        REQUIRE(s.parse(CssValueDef_DeclarationValue, &value));
        CHECK(s.text(value) == test.kept);
    }
}

TEST_CASE("a declaration value holds no top-level bang or semicolon") {
    for (PrStringView text : {"red !important", "red; rest"}) {
        INFO(textOf(text));
        Source s(text);
        CssValueNode value;
        CHECK_FALSE(s.parse(CssValueDef_DeclarationValue, &value));
    }
}

TEST_CASE("the pool grows for a value that decodes wider than it was written") {
    // Each \0 is two characters that decode to three bytes. Enough of them overrun the slack that the first decode
    // writes into.
    Source s("a\\0\\0\\0\\0");
    CssValueNode value;
    REQUIRE(s.parse(CssValueDef_Ident, &value));
    CHECK(s.text(value) == "a\uFFFD\uFFFD\uFFFD\uFFFD");
    CHECK(value.text.length == 13);
}

TEST_CASE("a keyword matches whatever case it was written in") {
    Source s("THIN");
    CssValueSpan span;
    REQUIRE(s.parseProperty("border-top-width", &span));
    REQUIRE(span.count == 1);

    const CssValueNode& value = s.pool.values.data[span.start];
    CHECK(value.kind == PrCssValueKind_Keyword);
    CHECK(cssKeywordName(cssValueKeyword(&value)) == "thin");
}

TEST_CASE("a multiplier repeats up to its bound and no further") {
    CHECK(matches("margin", "1px"));
    CHECK(matches("margin", "1px 2px 3px 4px"));
    CHECK_FALSE(matches("margin", "1px 2px 3px 4px 5px"));
}

TEST_CASE("a # multiplier wants commas between the repetitions") {
    CHECK(matches("transition-duration", "1s, 2s"));
    CHECK_FALSE(matches("transition-duration", "1s 2s"));
}

TEST_CASE("a bracketed range bounds what the type accepts") {
    CHECK(matches("border-top-width", "1px"));
    CHECK_FALSE(matches("border-top-width", "-1px"));
    CHECK(matches("z-index", "1"));
    CHECK_FALSE(matches("z-index", "1.5"));
}

TEST_CASE("an alternative that fits by itself gives way to one that fits the whole value") {
    // <position> tries one component before two, and `0 0` is two.
    CHECK(matches("background-position", "0 0"));
    CHECK(matches("background-position", "0"));

    // <bg-layer>#? accepts `none`, and must give it back for <final-bg-layer>.
    CHECK(matches("background", "none"));
}

TEST_CASE("an any-order group takes its parts in either order") {
    CHECK(matches("background-position", "top center"));
    CHECK(matches("background-position", "center top"));
    CHECK(matches("text-decoration-line", "underline overline"));
    CHECK(matches("text-decoration-line", "overline underline"));
}

TEST_CASE("a function holds what it matched as its children") {
    Source s("fit-content(10px)");
    CssValueSpan span;
    REQUIRE(s.parseProperty("width", &span));
    REQUIRE(span.count == 2);

    const CssValueNode& function = s.pool.values.data[span.start];
    CHECK(function.kind == PrCssValueKind_Function);
    CHECK(cssKeywordName(cssValueKeyword(&function)) == "fit-content");
    CHECK(function.next == 2);   // itself and the length below it

    const CssValueNode& length = s.pool.values.data[span.start + 1];
    CHECK(length.kind == PrCssValueKind_Dimension);
    CHECK(length.unit == PrCssUnit_Px);
    CHECK(length.number == 10);
}

TEST_CASE("a value keeps the literals its grammar matched") {
    Source s("1px / 2px");
    CssValueSpan span;
    REQUIRE(s.parseProperty("border-radius", &span));
    REQUIRE(span.count == 3);
    CHECK(s.at(span.start + 1).kind == PrCssValueKind_Literal);
    CHECK(cssValueLiteral(&s.at(span.start + 1)) == '/');

    // The comma that a # list writes between its items is a value too.
    Source list("1s, 2s");
    REQUIRE(list.parseProperty("transition-duration", &span));
    REQUIRE(span.count == 3);
    CHECK(list.at(span.start + 1).kind == PrCssValueKind_Literal);
    CHECK(cssValueLiteral(&list.at(span.start + 1)) == ',');
}

TEST_CASE("a unit no table names keeps its text") {
    Source s("3q\\75 ux");
    CssValueNode value;
    REQUIRE(s.parse(CssValueDef_Dimension, &value));
    CHECK(value.unit == PrCssUnit_Unknown);
    REQUIRE(value.next == 2);
    const CssValueNode& unit = s.at(1);
    CHECK(unit.kind == PrCssValueKind_Ident);
    CHECK(s.text(unit) == "quux");
}

TEST_CASE("a value that does not match leaves the stream and the pool alone") {
    Source s("nonsense");
    CssValueSpan span;
    CHECK_FALSE(cssParseValue(kCssPropertyGrammarRoot[PrCssProperty_ZIndex], &s.ts, &s.pool, &s.match, &span));
    CHECK(s.ts.pos == 0);
    CHECK(s.pool.values.count == 0);
    CHECK(s.pool.text.count == 0);
}

TEST_CASE("a grammar has to match the whole stream") {
    Source s("1 and then some");
    CssValueSpan span;
    CHECK_FALSE(cssParseValue(kCssPropertyGrammarRoot[PrCssProperty_ZIndex], &s.ts, &s.pool, &s.match, &span));
}

TEST_CASE("a definition with no grammar matches nothing") {
    Source s("1px");
    CssValueSpan span;
    CHECK_FALSE(cssParseValue(CssGrammarRoot_None, &s.ts, &s.pool, &s.match, &span));
}

TEST_CASE("a CSS-wide keyword is the value of any property at all") {
    for (PrStringView text : {"inherit", "INITIAL", "unset",
        "revert", "revert-layer", "revert-rule"}) {
        INFO(textOf(text));
        Source s(text);
        CssValueSpan span;
        REQUIRE(s.parseProperty("z-index", &span));
        REQUIRE(span.count == 1);
        CHECK(s.pool.values.data[span.start].kind == PrCssValueKind_Keyword);
    }

    CHECK(matches("z-index", "inherit"));
    CHECK_FALSE(matches("z-index", "inherit 1"));
}

TEST_CASE("a value reaching a substitution function is kept as tokens") {
    for (PrStringView text : {"var(--x)", "calc(var(--x) + 1px)",
        "env(safe-area-inset-top)"}) {
        INFO(textOf(text));
        Source s(text);
        CssValueSpan span;
        REQUIRE(s.parseProperty("width", &span));
        REQUIRE(span.count == 1);

        const CssValueNode& value = s.pool.values.data[span.start];
        CHECK(value.kind == PrCssValueKind_Tokens);
        CHECK(s.text(value) == textOf(text));
    }
}

TEST_CASE("an alias is parsed against the property it stands for") {
    CHECK(matches("-webkit-align-items", "center"));
    CHECK_FALSE(matches("-webkit-align-items", "1px"));
}

TEST_CASE("a property value must be the whole of what it was given") {
    CHECK_FALSE(matches("z-index", "1 2"));
    CHECK_FALSE(matches("margin", "1px )"));
}

TEST_CASE("a hex color is three, four, six or eight digits") {
    Source s("#1ac");
    CssValueSpan span;
    REQUIRE(s.parseProperty("color", &span));
    REQUIRE(span.count == 5);

    const CssValueNode& color = s.at(span.start);
    CHECK(color.kind == PrCssValueKind_Color);
    CHECK(cssValueColorSpace(&color) == PrColorSpace_Srgb);
    CHECK(color.next == 5);
    CHECK(s.at(span.start + 1).number == 0x11);
    CHECK(s.at(span.start + 2).number == 0xaa);
    CHECK(s.at(span.start + 3).number == 0xcc);
    CHECK(s.at(span.start + 4).number == 1.0);

    CHECK(matches("color", "#11aacc"));
    CHECK(matches("color", "#11aacc80"));
    CHECK(matches("color", "#1ac8"));
    CHECK_FALSE(matches("color", "#11aac"));
    CHECK_FALSE(matches("color", "#xyz"));
}

TEST_CASE("a short hex digit repeats rather than shifts") {
    Source s("#fff");
    CssValueSpan span;
    REQUIRE(s.parseProperty("color", &span));
    CHECK(s.at(span.start + 1).number == 255);
}

TEST_CASE("a named color stays the keyword it was written as") {
    // css-color-4/#resolving-sRGB-values: its declared value is the keyword, in lowercase.
    Source s("RebeccaPurple");
    CssValueSpan span;
    REQUIRE(s.parseProperty("color", &span));
    REQUIRE(span.count == 1);
    CHECK(s.at(span.start).kind == PrCssValueKind_Keyword);
    CHECK(cssValueKeyword(&s.at(span.start)) == PrCssKeyword_Rebeccapurple);
}

TEST_CASE("a system color stays a keyword, since the user picks its value") {
    Source s("CanvasText");
    CssValueSpan span;
    REQUIRE(s.parseProperty("color", &span));
    REQUIRE(span.count == 1);
    CHECK(s.at(span.start).kind == PrCssValueKind_Keyword);
    CHECK(cssValueKeyword(&s.at(span.start)) == PrCssKeyword_Canvastext);
}

TEST_CASE("a comma is omitted when what it would separate is not there") {
    // css-values-4/#component-types, which is what makes rgb( <number>#{3} , <alpha-value>? ) accept both.
    CHECK(matches("color", "rgb(1, 2, 3)"));
    CHECK(matches("color", "rgba(1, 2, 3, 0.5)"));
    CHECK(matches("color", "hsl(221, 14%, 14%)"));
    CHECK(matches("color", "rgb(1 2 3)"));

    // A comma must still separate two things that are present.
    CHECK_FALSE(matches("color", "rgb(1, 2, 3,)"));
    CHECK_FALSE(matches("color", "rgb(,1, 2, 3)"));

    // [ <linear-color-hint>? , <linear-color-stop> ] starts with the stop when the hint and its comma are omitted.
    CHECK(matches("background-image", "linear-gradient(45deg, red 25%, blue 50%)"));
    CHECK(matches("background-image", "linear-gradient(red, 30%, blue)"));
}

TEST_CASE("a value that cannot match gives up rather than searching forever") {
    // Backtracking over a comma-separated list grows with its length, so the matcher counts the nodes that it enters.
    std::string text;
    for (int i = 0; i < 16; ++i)
        text += "url(a) luminance 0 0 / 1px 1px repeat border-box border-box add, ";
    text += "ZZZ";

    Source s(text);
    CssValueSpan span;
    CHECK_FALSE(s.parseProperty("mask", &span));
}

TEST_CASE("a family name is its identifiers joined by single spaces") {
    Source s("Times   New\\ Roman");
    CssValueSpan span;
    REQUIRE(s.parseProperty("font-family", &span));
    REQUIRE(span.count == 1);
    CHECK(s.at(span.start).kind == PrCssValueKind_String);
    CHECK(s.text(s.at(span.start)) == "Times New Roman");
}

TEST_CASE("no font-family keyword opens a family name, and a CSS-wide keyword is one only among others") {
    CHECK(serialized("font-family", "Serif") == "serif");
    CHECK(serialized("font-family", "cursive serif") == "");
    CHECK(serialized("font-family", "simple serif") == "simple serif");
    CHECK(serialized("font-family", "a, inherit") == "");
    CHECK(serialized("font-family", "a, default") == "");
    CHECK(serialized("font-family", "inherit a") == "inherit a");
    CHECK(serialized("font-family", "a default") == "a default");
}

TEST_CASE("a family name serializes as identifiers where they read back as it") {
    CHECK(serialized("font-family", "'Twisty Tie', \"x\"") == "Twisty Tie, x");
    CHECK(serialized("font-family", "'34J'") == "\"34J\"");
    CHECK(serialized("font-family", "'A  B'") == "\"A  B\"");
    CHECK(serialized("font-family", "\"serif\", 'initial', 'a:b', ''") == "\"serif\", \"initial\", \"a:b\", \"\"");
    CHECK(serialized("font-family", "'initial a'") == "initial a");
    CHECK(serialized("font-family", "\\1f4a9") == "\xf0\x9f\x92\xa9");
}

TEST_CASE("a voice name is a family name, but alone none of the keywords it would be read as") {
    CHECK(serialized("voice-family", "john   doe, 'Henry'") == "john doe, Henry");
    CHECK(serialized("voice-family", "'male'") == "\"male\"");
    CHECK(serialized("voice-family", "young male 2") == "young male 2");
    CHECK(serialized("voice-family", "default") == "");
    CHECK(serialized("voice-family", "a, preserve") == "");
    CHECK(serialized("voice-family", "default voice") == "default voice");
}

TEST_CASE("a url takes each request modifier once, in the order the grammar writes them") {
    CHECK(serialized("background-image", "url(\"a\" integrity(\"x\") cross-origin(anonymous))") ==
        "url(\"a\" cross-origin(anonymous) integrity(\"x\"))");
    CHECK(serialized("background-image", "url(\"a\" cross-origin(anonymous) cross-origin(anonymous))") == "");
    CHECK(serialized("background-image", "url(\"a\" crossorigin(anonymous))") == "");
}

TEST_CASE("a calc-size() basis keyword is a sizing keyword of the property") {
    CHECK(matches("width", "calc-size(min-content, 10px)"));
    CHECK(matches("flex-basis", "calc-size(content, 10px)"));
    CHECK_FALSE(matches("width", "calc-size(content, 10px)"));
    CHECK_FALSE(matches("max-width", "calc-size(auto, 10px)"));
    CHECK_FALSE(matches("max-width", "calc-size(none, 10px)"));
}

TEST_CASE("prose productions written as grammar take what their prose says") {
    // css-ui-4/#typedef-id, css-ui-4/#typedef-target-name
    CHECK(serialized("nav-up", "#a \"_parent\"") == "#a \"_parent\"");
    CHECK_FALSE(matches("nav-up", "#1a"));
    // css-ui-4/#typedef-cursor-url-set
    CHECK(matches("cursor", "image-set(\"a.png\" 1x, url(b.png) 2x), auto"));
    CHECK_FALSE(matches("cursor", "image-set(linear-gradient(red, blue) 1x), auto"));
}

TEST_CASE("a whole-value function is a property's entire value, its arguments values of the property") {
    // css-values-5/#whole-value, css-values-5/#cycle-notation
    CHECK(serialized("list-style-type", "cycle(disc, circle, square)") == "cycle(disc, circle, square)");
    CHECK(serialized("background-position", "cycle(50px 50px, center)") == "cycle(50px 50px, center center)");
    CHECK_FALSE(matches("background-position", "10px cycle(50px, 100px)"));
    CHECK_FALSE(matches("list-style-type", "cycle(disc, 50px)"));
    CHECK_FALSE(matches("width", "cycle(1px, cycle(2px, 3px))"));
    CHECK_FALSE(matches("width", "cycle(1px, calc(2px + 1px))"));
    CHECK_FALSE(matches("width", "cycle(1px, attr(x))"));
    CHECK(serialized("width", "cycle({1px}, 2px)") == "cycle(1px, 2px)");

    // css-values-5/#funcdef-interpolate: a property that does not animate accepts no interpolate().
    CHECK(matches("float", "interpolate(90%, 0: left, 1: right)"));
    CHECK_FALSE(matches("animation-delay", "interpolate(0%, 0: 0s, 1: 2s)"));
    CHECK_FALSE(matches("color", "interpolate(90%, 0: #invalid, 1: #F00)"));

    // css-values-5/#first-valid: its arguments are any values.
    CHECK(matches("width", "first-valid(nonsense(), 10px)"));
}

TEST_CASE("an arbitrary substitution function is checked against its argument grammar") {
    // css-variables-2/#typedef-var-args: the name is checked only when it is substituted.
    CHECK(matches("width", "var(--x,)"));
    CHECK(matches("width", "var(--x ())"));
    CHECK(matches("width", "var(prop)"));
    CHECK_FALSE(matches("width", "var()"));
    CHECK_FALSE(matches("width", "var(, 10px)"));
    // css-values-5/#component-function-commas: an argument is a whole {} block, or contains none.
    CHECK(matches("width", "var({--x, --y}, 10px)"));
    CHECK_FALSE(matches("width", "var(--x {--y})"));
    CHECK_FALSE(matches("width", "var({})"));
    CHECK(matches("font-family", "random-item(--x, {Times, serif}, sans-serif)"));
    // css-env-1/#typedef-env-args
    CHECK(matches("width", "env(safe-area-inset-top, 10px)"));
    CHECK_FALSE(matches("width", "env()"));
}

TEST_CASE("an OpenType tag is four characters from space to tilde") {
    // css-fonts-4/#typedef-opentype-tag
    CHECK(serialized("font-variation-settings", "'wght' 700, \"a b \" 1") == "\"wght\" 700, \"a b \" 1");
    CHECK(serialized("font-variation-settings", "'wgt' 700") == "");
    CHECK(serialized("font-variation-settings", "'wghts' 700") == "");
    CHECK(serialized("font-variation-settings", "'w\xc3\xb8t' 700") == "");
    CHECK(serialized("font-variation-settings", "'\\77 ght' 700") == "\"wght\" 700");
}

TEST_CASE("a font value takes the one spelling its prose gives values of the same meaning") {
    // css-fonts-4/#feature-tag-value
    CHECK(serialized("font-feature-settings", "\"dlig\" 1, \"smcp\" on, \"liga\" off, \"swsh\" 2") ==
        "\"dlig\", \"smcp\", \"liga\" 0, \"swsh\" 2");
    // css-fonts-5/#propdef-font-size-adjust
    CHECK(serialized("font-size-adjust", "ex-height 0.5") == "0.5");
    CHECK(serialized("font-size-adjust", "cap-height 0.5") == "cap-height 0.5");
    // csswg-drafts#11430: `oblique 0deg` is `normal`.
    CHECK(serialized("font-style", "oblique 0grad") == "normal");
    CHECK(serialized("font-style", "oblique 10deg") == "oblique 10deg");
    // csswg-drafts#13185: an omitted <visual-box> is padding-box.
    CHECK(serialized("overflow-clip-margin", "10px padding-box") == "10px");
    CHECK(serialized("overflow-clip-margin", "padding-box") == "0px");
    CHECK(serialized("overflow-clip-margin", "content-box 0px") == "content-box");
    // css-fonts-4/#font-language-override-prop
    CHECK(serialized("font-language-override", "\" en  \"") == "\" en\"");
}

TEST_CASE("the children of a || or && are held in the order the grammar writes them") {
    CHECK(serialized("text-decoration-line", "blink line-through overline") == "overline line-through blink");
    CHECK(serialized("box-shadow", "inset 4px 4px green") == "green 4px 4px inset");
    CHECK(serialized("box-shadow", "4px 4px, inset red 1px 1px") == "4px 4px, red 1px 1px inset");
    CHECK(serialized("grid-row-start", "i 2 span") == "span 2 i");
    CHECK(serialized("background-position", "bottom 10% right 20%") == "right 20% bottom 10%");
    CHECK(serialized("font-variant", "historical-forms small-caps") == "small-caps historical-forms");
}

TEST_CASE("a component that has its implied value is left out") {
    CHECK(serialized("align-self", "first baseline") == "baseline");
    CHECK(serialized("align-self", "last baseline") == "last baseline");
    CHECK(serialized("scroll-snap-type", "x proximity") == "x");
    CHECK(serialized("grid-auto-flow", "dense row") == "dense");
    CHECK(serialized("grid-row-start", "span 1 i") == "span i");

    // A required || keeps a child where it would otherwise hold nothing.
    CHECK(serialized("grid-auto-flow", "row") == "row");
    CHECK(serialized("grid-row-start", "span 1") == "span 1");

    // A comma belongs with what it separated.
    CHECK(serialized("transition-timing-function", "steps(2, end)") == "steps(2)");
    CHECK(serialized("transition-timing-function", "steps(2, jump-end)") == "steps(2)");
    CHECK(serialized("clip-path", "polygon(nonzero, 1px 2px, 3px 4px)") == "polygon(1px 2px, 3px 4px)");
    CHECK(serialized("background-image", "radial-gradient(farthest-corner at center, red, blue)") ==
        "radial-gradient(red, blue)");
}

TEST_CASE("keywords the prose says are the same value as fewer are held as those") {
    CHECK(serialized("display", "table inline") == "inline-table");
    CHECK(serialized("display", "list-item flow block") == "list-item");
    CHECK(serialized("display", "flow") == "block");
    CHECK(serialized("display", "inline ruby") == "ruby");
    CHECK(serialized("display", "block ruby") == "block ruby");
    CHECK(serialized("display", "flow-root list-item inline") == "inline flow-root list-item");
    CHECK(serialized("position-area", "span-all top") == "top");
    CHECK(serialized("position-area", "center span-all") == "center span-all");
    CHECK(serialized("position-area", "center center") == "center");
    CHECK(serialized("mask-repeat", "repeat no-repeat, space space, round") == "repeat-x, space, round");
}

TEST_CASE("a percentage written for a number is held as the number") {
    CHECK(serialized("opacity", "50%") == "0.5");
    CHECK(serialized("opacity", "calc(50%)") == "calc(50%)");
    CHECK(serialized("scale", "100% 100% 1") == "1");
    CHECK(serialized("scale", "100% 200%") == "1 2");
}

TEST_CASE("a position and a ratio are held with the components their prose writes") {
    CHECK(serialized("object-position", "left") == "left center");
    CHECK(serialized("object-position", "top") == "center top");
    CHECK(serialized("mask-position", "10%, center") == "10% center, center center");
    CHECK(serialized("clip-path", "circle(at top)") == "circle(at center top)");
    CHECK(serialized("background-image", "radial-gradient(at center, red, blue)") == "radial-gradient(red, blue)");
    CHECK(serialized("aspect-ratio", "16 auto") == "auto 16 / 1");
    CHECK(serialized("aspect-ratio", "16 / 9") == "16 / 9");
}

TEST_CASE("a rotation is held with the axis its prose writes") {
    CHECK(serialized("rotate", "z 10deg") == "10deg");
    CHECK(serialized("rotate", "0 0 2 10deg") == "10deg");
    CHECK(serialized("rotate", "0 0 -2 10deg") == "-10deg");
    CHECK(serialized("rotate", "-0.5 0 0 10deg") == "x -10deg");
    CHECK(serialized("rotate", "10deg 0 1 0") == "y 10deg");
    CHECK(serialized("rotate", "1 1 0 10deg") == "1 1 0 10deg");
}

TEST_CASE("the last repetitions of a component go while each is what it would be left out") {
    CHECK(serialized("border-image-repeat", "space space") == "space");
    CHECK(serialized("border-image-repeat", "space round") == "space round");
    CHECK(serialized("hyphenate-limit-chars", "auto auto auto") == "auto");
    CHECK(serialized("hyphenate-limit-chars", "5 2 2") == "5 2");
    CHECK(serialized("scale", "100 100 1") == "100");
    CHECK(serialized("scale", "100 200 100%") == "100 200");
    CHECK(serialized("translate", "100px 0px 0px") == "100px");
    CHECK(serialized("translate", "100px 0px 1px") == "100px 0px 1px");
    CHECK(serialized("clip-path", "inset(1px 2px 1px 2px)") == "inset(1px 2px)");
}
