#include "css/src/css_grammar.h"
#include "css/src/css_keyword.h"
#include "css/src/css_property.h"
#include "css/src/css_rule.h"
#include "css/src/css_terminal.h"
#include "css/src/css_value_def.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

#include <ctype.h>

namespace pr_impl {

namespace {

uint32_t childCount(CssGrammarIndex parent) {
    CssGrammarIndex end = cssGrammarAfter(parent);
    uint32_t count = 0;
    for (CssGrammarIndex i = CssGrammarIndex(parent + 1); i < end; i = cssGrammarAfter(i))
        ++count;
    return count;
}

CssGrammarIndex childAt(CssGrammarIndex parent, uint32_t index) {
    CssGrammarIndex end = cssGrammarAfter(parent);
    CssGrammarIndex i = CssGrammarIndex(parent + 1);
    for (; index && i < end; --index)
        i = cssGrammarAfter(i);
    REQUIRE(i < end);
    return i;
}

CssGrammarIndex propertyRoot(PrStringView name) {
    PrCssProperty property = cssPropertyFromText(name);
    REQUIRE(property != PrCssProperty_Count);
    return kCssPropertyGrammarRoot[property];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("every node is in bounds") {
    for (CssGrammarIndex i = 1; i < kCssGrammarNodeCount; ++i) {
        CssGrammarNode node = kCssGrammarNodes[i];
        CAPTURE(i);
        CHECK(cssGrammarModifier(node)->min <= cssGrammarModifier(node)->max);
        CHECK(cssGrammarModifier(node)->range < kCssGrammarRangeCount);

        // next is the span of this subtree, so it always moves forward and stays inside.
        CHECK(cssGrammarNext(node) >= 1);
        CHECK(i + cssGrammarNext(node) <= kCssGrammarNodeCount);

        switch (cssGrammarType(node)) {
            case CssGrammarType_Keyword:
            case CssGrammarType_AtKeyword:
            case CssGrammarType_Function:
                CHECK(cssGrammarValue(node) < PrCssKeyword_Count);
                break;
            case CssGrammarType_Type:
                CHECK(cssGrammarValue(node) < CssValueDef_Count);
                break;
            case CssGrammarType_Property:
                CHECK(cssGrammarValue(node) < PrCssProperty_Count);
                break;
            case CssGrammarType_Literal:
                CHECK(cssGrammarValue(node) > 0);
                CHECK(cssGrammarValue(node) < 128);
                break;
            default:
                break;
        }
    }
}

TEST_CASE("every node has the children its type implies") {
    for (CssGrammarIndex i = 1; i < kCssGrammarNodeCount; ++i) {
        CssGrammarNode node = kCssGrammarNodes[i];
        CAPTURE(i);
        switch (cssGrammarType(node)) {
            case CssGrammarType_OneOf:
            case CssGrammarType_KeywordSet:
            case CssGrammarType_AnyOf:
            case CssGrammarType_AllOf:
                CHECK(childCount(i) >= 2);
                break;
            case CssGrammarType_Sequence:
                // One child is a group whose multiplier applies over the child's own.
                CHECK((childCount(i) >= 2 || cssGrammarModifier(node) != &kCssGrammarModifiers[0]));
                break;
            case CssGrammarType_Function:
            case CssGrammarType_Longhand:
                CHECK(childCount(i) == 1);
                break;
            case CssGrammarType_Type:
                CHECK(childCount(i) <= 1);   // one child when the type is parameterized
                break;
            default:
                CHECK(childCount(i) == 0);
                break;
        }
    }
}

TEST_CASE("a keyword set holds bare keywords, no two the same") {
    // The matcher reads the token's keyword once and compares it against the children in a flat loop. That gives the
    // same result as trying the alternatives in order, but only while each alternative is a bare keyword and no two are
    // the same.
    for (CssGrammarIndex i = 1; i < kCssGrammarNodeCount; ++i) {
        if (cssGrammarType(kCssGrammarNodes[i]) != CssGrammarType_KeywordSet)
            continue;

        CAPTURE(i);
        uint32_t count = childCount(i);
        for (uint32_t slot = 0; slot < count; ++slot) {
            CssGrammarIndex child = childAt(i, slot);
            CssGrammarNode node = kCssGrammarNodes[child];
            REQUIRE(cssGrammarType(node) == CssGrammarType_Keyword);
            CHECK(childCount(child) == 0);

            const CssGrammarModifier* modifier = cssGrammarModifier(node);
            CHECK(modifier->min == 1);
            CHECK(modifier->max == 1);
            CHECK(modifier->flags == 0);
            CHECK(modifier->range == CssGrammarRange_None);

            for (uint32_t other = 0; other < slot; ++other)
                CHECK(cssGrammarValue(kCssGrammarNodes[childAt(i, other)]) != cssGrammarValue(node));
        }
    }
}

TEST_CASE("node 0 is the empty grammar") {
    CHECK(cssGrammarNext(kCssGrammarNodes[0]) == 1);
    CHECK(childCount(0) == 0);
    CHECK(CssGrammarRoot_None == 0);
}

TEST_CASE("only aliases lack a root") {
    // gen_css.py refuses to emit a definition that has a grammar but no root. What is left to check here is which
    // definitions may have none.
    for (PrCssProperty i = PrCssProperty_AliasFirst; i < PrCssProperty_Count; ++i) {
        INFO(textOf(cssPropertyName(i)));
        CHECK(kCssPropertyGrammarRoot[i] == CssGrammarRoot_None);
    }
}

TEST_CASE("a subtree ends inside the array") {
    for (PrCssProperty i = 0; i < PrCssProperty_Count; ++i) {
        INFO(textOf(cssPropertyName(i)));
        CssGrammarIndex root = kCssPropertyGrammarRoot[i];
        CHECK(cssGrammarNext(kCssGrammarNodes[root]) >= 1);
        CHECK(root + cssGrammarNext(kCssGrammarNodes[root]) <= kCssGrammarNodeCount);
    }
}

TEST_CASE("prose terminals resolve to a terminal node") {
    // The productions that need hand-written parsers.
    for (CssValueDef def :
            {
                CssValueDef_Length, CssValueDef_String, CssValueDef_Integer, CssValueDef_Number,
                CssValueDef_CustomIdent, CssValueDef_HexColor, CssValueDef_DeclarationValue,
            }) {
        INFO(textOf(kCssValueDefName[def]));
        CHECK(cssGrammarType(kCssGrammarNodes[kCssValueDefGrammarRoot[def]]) == CssGrammarType_Terminal);
    }
    // <color> is not one of them: it resolves through css-color-5 down to <hex-color>.
    CHECK(kCssValueDefGrammarRoot[CssValueDef_Color] != CssGrammarRoot_None);
}

TEST_CASE("background-color is <color>") {
    CssGrammarNode node = kCssGrammarNodes[propertyRoot("background-color")];
    CHECK(cssGrammarType(node) == CssGrammarType_Type);
    CHECK(cssGrammarValue(node) == CssValueDef_Color);
    CHECK(cssGrammarModifier(node)->min == 1);
    CHECK(cssGrammarModifier(node)->max == 1);
}

TEST_CASE("margin is <'margin-top'>{1,4}") {
    CssGrammarNode node = kCssGrammarNodes[propertyRoot("margin")];
    CHECK(cssGrammarType(node) == CssGrammarType_Property);
    CHECK(cssGrammarValue(node) == PrCssProperty_MarginTop);
    CHECK(cssGrammarModifier(node)->min == 1);
    CHECK(cssGrammarModifier(node)->max == 4);
}

TEST_CASE("background is <bg-layer>#? , <final-bg-layer>, its components marked") {
    CssGrammarIndex root = propertyRoot("background");
    REQUIRE(cssGrammarType(kCssGrammarNodes[root]) == CssGrammarType_Sequence);
    REQUIRE(childCount(root) == 3);

    // <bg-layer> is inlined, because its components set longhands.
    CssGrammarIndex layers = childAt(root, 0);
    CssGrammarNode node = kCssGrammarNodes[layers];
    CHECK(cssGrammarType(node) == CssGrammarType_AnyOf);
    CHECK((cssGrammarModifier(node)->flags & CssGrammarFlag_Comma) != 0);
    CHECK(cssGrammarModifier(node)->min == 0);                       // the ? in #?
    CHECK(cssGrammarModifier(node)->max == kCssGrammarUnbounded);

    CssGrammarNode image = kCssGrammarNodes[childAt(layers, 0)];
    CHECK(cssGrammarType(image) == CssGrammarType_Longhand);
    CHECK(cssGrammarValue(image) == PrCssProperty_BackgroundImage);

    CHECK(cssGrammarType(kCssGrammarNodes[childAt(root, 1)]) == CssGrammarType_Literal);
    CHECK(cssGrammarValue(kCssGrammarNodes[childAt(root, 1)]) == ',');
    CHECK(cssGrammarType(kCssGrammarNodes[childAt(root, 2)]) == CssGrammarType_AnyOf);
}

TEST_CASE("<line-width> is a one-of over a bounded length and keywords") {
    CssGrammarIndex root = kCssValueDefGrammarRoot[CssValueDef_LineWidth];
    REQUIRE(cssGrammarType(kCssGrammarNodes[root]) == CssGrammarType_OneOf);
    REQUIRE(childCount(root) == 5);

    // A reference to a prose production is its terminal.
    CssGrammarNode length = kCssGrammarNodes[childAt(root, 0)];
    CHECK(cssGrammarType(length) == CssGrammarType_Terminal);
    CHECK(cssGrammarValue(length) == CssTerminal_Length);
    REQUIRE(cssGrammarModifier(length)->range != CssGrammarRange_None);

    const CssGrammarRange& range = kCssGrammarRanges[cssGrammarModifier(length)->range];
    CHECK(range.min == 0);
    CHECK((range.flags & CssGrammarRangeFlag_MaxUnbounded) != 0);
    CHECK((range.flags & CssGrammarRangeFlag_MinUnbounded) == 0);

    CssGrammarNode thick = kCssGrammarNodes[childAt(root, 4)];
    CHECK(cssGrammarType(thick) == CssGrammarType_Keyword);
    CHECK(cssKeywordName(PrCssKeyword(cssGrammarValue(thick))) == "thick");
}

TEST_CASE("range 0 is unbounded") {
    CHECK(kCssGrammarRanges[CssGrammarRange_None].flags ==
        (CssGrammarRangeFlag_MinUnbounded | CssGrammarRangeFlag_MaxUnbounded));
}

TEST_CASE("keyword names are unique and sorted, the private keywords after the others") {
    for (PrCssKeyword i = 0; i < PrCssKeyword_Count; ++i) {
        CAPTURE(i);
        REQUIRE(cssKeywordName(i).data != nullptr);
        CHECK(cssKeywordName(i).size != 0);
        CHECK(prStringStartsWith(cssKeywordName(i), "-pr-") == (i >= kCssKeywordFirstPrivate));
        if (i != 0 && i != kCssKeywordFirstPrivate)
            CHECK(prStringCompare(cssKeywordName(i - 1), cssKeywordName(i)) < 0);
    }
}

TEST_CASE("only the engine's own text names its private keywords") {
    CssDeclarationBlock block{};
    cssDeclarationBlockParse(&block, "text-transform: -pr-math-bold", nullptr, CssOrigin_Author);
    CHECK(block.count == 0);
    cssDeclarationBlockParse(&block, "text-transform: -pr-math-bold", nullptr, CssOrigin_Hint);
    CHECK(block.count == 1);
    cssDeclarationBlockReset(&block);
}

TEST_CASE("keywords are lowercase, since idents are case-insensitive") {
    for (PrCssKeyword i = 0; i < PrCssKeyword_Count; ++i) {
        PrStringView name = cssKeywordName(i);
        for (size_t at = 0; at < name.size; ++at) {
            INFO(textOf(name));
            CHECK_FALSE(isupper(uint8_t(name.data[at])));
        }
    }
}

TEST_CASE("aliases resolve to a property that has a grammar") {
    for (PrCssProperty i = 0; i < PrCssProperty_Count; ++i) {
        INFO(textOf(cssPropertyName(i)));
        if (i < PrCssProperty_AliasFirst) {
            CHECK(kCssPropertyAlias[i] == i);
        } else {
            CHECK(kCssPropertyAlias[i] < PrCssProperty_AliasFirst);
            CHECK(kCssPropertyGrammarRoot[i] == CssGrammarRoot_None);
        }
    }
    CHECK(kCssPropertyAlias[cssPropertyFromText("-webkit-align-items")] == PrCssProperty_AlignItems);
}

TEST_CASE("functions name their own keyword") {
    CssGrammarIndex root = kCssValueDefGrammarRoot[CssValueDef_CounterFn];
    REQUIRE(cssGrammarType(kCssGrammarNodes[root]) == CssGrammarType_Function);
    CHECK(cssKeywordName(PrCssKeyword(cssGrammarValue(kCssGrammarNodes[root]))) == "counter");
    CHECK(childCount(root) == 1);
}

TEST_CASE("a parameterized type is a production of its own for its parameter") {
    // <boolean-condition> = <boolean-expr[ <media()> | <supports()> ]>
    CssGrammarIndex root = kCssValueDefGrammarRoot[CssValueDef_BooleanCondition];
    REQUIRE(cssGrammarType(kCssGrammarNodes[root]) == CssGrammarType_Type);
    CHECK(cssGrammarValue(kCssGrammarNodes[root]) == CssValueDef_BooleanExprOfMediaSupports);
    CHECK(childCount(root) == 0);

    // <boolean-expr-group[ <media()> | <supports()> ]> = [ <media()> | <supports()> ] | ( <boolean-expr[ <media()> |
    // <supports()> ]> ) | <general-enclosed>
    CssGrammarIndex group = kCssValueDefGrammarRoot[CssValueDef_BooleanExprGroupOfMediaSupports];
    REQUIRE(cssGrammarType(kCssGrammarNodes[group]) == CssGrammarType_OneOf);
    CssGrammarIndex test = childAt(group, 0);
    REQUIRE(cssGrammarType(kCssGrammarNodes[test]) == CssGrammarType_OneOf);
    CHECK(cssGrammarValue(kCssGrammarNodes[childAt(test, 0)]) == CssValueDef_MediaFn);
    CHECK(cssGrammarValue(kCssGrammarNodes[childAt(test, 1)]) == CssValueDef_SupportsFn);
}

TEST_CASE("a property name reads back as its property") {
    CHECK(cssPropertyFromText("color") == PrCssProperty_Color);
    CHECK(cssPropertyFromText("COLOR") == PrCssProperty_Color);
    CHECK(cssPropertyFromText("-webkit-transform") == PrCssProperty_WebkitTransform);
    CHECK(cssPropertyFromText("nonesuch") == PrCssProperty_Count);
    CHECK(cssPropertyFromText("--x") == PrCssProperty_Count);

    // An alias keeps its own identity. kCssPropertyAlias resolves it.
    PrCssProperty stretch = cssPropertyFromText("font-stretch");
    CHECK(stretch == PrCssProperty_FontStretch);
    CHECK(kCssPropertyAlias[stretch] == PrCssProperty_FontWidth);
}
