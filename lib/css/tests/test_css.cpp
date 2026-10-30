#include "pr/css.h"
#include "pr/dom.h"

#include "css/src/css_object.h"
#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

// lib/css/tests/test_css_c_api.c, compiled as C.
extern "C" PrStringView prTestCApi(PrCssStyleDeclaration* style);

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Rule interfaces

PrCssRule* ruleAt(const CssTestSheet& sheet, uint32_t index) {
    return sheet.object(index);
}

// Returns the strings that a sequence function writes, and releases them.
template <typename Handle>
std::vector<std::string> strings(uint32_t (*function)(const Handle*, PrString* PR_NULLABLE, uint32_t), const Handle* handle) {
    std::vector<PrString> items(function(handle, nullptr, 0));
    function(handle, items.data(), uint32_t(items.size()));
    std::vector<std::string> out;
    for (PrString& item : items)
        out.push_back(take(item));
    return out;
}

// Returns the views that a sequence function writes, as strings.
template <typename Handle>
std::vector<std::string> strings(uint32_t (*function)(const Handle*, PrStringView* PR_NULLABLE, uint32_t), const Handle* handle) {
    std::vector<PrStringView> items(function(handle, nullptr, 0));
    function(handle, items.data(), uint32_t(items.size()));
    std::vector<std::string> out;
    for (PrStringView item : items)
        out.push_back(textOf(item));
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a CSS destroy takes null and does nothing") {
    cssStyleSheetRelease(nullptr);
    cssRuleRelease(nullptr);
    prCssStyleSheetRelease(nullptr);
    prCssRuleRelease(nullptr);
}

TEST_CASE("the public API compiles and links as C") {
    CssTestSheet sheet("a { color: red }");
    PrCssStyleDeclaration* style = prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(
        prCssStyleRuleQuery(prCssRuleListRuleAt(prCssStyleSheetCssRules(sheet.handle()), 0))));

    CHECK(textOf(prTestCApi(style)) == "");
}

TEST_CASE("the public header declares what a caller links against") {
    // A rule type is the number that cssom assigns it, not its position in the IDL.
    CHECK(PrCssRuleType_Style == 1);
    CHECK(PrCssRuleType_Keyframes == 7);
    CHECK(PrCssRuleType_Margin == 9);

    CHECK(prStatusName(PrStatus_Ok).size == 0);
    CHECK(prStatusName(PrStatus_OutOfRange).size == PrStringView("OutOfRange").size);
}

TEST_CASE("a sheet hands out its rules through the C API") {
    CssTestSheet sheet("a { color: red } @media print { b { color: blue } }");

    CHECK(prCssStyleSheetDisabled(sheet.handle()) == false);
    prCssStyleSheetSetDisabled(sheet.handle(), true);
    CHECK(prCssStyleSheetDisabled(sheet.handle()) == true);
    CHECK((prCssStyleSheetParentStyleSheet(sheet.handle()) == nullptr));
    CHECK((prCssStyleSheetOwnerRule(sheet.handle()) == nullptr));

    PrCssRuleList* rules = prCssStyleSheetCssRules(sheet.handle());
    CHECK(prCssRuleListLength(rules) == 2);
    CHECK((prCssRuleListRuleAt(rules, 2) == nullptr));

    PrCssRule* first = prCssRuleListRuleAt(rules, 0);
    REQUIRE((first != nullptr));
    CHECK(prCssRuleType(first) == PrCssRuleType_Style);
    CHECK(take(prCssRuleCssText(first)) == "a { color: red; }");
    CHECK((prCssRuleParentStyleSheet(first) == sheet.handle()));
    CHECK((prCssRuleParentRule(first) == nullptr));

    CHECK(prCssRuleType(prCssRuleListRuleAt(rules, 1)) == PrCssRuleType_Media);
}

