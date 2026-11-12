#include "css/src/css_serialize.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

String number(double value) {
    PrTextBuffer text = {};
    cssSerializeNumber(&text, value);
    return take(&text);
}

String ident(PrStringView value) {
    PrTextBuffer text = {};
    cssSerializeIdent(&text, value);
    return take(&text);
}

String string(PrStringView value) {
    PrTextBuffer text = {};
    cssSerializeString(&text, value);
    return take(&text);
}

// Tokenizes `source` and serializes every token of it.
String tokens(PrStringView source) {
    CssTestSource s(source);
    PrTextBuffer text = {};
    cssSerializeTokens(&text, &s.ts, cssStreamAll(&s.ts));
    return take(&text);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a number is the shortest decimal that reads back as itself") {
    CHECK(number(0.0) == "0");
    CHECK(number(-0.0) == "0");
    CHECK(number(1.0) == "1");
    CHECK(number(-1.5) == "-1.5");
    CHECK(number(0.1) == "0.1");
    CHECK(number(1.0 / 3.0) == "0.3333333333333333");
    CHECK(number(1000000.0) == "1000000");
    CHECK(number(123456789012345.0) == "123456789012345");
}

TEST_CASE("positional notation runs from 1e-6 to 1e21") {
    CHECK(number(1e-6) == "0.000001");
    CHECK(number(1e-7) == "1e-7");
    CHECK(number(1e20) == "100000000000000000000");
    CHECK(number(1e21) == "1e+21");
}

TEST_CASE("a number outside the range is clamped to it") {
    double huge = 1e308 * 10.0;
    CHECK(number(huge) == "1.7976931348623157e+308");
    CHECK(number(-huge) == "-1.7976931348623157e+308");
    CHECK(number(huge - huge) == "0");
}

TEST_CASE("an identifier escapes what would not tokenize back") {
    CHECK(ident("color") == "color");
    CHECK(ident("-webkit-box") == "-webkit-box");
    CHECK(ident("a b") == "a\\ b");
    CHECK(ident("2px") == "\\32 px");
    CHECK(ident("-2px") == "-\\32 px");
    CHECK(ident("-") == "\\-");
    CHECK(ident("--x") == "--x");

    // Only a `-` with nothing after it is escaped, and only a digit in second place after one is escaped
    // (cssom-1/#common-serializing-idioms).
    CHECK(ident("-x") == "-x");
    CHECK(ident("--") == "--");
    CHECK(ident("---") == "---");
    CHECK(ident("--0") == "--0");
    CHECK(ident("\x01") == "\\1 ");
    CHECK(ident("\xe2\x98\x83") == "\xe2\x98\x83");
}

TEST_CASE("a string is quoted with a double quote") {
    CHECK(string("a") == "\"a\"");
    CHECK(string("a\"b") == "\"a\\\"b\"");
    CHECK(string("a\\b") == "\"a\\\\b\"");
    CHECK(string("a'b") == "\"a'b\"");
    CHECK(string("\n") == "\"\\a \"");
}

TEST_CASE("a token serializes the way the spec defines") {
    CHECK(tokens("color") == "color");
    CHECK(tokens("@media") == "@media");
    CHECK(tokens("#fff") == "#fff");
    CHECK(tokens("#0") == "#0");
    CHECK(tokens("rgb(1,2,3)") == "rgb(1,2,3)");
    CHECK(tokens("url( a )") == "url(a)");
    CHECK(tokens("url('a')") == "url(\"a\")");
    CHECK(tokens("<!-- -->") == "<!-- -->");
    CHECK(tokens("+1.50e2px") == "150px");
    CHECK(tokens("50%") == "50%");
    CHECK(tokens("1FOO") == "1FOO");
}

TEST_CASE("a unit cannot open with what the number would take as an exponent") {
    // The tokenizer reads an unescaped exponent as part of the number, so only an escaped one reaches the unit.
    CHECK(tokens("1e5x") == "100000x");
    CHECK(tokens("1\\65 5") == "1\\65 5");
    CHECK(tokens("1em") == "1em");
    CHECK(tokens("1e-x") == "1e-x");
}

TEST_CASE("a url token is written as one, not as a string in url()") {
    CHECK(tokens("url(foo)") == "url(foo)");
    CHECK(tokens("url(a\\'b)") == "url(a\\'b)");
    CHECK(tokens("url(a\\ b)") == "url(a\\20 b)");
}

TEST_CASE("a comment separates tokens that would read back as one") {
    CHECK(tokens("a  b") == "a b");
    CHECK(tokens("--> a") == "--> a");
    CHECK(tokens("/*c*/") == "");
    CHECK(tokens("a/**/b") == "a/**/b");
    CHECK(tokens("1/**/2") == "1/**/2");
    CHECK(tokens("-/**/1") == "-/**/1");
    CHECK(tokens("a/**/-") == "a/**/-");
    CHECK(tokens("a/**/(") == "a/**/(");
    CHECK(tokens("#/**/a") == "#/**/a");
    CHECK(tokens("@a/**/-->") == "@a/**/-->");
    CHECK(tokens("1/**/a") == "1/**/a");
    CHECK(tokens("1/**/%") == "1/**/%");
    CHECK(tokens("*/") == "*/");
}

TEST_CASE("an escape reads back as the character it encodes") {
    CHECK(tokens("\\31 23") == "\\31 23");
    CHECK(tokens("c\\6flor") == "color");
    CHECK(tokens("\"a\\62 c\"") == "\"abc\"");
}

TEST_CASE("a run of verbatim tokens serializes as the source it covers") {
    CHECK(tokens("margin:0 auto") == "margin:0 auto");
    CHECK(tokens("translate(1px,2.5em)") == "translate(1px,2.5em)");
    CHECK(tokens("a > b[c=\"d\"]") == "a > b[c=\"d\"]");

    // One token that the source does not spell makes the whole run serialize token by token.
    CHECK(tokens("margin:1.50px  auto") == "margin:1.5px auto");
    CHECK(tokens("a 'b' c") == "a \"b\" c");
    CHECK(tokens("\\61 b") == "ab");
}
