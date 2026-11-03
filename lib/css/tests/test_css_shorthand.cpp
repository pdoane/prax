#include "css/src/css_parse.h"
#include "css/src/css_shorthand.h"
#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// A block of declarations parsed from `text`, released with the test.
struct Block {
    CssDeclarationBlock style = {};

    explicit Block(PrStringView text) { cssDeclarationBlockParse(&style, text, nullptr); }
    ~Block() { cssDeclarationBlockReset(&style); }

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    std::string value(PrStringView name) const {
        PrTextBuffer buffer = {};
        cssSerializePropertyValue(&buffer, &style, named(&style, name));
        return take(&buffer);
    }

    std::string text() const {
        PrTextBuffer buffer = {};
        cssSerializeDeclarationBlock(&buffer, &style);
        return take(&buffer);
    }

    uint32_t length() const { return cssDeclarationBlockLength(&style); }

    // Returns the position at which the block lists `property`, or the block's length if it does not list it.
    uint32_t position(PrCssProperty property) const {
        uint32_t i = 0;
        while (i < length() && cssDeclarationBlockItem(&style, i).property != property)
            ++i;
        return i;
    }

    bool set(PrStringView name, PrStringView value, bool important = false) {
        return cssDeclarationBlockSet(&style, named(&style, name), value, important, nullptr) != CssEditResult_Rejected;
    }

    bool remove(PrStringView name) {
        return cssDeclarationBlockRemove(&style, named(&style, name), nullptr) == CssEditResult_Changed;
    }
};