TEST_CASE("a style rule reaches its selector and its declarations") {
    CssTestSheet sheet("a, b { color: red; width: 1px !important }");
    PrCssStyleRule* rule = prCssStyleRuleQuery(prCssRuleListRuleAt(prCssStyleSheetCssRules(sheet.handle()), 0));
    REQUIRE((rule != nullptr));

    CHECK(take(prCssStyleRuleSelectorText(rule)) == "a, b");
    prCssStyleRuleSetSelectorText(rule, "  c  >  d  ");
    CHECK(take(prCssStyleRuleSelectorText(rule)) == "c > d");

    // Text that is not a selector list leaves the selector unmodified.
    prCssStyleRuleSetSelectorText(rule, "");
    CHECK(take(prCssStyleRuleSelectorText(rule)) == "c > d");

    PrCssStyleDeclaration* style = prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(rule));
    CHECK(prCssStyleDeclarationLength(style) == 2);
    PrCssDeclarationName name = {};
    prCssStyleDeclarationNameAt(style, 0, &name);
    CHECK(name.property == PrCssProperty_Color);
    prCssDeclarationNameReset(&name);
    const PrCssDeclarationName color = prCssDeclarationNameFromProperty(PrCssProperty_Color);
    const PrCssDeclarationName width = prCssDeclarationNameFromProperty(PrCssProperty_Width);
    CHECK(take(prCssStyleDeclarationGetPropertyValue(style, &color)) == "red");
    CHECK(take(prCssStyleDeclarationGetPropertyValue(style, Named(style, "COLOR"))) == "red");
    CHECK_FALSE(prCssStyleDeclarationNameFromText(style, "nonesuch", &name));
    CHECK(prCssStyleDeclarationIsImportant(style, &width));
    CHECK_FALSE(prCssStyleDeclarationIsImportant(style, &color));
    CHECK((prCssStyleDeclarationParentRule(style) == prCssStyleRuleAsCssRule(rule)));
}

TEST_CASE("a declaration block is set and removed through the C API") {
    CssTestSheet sheet("a { color: red }");
    PrCssStyleDeclaration* style = prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(
        prCssStyleRuleQuery(prCssRuleListRuleAt(prCssStyleSheetCssRules(sheet.handle()), 0))));

    const PrCssDeclarationName width = prCssDeclarationNameFromProperty(PrCssProperty_Width);
    CHECK(prCssStyleDeclarationSetProperty(style, &width, "1px", true) == PrStatus_Ok);
    CHECK(take(prCssStyleDeclarationCssText(style)) == "color: red; width: 1px !important;");

    // A value that the property does not accept, and a name that the block does not accept, leave the block unchanged.
    const PrCssDeclarationName height = prCssDeclarationNameFromProperty(PrCssProperty_Height);
    CHECK(prCssStyleDeclarationSetProperty(style, &height, "red", false) == PrStatus_InvalidData);
    const PrCssDeclarationName display = {PrCssProperty_Count, PrCssDescriptor_FontFaceFontDisplay, prStringNull()};
    CHECK(prCssStyleDeclarationSetProperty(style, &display, "swap", false) == PrStatus_InvalidData);
    CHECK(prCssStyleDeclarationRemoveProperty(style, &display, nullptr) == PrStatus_InvalidData);
    CHECK(prCssStyleDeclarationLength(style) == 2);

    const PrCssDeclarationName color = prCssDeclarationNameFromProperty(PrCssProperty_Color);
    PrString removed = {};
    CHECK(prCssStyleDeclarationRemoveProperty(style, &color, &removed) == PrStatus_Ok);
    CHECK(take(removed) == "red");
    CHECK(prCssStyleDeclarationRemoveProperty(style, &color, nullptr) == PrStatus_Ok);

    CHECK(prCssStyleDeclarationSetCssText(style, "top: 1px; left: 2px") == PrStatus_Ok);
    CHECK(take(prCssStyleDeclarationCssText(style)) == "top: 1px; left: 2px;");
}

TEST_CASE("rules are inserted and deleted through the C API") {
    CssTestSheet sheet("a { color: red }");
    PrCssStyleSheet* handle = sheet.handle();

    uint32_t index = 0xffffffffu;
    CHECK(prCssStyleSheetInsertRule(handle, "b { color: blue }", 0, &index) == PrStatus_Ok);
    CHECK(index == 0);
    CHECK(prCssStyleSheetInsertRule(handle, "c {}", 9, nullptr) == PrStatus_OutOfRange);
    CHECK(prCssStyleSheetInsertRule(handle, "nonsense", 0, nullptr) == PrStatus_Syntax);
    CHECK(prCssRuleListLength(prCssStyleSheetCssRules(handle)) == 2);

    CHECK(prCssStyleSheetDeleteRule(handle, 0) == PrStatus_Ok);
    CHECK(prCssStyleSheetDeleteRule(handle, 9) == PrStatus_OutOfRange);
    CHECK(prCssRuleListLength(prCssStyleSheetCssRules(handle)) == 1);
}

