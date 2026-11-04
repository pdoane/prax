#include "pr/css.h"

#include "css/src/css_object.h"
#include "css/src/css_supports.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

bool supports(const char* text) {
    return cssSupportsText(text);
}

bool matches(const char* condition) {
    std::string source = std::string("@supports ") + condition + " {}";
    CssTestSheet sheet(source);
    REQUIRE(sheet.count() == 1);
    return prCssSupportsRuleMatches(prCssSupportsRuleQuery((sheet.object(0))));
}

// Returns true if `text` names a declaration and a style rule accepts a declaration of that name with `value`.
bool supported(PrStringView text, PrStringView value) {
    CssDeclaredName name = {};
    return cssNameFromText(CssAtRule_None, text, &name) && cssDeclarationSupported(name, value);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a declaration is supported where a style rule takes it") {
    CHECK(supported("color", "red"));
    CHECK(supported("COLOR", " red "));
    CHECK(supported("width", "inherit"));
    CHECK(supported("--anything", "{ at all }"));
    CHECK(supported("--empty", ""));
    CHECK(!supported("width", "blah"));
    CHECK(!supported("color", "red !important"));
    CHECK(!supported("color", ""));
    CHECK(!supported("nonesuch", "1"));
    // css-fonts-4/#unicode-range-desc is a descriptor of @font-face, not a property.
    CHECK(!supported("unicode-range", "U+0-7F"));
}

TEST_CASE("a condition is and, or and not over what it tests") {
    CHECK(supports("(color: red)"));
    CHECK(supports("(color: red) and (width: 1px)"));
    CHECK(!supports("(color: red) and (width: blah)"));
    CHECK(supports("(color: blah) or (width: 1px)"));
    CHECK(supports("not (width: blah)"));
    CHECK(supports("((color: red) and (not (nonesuch: 1)))"));
    CHECK(supports("(color: var(--x))"));
    CHECK(supports("(--x: y)"));
    // css-conditional-3/#at-supports: <general-enclosed> is false.
    CHECK(!supports("(foo)"));
    CHECK(supports("not (foo)"));
    // Text that is not a condition is tried again in parentheses (css-conditional-3/#dom-css-supports-conditiontext).
    CHECK(supports("color: red"));
    CHECK(!supports("color: blah"));
    CHECK(!supports("(color: red) and"));
}

TEST_CASE("the functions of a condition test what the engine supports") {
    CHECK(supports("selector(a > b)"));
    CHECK(supports("selector(:is(.a, b))"));
    // css-conditional-4/#dfn-support-selector: nothing dropped from a forgiving list, and no unknown -webkit-
    // pseudo-element.
    CHECK(!supports("selector(:is(.a, :nonesuch))"));
    CHECK(!supports("selector(:where(:is(:nonesuch)))"));
    CHECK(!supports("selector(::-webkit-nonesuch)"));
    CHECK(!supports("selector(a, b)"));

    CHECK(supports("at-rule(@media)"));
    CHECK(supports("at-rule(@MEDIA)"));
    CHECK(!supports("at-rule(@charset)"));
    CHECK(supports("font-tech(color-colrv1)"));
    CHECK(supports("font-format(woff2)"));
    CHECK(!supports("font-format('woff2')"));
    CHECK(!supports("font-tech(incremental)"));
    CHECK(!supports("font-format(svg)"));
    CHECK(!supports("named-feature(anything)"));
    CHECK(!supports("env(anything)"));
}

TEST_CASE("an @supports rule matches where its condition is supported") {
    CHECK(matches("(display: flex)"));
    CHECK(!matches("(display: nonesuch)"));
    CHECK(matches("not selector(:is(:nonesuch))"));
    CHECK(matches("at-rule(@supports) and (color: red)"));
}

TEST_CASE("an at-rule() keeps the '@' of its at-keyword") {
    CssTestSheet sheet("@supports at-rule( @supports ) or at-rule(@\\31 23) {}");
    REQUIRE(sheet.count() == 1);
    PrTextBuffer buffer = {};
    cssSerializePrelude(&buffer, sheet.rule(0));
    CHECK(take(&buffer) == "at-rule(@supports) or at-rule(@\\31 23)");
}

TEST_CASE("CSS.escape serializes an identifier") {
    CHECK(take(prCssEscape("0a")) == "\\30 a");
    CHECK(take(prCssEscape("-")) == "\\-");
    CHECK(take(prCssEscape("--a")) == "--a");
    CHECK(take(prCssEscape("a b")) == "a\\ b");
    CHECK(take(prCssEscape("")) == "");
}

TEST_CASE("filter() is an image wherever an image is written, with a string or an image as its input") {
    CHECK(supported("background-image", "filter(url(a.png), blur(2px))"));
    CHECK(supported("background-image", "filter('a.png', hue-rotate(180deg)), filter(linear-gradient(red, blue), url(#f) invert())"));
    CHECK(supported("mask-image", "filter(filter(url(a.png), sepia()), opacity(0.5))"));
    CHECK(supported("list-style-image", "filter(url(a.png), grayscale(1))"));
    CHECK(supported("border-image-source", "filter(url(a.png), grayscale(1))"));
    CHECK(supported("content", "filter(url(a.png), grayscale(1))"));
    // The filter list is not optional, and filter() is no filter function.
    CHECK(!supported("background-image", "filter(url(a.png))"));
    CHECK(!supported("filter", "filter(url(a.png), blur(2px))"));
}

TEST_CASE("background-blend-mode takes a blend mode or plus-lighter for each layer") {
    CHECK(supported("background-blend-mode", "multiply, normal, plus-lighter"));
    CHECK(!supported("background-blend-mode", "multiply normal"));
}
