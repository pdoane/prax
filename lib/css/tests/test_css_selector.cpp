#include "css/src/css_parser.h"
#include "css/src/css_selector.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Parses `text` as a selector list, nested in a style rule as `nesting` says, and serializes it. Returns "" if `text`
// is not a selector list.
std::string roundTrip(PrStringView text, CssSelectorNesting nesting = CssSelectorNesting_None) {
    CssParser parser = {};
    CssSelectorContext context = {};
    context.nesting = nesting;

    PrTextBuffer buffer = {};
    PrCssSelectors* selectors = cssParseSelectorText(&parser, text, &context);
    if (selectors != nullptr)
        cssSerializeSelectors(&buffer, selectors);

    cssSelectorsDestroy(selectors);
    cssParserReset(&parser);
    return take(&buffer);
}

// Returns the specificity of the first selector of `text`.
CssSpecificity specificity(PrStringView text) {
    CssParser parser = {};
    CssSelectorContext context = {};
    PrCssSelectors* selectors = cssParseSelectorText(&parser, text, &context);
    REQUIRE(selectors != nullptr);
    CssSpecificity out = selectors->complex[selectors->list.first].specificity;
    cssSelectorsDestroy(selectors);
    cssParserReset(&parser);
    return out;
}

std::string nth(int32_t a, int32_t b) {
    PrTextBuffer buffer = {};
    cssSerializeNth(&buffer, a, b);
    return take(&buffer);
}

// Returns what the first selector of `text` styles (cssSelectorPseudo).
CssPseudo pseudoOf(PrStringView text) {
    CssParser parser = {};
    CssSelectorContext context = {};
    PrCssSelectors* selectors = cssParseSelectorText(&parser, text, &context);
    REQUIRE(selectors != nullptr);
    CssPseudo out = cssSelectorPseudo(selectors, selectors->list.first);
    cssSelectorsDestroy(selectors);
    cssParserReset(&parser);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a selector list is compounds joined by combinators") {
    CHECK(roundTrip("a") == "a");
    CHECK(roundTrip("a b") == "a b");
    CHECK(roundTrip("a   b") == "a b");
    CHECK(roundTrip("a > b + c ~ d") == "a > b + c ~ d");
    CHECK(roundTrip("a, b") == "a, b");
    CHECK(roundTrip("|a") == "|a");
    CHECK(roundTrip(".c#d") == ".c#d");

    // cssom-1/#serializing-selectors writes a combinator between two spaces and joins the list with ", ".
    CHECK(roundTrip("a>b+c~d") == "a > b + c ~ d");
    CHECK(roundTrip("a,b") == "a, b");
    CHECK(roundTrip("u+a") == "u + a");
    CHECK(roundTrip(":is(a,b>c)") == ":is(a, b > c)");
    CHECK(roundTrip(":nth-child(2n of a+b)") == ":nth-child(2n of a + b)");

    // A `|` with no name after it ends the type selector rather than prefixing one.
    CHECK(roundTrip("a||b") == "a || b");
    CHECK(roundTrip("a /for/ b") == "a /for/ b");
}

TEST_CASE("a legacy pseudo-class name is the pseudo-class it is an alias of") {
    // html/#selector-webkit-autofill
    CHECK(roundTrip(":-webkit-autofill") == ":autofill");
    CHECK(roundTrip("input:-WEBKIT-AUTOFILL ~ label") == "input:autofill ~ label");
    CHECK(roundTrip(":-webkit-autofill()") == "");
}

TEST_CASE("a compound is written in its source order, whatever order it is tested in") {
    CHECK(roundTrip(":has(a):hover[b].c#d") == ":has(a):hover[b].c#d");
    CHECK(roundTrip("*:nth-child(2):not(.a).b") == ":nth-child(2):not(.a).b");
    CHECK(roundTrip(":is(a).b::before:hover") == ":is(a).b::before:hover");
    CHECK(roundTrip(":has(> :is(a).b + .c).d") == ":has(> :is(a).b + .c).d");
}

TEST_CASE("a universal selector or a prefix that changes nothing is left out") {
    // cssom-1/#serialize-a-selector
    CHECK(roundTrip("*") == "*");
    CHECK(roundTrip("*.a") == ".a");
    CHECK(roundTrip("*::before") == "::before");
    CHECK(roundTrip("a *:hover") == "a :hover");

    // With no default namespace, `*|` means the same as no prefix. So does `|` on an attribute.
    CHECK(roundTrip("*|a") == "a");
    CHECK(roundTrip("*|*:not(*|*)") == ":not(*)");
    CHECK(roundTrip("[|a]") == "[a]");
    CHECK(roundTrip("[*|a]") == "[*|a]");
}

