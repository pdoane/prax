#include "css/src/css_object.h"
#include "css/src/css_parser.h"
#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

std::string text(const CssRuleData* rule) {
    TextBuffer buffer;
    cssSerializeRule(&buffer, rule);
    return take(&buffer);
}

std::string selectors(const CssRuleData* rule) {
    TextBuffer buffer;
    cssSerializeSelectorText(&buffer, rule);
    return take(&buffer);
}

std::string prelude(const CssRuleData* rule) {
    TextBuffer buffer;
    cssSerializePrelude(&buffer, rule);
    return take(&buffer);
}

// Returns true if a rule's prelude holds a node for `production`.
bool preludeHolds(const CssRuleData* rule, CssValueDef production) {
    for (uint32_t i = 0; i < rule->prelude.count; ++i) {
        if (cssValueIsProduction(&rule->prelude.values[i], production))
            return true;
    }
    return false;
}

std::string declarations(const CssDeclarationBlock* block) {
    TextBuffer buffer;
    cssSerializeDeclarationBlock(&buffer, block);
    return take(&buffer);
}

const CssDeclaration& declarationAt(const CssDeclarationBlock* block, uint32_t index) {
    return cssDeclarations(block)[index];
}

std::string nameAt(const CssDeclarationBlock* block, uint32_t index) {
    return textOf(cssDeclarationName(block, &declarationAt(block, index)));
}

std::string valueAt(const CssDeclarationBlock* block, uint32_t index) {
    TextBuffer buffer;
    cssSerializeDeclarationValue(&buffer, block, &declarationAt(block, index));
    return take(&buffer);
}