TEST_CASE("a nested rule reaches the rule that encloses it") {
    CssTestSheet sheet("a { & b { color: red } }");
    PrCssStyleRule* outer = prCssStyleRuleQuery(prCssRuleListRuleAt(prCssStyleSheetCssRules(sheet.handle()), 0));

    PrCssRuleList* children = prCssGroupingRuleCssRules(prCssStyleRuleAsCssGroupingRule(outer));
    REQUIRE(prCssRuleListLength(children) == 1);

    PrCssRule* inner = prCssRuleListRuleAt(children, 0);
    CHECK((prCssRuleParentRule(inner) == prCssStyleRuleAsCssRule(outer)));
    CHECK(take(prCssRuleCssText(inner)) == "& b { color: red; }");

    CHECK(prCssGroupingRuleInsertRule(prCssStyleRuleAsCssGroupingRule(outer), "& c {}", 1, nullptr) == PrStatus_Ok);
    CHECK(prCssRuleListLength(children) == 2);
    CHECK(prCssGroupingRuleDeleteRule(prCssStyleRuleAsCssGroupingRule(outer), 0) == PrStatus_Ok);
    CHECK(prCssRuleListLength(children) == 1);
}

TEST_CASE("a sheet a caller makes is a sheet a caller releases") {
    PrCssStyleSheet* plain = prCssStyleSheetCreate(nullptr);
    CHECK(prCssStyleSheetDisabled(plain) == false);
    CHECK(textOf(prCssStyleSheetHref(plain)) == "");
    CHECK(prCssRuleListLength(prCssStyleSheetCssRules(plain)) == 0);

    // Explicit defaults and an omitted dictionary select the same options.
    PrCssStyleSheetInit defaults = prCssStyleSheetInitDefault();
    PrCssStyleSheet* same = prCssStyleSheetCreate(&defaults);
    CHECK(prCssStyleSheetDisabled(same) == false);

    PrCssStyleSheetInit options = prCssStyleSheetInitDefault();
    options.baseUrl = "https://example.test/";
    options.disabled = true;

    PrCssStyleSheet* sheet = prCssStyleSheetCreate(&options);
    CHECK(prCssStyleSheetDisabled(sheet) == true);
    CHECK(textOf(prCssStyleSheetHref(sheet)) == "https://example.test/");

    // Only a constructed sheet accepts replaceSync, and this one is constructed.
    CHECK(prCssStyleSheetReplaceSync(sheet, "a { color: red }") == PrStatus_Ok);
    CHECK(prCssRuleListLength(prCssStyleSheetCssRules(sheet)) == 1);

    prCssStyleSheetRelease(sheet);
    prCssStyleSheetRelease(same);
    prCssStyleSheetRelease(plain);
}

TEST_CASE("only a constructed sheet is replaced in place") {
    CssTestSheet sheet("a { color: red }");
    CHECK(prCssStyleSheetReplaceSync(sheet.handle(), "b { color: blue }") == PrStatus_Ok);
    CHECK(take(prCssRuleCssText(prCssRuleListRuleAt(prCssStyleSheetCssRules(sheet.handle()), 0))) == "b { color: blue; }");

    sheet.sheet->constructed = false;
    CHECK(prCssStyleSheetReplaceSync(sheet.handle(), "c {}") == PrStatus_NotAllowed);
}