TEST_CASE("whitespace is the descendant combinator and nothing inside a compound") {
    CHECK(roundTrip("a .b") == "a .b");
    CHECK(roundTrip("a :hover") == "a :hover");
    CHECK(roundTrip("a:hover") == "a:hover");
    CHECK(roundTrip("a [b]") == "a [b]");
    CHECK(roundTrip("a[ b = c ]") == "a[b=\"c\"]");
    CHECK(roundTrip(": hover") == "");
    CHECK(roundTrip("a| b") == "");
    CHECK(roundTrip("[a ~= b]") == "[a~=\"b\"]");
    CHECK(roundTrip("[a ~ = b]") == "");
    CHECK(roundTrip("a | | b") == "");
}

TEST_CASE("what is not a selector list parses to nothing") {
    CHECK(roundTrip("") == "");
    CHECK(roundTrip("a,") == "");
    CHECK(roundTrip(",a") == "");
    CHECK(roundTrip("a >") == "");
    CHECK(roundTrip("a > > b") == "");
    CHECK(roundTrip("#1") == "");
    CHECK(roundTrip("a {") == "");

    // selectors-4/#invalid: a namespace prefix that no @namespace rule declares.
    CHECK(roundTrip("ns|a") == "");
    CHECK(roundTrip("[ns|a]") == "");
}

TEST_CASE("an attribute selector carries its matcher and its flag") {
    // cssom-1/#serialize-a-simple-selector writes the value as a string.
    CHECK(roundTrip("[a]") == "[a]");
    CHECK(roundTrip("[a=b]") == "[a=\"b\"]");
    CHECK(roundTrip("[a~=\"b\" i]") == "[a~=\"b\" i]");
    CHECK(roundTrip("[a^=b s]") == "[a^=\"b\" s]");
    CHECK(roundTrip("[a|=b]") == "[a|=\"b\"]");
    CHECK(roundTrip("[a$=b][c*=d]") == "[a$=\"b\"][c*=\"d\"]");
    CHECK(roundTrip("[*|a]") == "[*|a]");
    CHECK(roundTrip("[a!=b]") == "");
    CHECK(roundTrip("[a=]") == "");
    CHECK(roundTrip("[a=b c]") == "");
}

TEST_CASE("nothing but a pseudo-class follows a pseudo-element") {
    CHECK(roundTrip("a::before") == "a::before");
    CHECK(roundTrip("a::before:hover") == "a::before:hover");
    CHECK(roundTrip("a::before.x") == "");

    // selectors-4/#pseudo-element-attachment: only in the last compound.
    CHECK(roundTrip("a::before b") == "");
}

TEST_CASE("a pseudo-element is followed by what a spec allows after it") {
    // selectors-4/#pseudo-element-states: the user action and logical combination pseudo-classes may follow any
    // pseudo-element, except that the engine rejects the user action ones after some pseudo-elements, such as
    // ::selection. A spec allows other pseudo-classes after some pseudo-elements.
    CHECK(roundTrip("::before:focus-within") == "::before:focus-within");
    CHECK(roundTrip("::selection:focus-within") == "");
    CHECK(roundTrip("::selection:not(:focus)") == "");
    CHECK(roundTrip("::before:first-child") == "");
    CHECK(roundTrip("::search-text:current") == "::search-text:current");
    CHECK(roundTrip("::search-text:past") == "");
    CHECK(roundTrip("::view-transition-new(a):only-child") == "::view-transition-new(a):only-child");
    CHECK(roundTrip("::view-transition:only-child") == "");

    // selectors-4/#sub-pseudo-elements
    CHECK(roundTrip("::before::marker") == "::before::marker");
    CHECK(roundTrip("::before::after") == "");
    CHECK(roundTrip("::marker::marker") == "");
    CHECK(roundTrip("::slotted(a)::placeholder") == "::slotted(a)::placeholder");
    CHECK(roundTrip("::slotted(a)::first-line") == "");

    // css-pseudo-4/#element-backed: everything may follow ::part(), except the tree-structural pseudo-classes, :has(),
    // :scope and the :host ones (csswg-drafts#10786, csswg-drafts#10787).
    CHECK(roundTrip("::part(a):hover::before") == "::part(a):hover::before");
    CHECK(roundTrip("::part(a):first-child") == "");
    CHECK(roundTrip("::part(a):has(b)") == "");
    CHECK(roundTrip("::part(a)::before:first-child") == "");
}

