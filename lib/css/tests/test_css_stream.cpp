#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// An+B (css-syntax-3/#anb-microsyntax)

// Parses the whole input as an <an+b>, the way a selector would.
bool parseAnB(PrStringView text, int32_t* a, int32_t* b) {
    CssTestSource s(text);
    cssSkipWhitespace(&s.ts);
    if (!cssConsumeAnPlusB(&s.ts, a, b))
        return false;
    cssSkipWhitespace(&s.ts);
    return cssPeek(&s.ts) == CssTokenKind_Eof;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("peek and next walk the stream") {
    CssTestSource s("a b");
    CHECK(cssPeek(&s.ts) == CssTokenKind_Ident);
    CHECK(s.kind(cssNext(&s.ts)) == CssTokenKind_Ident);
    CHECK(s.kind(cssNext(&s.ts)) == CssTokenKind_Whitespace);
    CHECK(s.kind(cssNext(&s.ts)) == CssTokenKind_Ident);
    CHECK(s.kind(cssNext(&s.ts)) == CssTokenKind_Eof);

    // Past the end it stays at Eof rather than running off.
    CHECK(s.kind(cssNext(&s.ts)) == CssTokenKind_Eof);
    CHECK(cssPeek(&s.ts) == CssTokenKind_Eof);
}

TEST_CASE("a mark restores the position") {
    CssTestSource s("a b c");
    uint32_t mark = cssMark(&s.ts);
    cssNext(&s.ts);
    cssNext(&s.ts);
    CHECK(cssMark(&s.ts) != mark);
    cssReset(&s.ts, mark);
    CHECK(cssPeek(&s.ts) == CssTokenKind_Ident);
}

TEST_CASE("skipping whitespace stops at the first token that is not") {
    CssTestSource s("   \n a");
    cssSkipWhitespace(&s.ts);
    CHECK(cssPeek(&s.ts) == CssTokenKind_Ident);
    cssSkipWhitespace(&s.ts);
    CHECK(cssPeek(&s.ts) == CssTokenKind_Ident);   // idempotent
}

TEST_CASE("keywords match case insensitively, through escapes") {
    CssTestSource s("Auto AUTO \\61 uto autos notakeyword");
    CHECK(cssTokenKeyword(&s.tokens, 0) == PrCssKeyword_Auto);
    CHECK(cssTokenKeyword(&s.tokens, 2) == PrCssKeyword_Auto);
    CHECK(cssTokenKeyword(&s.tokens, 4) == PrCssKeyword_Auto);   // \61 uto decodes to "auto"
    CHECK(cssTokenKeyword(&s.tokens, 6) != PrCssKeyword_Auto);
    CHECK(cssTokenKeyword(&s.tokens, 8) == PrCssKeyword_Unknown);
}

TEST_CASE("a token longer than any keyword matches none") {
    CssTestSource s("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\61 ");
    CHECK(cssTokenKeyword(&s.tokens, 0) == PrCssKeyword_Unknown);
}

TEST_CASE("a component value is one token") {
    CssTestSource s("red 10px");
    CssTokenSpan span;
    REQUIRE(cssConsumeComponentValue(&s.ts, &span));
    CHECK(s.text(span) == "red");
    REQUIRE(cssConsumeComponentValue(&s.ts, &span));
    CHECK(s.text(span) == " ");
    REQUIRE(cssConsumeComponentValue(&s.ts, &span));
    CHECK(s.text(span) == "10px");
    CHECK_FALSE(cssConsumeComponentValue(&s.ts, &span));
}

TEST_CASE("a component value is a whole block") {
    CssTestSource s("[a (b {c}) d] after");
    CssTokenSpan span;
    REQUIRE(cssConsumeComponentValue(&s.ts, &span));
    CHECK(s.text(span) == "[a (b {c}) d]");
    CHECK(cssPeek(&s.ts) == CssTokenKind_Whitespace);
}

TEST_CASE("a function is a block, parentheses and all") {
    CssTestSource s("rgb(1, calc(2 + 3)) x");
    CssTokenSpan span;
    REQUIRE(cssConsumeComponentValue(&s.ts, &span));
    CHECK(s.text(span) == "rgb(1, calc(2 + 3))");
}

TEST_CASE("a block left open ends at the input") {
    CssTestSource s("(a (b");
    CssTokenSpan span;
    REQUIRE(cssConsumeComponentValue(&s.ts, &span));
    CHECK(s.text(span) == "(a (b");
    CHECK(cssPeek(&s.ts) == CssTokenKind_Eof);
}

TEST_CASE("a declaration value stops at the top level semicolon") {
    CssTestSource s("1px solid red; next");
    CssTokenSpan span;
    REQUIRE(cssConsumeDeclarationValue(&s.ts, &span));
    CHECK(s.text(span) == "1px solid red");
    CHECK(cssPeek(&s.ts) == CssTokenKind_Semicolon);
}

TEST_CASE("a declaration value stops at !") {
    CssTestSource s("red !important");
    CssTokenSpan span;
    REQUIRE(cssConsumeDeclarationValue(&s.ts, &span));
    CHECK(s.text(span) == "red ");
    CHECK(cssPeek(&s.ts) == CssTokenKind_Delim);
}

TEST_CASE("a semicolon inside a block belongs to the block") {
    CssTestSource s("foo(a; b) ; after");
    CssTokenSpan span;
    REQUIRE(cssConsumeDeclarationValue(&s.ts, &span));
    CHECK(s.text(span) == "foo(a; b) ");
}

TEST_CASE("a closing bracket this run did not open ends it") {
    CssTestSource s("a b) c");
    CssTokenSpan span;
    REQUIRE(cssConsumeDeclarationValue(&s.ts, &span));
    CHECK(s.text(span) == "a b");
    CHECK(cssPeek(&s.ts) == CssTokenKind_CloseParen);
}

TEST_CASE("an any value keeps the semicolon") {
    CssTestSource s("a; b) c");
    CssTokenSpan span;
    REQUIRE(cssConsumeAnyValue(&s.ts, &span));
    CHECK(s.text(span) == "a; b");   // still stops at the unmatched ')'
}

TEST_CASE("an empty run is not a value") {
    CssTestSource s("; x");
    CssTokenSpan span;
    CHECK_FALSE(cssConsumeDeclarationValue(&s.ts, &span));
    CHECK(span.count == 0);
}

TEST_CASE("the an+b keywords") {
    int32_t a = 0, b = 0;
    REQUIRE(parseAnB("odd", &a, &b));
    CHECK(a == 2);
    CHECK(b == 1);
    REQUIRE(parseAnB(" /**/\t EveN /**/\n", &a, &b));
    CHECK(a == 2);
    CHECK(b == 0);
    CHECK_FALSE(parseAnB("\xc3\xb6" "dd", &a, &b));   // ödd is not odd
}

TEST_CASE("an+b reads how a token was written, not what it means") {
    int32_t a = 0, b = 0;
    REQUIRE(parseAnB("3n", &a, &b));
    CHECK(a == 3);
    CHECK(b == 0);

    REQUIRE(parseAnB("+2n+1", &a, &b));
    CHECK(a == 2);
    CHECK(b == 1);

    // "3n-2" is one dimension token whose unit is "n-2".
    REQUIRE(parseAnB("3n-2", &a, &b));
    CHECK(a == 3);
    CHECK(b == -2);

    // "3n- 1" is a dimension whose unit is "n-", and the digits follow.
    REQUIRE(parseAnB("3n- 1", &a, &b));
    CHECK(a == 3);
    CHECK(b == -1);

    REQUIRE(parseAnB("-n+3", &a, &b));
    CHECK(a == -1);
    CHECK(b == 3);

    REQUIRE(parseAnB("4", &a, &b));
    CHECK(a == 0);
    CHECK(b == 4);
}

TEST_CASE("an+b rejects what is only nearly one") {
    int32_t a = 0, b = 0;
    CHECK_FALSE(parseAnB("", &a, &b));
    CHECK_FALSE(parseAnB("n-", &a, &b));       // the digits are not optional
    CHECK_FALSE(parseAnB("3.5n", &a, &b));     // an+b is integers
    CHECK_FALSE(parseAnB("+ 2n", &a, &b));     // the sign must be against what follows
    CHECK_FALSE(parseAnB("2n + +1", &a, &b));  // the second sign is not signless
}