// Returns the number of longhands that `all` sets.
uint32_t allCount() {
    return cssPropertyLonghands(PrCssProperty_All).count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Generated facts

TEST_CASE("every longhand's initial value is one its grammar takes, written as it serializes") {
    CssValuePool pool = {};
    CssMatchState state = {};
    for (PrCssProperty property = 0; property < PrCssProperty_Count; ++property) {
        PrStringView initial = cssPropertyInitial(property);
        if (initial.data == nullptr)
            continue;
        CAPTURE(textOf(cssPropertyName(property)));
        CssTestSource source(initial);
        CssValueSpan span;
        pool.values.count = 0;
        pool.text.count = 0;
        bool parsed = cssParsePropertyValue(property, &source.ts, &pool, &state, &span);
        CHECK(parsed);
        if (!parsed)
            continue;
        PrTextBuffer buffer = {};
        cssSerializeValues(&buffer, cssValuePoolRun(&pool, span));
        CHECK(take(&buffer) == textOf(initial));
    }
    cssValuePoolReset(&pool);
    cssMatchStateReset(&state);
}

TEST_CASE("a shorthand's longhands are longhands") {
    for (PrCssProperty property = 0; property < PrCssProperty_Count; ++property) {
        CssPropertyList longhands = cssPropertyLonghands(property);
        CAPTURE(textOf(cssPropertyName(property)));
        for (uint32_t j = 0; j < longhands.count; ++j)
            CHECK_FALSE(cssIsShorthand(longhands.data[j]));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Expansion and serialization

TEST_CASE("a box shorthand sets four sides and writes as few values as give them") {
    Block block("margin: 1px 2px");
    CHECK(block.style.count == 4);
    CHECK(block.value("margin-top") == "1px");
    CHECK(block.value("margin-right") == "2px");
    CHECK(block.value("margin-bottom") == "1px");
    CHECK(block.value("margin-left") == "2px");
    CHECK(block.value("margin") == "1px 2px");
    CHECK(block.text() == "margin: 1px 2px;");
}

TEST_CASE("a CSS-wide keyword sets every longhand, and a shorthand writes it where they all hold it") {
    Block block("margin: inherit");
    CHECK(block.value("margin-left") == "inherit");
    CHECK(block.value("margin") == "inherit");
    CHECK(block.text() == "margin: inherit;");

    Block mixed("margin: inherit; margin-left: 1px");
    CHECK(mixed.value("margin") == "");
    CHECK(mixed.text() == "margin-top: inherit; margin-right: inherit; margin-bottom: inherit; margin-left: 1px;");
}

TEST_CASE("a shorthand with var() leaves each longhand a pending-substitution value") {
    Block block("margin: var(--x) 1px");
    CHECK(block.length() == 4);
    CHECK(block.value("margin-top") == "");
    CHECK(block.value("margin") == "var(--x) 1px");
    CHECK(block.text() == "margin: var(--x) 1px;");

    // A longhand set on its own no longer holds the shorthand's value, so the shorthand cannot be written.
    Block changed("margin: var(--x); margin-top: 1px");
    CHECK(changed.value("margin") == "");
    CHECK(changed.value("margin-top") == "1px");

    // A shorthand within it is not the one whose value the longhands hold.
    Block border("border: var(--x)");
    CHECK(border.value("border") == "var(--x)");
    CHECK(border.value("border-top") == "");
}

// ---------------------------------------------------------------------------------------------------------------------
// Shared declarations

TEST_CASE("a CSS-wide keyword or a pending value is held once, and lists each longhand it sets") {
    Block wide("all: initial");
    CHECK(wide.style.count == 1);
    CHECK(wide.length() == allCount());
    CHECK(wide.value("color") == "initial");
    CHECK(wide.value("direction") == "");
    CHECK(wide.text() == "all: initial;");

    Block pending("all: var(--x)");
    CHECK(pending.style.count == 1);
    CHECK(pending.length() == allCount());
    CHECK(pending.value("all") == "var(--x)");
    CHECK(pending.value("color") == "");
    CHECK(pending.text() == "all: var(--x);");

    Block font("font: caption");
    CHECK(font.style.count == 1);
    CHECK(font.value("font") == "caption");
}

TEST_CASE("a longhand after a shared declaration is listed where it was written") {
    Block after("all: initial; color: red");
    CHECK(after.style.count == 2);
    CHECK(after.length() == allCount());
    CHECK(after.position(PrCssProperty_Color) == allCount() - 1);
    CHECK(after.value("color") == "red");
    CHECK(after.value("all") == "");

    // An earlier declaration is replaced, except an important one, which stays where it was.
    Block before("color: red; all: initial");
    CHECK(before.style.count == 1);
    CHECK(before.text() == "all: initial;");

    Block important("color: red !important; all: initial");
    CHECK(important.length() == allCount());
    CHECK(important.position(PrCssProperty_Color) == 0);
    CHECK(important.value("color") == "red");

    Block under("all: initial !important; color: red");
    CHECK(under.style.count == 1);
    CHECK(under.value("color") == "initial");
}

TEST_CASE("of shared declarations setting a longhand, the last important one or the last declares it") {
    Block later("border-top: inherit; border-color: initial");
    CHECK(later.style.count == 2);
    CHECK(later.length() == 6);
    CHECK(later.value("border-top-width") == "inherit");
    CHECK(later.value("border-top-color") == "initial");
    CHECK(later.position(PrCssProperty_BorderTopColor) == 2);

    Block important("margin: inherit !important; all: initial");
    CHECK(important.style.count == 2);
    CHECK(important.length() == allCount());
    CHECK(important.value("margin") == "inherit");
    CHECK(important.position(PrCssProperty_MarginTop) == 0);

    // A shared declaration that declares no longhand is not kept.
    Block covered("margin: inherit; all: initial");
    CHECK(covered.style.count == 1);
    CHECK(covered.text() == "all: initial;");
}

TEST_CASE("setProperty sets a longhand a shared declaration lists where it is listed") {
    Block block("all: initial");
    uint32_t color = block.position(PrCssProperty_Color);
    CHECK(block.set("color", "blue"));
    CHECK(block.length() == allCount());
    CHECK(block.position(PrCssProperty_Color) == color);
    CHECK(block.value("color") == "blue");
    CHECK(block.value("margin-top") == "initial");
}

TEST_CASE("setProperty of a shared value sets the longhands declared where they are") {
    Block block("color: red; margin: inherit");
    CHECK(block.set("all", "unset"));
    CHECK(block.length() == allCount());
    CHECK(block.position(PrCssProperty_Color) == 0);
    CHECK(block.position(PrCssProperty_MarginTop) == 1);
    CHECK(block.value("margin-left") == "unset");
    CHECK(block.text() == "all: unset;");

    Block again("all: initial");
    CHECK(again.set("all", "var(--x)", true));
    CHECK(again.style.count == 1);
    CHECK(again.value("all") == "var(--x)");
    CHECK(again.text() == "all: var(--x) !important;");
}

TEST_CASE("removeProperty removes the longhands a shared declaration lists") {
    Block longhand("all: initial; color: red");
    CHECK(longhand.remove("color"));
    CHECK(longhand.length() == allCount() - 1);
    CHECK(longhand.value("color") == "");
    CHECK_FALSE(longhand.remove("color"));

    Block shorthand("all: initial");
    CHECK(shorthand.remove("margin"));
    CHECK(shorthand.length() == allCount() - 4);
    CHECK(shorthand.value("margin-top") == "");
    CHECK(shorthand.value("padding-top") == "initial");

    Block all("direction: rtl; margin: var(--x); all: inherit !important");
    CHECK(all.remove("all"));
    CHECK(all.text() == "direction: rtl;");
}

TEST_CASE("a change that writes what the block holds leaves it unchanged") {
    Block block("margin: 1px 2px; all: initial; color: red");
    auto set = [&](PrStringView name, PrStringView value, bool important = false) {
        return cssDeclarationBlockSet(&block.style, named(&block.style, name), value, important, nullptr);
    };
    CHECK(set("color", "red") == CssEditResult_Unchanged);
    CHECK(set("color", "red", true) == CssEditResult_Changed);
    CHECK(set("color", "nonesuch") == CssEditResult_Rejected);

    // A longhand that a shared declaration lists has its value, and a shorthand has each longhand's value.
    CHECK(set("padding-top", "initial") == CssEditResult_Unchanged);
    CHECK(set("padding", "initial") == CssEditResult_Unchanged);
    CHECK(set("padding", "inherit") == CssEditResult_Changed);
    CHECK(cssDeclarationBlockRemove(&block.style, named(&block.style, "--absent"), nullptr) == CssEditResult_Unchanged);

    Block written("margin: 1px 2px");
    CHECK(cssDeclarationBlockSet(&written.style, named(&written.style, "margin"), "1px 2px 1px", false, nullptr) ==
        CssEditResult_Unchanged);
    CHECK(cssDeclarationBlockSet(&written.style, named(&written.style, "margin-left"), "2px", false, nullptr) ==
        CssEditResult_Unchanged);
}

TEST_CASE("setProperty sets each longhand where it is, and removeProperty removes them") {
    Block block("color: red; margin-left: 5px");
    CHECK(cssDeclarationBlockSet(&block.style, named(&block.style, "margin"), "1px", true, nullptr) ==
        CssEditResult_Changed);
    CHECK(block.text() == "color: red; margin: 1px !important;");
    CHECK(textOf(cssDeclarationName(&block.style, &cssDeclarations(&block.style)[1])) == "margin-left");
    CHECK(cssDeclarationBlockImportant(&block.style, named(&block.style, "margin")));

    CHECK(cssDeclarationBlockSet(&block.style, named(&block.style, "margin-top"), "2px", false, nullptr) ==
        CssEditResult_Changed);
    CHECK_FALSE(cssDeclarationBlockImportant(&block.style, named(&block.style, "margin")));
    CHECK(block.value("margin") == "");

    PrString removed = prStringNull();
    CHECK(cssDeclarationBlockRemove(&block.style, named(&block.style, "margin"), &removed) == CssEditResult_Changed);
    CHECK(take(removed) == "");
    CHECK(block.text() == "color: red;");
    CHECK(cssDeclarationBlockRemove(&block.style, named(&block.style, "margin"), nullptr) == CssEditResult_Unchanged);
}

TEST_CASE("an important declaration stands against a later one in the same block that is not") {
    Block block("margin: 1px !important; margin-top: 2px");
    CHECK(block.value("margin-top") == "1px");
    CHECK(block.text() == "margin: 1px !important;");
}

TEST_CASE("a component sets the longhand it names, and the rest take their initial values") {
    Block block("outline: red dashed");
    CHECK(block.value("outline-width") == "medium");
    CHECK(block.value("outline-style") == "dashed");
    CHECK(block.value("outline-color") == "red");
    CHECK(block.value("outline") == "dashed red");

    // border sets the four sides from one component each, and resets border-image.
    Block border("border: 1px solid");
    CHECK(border.style.count == 17);
    CHECK(border.value("border-left-width") == "1px");
    CHECK(border.value("border-image-slice") == "100%");
    CHECK(border.value("border") == "1px solid");
    CHECK(border.value("border-top") == "1px solid");
    CHECK(border.text() == "border: 1px solid;");
}

TEST_CASE("vertical-align sets baseline-source, alignment-baseline and baseline-shift, and writes baseline for none") {
    Block block("vertical-align: text-top 10px");
    CHECK(block.value("baseline-source") == "auto");
    CHECK(block.value("alignment-baseline") == "text-top");
    CHECK(block.value("baseline-shift") == "10px");
    CHECK(block.value("vertical-align") == "text-top 10px");

    // A CSS2 value is the value of one longhand, and the initial values write as baseline, which first | last cannot.
    Block sub("vertical-align: sub");
    CHECK(sub.value("baseline-shift") == "sub");
    CHECK(sub.value("vertical-align") == "sub");
    Block initial("vertical-align: baseline");
    CHECK(initial.value("vertical-align") == "baseline");
    Block first("vertical-align: first");
    CHECK(first.value("baseline-source") == "first");
    CHECK(first.value("vertical-align") == "first");

    // If every component could write the initial values, the first component writes them.
    Block background("background: none");
    CHECK(background.value("background") == "none");
}

TEST_CASE("layers give each longhand an entry per layer") {
    Block block("transition: opacity 1s, 2s 3s steps(2)");
    CHECK(block.value("transition-property") == "opacity, all");
    CHECK(block.value("transition-duration") == "1s, 2s");
    CHECK(block.value("transition-delay") == "0s, 3s");
    CHECK(block.value("transition-timing-function") == "ease, steps(2)");
    CHECK(block.value("transition") == "opacity 1s, 2s steps(2) 3s");

    Block uneven("transition: opacity 1s; transition-delay: 1s, 2s");
    CHECK(uneven.value("transition") == "");
}

TEST_CASE("flex, place-content and grid-area follow their prose") {
    Block none("flex: none");
    CHECK(none.value("flex-grow") == "0");
    CHECK(none.value("flex-basis") == "auto");
    Block one("flex: 2");
    CHECK(one.value("flex-shrink") == "1");
    CHECK(one.value("flex-basis") == "0%");
    CHECK(one.value("flex") == "2 1 0%");

    Block baseline("place-content: baseline");
    CHECK(baseline.value("justify-content") == "start");
    CHECK(baseline.value("place-content") == "baseline start");

    Block area("grid-area: a / 2");
    CHECK(area.value("grid-row-end") == "a");
    CHECK(area.value("grid-column-end") == "auto");
    CHECK(area.value("grid-area") == "a / 2");
}

TEST_CASE("font writes the longhands it sets and resets the rest") {
    Block block("font: italic bold 12px/1.5 serif");
    CHECK(block.value("font-style") == "italic");
    CHECK(block.value("font-weight") == "bold");
    CHECK(block.value("line-height") == "1.5");
    CHECK(block.value("font-kerning") == "auto");
    CHECK(block.value("font") == "italic bold 12px / 1.5 serif");

    // A font with a font-width writes the keyword for its percentage, which may come from a calculation.
    CHECK(cssDeclarationBlockSet(&block.style, named(&block.style, "font-stretch"), "calc(50%)", false, nullptr) ==
        CssEditResult_Changed);
    CHECK(block.value("font") == "italic bold ultra-condensed 12px / 1.5 serif");
    CHECK(cssDeclarationBlockSet(&block.style, named(&block.style, "font-stretch"), "calc(50% + 1%)", false, nullptr) ==
        CssEditResult_Changed);
    CHECK(block.value("font") == "");

    CHECK(cssDeclarationBlockSet(&block.style, named(&block.style, "font-variant-caps"),
        "all-small-caps", false, nullptr) == CssEditResult_Changed);
    CHECK(block.value("font") == "");

    // A system font's longhands hold the font until computed.
    Block system("font: caption");
    CHECK(system.value("font-family") == "");
    CHECK(system.value("font") == "caption");
    CHECK(system.text() == "font: caption;");
}

TEST_CASE("text-box leaves out a trim of trim-both and an edge of auto") {
    Block edge("text-box: text");
    CHECK(edge.value("text-box-trim") == "trim-both");
    CHECK(edge.value("text-box-edge") == "text");
    CHECK(edge.value("text-box") == "text");

    Block normal("text-box: normal");
    CHECK(normal.value("text-box-trim") == "none");
    CHECK(normal.value("text-box") == "normal");
}

TEST_CASE("column-rule gives each list an entry per rule, repeat() and all") {
    Block block("column-rule: solid 10px rgb(0, 0, 255), repeat(auto, blue 6px, 5px solid red)");
    CHECK(block.value("column-rule-width") == "10px, repeat(auto, 6px, 5px)");
    CHECK(block.value("column-rule-style") == "solid, repeat(auto, none, solid)");
    CHECK(block.value("column-rule-color") == "rgb(0, 0, 255), repeat(auto, blue, red)");
    CHECK(block.value("column-rule") == "10px solid rgb(0, 0, 255), repeat(auto, 6px blue, 5px solid red)");
}

TEST_CASE("background sets a list entry per layer, and its color from the final one") {
    Block block("background: yellow");
    CHECK(block.value("background-color") == "yellow");
    CHECK(block.value("background-image") == "none");
    CHECK(block.value("background") == "yellow");
}

TEST_CASE("a side's radius shorthand sets its two corners, horizontal radii before the slash and vertical after") {
    Block both("border-top-radius: 1px 2px / 3px 4px");
    CHECK(both.value("border-top-left-radius") == "1px 3px");
    CHECK(both.value("border-top-right-radius") == "2px 4px");
    CHECK(both.value("border-top-radius") == "1px 2px / 3px 4px");
    Block one("border-left-radius: 5px");
    CHECK(one.value("border-top-left-radius") == "5px");
    CHECK(one.value("border-bottom-left-radius") == "5px");
    CHECK(one.value("border-left-radius") == "5px");
    Block logical("border-block-end-radius: 1px 2px");
    CHECK(logical.value("border-end-start-radius") == "1px");
    CHECK(logical.value("border-end-end-radius") == "2px");
    CHECK(logical.value("border-block-end-radius") == "1px 2px");
}

TEST_CASE("shorthand serialization respects intermixed logical mappings") {
    Block blocked("margin-top: 1px; margin-inline-start: 2px; margin-right: 1px; margin-bottom: 1px; margin-left: 1px");
    CHECK(blocked.text() ==
        "margin-top: 1px; margin-inline-start: 2px; margin-right: 1px; margin-bottom: 1px; margin-left: 1px;");
    // getPropertyValue does not reorder the declaration block.
    CHECK(blocked.value("margin") == "1px");
    Block unrelated("margin-top: 1px; padding-inline-start: 2px; margin-right: 1px; margin-bottom: 1px; margin-left: 1px");
    CHECK(unrelated.text() == "margin: 1px; padding-inline-start: 2px;");
    Block logical("margin-inline-start: 1px; margin-left: 2px; margin-inline-end: 1px");
    CHECK(logical.text() == "margin-inline-start: 1px; margin-left: 2px; margin-inline-end: 1px;");
    Block after("margin: 1px; margin-inline-start: 2px");
    CHECK(after.text() == "margin: 1px; margin-inline-start: 2px;");
    Block important("margin-top: 1px !important; margin-inline-start: 2px; "
        "margin-right: 1px !important; margin-bottom: 1px !important; margin-left: 1px !important");
    CHECK(important.text().find("margin: ") == std::string::npos);
    Block corners("border-top-left-radius: 1px; border-start-start-radius: 2px; "
        "border-top-right-radius: 1px; border-bottom-right-radius: 1px; border-bottom-left-radius: 1px");
    CHECK(corners.text().find("border-radius: ") == std::string::npos);
}