TEST_CASE("a logical combination passes on what its position allows") {
    // selectors-4/#logical-combination
    CHECK(roundTrip("::before:is(:hover, .a)") == "::before:is(:hover, .a)");
    CHECK(roundTrip("::search-text:not(:current)") == "::search-text:not(:current)");
    CHECK(roundTrip("::before:not(:first-child)") == "");
    CHECK(roundTrip("::before:not(a b)") == "");
    CHECK(roundTrip(":host(:is(a b, c))") == ":host(:is(a b, c))");
    CHECK(roundTrip(":host(:not(a b))") == "");
    CHECK(roundTrip("::slotted(:is(a > b))") == "::slotted(:is(a > b))");
}

TEST_CASE("a named view transition pseudo-element takes a name and classes") {
    // css-view-transitions-2/#pseudo-element-class-additions
    CHECK(roundTrip("::view-transition-group(*)") == "::view-transition-group(*)");
    CHECK(roundTrip("::view-transition-group(\\*)") == "::view-transition-group(\\*)");
    CHECK(roundTrip("::view-transition-old(a.b.c)") == "::view-transition-old(a.b.c)");
    CHECK(roundTrip("::view-transition-new(*.b)") == "::view-transition-new(*.b)");
    CHECK(roundTrip("::view-transition-image-pair(.b)") == "::view-transition-image-pair(.b)");
    CHECK(roundTrip("::view-transition-group(*. b)") == "");
    CHECK(roundTrip("::view-transition-group(a .b)") == "");
    CHECK(roundTrip("::view-transition-group(initial)") == "");
    CHECK(roundTrip("::view-transition-group(a, b)") == "");
    CHECK(roundTrip("::view-transition-group()") == "");
}

TEST_CASE("the four CSS 2 pseudo-elements may be written with one colon") {
    CHECK(roundTrip("a:before") == "a::before");
    CHECK(roundTrip("a:FIRST-LINE") == "a::first-line");
    CHECK(roundTrip("a:before.x") == "");
    CHECK(roundTrip("a:marker") == "");
}

TEST_CASE("a pseudo-class or pseudo-element has to be one there is") {
    CHECK(roundTrip(":HOVER") == ":hover");
    CHECK(roundTrip(":bogus") == "");
    CHECK(roundTrip(":bogus(a)") == "");
    CHECK(roundTrip("::bogus") == "");
    CHECK(roundTrip(":hover()") == "");

    // selectors-4/#unknown--webkit--pseudo-elements, written in lowercase.
    CHECK(roundTrip("::-WebKit-Thing") == "::-webkit-thing");
    CHECK(roundTrip("::-webkit-thing()") == "");
}

TEST_CASE("a functional pseudo-class takes what it is defined to take") {
    CHECK(roundTrip(":is(a, b)") == ":is(a, b)");
    CHECK(roundTrip(":not(.x > .y)") == ":not(.x > .y)");
    CHECK(roundTrip(":not(a,)") == "");
    CHECK(roundTrip(":nth-child(2n of .x)") == ":nth-child(2n of .x)");
    CHECK(roundTrip(":nth-of-type(2n of .x)") == "");
    CHECK(roundTrip(":nth-child(bogus)") == "");
    CHECK(roundTrip(":dir(rtl)") == ":dir(rtl)");
    CHECK(roundTrip(":lang(en, \"fr\")") == ":lang(en, \"fr\")");
    CHECK(roundTrip("::part(a b)") == "::part(a b)");
    CHECK(roundTrip(":state(open)") == ":state(open)");
    CHECK(roundTrip(":heading(1,2)") == ":heading(1, 2)");
    CHECK(roundTrip(":heading(1.0)") == "");
    CHECK(roundTrip(":heading(2n)") == "");
    CHECK(roundTrip(":active-view-transition-type(a,b)") == ":active-view-transition-type(a, b)");
    CHECK(roundTrip(":active-view-transition-type(inherit)") == "");
    CHECK(roundTrip("::picker(SELECT)") == "::picker(select)");
    CHECK(roundTrip("::picker(input)") == "");
    CHECK(roundTrip("::scroll-button( * )") == "::scroll-button(*)");
    CHECK(roundTrip("::scroll-button(block-end)") == "::scroll-button(block-end)");
    CHECK(roundTrip("::scroll-button(north)") == "");
}

TEST_CASE("the end of the input closes the blocks and functions a selector leaves open") {
    CHECK(roundTrip("[a=\"b\"") == "[a=\"b\"]");
    CHECK(roundTrip("[a=\"b") == "[a=\"b\"]");
    CHECK(roundTrip("a:not([b") == "a:not([b])");
    CHECK(roundTrip(":nth-child(2 ") == ":nth-child(2)");
    CHECK(roundTrip("::slotted(a") == "::slotted(a)");
    CHECK(roundTrip("[a ,b") == "");
}

