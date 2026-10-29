#include "css/src/css_parser.h"
#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// A source and the parser it parses into.
struct Source : CssTestSource {
    CssParser parser;

    explicit Source(PrStringView source) : CssTestSource(source) { cssParserInit(&parser); }

    ~Source() { cssParserReset(&parser); }

    // Returns the item at `index` of a block. Items live in the parser's arrays.
    const CssItem& item(CssBlock block, uint32_t index) const {
        return parser.items.data[block.items.start + index];
    }

    const CssSyntaxDeclaration& declaration(CssBlock block, uint32_t index) const {
        return parser.declarations.data[block.declarations.start + index];
    }

    const CssSyntaxRule& rule(uint32_t index) const { return parser.rules.data[index]; }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a declaration is a name, a value, and whether it is important") {
    Source s("color: red");
    CssSyntaxDeclaration declaration;
    REQUIRE(cssParseDeclaration(&s.ts, &declaration));
    CHECK(s.text(declaration.name) == "color");
    CHECK(s.text(declaration.value) == "red");
    CHECK_FALSE(declaration.important);
}

TEST_CASE("a declaration value is trimmed at both ends") {
    Source s("margin:   1px  2px   ");
    CssSyntaxDeclaration declaration;
    REQUIRE(cssParseDeclaration(&s.ts, &declaration));
    CHECK(s.text(declaration.value) == "1px  2px");
}

TEST_CASE("important is recognized however it is spelled and spaced") {
    for (PrStringView text : {"a: b !important", "a: b!IMPORTANT",
                              "a: b ! /**/ Important  "}) {
        INFO(textOf(text));
        Source s(text);
        CssSyntaxDeclaration declaration;
        REQUIRE(cssParseDeclaration(&s.ts, &declaration));
        CHECK(declaration.important);
        CHECK(s.text(declaration.value) == "b");
    }
}

TEST_CASE("a bang that is not important stays in the value") {
    Source s("a: b !x");
    CssSyntaxDeclaration declaration;
    REQUIRE(cssParseDeclaration(&s.ts, &declaration));
    CHECK_FALSE(declaration.important);
    CHECK(s.text(declaration.value) == "b !x");
}

TEST_CASE("a declaration needs a name and a colon") {
    for (PrStringView text : {"", ";", "color", "color red",
                              "#foo: red", "@foo: red"}) {
        INFO(textOf(text));
        Source s(text);
        CssSyntaxDeclaration declaration;
        CHECK_FALSE(cssParseDeclaration(&s.ts, &declaration));
    }
}

TEST_CASE("a block keeps what parsed and what did not, in order") {
    Source s("a:1; bad; b:2");
    CssBlock block = {};
    cssParseBlockContents(&s.ts, &s.parser, &block);

    REQUIRE(block.items.count == 3);
    CHECK(s.item(block, 0).type == CssItemType_Declaration);
    CHECK(s.item(block, 1).type == CssItemType_Invalid);
    CHECK(s.item(block, 2).type == CssItemType_Declaration);
    CHECK(s.text(s.item(block, 1).span) == "bad");

    REQUIRE(block.declarations.count == 2);
    CHECK(s.text(s.declaration(block, 0).name) == "a");
    CHECK(s.text(s.declaration(block, 1).name) == "b");
}

TEST_CASE("an at-rule inside a block keeps its place") {
    Source s("a:1; @media print { b:2 } c:3");
    CssBlock block = {};
    cssParseBlockContents(&s.ts, &s.parser, &block);

    REQUIRE(block.items.count == 3);
    CHECK(s.item(block, 0).type == CssItemType_Declaration);
    CHECK(s.item(block, 1).type == CssItemType_Rule);
    CHECK(s.item(block, 2).type == CssItemType_Declaration);

    const CssSyntaxRule& rule = s.rule(s.item(block, 1).index);
    CHECK(s.text(rule.name) == "@media");
    CHECK(s.text(rule.prelude) == " print ");
}

TEST_CASE("a qualified rule is a prelude and a block") {
    Source s("div > p { color: red }");
    CssSyntaxRule rule;
    REQUIRE(cssParseRule(&s.ts, &rule));
    CHECK(rule.type == CssSyntaxRuleType_Qualified);
    CHECK(s.text(rule.prelude) == "div > p ");
    CHECK((rule.flags & CssSyntaxRuleFlag_HasBlock) != 0);
    CHECK(s.text(rule.block) == " color: red ");
}

TEST_CASE("a prelude is kept as written") {
    // Only a declaration's value is trimmed. A selector needs its whitespace.
    Source s("  a   >   b  {}");
    CssSyntaxRule rule;
    REQUIRE(cssParseRule(&s.ts, &rule));
    CHECK(s.text(rule.prelude) == "a   >   b  ");
}

TEST_CASE("an at-rule may end at a semicolon instead of a block") {
    Source s("@import url(x);");
    CssSyntaxRule rule;
    REQUIRE(cssParseRule(&s.ts, &rule));
    CHECK(rule.type == CssSyntaxRuleType_At);
    CHECK(s.text(rule.name) == "@import");
    CHECK(s.text(rule.prelude) == " url(x)");
    CHECK((rule.flags & CssSyntaxRuleFlag_HasBlock) == 0);
}