TEST_CASE("a media list is the media queries of a sheet, an @media rule and an @import rule") {
    CssTestSheet sheet("@import url(a.css) layer(x) supports(display: grid) screen, print;" "@media (color), bad$ {}");
    PrCssImportRule* import = prCssImportRuleQuery(ruleAt(sheet, 0));
    REQUIRE((import != nullptr));
    CHECK(textOf(prCssImportRuleHref(import)) == "a.css");
    CHECK(take(prCssImportRuleLayerName(import)) == "x");
    CHECK(take(prCssImportRuleSupportsText(import)) == "display: grid");
    CHECK((prCssImportRuleStyleSheet(import) == nullptr));

    PrCssMediaList* media = prCssImportRuleMedia(import);
    CHECK(prCssMediaListLength(media) == 2);
    CHECK(take(prCssMediaListQueryAt(media, 1)) == "print");
    PrString past = prCssMediaListQueryAt(media, 2);
    CHECK(prStringIsNull(&past));

    // mediaqueries-5/#error-handling: an entry that is not a media query becomes `not all`.
    PrCssMediaList* rule = prCssMediaRuleMedia(prCssMediaRuleQuery(ruleAt(sheet, 1)));
    CHECK(take(prCssMediaListMediaText(rule)) == "(color), not all");

    // cssom-1/#dom-medialist-appendmedium and cssom-1/#dom-medialist-deletemedium compare serializations.
    prCssMediaListSetMediaText(media, "SCREEN");
    prCssMediaListAppendMedium(media, "screen");
    prCssMediaListAppendMedium(media, "print");
    CHECK(take(prCssMediaListMediaText(media)) == "screen, print");
    CHECK(prCssMediaListDeleteMedium(media, "Screen") == PrStatus_Ok);
    CHECK(prCssMediaListDeleteMedium(media, "screen") == PrStatus_NotFound);
    CHECK(take(prCssRuleCssText(ruleAt(sheet, 0))) == "@import url(\"a.css\") layer(x) supports(display: grid) print;");

    PrCssMediaList* own = prCssStyleSheetMedia(sheet.handle());
    CHECK(prCssMediaListLength(own) == 0);
}

TEST_CASE("a keyframes rule finds, appends and deletes its keyframes by their selectors") {
    CssTestSheet sheet("@keyframes spin { from { top: 0 } 50% { top: 1px } 0% { top: 2px } }");
    PrCssKeyframesRule* keyframes = prCssKeyframesRuleQuery(ruleAt(sheet, 0));
    REQUIRE((keyframes != nullptr));
    CHECK(textOf(prCssKeyframesRuleName(keyframes)) == "spin");

    // css-animations-1/#dom-csskeyframesrule-findrule: the last rule whose key text matches the selectors.
    PrCssKeyframeRule* found = prCssKeyframesRuleFindRule(keyframes, "from");
    REQUIRE((found != nullptr));
    CHECK(take(prCssKeyframeRuleKeyText(found)) == "0%");
    CHECK((found == prCssKeyframesRuleKeyframeRuleAt(keyframes, 2)));
    // The rule is retained, so it survives a change that removes it from its list.
    prCssRuleRetain(prCssKeyframeRuleAsCssRule(found));

    prCssKeyframesRuleAppendRule(keyframes, "to { top: 3px }");
    prCssKeyframesRuleAppendRule(keyframes, "nonsense");
    CHECK(prCssKeyframesRuleLength(keyframes) == 4);
    prCssKeyframesRuleDeleteRule(keyframes, "0%");
    CHECK(prCssKeyframesRuleLength(keyframes) == 3);

    // cssom-1/#remove-a-css-rule: the removed rule has no parent.
    CHECK((prCssRuleParentRule(prCssKeyframeRuleAsCssRule(found)) == nullptr));
    CHECK(prCssKeyframeRuleSetKeyText(found, "nonsense") == PrStatus_Syntax);

    prCssKeyframesRuleSetName(keyframes, "none");
    CHECK(take(prCssRuleCssText(ruleAt(sheet, 0))).rfind("@keyframes \"none\" {", 0) == 0);
    prCssRuleRelease(prCssKeyframeRuleAsCssRule(found));
}

TEST_CASE("a rule's prelude answers its interface's attributes") {
    CssTestSheet sheet("@namespace svg url(http://x); @layer a.b, c; @container name (width > 1px), (color) {}"
        "@scope (.a) to (.b) {} @layer {}");
    PrCssNamespaceRule* space = prCssNamespaceRuleQuery(ruleAt(sheet, 0));
    CHECK(textOf(prCssNamespaceRulePrefix(space)) == "svg");
    CHECK(textOf(prCssNamespaceRuleNamespaceUri(space)) == "http://x");

    PrCssLayerStatementRule* layers = prCssLayerStatementRuleQuery(ruleAt(sheet, 1));
    std::vector<std::string> names = strings(prCssLayerStatementRuleNameList, layers);
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "a.b");
    CHECK(names[1] == "c");

    PrCssContainerRule* container = prCssContainerRuleQuery(ruleAt(sheet, 2));
    CHECK(take(prCssContainerRuleContainerName(container)) == "");
    PrCssContainerCondition conditions[2];
    REQUIRE(prCssContainerRuleConditions(container, conditions, 2) == 2);
    CHECK(textOf(conditions[0].name) == "name");
    CHECK(textOf(conditions[0].query) == "(width > 1px)");
    for (PrCssContainerCondition& condition : conditions)
        prCssContainerConditionReset(&condition);

    PrCssScopeRule* scope = prCssScopeRuleQuery(ruleAt(sheet, 3));
    CHECK(take(prCssScopeRuleStart(scope)) == ".a");
    CHECK(take(prCssScopeRuleEnd(scope)) == ".b");

    CHECK(take(prCssLayerBlockRuleName(prCssLayerBlockRuleQuery(ruleAt(sheet, 4)))) == "");
    CHECK((prCssLayerBlockRuleQuery(ruleAt(sheet, 1)) == nullptr));
}