const CssRuleData* child(const CssRuleData* rule, uint32_t index) {
    return rule->children.data[index];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Rules

TEST_CASE("a sheet parses into the rules it holds") {
    CssTestSheet sheet("a { color: red } @media print { b { color: blue } } c {}");
    REQUIRE(sheet.count() == 3);

    CHECK(cssRuleType(sheet.rule(0)) == PrCssRuleType_Style);
    CHECK(selectors(sheet.rule(0)) == "a");
    CHECK(cssRuleType(sheet.rule(1)) == PrCssRuleType_Media);
    CHECK(sheet.rule(1)->atRule == CssAtRule_Media);
    CHECK(prelude(sheet.rule(1)) == "print");
    CHECK(cssRuleType(sheet.rule(2)) == PrCssRuleType_Style);
    CHECK(sheet.rule(2)->style.count == 0);
}

TEST_CASE("a rule serializes the way cssom defines") {
    CssTestSheet sheet("a { color: red; width: 1px !important }");
    CHECK(text(sheet.rule(0)) == "a { color: red; width: 1px !important; }");

    CssTestSheet empty("a {}");
    CHECK(text(empty.rule(0)) == "a { }");

    CssTestSheet media("@media print { a { color: red } }");
    CHECK(text(media.rule(0)) == "@media print {\n  a { color: red; }\n}");

    // cssom-1/#serialize-a-css-rule writes the space after @media, and an empty query list.
    CssTestSheet all("@media {}");
    CHECK(text(all.rule(0)) == "@media  {\n}");

    CssTestSheet import("@import url(x); @import \"y\" print, screen;");
    CHECK(text(import.rule(0)) == "@import url(\"x\");");
    CHECK(text(import.rule(1)) == "@import url(\"y\") print, screen;");
}

TEST_CASE("an at-rule nothing defines is not a rule") {
    // There is no @charset rule, and an at-rule name that no spec gives meaning to is a parse error.
    CssTestSheet sheet("@charset \"utf-8\"; @nonesuch { a {} } a {}");
    REQUIRE(sheet.count() == 1);
    CHECK(cssRuleType(sheet.rule(0)) == PrCssRuleType_Style);
}

TEST_CASE("an at-rule with no CSSRule constant is still a rule") {
    CssTestSheet sheet("@layer base { a { color: red } } @layer x, y; @container (width > 1px) { b {} }");
    REQUIRE(sheet.count() == 3);
    CHECK(sheet.rule(0)->atRule == CssAtRule_Layer);
    CHECK(cssRuleType(sheet.rule(0)) == 0);
    CHECK(text(sheet.rule(0)) == "@layer base {\n  a { color: red; }\n}");
    CHECK(text(sheet.rule(1)) == "@layer x, y;");
    CHECK(text(sheet.rule(2)) == "@container (width > 1px) {\n  b { }\n}");
}

TEST_CASE("an at-rule's prelude has to be one its grammar takes") {
    // css-cascade-5/#layer-names: no ident of a layer name is a CSS-wide keyword.
    CssTestSheet layer("@layer initial { } @layer a.b { }");
    REQUIRE(layer.count() == 1);
    CHECK(prelude(layer.rule(0)) == "a.b");

    // css-cascade-5/#layer-names: no whitespace inside a layer name.
    CssTestSheet spaced("@layer a . b { }");
    CHECK(spaced.count() == 0);

    // @font-face accepts no prelude, and @namespace no block.
    CssTestSheet font("@font-face x { } @namespace x url(y) { }");
    CHECK(font.count() == 0);
}

TEST_CASE("a legacy alias parses as the at-rule it stands for") {
    CssTestSheet sheet("@-webkit-keyframes spin { from { top: 0 } }");
    REQUIRE(sheet.count() == 1);
    CHECK(cssRuleType(sheet.rule(0)) == PrCssRuleType_Keyframes);
    CHECK(text(sheet.rule(0)) == "@-webkit-keyframes spin {\n  0% { top: 0px; }\n}");
}

TEST_CASE("@keyframes holds keyframe rules") {
    CssTestSheet sheet("@keyframes spin { from { top: 0 } 50%, 100% { top: 2px !important; left: 1px } 120% { } a { } }");
    REQUIRE(sheet.count() == 1);
    const CssRuleData* keyframes = sheet.rule(0);
    REQUIRE(keyframes->children.count == 2);
    CHECK(cssRuleType(child(keyframes, 0)) == PrCssRuleType_Keyframe);
    CHECK(prelude(child(keyframes, 1)) == "50%, 100%");

    // css-animations-1/#keyframes: !important is ignored in a keyframe block.
    CHECK(text(child(keyframes, 1)) == "50%, 100% { left: 1px; }");

    // css-animations-1/#keyframes: a keyframes name is not `none`.
    CssTestSheet none("@keyframes none { } @keyframes \"none\" { }");
    REQUIRE(none.count() == 1);
    CHECK(prelude(none.rule(0)) == "\"none\"");
}

// ---------------------------------------------------------------------------------------------------------------------
// Media queries

TEST_CASE("a media query list serializes the way cssom defines") {
    struct Case {
        PrStringView written;
        const char* serialized;
    };
    for (const Case& test :
         {Case{"@media aLL {}", "all"}, Case{"@media not PRINT {}", "not print"},
          Case{"@media all and (color) {}", "(color)"},
          Case{"@media not all and (color) {}", "not all and (color)"},
          Case{"@media screen and (MIN-width:5px) AND (max-width:40px) {}",
               "screen and (min-width: 5px) and (max-width: 40px)"},
          Case{"@media (400px <= width < 700px), print {}",
               "(400px <= width < 700px), print"}}) {
        INFO(textOf(test.written));
        CssTestSheet sheet(test.written);
        REQUIRE(sheet.count() == 1);
        CHECK(prelude(sheet.rule(0)) == test.serialized);
    }
}

TEST_CASE("a media query that is not one is not all") {
    // mediaqueries-5/#error-handling: an entry that does not match the grammar is `not all`. An entry that names an
    // unknown feature, gives a feature a value it does not accept, or puts a prefix on a feature that accepts none is
    // unknown, and is kept as written (csswg-drafts#7595).
    struct Case {
        PrStringView written;
        const char* serialized;
    };
    for (const Case& test :
         {Case{"@media (example, all,), speech {}", "(example, all,), speech"},
          Case{"@media &test, speech {}", "not all, speech"},
          Case{"@media screen and (max-weight: 3kg) and (color), (color) {}",
               "screen and (max-weight: 3kg) and (color), (color)"},
          Case{"@media (min-orientation: portrait) {}", "(min-orientation: portrait)"},
          Case{"@media (color: 20example) {}", "(color: 20example)"},
          Case{"@media or and (color) {}", "not all"},
          Case{"@media (orientation < portrait) {}", "(orientation < portrait)"}}) {
        INFO(textOf(test.written));
        CssTestSheet sheet(test.written);
        REQUIRE(sheet.count() == 1);
        CHECK(prelude(sheet.rule(0)) == test.serialized);
    }

    // An unknown media type is part of the grammar and stays, and so does <general-enclosed>.
    CssTestSheet unknown("@media unknown, (not a feature) {}");
    CHECK(prelude(unknown.rule(0)) == "unknown, (not a feature)");

    // An @custom-media rule whose list holds an entry that is not a media query is invalid.
    CssTestSheet custom("@custom-media --a true!; @custom-media --b (color), print;");
    REQUIRE(custom.count() == 1);
    CHECK(prelude(custom.rule(0)) == "--b (color), print");
}

// ---------------------------------------------------------------------------------------------------------------------
// Supports

TEST_CASE("a container query's features are the ones of the function they are in") {
    // css-conditional-5/#size-container, css-conditional-5/#scroll-state-container,
    // css-anchor-position-2/#anchored-container: each query accepts its own section's features. Any other feature is
    // <general-enclosed>, kept as written.
    struct Case {
        PrStringView written;
        bool known;
    };
    for (const Case& test :
         {Case{"@container (width > 1px) {}", true},
          Case{"@container (stuck: top) {}", false},
          Case{"@container scroll-state(stuck: top) {}", true},
          Case{"@container scroll-state((snapped: x) or (scrollable)) {}", true},
          Case{"@container scroll-state(width > 1px) {}", false},
          Case{"@container anchored(fallback: none) {}", true},
          Case{"@container anchored(fallback: --a flip-block) {}", true},
          Case{"@container anchored(fallback: 0) {}", false},
          Case{"@container anchored(fallback:) {}", false},
          Case{"@container anchored(stuck: top) {}", false}}) {
        INFO(textOf(test.written));
        CssTestSheet sheet(test.written);
        REQUIRE(sheet.count() == 1);
        CHECK(preludeHolds(sheet.rule(0), CssValueDef_GeneralEnclosed) == !test.known);
    }
}

TEST_CASE("a style query holds a declaration or a range, and a value holds no comparison") {
    CssTestSheet sheet("@container style(--a: 1) and style(color) and style(--b > 2) {}"
                             "@container style(--a = 1 = 2) {}");
    REQUIRE(sheet.count() == 2);
    CHECK(!preludeHolds(sheet.rule(0), CssValueDef_GeneralEnclosed));
    CHECK(preludeHolds(sheet.rule(1), CssValueDef_GeneralEnclosed));
}

TEST_CASE("@when takes media() and supports() as the leaves of its condition") {
    CssTestSheet sheet(
        "@when media(width > 1px) and (supports(display: grid) or unknown(x)) {}"
              "@when (width > 1px) {}");
    CHECK(sheet.count() == 2);
    CHECK(prelude(sheet.rule(0))
          == "media(width > 1px) and (supports(display: grid) or unknown(x))");
    CHECK(preludeHolds(sheet.rule(1), CssValueDef_GeneralEnclosed));
}

TEST_CASE("an @supports condition is kept as written, whitespace aside") {
    CssTestSheet sheet("@supports (display:flex) and ( not (color : red !important)) { a {} }");
    REQUIRE(sheet.count() == 1);
    CHECK(prelude(sheet.rule(0)) == "(display: flex) and (not (color: red !important))");
    CHECK(text(sheet.rule(0))
          == "@supports (display: flex) and (not (color: red !important)) {\n  a { }\n}");

    // A declaration of an unknown property, or of a value that the property does not accept, is still a declaration.
    CssTestSheet unknown("@supports (nonesuch: 1) or (color: 1px) or selector(a > b) {}");
    REQUIRE(unknown.count() == 1);
    CHECK(prelude(unknown.rule(0)) == "(nonesuch: 1) or (color: 1px) or selector(a > b)");

    // css-conditional-3/#at-supports: text that does not even match <general-enclosed> is not a rule.
    CssTestSheet invalid("@supports (a: b) and {} @supports a {}");
    CHECK(invalid.count() == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Where rules are

TEST_CASE("@import and @namespace come before every other rule") {
    // css-cascade-5/#at-import, css-namespaces-3/#declaration: an @layer statement may come first.
    CssTestSheet sheet(
        "@layer a; @import url(x); @namespace svg url(y); @import url(z); a {} "
              "@namespace url(w);");
    REQUIRE(sheet.count() == 4);
    CHECK(text(sheet.rule(0)) == "@layer a;");
    CHECK(text(sheet.rule(1)) == "@import url(\"x\");");
    CHECK(text(sheet.rule(2)) == "@namespace svg url(\"y\");");
    CHECK(selectors(sheet.rule(3)) == "a");

    // Neither is valid in a grouping rule.
    CssTestSheet grouped("@media print { @import url(x); @namespace url(y); @media screen {} }");
    REQUIRE(grouped.count() == 1);
    CHECK(grouped.rule(0)->children.count == 1);
}

TEST_CASE("an @namespace rule declares what a selector's prefix names") {
    CssTestSheet sheet("@namespace svg url(http://w3.org/svg); @namespace \"http://x\"; "
                             "svg|a {} b|c {}");
    REQUIRE(sheet.count() == 3);
    PrStringView url;
    REQUIRE(cssSheetNamespace(sheet.sheet->data, "svg", &url));
    CHECK(textOf(url) == "http://w3.org/svg");
    REQUIRE(cssSheetNamespace(sheet.sheet->data, PrStringView{}, &url));
    CHECK(textOf(url) == "http://x");
    CHECK_FALSE(cssSheetNamespace(sheet.sheet->data, "SVG", &url));

    // selectors-4/#invalid: a prefix that no rule declares makes the selector invalid.
    CHECK(selectors(sheet.rule(2)) == "svg|a");
}

TEST_CASE("a margin rule is valid in @page alone") {
    CssTestSheet sheet("@page :first { margin-top: 1px; @top-left { content: \"x\" } } @top-left {}");
    REQUIRE(sheet.count() == 1);
    const CssRuleData* page = sheet.rule(0);
    REQUIRE(page->children.count == 1);
    CHECK(cssRuleType(child(page, 0)) == PrCssRuleType_Margin);
    CHECK(text(page) == "@page :first { margin-top: 1px; @top-left { content: \"x\"; } }");
}

// ---------------------------------------------------------------------------------------------------------------------
// Nesting

TEST_CASE("a nested rule is a rule of the one that encloses it") {
    CssTestSheet sheet("a { color: red; & b { color: blue } }");
    REQUIRE(sheet.count() == 1);

    const CssRuleData* outer = sheet.rule(0);
    REQUIRE(outer->children.count == 1);
    CHECK(selectors(child(outer, 0)) == "& b");
    CHECK(text(outer) == "a {\n  color: red;\n  & b { color: blue; }\n}");
}

TEST_CASE("a nested rule's prelude is a relative selector list") {
    // css-nesting-1/#cssom: the nesting selector that a relative selector implies is written out.
    CssTestSheet implied("a { b { } > c { } .d, + .e { } }");
    const CssRuleData* outer = implied.rule(0);
    REQUIRE(outer->children.count == 3);
    CHECK(selectors(child(outer, 0)) == "& b");
    CHECK(selectors(child(outer, 1)) == "& > c");
    CHECK(selectors(child(outer, 2)) == "& .d, & + .e");

    // A selector that already contains `&` is not relative.
    CssTestSheet written("a { &:hover { } :is(&) b { } }");
    REQUIRE(written.rule(0)->children.count == 2);
    CHECK(selectors(child(written.rule(0), 0)) == "&:hover");
    CHECK(selectors(child(written.rule(0), 1)) == ":is(&) b");

    // A top-level rule accepts no relative selector.
    CssTestSheet top("> a { }");
    CHECK(top.count() == 0);
}

TEST_CASE("declarations after a rule are a nested declarations rule") {
    // css-syntax-3/#consume-block-contents, css-nesting-1/#nested-declarations-rule.
    CssTestSheet sheet("a { color: red; & b { } width: 1px; height: 2px; & c { } }");
    const CssRuleData* outer = sheet.rule(0);
    CHECK(declarations(&outer->style) == "color: red;");
    REQUIRE(outer->children.count == 3);
    CHECK(child(outer, 1)->kind == CssRuleDataKind_NestedDeclarations);
    CHECK(cssRuleType(child(outer, 1)) == 0);
    CHECK(text(child(outer, 1)) == "width: 1px; height: 2px;");
    CHECK(text(outer) == "a {\n  color: red;\n  & b { }\n  width: 1px; height: 2px;\n  & c { }\n}");

    // A custom property whose value contains a block is a declaration, not a rule.
    CssTestSheet custom("div { .a { } --x:hover { } .b { } }");
    const CssRuleData* div = custom.rule(0);
    REQUIRE(div->children.count == 2);
    CHECK(selectors(child(div, 0)) == "& .a");
    CHECK(child(div, 1)->kind == CssRuleDataKind_NestedDeclarations);
    CHECK(valueAt(&child(div, 1)->style, 0) == "hover { } .b { }");
}

TEST_CASE("a rule that is not valid ends no run of declarations") {
    // css-syntax-3/#consume-block-contents ends a run at an invalid rule, but the engine reads past it
    // (css-syntax/var-with-blocks.html, csswg-drafts#11271).
    CssTestSheet sheet("div { @at {} color: green; & b {} width: 1px }");
    const CssRuleData* div = sheet.rule(0);
    CHECK(declarations(&div->style) == "color: green;");
    REQUIRE(div->children.count == 2);
    CHECK(text(child(div, 1)) == "width: 1px;");

    CssTestSheet run("div { color: green; @at {} width: 1px; . {} height: 1px }");
    CHECK(declarations(&run.rule(0)->style) == "color: green; width: 1px; height: 1px;");
    CHECK(run.rule(0)->children.count == 0);

    // Outside a nested context, the declarations are the rule's own, wherever they appear.
    CssTestSheet page("@page { @at {} margin-top: 20px; @top-left {} margin-left: 1px }");
    CHECK(declarations(&page.rule(0)->style) == "margin-top: 20px; margin-left: 1px;");
}

TEST_CASE("a group rule nested in a style rule holds declarations as nested declarations") {
    // css-nesting-1/#nested-group-rules
    CssTestSheet sheet(".a { @media (width > 1px) { left: 1px; & .b { } right: 1px } }");
    const CssRuleData* outer = sheet.rule(0);
    REQUIRE(outer->children.count == 1);
    const CssRuleData* media = child(outer, 0);
    REQUIRE(media->children.count == 3);
    CHECK(child(media, 0)->kind == CssRuleDataKind_NestedDeclarations);
    CHECK(selectors(child(media, 1)) == "& .b");
    CHECK(text(media) == "@media (width > 1px) {\n  left: 1px;\n  & .b { }\n  right: 1px;\n}");

    // At the top level of the sheet, @media accepts no declarations.
    CssTestSheet top("@media print { left: 1px; a { } }");
    REQUIRE(top.rule(0)->children.count == 1);
    CHECK(top.rule(0)->children.data[0]->kind == CssRuleDataKind_Style);

    // Nor does a style rule accept a rule that holds no style rules.
    CssTestSheet font("a { @font-face { } @media print { } }");
    REQUIRE(font.rule(0)->children.count == 1);
}

TEST_CASE("a style rule inside @scope is relative to its :scope") {
    // css-nesting-1/#nesting-at-scope
    CssTestSheet sheet("@scope (.a) to (.b) { --x: 1; div { display: block } }");
    REQUIRE(sheet.count() == 1);
    const CssRuleData* scope = sheet.rule(0);
    CHECK(prelude(scope) == "(.a) to (.b)");
    REQUIRE(scope->children.count == 2);
    CHECK(child(scope, 0)->kind == CssRuleDataKind_NestedDeclarations);
    CHECK(selectors(child(scope, 1)) == "div");
    CHECK(text(scope) == "@scope (.a) to (.b) {\n  --x: 1;\n  div { display: block; }\n}");
}

TEST_CASE("a selector in a prelude is written as a style rule's is") {
    struct Case {
        const char* rule;
        const char* prelude;
    };
    const Case cases[] = {
        {"@scope (:is(div,span)) {}", "(:is(div, span))"},
        {"@scope (.a) to (:has(>b , c)) {}", "(.a) to (:has(> b, c))"},
        {"@scope (:nth-child( 2N + 1 of a,b )) {}", "(:nth-child(2n+1 of a, b))"},
        {"@supports selector(:nth-last-child(-n+3)) {}", "selector(:nth-last-child(-n+3))"},
        {"@supports selector(:not(.a,.b)) {}", "selector(:not(.a, .b))"},
        {"@supports selector(:lang(en,\"fr\")) {}", "selector(:lang(en, \"fr\"))"},
        {"@supports selector(::part(a b)) {}", "selector(::part(a b))"},
        {"@supports selector(::view-transition-group(*.a)) {}",
         "selector(::view-transition-group(*.a))"},
        {"@custom-selector :--x :where(a,b);", ":--x :where(a, b)"},
        // cssom-1/#serialize-a-simple-selector, in the way that a style rule's selectors are written.
        {"@supports selector([a=b i]) {}", "selector([a=\"b\" i])"},
        {"@supports selector(P:BEFORE) {}", "selector(P::before)"},
        {"@page :nth( 2N + 1 of chapter ) {}", ":nth(2n+1 of chapter)"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.rule);
        CssTestSheet sheet(c.rule);
        REQUIRE(sheet.count() == 1);
        CHECK(prelude(sheet.rule(0)) == c.prelude);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Declarations

TEST_CASE("a declaration keeps the property it names and drops what names none") {
    // The last declaration of a repeated name wins, at the place where it was written.
    CssTestSheet sheet("a { color: red; nonesuch: 1; --custom: x; COLOR: blue }");
    const CssDeclarationBlock* style = &sheet.rule(0)->style;
    REQUIRE(style->count == 2);

    CHECK(declarationAt(style, 0).property == PrCssProperty_Count);
    CHECK(nameAt(style, 0) == "--custom");
    CHECK(declarationAt(style, 1).property == PrCssProperty_Color);
    CHECK(valueAt(style, 1) == "blue");
}

TEST_CASE("a repeated custom property keeps the last, where the last was written") {
    // A custom property name matches exactly rather than ASCII case-insensitively.
    CssTestSheet sheet("a { --x: 1; --y: 2; --X: 3; --x: 4 }");
    const CssDeclarationBlock* style = &sheet.rule(0)->style;
    REQUIRE(style->count == 3);
    CHECK(nameAt(style, 0) == "--y");
    CHECK(nameAt(style, 1) == "--X");
    CHECK(nameAt(style, 2) == "--x");
    CHECK(valueAt(style, 2) == "4");
    // The names are hashed the way they compare, so a name differing only in case is not a candidate duplicate either.
    CHECK(declarationAt(style, 1).hash != declarationAt(style, 2).hash);
}

TEST_CASE("a declaration holds its value, which serializes from what it holds") {
    CssTestSheet sheet("a { width: 0; color: RED; line-height: 1.50; margin-left: +1E1px; "
                             "background-color: #1ac; outline-color: #11aacc80 }");
    const CssDeclarationBlock* style = &sheet.rule(0)->style;
    REQUIRE(style->count == 6);
    CHECK(valueAt(style, 0) == "0px");
    CHECK(valueAt(style, 1) == "red");
    CHECK(valueAt(style, 2) == "1.5");
    CHECK(valueAt(style, 3) == "10px");

    // css-color-4/#css-serialization-of-srgb
    CHECK(valueAt(style, 4) == "rgb(17, 170, 204)");
    CHECK(valueAt(style, 5) == "rgba(17, 170, 204, 0.5)");
}

TEST_CASE("a declaration whose value the property does not take declares nothing") {
    CssTestSheet sheet("a { color: red; width: red; line-height: 1 2; color: \"blue }");
    CHECK(declarations(&sheet.rule(0)->style) == "color: red;");

    // A custom property accepts anything that a declaration value can hold (css-variables-1/#syntax), including
    // nothing, which serializes as a single space (css-variables-1/#serializing-custom-props).
    CssTestSheet custom("a { --x: red  blue /**/ 1px { }; --y:; --z: }");
    CHECK(declarations(&custom.rule(0)->style) == "--x: red  blue /**/ 1px { }; --y:  ; --z:  ;");

    // A closer that nothing opened is not allowed (css-syntax-3/#consume-list-of-components).
    CssTestSheet stray("a { --x: red ] }");
    CHECK(declarations(&stray.rule(0)->style) == "");
    CssTestSheet nested("a { --x: red { ] } }");
    CHECK(declarations(&nested.rule(0)->style) == "");
}

TEST_CASE("an at-rule that cssom gives a style declares descriptors") {
    CssTestSheet sheet("@font-face { font-family: foo; font-display: swap; color: red; --x: 1 }");
    REQUIRE(sheet.count() == 1);

    const CssRuleData* rule = sheet.rule(0);
    CHECK(cssRuleType(rule) == PrCssRuleType_FontFace);
    CHECK(rule->style.rule == CssAtRule_FontFace);
    CHECK(declarations(&rule->style) == "font-family: foo; font-display: swap;");
    CHECK(declarationAt(&rule->style, 0).descriptor == PrCssDescriptor_FontFaceFontFamily);
    CHECK(text(rule) == "@font-face { font-family: foo; font-display: swap; }");

    // css-page-3/#syntax-page-selector: the page context accepts properties as well.
    CssTestSheet page("@page { margin-top: 2px; page-orientation: upright; nonesuch: 1 }");
    CHECK(declarations(&page.rule(0)->style) == "margin-top: 2px; page-orientation: upright;");

    // css-mixins-1/#function-body: a function body accepts custom properties, and no others. Its declarations form a
    // rule of their own.
    CssTestSheet function("@function --f() { --x: 1; color: red; result: 2 }");
    REQUIRE(function.count() == 1);
    REQUIRE(function.rule(0)->children.count == 1);
    const CssRuleData* body = child(function.rule(0), 0);
    CHECK(body->style.rule == CssAtRule_Function);
    CHECK(declarations(&body->style) == "--x: 1; result: 2;");
}

TEST_CASE("an at-rule's block holds what its spec requires of it") {
    // css-properties-values-api-1/#at-property-rule: syntax and inherits, and an initial value except for the universal
    // syntax, in any written order.
    CssTestSheet property(
        "@property --a { syntax: '*'; inherits: false }"
              "@property --b { syntax: '<length>'; inherits: false }"
              "@property --c { initial-value: 1px; inherits: true; syntax: '<length>' }");
    REQUIRE(property.count() == 2);
    CHECK(text(property.rule(1))
          == "@property --c { syntax: \"<length>\"; inherits: true; initial-value: 1px; }");

    // css-counter-styles-3/#the-counter-style-rule: no rule defines decimal, disc, square, circle, disclosure-open or
    // disclosure-closed.
    CssTestSheet counter("@counter-style DECIMAL {} @counter-style disc {} @counter-style decimals {}");
    REQUIRE(counter.count() == 1);
    CHECK(prelude(counter.rule(0)) == "decimals");
}

TEST_CASE("a prose production parses what its spec lists") {
    // css-page-3/#typedef-page-size-page-size, css-fonts-4/#font-face-src-parsing, which drops a source that is not
    // valid, and scroll-animations-1/#typedef-timeline-range-name.
    CssTestSheet sheet("@page { size: A3 landscape }"
                             "@font-face { src: url(a) format(woff), bogus, local(x) }"
                             "@keyframes k { entry 10% { top: 0 } }");
    REQUIRE(sheet.count() == 3);
    CHECK(declarations(&sheet.rule(0)->style) == "size: a3 landscape;");
    CHECK(declarations(&sheet.rule(1)->style) == "src: url(\"a\") format(woff), local(\"x\");");
    CHECK(prelude(child(sheet.rule(2), 0)) == "entry 10%");
}

TEST_CASE("a rule that is valid but not where it is inserted is not a syntax error") {
    CssTestSheet sheet("a {}");
    CHECK(
        cssRuleListInsert(sheet.sheet->data, nullptr, nullptr, "@import url(x);", 1, nullptr)
        == PrStatus_NotAllowed);
    CHECK(cssRuleListInsert(sheet.sheet->data, nullptr, nullptr, "@layer;", 0, nullptr) == PrStatus_Syntax);
    CHECK(cssRuleListInsert(sheet.sheet->data, nullptr, nullptr, "@nonesuch {}", 0, nullptr) == PrStatus_Syntax);
}

TEST_CASE("an @function body holds each run of declarations as a rule, conditional rules between") {
    CssTestSheet sheet("@function --f() { --x: 1; @supports (width: 1px) { result: 1; } result: 2 }");
    REQUIRE(sheet.count() == 1);
    const CssRuleData* function = sheet.rule(0);
    REQUIRE(function->children.count == 3);
    CHECK(declarations(&child(function, 0)->style) == "--x: 1;");
    CHECK(declarations(&child(child(function, 1), 0)->style) == "result: 1;");
    CHECK(child(child(function, 1), 0)->style.rule == CssAtRule_Function);
    CHECK(declarations(&child(function, 2)->style) == "result: 2;");
}

TEST_CASE("the unicode-range descriptor reads its value with unicode ranges allowed") {
    CssTestSheet sheet("@font-face { unicode-range: u+a?, U+0-10ffff, u+416 }");
    CHECK(declarations(&sheet.rule(0)->style)
          == "unicode-range: U+A0-AF, U+0-10FFFF, U+416;");

    // css-fonts-4/#unicode-range-desc: a bound above the maximum code point, or an end before the start.
    CssTestSheet high("@font-face { unicode-range: u+0-110000 }");
    CHECK(declarations(&high.rule(0)->style) == "");
    CssTestSheet backwards("@font-face { unicode-range: u+5-1 }");
    CHECK(declarations(&backwards.rule(0)->style) == "");

    // Nothing else is tokenized that way, so `u+a` stays an ident and a delim.
    CssTestSheet other("a { --x: u+a }");
    CHECK(declarations(&other.rule(0)->style) == "--x: u+a;");
}

TEST_CASE("a descriptor is found by name and wins over the property of that name") {
    CssTestSheet sheet("@page { size: 4px; margin-top: 2px }");
    CssDeclarationBlock* style = &sheet.rule(0)->style;

    REQUIRE(cssDeclarationBlockFind(style, named(style, "SIZE")) == 0);
    CHECK(declarationAt(style, 0).descriptor == PrCssDescriptor_PageSize);
    CHECK(nameAt(style, 0) == "size");

    CHECK(cssDeclarationBlockSet(style, named(style, "size"), "auto", false, nullptr) == CssEditResult_Changed);
    CHECK(cssDeclarationBlockSet(style, named(style, "size"), "nonesuch", false,
                                 nullptr)
          == CssEditResult_Rejected);
    CHECK(declarations(style) == "size: auto; margin-top: 2px;");
}

TEST_CASE("a declaration block is found, set and removed by name") {
    CssTestSheet sheet("a { color: red; --y: z }");
    CssDeclarationBlock* style = &sheet.rule(0)->style;

    CHECK(cssDeclarationBlockFind(style, named(style, "color")) == 0);
    CHECK(cssDeclarationBlockFind(style, named(style, "COLOR")) == 0);
    CHECK(cssDeclarationBlockFind(style, named(style, "--y")) == 1);
    CHECK(cssDeclarationBlockFind(style, named(style, "width")) == 2);

    CHECK(cssDeclarationBlockSet(style, named(style, "width"), "  1px  ", false,
                                 nullptr)
          == CssEditResult_Changed);
    CHECK(declarations(style) == "color: red; --y: z; width: 1px;");

    CssDeclaredName unknown = {};
    CHECK_FALSE(cssNameFromText(style->rule, "nonesuch", &unknown));
    CHECK(cssDeclarationBlockSet(style, named(style, "width"), "red", false, nullptr) == CssEditResult_Rejected);
    // cssom-1/#set-a-css-declaration sets the declaration that it finds, which stays in place.
    CHECK(cssDeclarationBlockSet(style, named(style, "color"), "blue", true, nullptr) == CssEditResult_Changed);
    CHECK(declarations(style) == "color: blue !important; --y: z; width: 1px;");

    PrString removed = {};
    cssDeclarationBlockRemove(style, named(style, "width"), &removed);
    CHECK(textOf(removed) == "1px");
    prStringReset(&removed);
    CHECK(declarations(style) == "color: blue !important; --y: z;");

    // Setting an empty value removes the declaration (cssom-1/#dom-cssstyledeclaration-setproperty).
    CHECK(cssDeclarationBlockSet(style, named(style, "color"), "  ", false, nullptr) == CssEditResult_Changed);
    CHECK(declarations(style) == "--y: z;");
}

TEST_CASE("a declaration block reparses from text") {
    CssTestSheet sheet("a { color: red }");
    CssDeclarationBlock* style = &sheet.rule(0)->style;

    cssDeclarationBlockParse(style, "width: 2px; height: 3px; & b { }", nullptr);
    CHECK(declarations(style) == "width: 2px; height: 3px;");
}

TEST_CASE("a changed block keeps its declarations' order and values, and a parse packs it again") {
    CssTestSheet sheet("a { --long: [a b c d e]; color: red; background-image: url(x.png) }");
    CssDeclarationBlock* style = &sheet.rule(0)->style;
    CHECK(style->stored == nullptr);

    CHECK(cssDeclarationBlockSet(style, named(style, "color"), "blue", false, nullptr) == CssEditResult_Changed);
    CHECK(style->stored != nullptr);
    for (int i = 0; i < 20; ++i) {
        char width[8] = {char('0' + i / 10), char('0' + i % 10), 'p', 'x'};
        CHECK(cssDeclarationBlockSet(style, named(style, "width"), PrStringView{width, 4},
                                     false, nullptr)
              == CssEditResult_Changed);
    }
    CHECK(
        cssDeclarationBlockSet(style, named(style, "--new"), "  q  ", false, nullptr)
        == CssEditResult_Changed);
    CHECK(
        declarations(style)
        == "--long: [a b c d e]; color: blue; background-image: url(\"x.png\"); width: 19px; --new: q;");

    // A shorthand's value sets every longhand or none.
    CHECK(cssDeclarationBlockSet(style, named(style, "margin"), "1px nonesuch", false,
                                 nullptr)
          == CssEditResult_Rejected);
    CHECK(cssDeclarationBlockSet(style, named(style, "margin"), "1px 2px", false,
                                 nullptr)
          == CssEditResult_Changed);
    CHECK(cssDeclarationBlockRemove(style, named(style, "color"), nullptr) == CssEditResult_Changed);
    CHECK(
        declarations(style)
        == "--long: [a b c d e]; background-image: url(\"x.png\"); width: 19px; --new: q; margin: 1px 2px;");
    CHECK(nameAt(style, 0) == "--long");
    CHECK(nameAt(style, 3) == "--new");

    // A copy of a changed block has the same changes, and is independent of it.
    CssRuleData* copy = cssRuleDataCopy(sheet.rule(0));
    CssDeclarationBlock* copied = &copy->style;
    CHECK(cssDeclarationBlockRemove(style, named(style, "margin"), nullptr) == CssEditResult_Changed);
    CHECK(
        declarations(copied)
        == "--long: [a b c d e]; background-image: url(\"x.png\"); width: 19px; --new: q; margin: 1px 2px;");
    cssRuleDataRelease(copy);

    cssDeclarationBlockParse(style, "--k: 1; top: 0", nullptr);
    CHECK(style->stored == nullptr);
    CHECK(declarations(style) == "--k: 1; top: 0px;");
}

// ---------------------------------------------------------------------------------------------------------------------
// Rule lists

TEST_CASE("a rule list inserts and deletes by index") {
    CssTestSheet sheet("a { color: red }");
    CssSheetData* data = sheet.sheet->data;

    CHECK(cssRuleListInsert(data, nullptr, nullptr, "b { color: blue }", 0, nullptr) == PrStatus_Ok);
    CHECK(sheet.count() == 2);
    CHECK(selectors(sheet.rule(0)) == "b");

    CHECK(cssRuleListInsert(data, nullptr, nullptr, "c {", 5, nullptr) == PrStatus_OutOfRange);
    CHECK(cssRuleListInsert(data, nullptr, nullptr, "not a rule", 0, nullptr) == PrStatus_Syntax);

    // cssom-1/#parse-a-css-rule: a rule whose prelude is invalid is a syntax error.
    CHECK(cssRuleListInsert(data, nullptr, nullptr, ":bogus {}", 0, nullptr) == PrStatus_Syntax);
    CHECK(cssRuleListInsert(data, nullptr, nullptr, "@bogus;", 0, nullptr) == PrStatus_Syntax);
    // cssom-1/#parse-a-css-rule: a rule that is invalid anywhere is a syntax error.
    CHECK(cssRuleListInsert(data, nullptr, nullptr, "@nonesuch {}", 0, nullptr) == PrStatus_Syntax);
    CHECK(
        cssRuleListInsert(data, nullptr, nullptr, "@font-feature-values bongo;", 0, nullptr)
        == PrStatus_Syntax);
    CHECK(cssRuleListInsert(data, nullptr, nullptr, "@media (;) {} x", 0, nullptr) == PrStatus_Syntax);

    CHECK(cssRuleListRemovable(data, nullptr, 0) == PrStatus_Ok);
    cssRuleListRemove(data, nullptr, 0);
    CHECK(cssRuleListRemovable(data, nullptr, 1) == PrStatus_OutOfRange);
    CHECK(sheet.count() == 1);
    CHECK(selectors(sheet.rule(0)) == "a");
}

TEST_CASE("a rule is inserted only where it may stand") {
    // cssom-1/#insert-a-css-rule: breaking the constraints that css-cascade-5/#at-import sets is a
    // HierarchyRequestError.
    CssTestSheet sheet("@import url(x); a {}");
    CssSheetData* data = sheet.sheet->data;
    CHECK(cssRuleListInsert(data, nullptr, nullptr, "@import url(y);", 2, nullptr) == PrStatus_NotAllowed);
    CHECK(cssRuleListInsert(data, nullptr, nullptr, "b {}", 0, nullptr) == PrStatus_NotAllowed);
    CHECK(cssRuleListInsert(data, nullptr, nullptr, "@import url(y);", 1, nullptr) == PrStatus_Ok);

    // An @namespace rule, inserted while the list holds a rule that is neither @import nor @namespace, is an
    // InvalidStateError.
    CHECK(cssRuleListInsert(data, nullptr, nullptr, "@namespace url(z);", 2, nullptr) == PrStatus_InvalidState);
    CHECK(sheet.count() == 3);

    // Inserting a rule that a grouping rule's block does not accept.
    CssTestSheet media("@media print {}");
    CHECK(cssRuleListInsert(media.sheet->data, media.rule(0), nullptr, "@import url(x);", 0,
                            nullptr)
          == PrStatus_NotAllowed);
}

TEST_CASE("text inserted into a style rule that is no rule is declarations") {
    // cssom-1/#insert-a-css-rule, into a nested list.
    CssTestSheet sheet("a { color: red }");
    CssRuleData* rule = sheet.rule(0);
    CHECK(cssRuleListInsert(sheet.sheet->data, rule, nullptr, "width: 1px; height: 2px", 0,
                            nullptr)
          == PrStatus_Ok);
    REQUIRE(rule->children.count == 1);
    CHECK(child(rule, 0)->kind == CssRuleDataKind_NestedDeclarations);
    CHECK(text(child(rule, 0)) == "width: 1px; height: 2px;");
    CHECK(cssRuleListInsert(sheet.sheet->data, rule, nullptr, "nonesuch", 0, nullptr) == PrStatus_Syntax);
}

TEST_CASE("a style rule's new selectors change its nested rules' specificity") {
    // css-nesting-1/#nest-selector: `&` has the greatest specificity of its parent's selectors.
    CssTestSheet sheet("a { @media print { & b { } } }");
    CssRuleData* outer = sheet.rule(0);
    const CssRuleData* inner = child(child(outer, 0), 0);
    CHECK(inner->selectors->complex[0].specificity == cssSpecificity(0, 0, 2));

    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    REQUIRE(cssStyleRulePrepareSelectorText(&edit, nullptr, sheet.sheet->data, "#x, .y") == CssEditResult_Changed);
    cssRuleCommit(&edit, outer);
    cssEditEnd(&edit);
    CHECK(inner->selectors->complex[0].specificity == cssSpecificity(1, 0, 1));
}

TEST_CASE("a legacy descriptor name declares the descriptor it is an alias of") {
    CssTestSheet sheet("@font-face { font-stretch: condensed; font-width: 75% 125% }");
    const CssDeclarationBlock* style = &sheet.rule(0)->style;
    REQUIRE(style->count == 1);
    CHECK(declarationAt(style, 0).descriptor == PrCssDescriptor_FontFaceFontWidth);
    CHECK(declarations(style) == "font-width: 75% 125%;");
}

TEST_CASE("a range descriptor whose bounds are the same serializes as the one value") {
    // css-fonts-4/#serializing-at-rules
    CssTestSheet sheet("@font-face { font-weight: 500 500; font-width: 75% 125%; font-style: oblique 10deg 10deg; font-size: calc(1) calc(1) }");
    CHECK(
        declarations(&sheet.rule(0)->style)
        == "font-weight: 500; font-width: 75% 125%; font-style: oblique 10deg; font-size: calc(1) calc(1);");
}

TEST_CASE("a palette's override colors are absolute colors") {
    // css-fonts-4/#descdef-font-palette-values-override-colors
    auto colors = [](PrStringView value) {
        std::string rule = "@font-palette-values --a { override-colors: " + textOf(value) + " }";
        CssTestSheet sheet(rule);
        return declarations(&sheet.rule(0)->style);
    };
    CHECK(colors("0 #123, 1 color-mix(in lch, red, blue)")
          == "override-colors: 0 rgb(17, 34, 51), 1 color-mix(in lch, red, blue);");
    CHECK(colors("0 red, 1 canvas") == "");
    CHECK(colors("0 currentcolor") == "");
    CHECK(colors("0 light-dark(white, black)") == "");
    CHECK(colors("0 color-mix(in lch, red, color-mix(in lch, currentcolor, black))") == "");
}

TEST_CASE("a src descriptor keeps the sources that parse and that the UA supports") {
    auto src = [](PrStringView value) {
        std::string rule = "@font-face { src: " + textOf(value) + " }";
        CssTestSheet sheet(rule);
        return declarations(&sheet.rule(0)->style);
    };
    CHECK(src("url(a.woff2) format(woff2) tech(variations, color-COLRv1), local(\"B\")")
          == "src: url(\"a.woff2\") format(woff2) tech(variations, color-colrv1), local(\"B\");");
    CHECK(src("local(inherit), url(a) format(bad), junk, url(b) format(\"woff-variations\")")
          == "src: url(\"b\") format(\"woff-variations\");");
    CHECK(src("url(a) tech(incremental), url(b) format(svg), local(C D)") == "src: local(\"C D\");");
    CHECK(src("local(), url(a) format(\"svg\"), url(b) format(woff) dummy()") == "");
}

TEST_CASE("a feature value block maps the names its author gives to lists of indexes") {
    // css-fonts-4/#font-feature-values-syntax
    CssTestSheet sheet("@font-feature-values Bongo { font-display: swap; @swash { pretty: 1; Pretty: 2; twice: 1 2; } "
        "@character-variant { a: 1 2; b: 1 2 3; } "
        "@styleset { \\:x: 1 2 3; neg: 1 -1; frac: 1.5; } }");
    REQUIRE(sheet.count() == 1);
    const CssRuleData* rule = sheet.rule(0);
    CHECK(cssRuleType(rule) == PrCssRuleType_FontFeatureValues);
    // A block for each of the seven feature types (css-fonts-4/#om-fontfeaturevalues), with those holding declarations
    // first.
    REQUIRE(rule->children.count == 7);
    CHECK(declarations(&child(rule, 0)->style) == "pretty: 1; Pretty: 2;");
    CHECK(declarations(&child(rule, 1)->style) == "a: 1 2;");
    CHECK(declarations(&child(rule, 2)->style) == "\\:x: 1 2 3;");
    CHECK(text(rule)
          == "@font-feature-values Bongo { font-display: swap; @swash { pretty: 1; Pretty: 2; } "
             "@character-variant { a: 1 2; } @styleset { \\:x: 1 2 3; } }");
}

TEST_CASE("feature value blocks of one type are one block, the last value of a name standing") {
    // css-fonts-4/#serializing-at-rules
    CssTestSheet sheet("@font-feature-values foo { @swash { pretty: 0; cool: 2; } "
                             "@ornaments { x: 1 } @swash { pretty: 1; } }");
    const CssRuleData* rule = sheet.rule(0);
    REQUIRE(rule->children.count == 7);
    CHECK(text(child(rule, 0)) == "@swash { cool: 2; pretty: 1; }");
    CHECK(text(child(rule, 1)) == "@ornaments { x: 1; }");
}