TEST_CASE("a qualified rule with no block is not a rule") {
    Source s("div > p");
    CssSyntaxRule rule;
    CHECK_FALSE(cssParseRule(&s.ts, &rule));
}

TEST_CASE("a block left open ends at the input, and says so") {
    Source s("div { color: red");
    CssSyntaxRule rule;
    REQUIRE(cssParseRule(&s.ts, &rule));
    CHECK((rule.flags & CssSyntaxRuleFlag_EofTerminated) != 0);
    CHECK(s.text(rule.block) == " color: red");
}

TEST_CASE("a stylesheet ignores CDO and CDC between rules") {
    Source sheet("<!-- a{} --> b{}");
    CssBlock block = {};
    cssParseStylesheetContents(&sheet.ts, &sheet.parser, &block);
    REQUIRE(block.items.count == 2);
    CHECK(sheet.item(block, 0).type == CssItemType_Rule);
    CHECK(sheet.item(block, 1).type == CssItemType_Rule);
}

TEST_CASE("a stylesheet records what did not parse") {
    Source s("a{} bad b{}");
    CssBlock block = {};
    cssParseStylesheetContents(&s.ts, &s.parser, &block);
    REQUIRE(block.items.count == 2);
    CHECK(s.item(block, 0).type == CssItemType_Rule);
    CHECK(s.item(block, 1).type == CssItemType_Rule);   // "bad b{}" is one qualified rule
    CHECK(s.text(s.rule(1).prelude) == "bad b");
}

TEST_CASE("a block tells a nested style rule from a declaration") {
    Source s("color: red; & > b { color: blue } font: bold");
    CssBlock block = {};
    cssParseBlockContents(&s.ts, &s.parser, &block);

    REQUIRE(block.items.count == 3);
    CHECK(s.item(block, 0).type == CssItemType_Declaration);
    CHECK(s.item(block, 1).type == CssItemType_Rule);
    CHECK(s.item(block, 2).type == CssItemType_Declaration);

    const CssSyntaxRule& rule = s.rule(s.item(block, 1).index);
    CHECK(rule.type == CssSyntaxRuleType_Qualified);
    CHECK(s.text(rule.prelude) == "& > b ");
    CHECK(s.text(rule.block) == " color: blue ");
}

TEST_CASE("a nested rule whose prelude looks like a property is still a rule") {
    // `font` is an ident and a colon, but a {}-block cannot be a property's whole value here.
    Source s("font:bar { color: red } a: 1");
    CssBlock block = {};
    cssParseBlockContents(&s.ts, &s.parser, &block);

    REQUIRE(block.items.count == 2);
    CHECK(s.item(block, 0).type == CssItemType_Rule);
    CHECK(s.text(s.rule(0).prelude) == "font:bar ");
    CHECK(s.item(block, 1).type == CssItemType_Declaration);
}

TEST_CASE("a custom property keeps a block in its value, and does not become a rule") {
    Source s("--x: a { b } c; d: 1");
    CssBlock block = {};
    cssParseBlockContents(&s.ts, &s.parser, &block);

    REQUIRE(block.items.count == 2);
    CHECK(s.item(block, 0).type == CssItemType_Declaration);
    CHECK(s.text(s.declaration(block, 0).value) == "a { b } c");
    CHECK(s.item(block, 1).type == CssItemType_Declaration);
}

TEST_CASE("a custom property wins over a rule that would spell the same thing") {
    Source s("--x:hover { color: red }; a: 1");
    CssBlock block = {};
    cssParseBlockContents(&s.ts, &s.parser, &block);

    REQUIRE(block.items.count == 2);
    CHECK(s.item(block, 0).type == CssItemType_Declaration);
    CHECK(s.text(s.declaration(block, 0).name) == "--x");
    CHECK(s.text(s.declaration(block, 0).value) == "hover { color: red }");
    CHECK(s.item(block, 1).type == CssItemType_Declaration);
}

TEST_CASE("a stylesheet discards a rule whose prelude looks like a custom property") {
    Source s("--x:hover { color: red } a{}");
    CssBlock block = {};
    cssParseStylesheetContents(&s.ts, &s.parser, &block);

    REQUIRE(block.items.count == 2);
    CHECK(s.item(block, 0).type == CssItemType_Invalid);
    CHECK(s.item(block, 1).type == CssItemType_Rule);
    CHECK(s.text(s.rule(0).prelude) == "a");
}

TEST_CASE("a nested at-rule ends at the enclosing block's brace") {
    Source s("a { @media print }");
    CssSyntaxRule rule;
    REQUIRE(cssParseRule(&s.ts, &rule));

    CssTokenStream inner = cssSubStream(&s.ts, rule.block);
    CssBlock block = {};
    cssParseBlockContents(&inner, &s.parser, &block);
    REQUIRE(block.rules.count == 1);
    CHECK(s.text(s.rule(0).name) == "@media");
    CHECK((s.rule(0).flags & CssSyntaxRuleFlag_HasBlock) == 0);
}

TEST_CASE("a declaration ending at a brace does not run past it") {
    Source s("a { color: red }");
    CssSyntaxRule rule;
    REQUIRE(cssParseRule(&s.ts, &rule));
    CHECK(s.text(rule.block) == " color: red ");
}