TEST_CASE("a descriptor attribute reads and writes its rule's declaration") {
    CssTestSheet sheet("@counter-style foo { system: cyclic; symbols: a b }"
        "@property --x { syntax: '<length>'; inherits: true; initial-value: 0px }@font-face { font-family: bar }"
        "@view-transition { navigation: auto; types: a b }");
    PrCssCounterStyleRule* counter = prCssCounterStyleRuleQuery(ruleAt(sheet, 0));
    CHECK(take(prCssCounterStyleRuleSystem(counter)) == "cyclic");
    prCssCounterStyleRuleSetPad(counter, "2 \"0\"");
    prCssCounterStyleRuleSetPad(counter, "nonsense");
    CHECK(take(prCssCounterStyleRulePad(counter)) == "2 \"0\"");
    prCssCounterStyleRuleSetName(counter, "decimal");
    CHECK(textOf(prCssCounterStyleRuleName(counter)) == "foo");

    PrCssPropertyRule* property = prCssPropertyRuleQuery(ruleAt(sheet, 1));
    CHECK(textOf(prCssPropertyRuleSyntax(property)) == "<length>");
    CHECK(prCssPropertyRuleInherits(property));
    CHECK(take(prCssPropertyRuleInitialValue(property)) == "0px");

    PrCssFontFaceDescriptors* face = prCssFontFaceRuleStyle(prCssFontFaceRuleQuery(ruleAt(sheet, 2)));
    PrCssStyleDeclaration* descriptors = prCssFontFaceDescriptorsAsCssStyleDeclaration(face);
    CHECK(take(prCssStyleDeclarationGetPropertyValue(descriptors, Named(descriptors, "font-family"))) == "bar");
    const PrCssDeclarationName display = {PrCssProperty_Count, PrCssDescriptor_FontFaceFontDisplay, prStringNull()};
    CHECK(prCssStyleDeclarationSetProperty(descriptors, &display, "swap", false) == PrStatus_Ok);
    CHECK(take(prCssStyleDeclarationCssText(descriptors)) == "font-family: bar; font-display: swap;");

    PrCssViewTransitionRule* transition = prCssViewTransitionRuleQuery(ruleAt(sheet, 3));
    CHECK(strings(prCssViewTransitionRuleTypes, transition) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("a font feature values rule holds a map of each feature type") {
    CssTestSheet sheet("@font-feature-values F { @styleset { a: 1 2; b: 3 } @swash { c: 4 } @styleset { a: 5 } }");
    PrCssFontFeatureValuesRule* rule = prCssFontFeatureValuesRuleQuery(ruleAt(sheet, 0));
    PrCssFontFeatureValuesMap* styleset = prCssFontFeatureValuesRuleStyleset(rule);
    CHECK(prCssFontFeatureValuesMapSize(styleset) == 2);
    uint32_t values[2] = {};
    CHECK(prCssFontFeatureValuesMapGet(styleset, "a", values, 2) == 1);
    CHECK(values[0] == 5);
    CHECK(prCssFontFeatureValuesMapSize(prCssFontFeatureValuesRuleAnnotation(rule)) == 0);

    // css-fonts-4/#multi-value-features: a swash accepts one index.
    const uint32_t two[] = {6, 7};
    PrCssFontFeatureValuesMap* swash = prCssFontFeatureValuesRuleSwash(rule);
    prCssFontFeatureValuesMapSet(swash, "d", two, 2);
    prCssFontFeatureValuesMapSet(swash, "e", two, 1);
    CHECK(prCssFontFeatureValuesMapHas(swash, "e"));
    CHECK(!prCssFontFeatureValuesMapHas(swash, "d"));
    CHECK(prCssFontFeatureValuesMapDelete(swash, "c"));
    CHECK(take(prCssRuleCssText(ruleAt(sheet, 0))) == "@font-feature-values F { @styleset { b: 3; a: 5; } @swash { e: 6; } }");
}

TEST_CASE("custom media keeps its boolean or live media list in the rule") {
    CssTestSheet sheet("@custom-media --yes true; @custom-media --no false; "
        "@custom-media --query (color), print; @custom-media --empty;");
    REQUIRE(sheet.count() == 4);
    PrCssCustomMediaRule* yes = prCssCustomMediaRuleQuery(ruleAt(sheet, 0));
    REQUIRE(yes != nullptr);
    CHECK(textOf(prCssCustomMediaRuleName(yes)) == "--yes");
    CHECK(prCssCustomMediaRuleQueryValue(yes).mediaList == nullptr);
    CHECK(prCssCustomMediaRuleQueryValue(yes).boolean);
    CHECK(!prCssCustomMediaRuleQueryValue(prCssCustomMediaRuleQuery(ruleAt(sheet, 1))).boolean);
    PrCssCustomMediaRule* query = prCssCustomMediaRuleQuery(ruleAt(sheet, 2));
    PrCssMediaList* list = prCssCustomMediaRuleQueryValue(query).mediaList;
    REQUIRE(list != nullptr);
    CHECK(prCssMediaListLength(list) == 2);
    CHECK(prCssCustomMediaRuleQueryValue(query).mediaList == list);
    prCssMediaListSetMediaText(list, "screen");
    CHECK(take(prCssRuleCssText(ruleAt(sheet, 2))) == "@custom-media --query screen;");
    CHECK(prCssMediaListLength(prCssCustomMediaRuleQueryValue(prCssCustomMediaRuleQuery(ruleAt(sheet, 3))).mediaList) == 0);
}

TEST_CASE("mixin and apply rules retain parameters arguments and nested declarations") {
    CssTestSheet sheet("@mixin --m(--x <length>: 1px) { color: red; @contents { color: blue; } @contents; }"
        "a { @apply --m(1px, { red, blue }, calc(1px + 2px)) { color: green; } @apply --m; }");
    REQUIRE(sheet.count() == 2);
    PrCssMixinRule* mixin = prCssMixinRuleQuery(ruleAt(sheet, 0));
    REQUIRE(mixin != nullptr);
    CHECK(take(prCssMixinRuleName(mixin)) == "--m");
    CHECK(prCssMixinRuleContents(mixin));
    CHECK(prCssMixinRuleGetParameters(mixin, nullptr, 1) == 1);
    PrCssFunctionParameter parameter = {};
    REQUIRE(prCssMixinRuleGetParameters(mixin, &parameter, 1) == 1);
    CHECK(take(parameter.name) == "--x");
    CHECK(take(parameter.type) == "<length>");
    CHECK(take(parameter.defaultValue) == "1px");
    PrCssRuleList* rules = prCssGroupingRuleCssRules(prCssMixinRuleAsCssGroupingRule(mixin));
    REQUIRE(prCssRuleListLength(rules) == 3);
    CHECK(prCssNestedDeclarationsQuery(prCssRuleListRuleAt(rules, 0)) != nullptr);
    PrCssContentsBlockRule* contents = prCssContentsBlockRuleQuery(prCssRuleListRuleAt(rules, 1));
    REQUIRE(contents != nullptr);
    CHECK(prCssContentsStatementRuleQuery(prCssRuleListRuleAt(rules, 1)) == nullptr);
    CHECK(prCssRuleListLength(prCssGroupingRuleCssRules(prCssContentsBlockRuleAsCssGroupingRule(contents))) == 1);
    CHECK(prCssContentsStatementRuleQuery(prCssRuleListRuleAt(rules, 2)) != nullptr);
    rules = prCssGroupingRuleCssRules(prCssGroupingRuleQuery(ruleAt(sheet, 1)));
    REQUIRE(prCssRuleListLength(rules) == 2);
    PrCssApplyBlockRule* apply = prCssApplyBlockRuleQuery(prCssRuleListRuleAt(rules, 0));
    REQUIRE(apply != nullptr);
    CHECK(prCssApplyBlockRuleGetArguments(apply, nullptr, 3) == 3);
    CHECK(strings(prCssApplyBlockRuleGetArguments, apply) == std::vector<std::string>{"1px", "red, blue", "calc(1px + 2px)"});
    CHECK(prCssRuleListLength(prCssGroupingRuleCssRules(prCssApplyBlockRuleAsCssGroupingRule(apply))) == 1);
    CHECK(prCssApplyStatementRuleQuery(prCssRuleListRuleAt(rules, 1)) != nullptr);
    CHECK(prCssGroupingRuleQuery(prCssRuleListRuleAt(rules, 1)) == nullptr);
}

TEST_CASE("apply serialization omits empty parentheses and unnecessary argument braces") {
    CssTestSheet sheet("@apply --invalid(); @mixin --empty() {}a { @apply --empty(); @apply --m({ 1px }, { red, blue }); }");
    REQUIRE(sheet.count() == 2);
    CHECK(take(prCssRuleCssText(ruleAt(sheet, 0))) == "@mixin --empty {\n}");
    PrCssRuleList* rules = prCssGroupingRuleCssRules(prCssGroupingRuleQuery(ruleAt(sheet, 1)));
    REQUIRE(prCssRuleListLength(rules) == 2);
    CHECK(take(prCssRuleCssText(prCssRuleListRuleAt(rules, 0))) == "@apply --empty;");
    CHECK(take(prCssRuleCssText(prCssRuleListRuleAt(rules, 1))) == "@apply --m(1px, { red, blue });");

    // EOF implicitly closes component blocks, including an otherwise empty argument wrapper.
    CssTestSheet unfinished("a { @apply --m({");
    REQUIRE(unfinished.count() == 1);
    rules = prCssGroupingRuleCssRules(prCssGroupingRuleQuery(ruleAt(unfinished, 0)));
    REQUIRE(prCssRuleListLength(rules) == 1);
    CHECK(strings(prCssApplyStatementRuleGetArguments, prCssApplyStatementRuleQuery(prCssRuleListRuleAt(rules, 0))) ==
        std::vector<std::string>{""});
}

TEST_CASE("a highlight holds its ranges, and a registry names its highlights without holding them") {
    PrDomDocument* document = prDomDocumentCreate();
    PrDomRange* range = prDomRangeCreate(document);
    PrCssHighlight* highlight = prCssHighlightCreate();
    uint32_t version = prCssHighlightVersion(highlight);
    prCssHighlightAdd(highlight, range);
    prCssHighlightAdd(highlight, range);
    CHECK(prCssHighlightSize(highlight) == 1);
    CHECK(prCssHighlightVersion(highlight) != version);
    prDomRangeRelease(range);
    CHECK(prCssHighlightAt(highlight, 0) == range);
    CHECK(prCssHighlightType(highlight) == PrCssHighlightType_Highlight);

    PrCssHighlightRegistry* registry = prCssDocumentHighlights(document);
    PrCssHighlight* other = prCssHighlightCreate();
    prCssHighlightRegistrySet(registry, "a", highlight);
    prCssHighlightRegistrySet(registry, "b", other);
    // A name set again keeps its place.
    prCssHighlightRegistrySet(registry, "a", other);
    REQUIRE(prCssHighlightRegistrySize(registry) == 2);
    CHECK(textOf(prCssHighlightRegistryNameAt(registry, 0)) == "a");
    CHECK(prCssHighlightRegistryGet(registry, "a") == other);
    prCssHighlightRegistrySet(registry, "a", highlight);

    // The ranges keep their document alive, which the highlight does not own.
    prDomDocumentRelease(document);
    version = prCssHighlightRegistryVersion(registry);
    prCssHighlightRelease(other);
    CHECK(prCssHighlightRegistryVersion(registry) != version);
    CHECK(prCssHighlightRegistrySize(registry) == 1);
    CHECK(prCssHighlightRegistryGet(registry, "b") == nullptr);
    CHECK(prCssHighlightRegistryDelete(registry, "a"));
    CHECK_FALSE(prCssHighlightRegistryDelete(registry, "a"));
    prCssHighlightRelease(highlight);
}
