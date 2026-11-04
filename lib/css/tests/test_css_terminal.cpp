#include "css/src/css_at_rule.h"
#include "css/src/css_grammar.h"
#include "css/src/css_parse.h"
#include "css/src/css_property.h"
#include "css/src/css_terminal.h"
#include "css/src/css_value_def.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns the root of a prose production, or CssTerminal_None if the production has a grammar or has neither.
CssTerminal terminalOf(CssValueDef def) {
    CssGrammarIndex root = kCssValueDefGrammarRoot[def];
    if (root == CssGrammarRoot_None || cssGrammarType(kCssGrammarNodes[root]) != CssGrammarType_Terminal)
        return CssTerminal_None;
    return CssTerminal(cssGrammarValue(kCssGrammarNodes[root]));
}

// Walks every type that a property value can reach, to confirm the partition in css_terminal.h.
void markReachable(CssGrammarIndex root, bool* visitedDef, bool* reached) {
    for (CssGrammarIndex i = root; i < cssGrammarAfter(root); ++i) {
        CssGrammarNode node = kCssGrammarNodes[i];
        if (cssGrammarType(node) == CssGrammarType_Terminal)
            reached[cssGrammarValue(node)] = true;
        if (cssGrammarType(node) != CssGrammarType_Type || visitedDef[cssGrammarValue(node)])
            continue;
        visitedDef[cssGrammarValue(node)] = true;
        CssGrammarIndex target = kCssValueDefGrammarRoot[cssGrammarValue(node)];
        if (target != CssGrammarRoot_None)
            markReachable(target, visitedDef, reached);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("every terminal is a prose production") {
    for (CssValueDef def = 0; def < CssValueDef_Count; ++def) {
        if (terminalOf(def) == CssTerminal_None)
            continue;
        INFO(textOf(kCssValueDefName[def]));
        CHECK(cssGrammarNext(kCssGrammarNodes[kCssValueDefGrammarRoot[def]]) == 1);
    }
}

TEST_CASE("every terminal has exactly one value def") {
    uint32_t seen[CssTerminal_Count] = {};
    for (CssValueDef def = 0; def < CssValueDef_Count; ++def) {
        CssTerminal terminal = terminalOf(def);
        if (terminal != CssTerminal_None)
            ++seen[terminal];
    }
    for (CssTerminal i = 0; i < CssTerminal_Count; ++i) {
        CAPTURE(i);
        CHECK(seen[i] == 1);
    }
}

TEST_CASE("a definition with more than one node is not a terminal") {
    for (CssValueDef def = 0; def < CssValueDef_Count; ++def) {
        CssGrammarIndex root = kCssValueDefGrammarRoot[def];
        if (root == CssGrammarRoot_None || cssGrammarNext(kCssGrammarNodes[root]) == 1)
            continue;
        INFO(textOf(kCssValueDefName[def]));
        CHECK(terminalOf(def) == CssTerminal_None);
    }
}

TEST_CASE("a terminal is one node") {
    for (CssGrammarIndex i = 1; i < kCssGrammarNodeCount; ++i) {
        CssGrammarNode node = kCssGrammarNodes[i];
        if (cssGrammarType(node) != CssGrammarType_Terminal)
            continue;
        CAPTURE(i);
        CHECK(cssGrammarValue(node) < CssTerminal_Count);
        CHECK(cssGrammarNext(node) == 1);
    }
}

TEST_CASE("a terminal root is taken once") {
    // A reference to a prose production is its terminal and carries the reference's multiplier. The production's own
    // root has none.
    for (CssValueDef def = 0; def < CssValueDef_Count; ++def) {
        if (terminalOf(def) == CssTerminal_None)
            continue;
        CssGrammarNode node = kCssGrammarNodes[kCssValueDefGrammarRoot[def]];
        INFO(textOf(kCssValueDefName[def]));
        CHECK(cssGrammarModifier(node)->min == 1);
        CHECK(cssGrammarModifier(node)->max == 1);
    }
}

TEST_CASE("every type reference resolves") {
    // Giving terminals a root ensures that a <type> always leads somewhere.
    for (CssGrammarIndex i = 1; i < kCssGrammarNodeCount; ++i) {
        CssGrammarNode node = kCssGrammarNodes[i];
        if (cssGrammarType(node) != CssGrammarType_Type)
            continue;
        INFO(textOf(kCssValueDefName[cssGrammarValue(node)]));
        CHECK(kCssValueDefGrammarRoot[cssGrammarValue(node)] != CssGrammarRoot_None);
    }
}

TEST_CASE("the heavy terminals are present") {
    // The dozen productions that carry most of the references.
    for (PrStringView name :
            {
                "<number>", "<percentage>", "<length>", "<string>",
                "<integer>", "<custom-ident>", "<angle>", "<dashed-ident>",
                "<ident>", "<time>", "<zero>", "<flex>",
            }) {
        INFO(textOf(name));
        bool found = false;
        for (CssValueDef def = 0; def < CssValueDef_Count; ++def) {
            if (kCssValueDefName[def] == name)
                found = terminalOf(def) != CssTerminal_None;
        }
        CHECK(found);
    }
}

TEST_CASE("terminal names match the value def they came from") {
    for (CssValueDef def = 0; def < CssValueDef_Count; ++def) {
        CssTerminal terminal = terminalOf(def);
        if (terminal == CssTerminal_None)
            continue;
        INFO(textOf(kCssValueDefName[def]));
        REQUIRE(kCssTerminalName[terminal].data != nullptr);
        CHECK(kCssTerminalName[terminal] == kCssValueDefName[def]);
    }
}

TEST_CASE("a terminal begins with a token its first set holds") {
    // The generator's sample of what each prose production starts with, checked against the parsers.
    const char* samples[] = {
        "0", "3", "1.5", "-2", "10px", "1deg", "2s", "3hz", "2dppx", "1fr",
        "3db", "2st", "5%", "auto", "red", "canvas", "--x", "\"s\"", "#abc",
        "#abcd12", "url(x)", "calc(1px)", "@media", "U+26", ",", "/", "(",
        "[", ":",
    };
    for (CssValueDef def = 0; def < CssValueDef_Count; ++def) {
        CssTerminal terminal = terminalOf(def);
        if (terminal == CssTerminal_None)
            continue;
        const CssGrammarFirst& first = kCssGrammarFirsts[kCssGrammarFirstOf[kCssValueDefGrammarRoot[def]]];
        for (const char* sample : samples) {
            CssTestSource source(sample, CssTokenizeFlag_UnicodeRanges);
            CssValuePool pool = {};
            CssMatchState match = {};
            CssValueSpan span;
            CssTokenKind kind = cssPeek(&source.ts);
            if (cssParseValue(kCssValueDefGrammarRoot[def], &source.ts, &pool, &match, &span)) {
                INFO(textOf(kCssValueDefName[def]), " parsed ", std::string(sample));
                CHECK((first.kinds & (1u << kind)) != 0);
            }
            cssValuePoolReset(&pool);
            cssMatchStateReset(&match);
        }
    }
}

TEST_CASE("only value terminals are reachable from a declaration") {
    static bool visitedDef[CssValueDef_Count];
    static bool reached[CssTerminal_Count];
    for (PrCssProperty i = 0; i < PrCssProperty_Count; ++i) {
        if (kCssPropertyGrammarRoot[i] != CssGrammarRoot_None)
            markReachable(kCssPropertyGrammarRoot[i], visitedDef, reached);
    }
    for (PrCssDescriptor i = 0; i < PrCssDescriptor_Count; ++i) {
        if (kCssDescriptorGrammarRoot[i] != CssGrammarRoot_None)
            markReachable(kCssDescriptorGrammarRoot[i], visitedDef, reached);
    }
    for (CssTerminal i = 0; i < CssTerminal_Count; ++i) {
        INFO(textOf(kCssTerminalName[i]));
        CHECK(reached[i] == (i < CssTerminal_StructuralFirst));
    }
}