TEST_CASE("the arguments of :is() and :where() forgive what is not a selector") {
    // selectors-4/#forgiving-selector: an item that is not a selector is kept as written, as csswg-drafts#8356
    // resolved.
    CHECK(roundTrip(":is(a,)") == ":is(a, )");
    CHECK(roundTrip(":where(a, :bogus, b)") == ":where(a, :bogus, b)");
    CHECK(roundTrip(":is()") == ":is()");
    CHECK(roundTrip(":is(  ::before  )") == ":is(::before)");
    CHECK(roundTrip(":is(.b+.c, a b)") == ":is(.b + .c, a b)");
}

TEST_CASE(":has() takes relative selectors and no :has()") {
    CHECK(roundTrip(":has(> a, + b)") == ":has(> a, + b)");
    CHECK(roundTrip(":has(a)") == ":has(a)");
    CHECK(roundTrip(":has(:has(a))") == "");

    // An :is() inside a :has() forgives the nested :has() that it cannot hold, which it keeps as written.
    CHECK(roundTrip(":has(:is(:has(a)))") == ":has(:is(:has(a)))");
}

TEST_CASE("a nested rule's selectors are relative to its parent") {
    // css-nesting-1/#nesting: `&` is implied unless the selector contains one and starts with no combinator. It is
    // written out when the list serializes (css-nesting-1/#cssom).
    CHECK(roundTrip("a", CssSelectorNesting_Style) == "& a");
    CHECK(roundTrip("> a", CssSelectorNesting_Style) == "& > a");
    CHECK(roundTrip("&.x", CssSelectorNesting_Style) == "&.x");
    CHECK(roundTrip("a &", CssSelectorNesting_Style) == "a &");
    CHECK(roundTrip("> &", CssSelectorNesting_Style) == "& > &");
    CHECK(roundTrip("div&", CssSelectorNesting_Style) == "div&");
}

TEST_CASE("an an+b has one written form") {
    CHECK(nth(0, 0) == "0");
    CHECK(nth(0, 3) == "3");
    CHECK(nth(0, -3) == "-3");
    CHECK(nth(1, 0) == "n");
    CHECK(nth(-1, 0) == "-n");
    CHECK(nth(2, 1) == "2n+1");
    CHECK(nth(2, -1) == "2n-1");
    CHECK(nth(-2, 3) == "-2n+3");
    CHECK(nth(-1, 3) == "-n+3");
}

TEST_CASE("an an+b is written the same whatever it was spelled") {
    CHECK(roundTrip(":nth-child(odd)") == ":nth-child(2n+1)");
    CHECK(roundTrip(":nth-child(even)") == ":nth-child(2n)");
    CHECK(roundTrip(":nth-child(+3)") == ":nth-child(3)");
    CHECK(roundTrip(":nth-child( 2n + 1 )") == ":nth-child(2n+1)");
}

TEST_CASE("a pseudo-element counts as a type selector, with exceptions its spec makes") {
    CHECK(specificity("a::before") == cssSpecificity(0, 0, 2));

    // css-shadow-1/#slotted-pseudo adds the argument's specificity.
    CHECK(specificity("::slotted(.a#b)") == cssSpecificity(1, 1, 1));

    // css-view-transitions-2/#pseudo-element-class-additions: `*` with no class counts nothing.
    CHECK(specificity("::view-transition-group(*)") == cssSpecificity(0, 0, 0));
    CHECK(specificity("::view-transition-group(*.a)") == cssSpecificity(0, 0, 1));
    CHECK(specificity("::view-transition-group(a)") == cssSpecificity(0, 0, 1));
}

TEST_CASE("a selector styles its element, a pseudo-element that the cascade computes, or nothing") {
    CHECK(pseudoOf("p") == CssPseudo_None);
    CHECK(pseudoOf("p::before") == CssPseudo_Before);
    CHECK(pseudoOf("li::after::marker") == CssPseudo_AfterMarker);
    CHECK(pseudoOf("p::first-line") == CssPseudo_FirstLine);
    CHECK(pseudoOf("p:first-letter") == CssPseudo_FirstLetter);
    CHECK(pseudoOf("p::first-letter:hover") == CssPseudo_Count);
    CHECK(pseudoOf("p::selection") == CssPseudo_Selection);
    CHECK(pseudoOf("p::target-text") == CssPseudo_Count);
}
