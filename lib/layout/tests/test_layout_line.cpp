#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/memory.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// The line box (CSS2/#line-height): vertical-align, and a line's height from the boxes on it. Each case is a stage of
// 20px Arimo on 40px lines that opens with a marker, an empty inline-block whose top is the line's baseline. Each case
// checks the engine's numbers for its stage.

// Returns a font of the fixture's family at `size`, on lines `lineHeight` tall, or normal lines if `lineHeight` is
// negative.
PrLayoutFontStyle font(float size, float lineHeight) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(size);
    font.lineHeight = lineHeight < 0 ? prLayoutLineHeightNormal() : prLayoutLineHeightPx(lineHeight);
    return font;
}

// Returns an inline-block `width` by `height`, each auto if negative.
PrLayoutBoxStyle inlineBlock(float width, float height) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_InlineBlock;
    box.width = width < 0 ? prLayoutLengthAuto() : prLayoutLengthPx(width);
    box.height = height < 0 ? prLayoutLengthAuto() : prLayoutLengthPx(height);
    return box;
}

// Returns a box moved by `shift` once aligned, or aligned by its baseline `name`.
PrLayoutBoxStyle aligned(PrLayoutBoxStyle box, PrLayoutBaselineShift shift) {
    box.baselineShift = shift;
    return box;
}
PrLayoutBoxStyle aligned(PrLayoutBoxStyle box, PrLayoutAlignmentBaseline name) {
    box.alignmentBaseline = name;
    return box;
}

PrLayoutBaselineShift keyword(PrLayoutKeyword keyword) {
    return prLayoutBaselineShiftKeyword(keyword);
}

// How wide a stage is, and whether it opens with its marker. As an atomic inline, the marker keeps its line from being
// a phantom line. A stage is a tree whose root holds a block 200px wide of 20px text on 40px lines, or of `style`,
// which opens with a marker.
struct Width {
    float px;
    bool marked = true;
};

struct Stage {
    Fixture f;
    PrLayoutFontStyle style = font(20, 40);
    PrLayoutNode stage = kPrLayoutNodeNone;
    PrLayoutNode marker = kPrLayoutNodeNone;

    Stage() { build(200); }
    explicit Stage(Width width) { build(width.px, width.marked); }

    // Creates a stage `width` wide in the font that `make` gives, which may add faces to the fixture's set.
    template <typename Make>
    explicit Stage(Make make, float width = 200) {
        style = make(f);
        build(width);
    }

    // Builds the stage `width` wide, in `style`, opening with the marker if `marked` is set.
    void build(float width, bool marked = true) {
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPx(width);
        stage = f.element(root, box, nullptr, nullptr, &style);
        if (marked)
            marker = add(stage, inlineBlock(0, 0));
    }

    // Creates a line break under `parent`, styled by `box`.
    PrLayoutNode lineBreak(PrLayoutNode parent, const PrLayoutBoxStyle& box = prLayoutBoxStyleDefault()) {
        return prLayoutNodeCreateLineBreak(f.tree, parent, f.style(box, nullptr, nullptr, &style));
    }

    // Creates an element under `parent` styled by `box`, in `withFont` if it is given, and otherwise in the stage's
    // font.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutFontStyle* PR_NULLABLE withFont = nullptr) {
        return f.element(parent, box, nullptr, nullptr, withFont != nullptr ? withFont : &style);
    }
    PrLayoutNode text(PrLayoutNode parent, const char* value) {
        return f.text(parent, value);
    }

    // Lays the tree out, and checks the incremental work against a layout from scratch.
    void layout() {
        f.layout(px(400, 300));
        CHECK(verified(f.tree));
    }

    // Return a box's top relative to the stage's top, its height, and the line's baseline relative to the stage's top.
    double top(PrLayoutNode node) {
        return double(f.bounds(node).top - f.bounds(stage).top);
    }
    double height(PrLayoutNode node) {
        PrRect bounds = f.bounds(node);
        return double(bounds.bottom - bounds.top);
    }
    double baseline() {
        return top(marker);
    }
};

// Returns a square 10px on a side.
PrLayoutBoxStyle square() {
    return inlineBlock(10, 10);
}

// Returns `box` made to clip its content, which makes it a scroll container.
PrLayoutBoxStyle clipping(PrLayoutBoxStyle box) {
    box.overflowX = PrLayoutOverflow_Hidden;
    box.overflowY = PrLayoutOverflow_Hidden;
    return box;
}

PrLayoutBoxStyle sourced(PrLayoutBoxStyle box, PrLayoutBaselineSource source) {
    box.baselineSource = source;
    return box;
}

// Returns the metrics of one of the test fonts.
PrFontMetrics metricsOf(const char* file) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + file;
    const PrFont* PR_NULLABLE loaded = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &loaded) == PrStatus_Ok);
    PrFontMetrics metrics = prFontMetrics(loaded);
    prFontRelease(loaded);
    return metrics;
}

// Returns a baseline of Arimo below its alphabetic baseline at `size`, in pixels, as layout takes it: in units,
// unrounded.
double arimoBaseline(float PrFontMetrics::*name, float size) {
    return double(prLayoutUnitToPx(prLayoutUnitFromPx(metricsOf("Arimo.ttf").*name * size)));
}

// Adds the test font `file` to the fixture's set as family `family`, serving the code points of `ranges` if given.
void addFace(Fixture& f, const char* file, const char* family, const PrFontCodePointRange* PR_NULLABLE ranges, uint32_t count) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + file;
    const PrFont* PR_NULLABLE loaded = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &loaded) == PrStatus_Ok);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    descriptor.family = family;
    descriptor.automatic = PrFontFaceAutoFlags(descriptor.automatic & ~PrFontFaceAutoFlag_Family);
    descriptor.ranges = ranges;
    descriptor.rangeCount = count;
    prFontSetAdd(f.fonts, &descriptor, loaded);
    prFontRelease(loaded);
}

// Returns a font of Arimo for capitals and punctuation, and Literata, whose metrics differ, for every other letter, at
// `size` on lines `lineHeight` tall, or normal lines if `lineHeight` is negative, as the probe's fallback cases set
// text.
PrLayoutFontStyle upperAndFallback(Fixture& f, float size, float lineHeight) {
    static const PrFontCodePointRange kUpper[] = {{0x0000, 0x005f}};
    addFace(f, "Arimo.ttf", "Upper", kUpper, 1);
    addFace(f, "Literata.ttf", "Fallback", nullptr, 0);
    PrFontFamily families[] = {prFontFamilyFromName("Upper"), prFontFamilyFromName("Fallback")};
    PrLayoutFontStyle text = font(size, lineHeight);
    text.font = f.textFont(size, {families[0], families[1]});
    return text;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a line's strut sets its baseline, with the font's metrics in whole target pixels and the half-leading "
        "above floored to one") {
    Stage s;
    s.text(s.stage, "Xx");
    PrLayoutNode box = s.add(s.stage, square());
    s.layout();
    CHECK(s.baseline() == 27);
    CHECK(s.height(s.stage) == 40);
    CHECK(s.top(box) == 17);
}

TEST_CASE("sub and super shift a box by its parent's font size") {
    SUBCASE("sub") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, aligned(square(), keyword(PrLayoutKeyword_Sub)));
        s.layout();
        CHECK(s.top(box) == 22);
        CHECK(s.height(s.stage) == 40);
    }
    SUBCASE("super") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, aligned(square(), keyword(PrLayoutKeyword_Super)));
        s.layout();
        CHECK(s.top(box) == doctest::Approx(9.34).epsilon(0.001));
    }
    SUBCASE("in a 30px span") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle large = font(30, 40);
        PrLayoutNode span = s.add(s.stage, prLayoutBoxStyleDefault(), &large);
        s.text(span, "X");
        PrLayoutNode sub = s.add(span, aligned(square(), keyword(PrLayoutKeyword_Sub)), &large);
        PrLayoutNode super = s.add(span, aligned(square(), keyword(PrLayoutKeyword_Super)), &large);
        s.layout();
        CHECK(s.baseline() == 30);
        CHECK(s.top(sub) == 27);
        CHECK(s.top(super) == 9);
    }
    SUBCASE("a span of smaller text, by the parent's size and not its own") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle small = font(10, 40);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Sub)), &small);
        s.text(span, "x");
        s.layout();
        CHECK(s.top(span) == 23);
        CHECK(s.height(span) == 11);
    }
}

TEST_CASE("text-top, text-bottom and middle align with the parent's content area and x-height") {
    SUBCASE("squares") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode top = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_TextTop));
        PrLayoutNode bottom = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_TextBottom));
        PrLayoutNode middle = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_Middle));
        s.layout();
        CHECK(s.top(top) == 9);
        CHECK(s.top(bottom) == 21);
        CHECK(s.top(middle) == doctest::Approx(16.72).epsilon(0.001));
        CHECK(s.height(s.stage) == 40);
    }
    SUBCASE("middle on a box with margins, by its margin box") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutBoxStyle box = aligned(inlineBlock(40, 40), PrLayoutAlignmentBaseline_Middle);
        box.margin[PrRectSide_Top] = prLayoutLengthPx(5);
        box.margin[PrRectSide_Bottom] = prLayoutLengthPx(15);
        PrLayoutNode node = s.add(s.stage, box);
        s.layout();
        CHECK(s.baseline() == doctest::Approx(35.28).epsilon(0.001));
        CHECK(s.top(node) == 5);
        CHECK(s.height(s.stage) == 60);
    }
    SUBCASE("inline boxes, by their leading boxes") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle small = font(10, 30);
        PrLayoutBoxStyle topBox = aligned(prLayoutBoxStyleDefault(), PrLayoutAlignmentBaseline_TextTop);
        PrLayoutNode top = s.add(s.stage, topBox, &small);
        s.text(top, "x");
        PrLayoutNode bottom = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), PrLayoutAlignmentBaseline_TextBottom), &small);
        s.text(bottom, "x");
        s.layout();
        CHECK(s.top(top) == 18);
        CHECK(s.top(bottom) == 10);
        CHECK(s.height(s.stage) == 40);
    }
    SUBCASE("middle on an inline box of larger text") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle large = font(40, 40);
        PrLayoutBoxStyle spanBox = aligned(prLayoutBoxStyleDefault(), PrLayoutAlignmentBaseline_Middle);
        PrLayoutNode span = s.add(s.stage, spanBox, &large);
        s.text(span, "x");
        s.layout();
        CHECK(s.top(span) == doctest::Approx(-0.28).epsilon(0.01));
        CHECK(s.height(span) == 44);
        CHECK(s.height(s.stage) == doctest::Approx(41.72).epsilon(0.001));
    }
    SUBCASE("text-top in a 30px span, its content area's") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle large = font(30, 40);
        PrLayoutNode span = s.add(s.stage, prLayoutBoxStyleDefault(), &large);
        s.text(span, "X");
        PrLayoutNode box = s.add(span, aligned(square(), PrLayoutAlignmentBaseline_TextTop), &large);
        s.layout();
        CHECK(s.top(box) == 3);
    }
}

TEST_CASE("a length raises a box, and a percentage of its own line height") {
    SUBCASE("lengths") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode up = s.add(s.stage, aligned(square(), prLayoutBaselineShiftPx(20)));
        s.layout();
        CHECK(s.baseline() == 30);
        CHECK(s.top(up) == 0);
        CHECK(s.height(s.stage) == 43);
    }
    SUBCASE("a negative length") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode down = s.add(s.stage, aligned(square(), prLayoutBaselineShiftPx(-10)));
        s.layout();
        CHECK(s.top(down) == 27);
        CHECK(s.height(s.stage) == 40);
    }
    SUBCASE("percentages") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode half = s.add(s.stage, aligned(square(), prLayoutBaselineShiftPercent(50)));
        s.layout();
        CHECK(s.top(half) == 0);
        CHECK(s.height(s.stage) == 43);
    }
    SUBCASE("a percentage of a normal line height") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle normal = font(20, -1);
        PrLayoutNode half = s.add(s.stage, aligned(square(), prLayoutBaselineShiftPercent(50)), &normal);
        s.layout();
        CHECK(s.top(half) == 5.5);
    }
    SUBCASE("a percentage of an inline box") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle lines20 = font(20, 20);
        PrLayoutBoxStyle spanBox = aligned(prLayoutBoxStyleDefault(), prLayoutBaselineShiftPercent(-50));
        PrLayoutNode span = s.add(s.stage, spanBox, &lines20);
        s.text(span, "x");
        s.layout();
        CHECK(s.top(span) == 19);
        CHECK(s.height(s.stage) == 40);
    }
    SUBCASE("raises add down the tree") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), prLayoutBaselineShiftPx(10)));
        s.text(span, "x");
        PrLayoutNode box = s.add(span, aligned(square(), prLayoutBaselineShiftPx(10)));
        s.layout();
        CHECK(s.baseline() == 37);
        CHECK(s.top(box) == 7);
    }
    SUBCASE("a calculation") {
        Stage s;
        s.text(s.stage, "Xx ");
        const PrLayoutCalcNode nodes[] = {
            prLayoutCalcOperation(PrLayoutCalcOp_Sum, 2),
            prLayoutCalcLeaf(PrLayoutCalcLeaf_Percent, 50),
            prLayoutCalcLeaf(PrLayoutCalcLeaf_Px, 5),
        };
        PrLayoutLength calc = prLayoutLengthIntern(s.f.tree, nodes, 3);
        PrLayoutNode box = s.add(s.stage, aligned(square(), prLayoutBaselineShiftLength(calc)));
        s.layout();
        CHECK(s.baseline() == 35);
        CHECK(s.top(box) == 0);
    }
}

TEST_CASE("top and bottom align a box's aligned subtree with the line box") {
    // A box 60px tall on the baseline, which the line grows above the baseline to hold.
    auto tall = [](Stage& s) { s.add(s.stage, inlineBlock(10, 60)); };
    SUBCASE("top and bottom") {
        Stage s;
        s.text(s.stage, "Xx ");
        tall(s);
        PrLayoutNode top = s.add(s.stage, aligned(square(), keyword(PrLayoutKeyword_Top)));
        PrLayoutNode bottom = s.add(s.stage, aligned(square(), keyword(PrLayoutKeyword_Bottom)));
        s.layout();
        CHECK(s.baseline() == 60);
        CHECK(s.top(top) == 0);
        CHECK(s.top(bottom) == 63);
        CHECK(s.height(s.stage) == 73);
    }
    SUBCASE("a box at top taller than the rest grows the line below the baseline") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, aligned(inlineBlock(10, 100), keyword(PrLayoutKeyword_Top)));
        s.layout();
        CHECK(s.baseline() == 27);
        CHECK(s.top(box) == 0);
        CHECK(s.height(s.stage) == 100);
    }
    SUBCASE("a box at bottom taller than the rest grows it above") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, aligned(inlineBlock(10, 100), keyword(PrLayoutKeyword_Bottom)));
        s.layout();
        CHECK(s.baseline() == 87);
        CHECK(s.top(box) == 0);
        CHECK(s.height(s.stage) == 100);
    }
    SUBCASE("the subtrees grow the line in the order the line meets them") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode top = s.add(s.stage, aligned(inlineBlock(10, 100), keyword(PrLayoutKeyword_Top)));
        PrLayoutNode bottom = s.add(s.stage, aligned(inlineBlock(10, 80), keyword(PrLayoutKeyword_Bottom)));
        s.layout();
        CHECK(s.baseline() == 27);
        CHECK(s.top(top) == 0);
        CHECK(s.top(bottom) == 20);
        CHECK(s.height(s.stage) == 100);
    }
    SUBCASE("an inline box at top, by its leading box") {
        Stage s;
        s.text(s.stage, "Xx ");
        tall(s);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Top)));
        s.text(span, "top");
        s.layout();
        CHECK(s.top(span) == 9);
        CHECK(s.height(span) == 22);
    }
    SUBCASE("the subtree's top is its highest box's") {
        Stage s;
        s.text(s.stage, "Xx ");
        tall(s);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Top)));
        s.text(span, "top");
        PrLayoutNode kid = s.add(span, aligned(square(), prLayoutBaselineShiftPx(30)));
        s.layout();
        CHECK(s.top(span) == 22);
        CHECK(s.top(kid) == 0);
        CHECK(s.height(s.stage) == 73);
    }
    SUBCASE("a box at bottom inside one at top roots a subtree of its own") {
        Stage s;
        s.text(s.stage, "Xx ");
        tall(s);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Top)));
        s.text(span, "top");
        PrLayoutNode kid = s.add(span, aligned(square(), keyword(PrLayoutKeyword_Bottom)));
        s.layout();
        CHECK(s.top(span) == 9);
        CHECK(s.top(kid) == 63);
    }
    SUBCASE("an atomic inline at top, by its margin box") {
        Stage s;
        s.text(s.stage, "Xx ");
        tall(s);
        PrLayoutBoxStyle box = aligned(inlineBlock(10, 20), keyword(PrLayoutKeyword_Top));
        box.margin[PrRectSide_Top] = prLayoutLengthPx(10);
        box.margin[PrRectSide_Bottom] = prLayoutLengthPx(10);
        PrLayoutNode node = s.add(s.stage, box);
        s.layout();
        CHECK(s.top(node) == 10);
    }
    SUBCASE("an inline box at top on short lines") {
        Stage s;
        s.text(s.stage, "Xx ");
        tall(s);
        PrLayoutFontStyle short10 = font(20, 10);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Top)), &short10);
        s.text(span, "t");
        PrLayoutNode kid = s.add(span, square(), &short10);
        s.layout();
        CHECK(s.top(span) == -6);
        CHECK(s.top(kid) == 2);
    }
}

TEST_CASE("a line is as tall as its boxes' leading boxes, aligned, and its strut") {
    SUBCASE("a span on taller lines") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle tall = font(20, 60);
        PrLayoutNode span = s.add(s.stage, prLayoutBoxStyleDefault(), &tall);
        s.text(span, "span");
        s.layout();
        CHECK(s.baseline() == 37);
        CHECK(s.top(span) == 19);
        CHECK(s.height(s.stage) == 60);
    }
    SUBCASE("small text on short lines, within the strut") {
        Stage s;
        PrLayoutFontStyle small = font(10, 10);
        PrLayoutNode span = s.add(s.stage, prLayoutBoxStyleDefault(), &small);
        s.text(span, "small");
        s.layout();
        CHECK(s.baseline() == 27);
        CHECK(s.top(span) == 18);
        CHECK(s.height(s.stage) == 40);
    }
    SUBCASE("a raised span") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), prLayoutBaselineShiftPx(20)));
        s.text(span, "up");
        s.layout();
        CHECK(s.baseline() == 47);
        CHECK(s.top(span) == 9);
        CHECK(s.height(s.stage) == 60);
    }
    SUBCASE("an empty span") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle tall = font(20, 60);
        s.add(s.stage, prLayoutBoxStyleDefault(), &tall);
        s.layout();
        CHECK(s.baseline() == 37);
        CHECK(s.height(s.stage) == 60);
    }
    SUBCASE("a box at top across two lines aligns on each") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle tall = font(20, 60);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Top)), &tall);
        s.text(span, "aaaa bbbb cccc dddd eeee ffff ");
        PrLayoutNode kid = s.add(span, square(), &tall);
        s.layout();
        CHECK(s.top(span) == 19);
        CHECK(s.top(kid) == 87);
        CHECK(s.height(s.stage) == 120);
    }
}

TEST_CASE("a change of vertical-align moves the box and lays out nothing inside it") {
    Stage s;
    s.text(s.stage, "Xx ");
    s.add(s.stage, inlineBlock(10, 60));
    PrLayoutNode outer = s.add(s.stage, inlineBlock(30, 30));
    PrLayoutNode inner = s.add(outer, block(10));
    s.layout();
    CHECK(s.top(outer) == 30);
    CHECK(s.top(inner) == 30);
    PrLayoutFontStyle text = font(20, 40);
    prLayoutNodeSetStyle(s.f.tree, outer,
        s.f.style(aligned(inlineBlock(30, 30), keyword(PrLayoutKeyword_Bottom)), nullptr, nullptr, &text));
    s.layout();
    CHECK(s.top(outer) == 43);
    CHECK(s.top(inner) == 43);
    CHECK(prLayoutTreeStats(s.f.tree).blocksReused >= 1);
}

TEST_CASE("alignment-baseline aligns a box's baseline of each name with its parent's of the same name") {
    // Arimo has no ideographic baselines. They are at its ascent and descent, 18 and 4 at 20px, and central is midway.
    double central = -7;
    double ideographic = 4;
    double hanging = arimoBaseline(&PrFontMetrics::hangingBaseline, 20);
    double math = arimoBaseline(&PrFontMetrics::mathBaseline, 20);
    SUBCASE("squares, whose baselines are synthesized from their margin boxes") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode alphabetic = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_Alphabetic));
        PrLayoutNode middle = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_Middle));
        PrLayoutNode c = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_Central));
        PrLayoutNode i = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_Ideographic));
        PrLayoutNode h = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_Hanging));
        PrLayoutNode m = s.add(s.stage, aligned(square(), PrLayoutAlignmentBaseline_Mathematical));
        s.layout();
        CHECK(s.baseline() == 27);
        CHECK(s.top(alphabetic) == 17);
        CHECK(s.top(middle) == doctest::Approx(16.72).epsilon(0.001));
        CHECK(s.top(c) + 5 == doctest::Approx(27 + central));
        CHECK(s.top(i) + 10 == doctest::Approx(27 + ideographic));
        CHECK(s.top(h) == doctest::Approx(27 + hanging));
        CHECK(s.top(m) + 5 == doctest::Approx(27 + math));
    }
    SUBCASE("an inline box, by its font's baselines") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle large = font(40, 40);
        PrLayoutBoxStyle spanBox = aligned(prLayoutBoxStyleDefault(), PrLayoutAlignmentBaseline_Central);
        PrLayoutNode span = s.add(s.stage, spanBox, &large);
        s.text(span, "x");
        s.layout();
        // Its content area's top is its ascent, 36, above its baseline.
        double central40 = -14;
        CHECK(s.top(span) + 36 + central40 == doctest::Approx(s.baseline() + central));
    }
    SUBCASE("an inline-block holding text, by its line's font's baselines") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutFontStyle large = font(40, 60);
        PrLayoutNode box = s.add(s.stage, aligned(inlineBlock(-1, -1), PrLayoutAlignmentBaseline_Central), &large);
        s.text(box, "x");
        s.layout();
        // Its line's baseline is 36 + 8 below its top.
        double central40 = -14;
        CHECK(s.top(box) + 44 + central40 == doctest::Approx(s.baseline() + central));
    }
    SUBCASE("aligned, then shifted") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, aligned(aligned(square(), PrLayoutAlignmentBaseline_TextTop),
            prLayoutBaselineShiftPx(10)));
        s.layout();
        CHECK(s.baseline() == 28);
        CHECK(s.top(box) == 0);
    }
}

TEST_CASE("center aligns a subtree's center with the line box's, growing the line above where it is taller") {
    SUBCASE("beside a taller box") {
        Stage s;
        s.text(s.stage, "Xx ");
        s.add(s.stage, inlineBlock(10, 60));
        PrLayoutNode box = s.add(s.stage, aligned(square(), keyword(PrLayoutKeyword_Center)));
        s.layout();
        CHECK(s.height(s.stage) == 73);
        CHECK(s.top(box) == 31.5);
    }
    SUBCASE("taller than the rest") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, aligned(inlineBlock(10, 100), keyword(PrLayoutKeyword_Center)));
        s.layout();
        CHECK(s.baseline() == 87);
        CHECK(s.top(box) == 0);
        CHECK(s.height(s.stage) == 100);
    }
    SUBCASE("after a taller box at top") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode top = s.add(s.stage, aligned(inlineBlock(10, 100), keyword(PrLayoutKeyword_Top)));
        PrLayoutNode center = s.add(s.stage, aligned(inlineBlock(10, 80), keyword(PrLayoutKeyword_Center)));
        s.layout();
        CHECK(s.baseline() == 27);
        CHECK(s.top(top) == 0);
        CHECK(s.top(center) == 10);
    }
}

TEST_CASE("baseline-source picks an inline-block's first or last baseline") {
    // An inline-block of two lines, each in a block child: its baselines are 27 and 67 below its top.
    auto twoLines = [](Stage& s, const PrLayoutBoxStyle& box) {
        PrLayoutNode node = s.add(s.stage, box);
        s.text(s.add(node, block()), "one");
        s.text(s.add(node, block()), "two");
        return node;
    };
    PrLayoutBoxStyle lines = inlineBlock(-1, -1);
    SUBCASE("auto and last: its last") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode a = twoLines(s, lines);
        PrLayoutNode b = twoLines(s, sourced(lines, PrLayoutBaselineSource_Last));
        s.layout();
        CHECK(s.baseline() == 67);
        CHECK(s.top(a) == 0);
        CHECK(s.top(b) == 0);
    }
    SUBCASE("first") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode a = twoLines(s, sourced(lines, PrLayoutBaselineSource_First));
        s.layout();
        CHECK(s.baseline() == 27);
        CHECK(s.top(a) == 0);
    }
    SUBCASE("first, past an empty block") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode node = s.add(s.stage, sourced(lines, PrLayoutBaselineSource_First));
        s.add(node, block(10));
        s.text(s.add(node, block()), "one");
        s.layout();
        CHECK(s.baseline() == 37);
    }
    SUBCASE("first, where there is no line: the margin box's bottom") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode node = s.add(s.stage, sourced(inlineBlock(10, 30), PrLayoutBaselineSource_First));
        s.layout();
        CHECK(s.baseline() == 30);
        CHECK(s.top(node) == 0);
    }
    SUBCASE("a scroll container: its margin box's bottom under auto, and its lines under first or last") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode a = twoLines(s, clipping(lines));
        s.layout();
        CHECK(s.baseline() == 80);
        Stage first;
        first.text(first.stage, "Xx ");
        twoLines(first, clipping(sourced(lines, PrLayoutBaselineSource_First)));
        first.layout();
        CHECK(first.baseline() == 27);
        Stage last;
        last.text(last.stage, "Xx ");
        twoLines(last, clipping(sourced(lines, PrLayoutBaselineSource_Last)));
        last.layout();
        CHECK(last.baseline() == 67);
        CHECK(s.top(a) == 0);
    }
    SUBCASE("a scroll container's baselines are clamped to its border box") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutBoxStyle box = clipping(sourced(lines, PrLayoutBaselineSource_Last));
        box.height = prLayoutLengthPx(30);
        twoLines(s, box);
        s.layout();
        CHECK(s.baseline() == 30);
    }
}

TEST_CASE("a block-level scroll container passes up its margin box's bottom as its last baseline") {
    auto nested = [](Stage& s, PrLayoutBaselineSource outer, PrLayoutBaselineSource inner) {
        PrLayoutNode box = s.add(s.stage, sourced(inlineBlock(-1, -1), outer));
        PrLayoutNode scroller = s.add(box, sourced(clipping(block()), inner));
        s.text(s.add(scroller, block()), "one");
        s.text(s.add(scroller, block()), "two");
        return box;
    };
    Stage s;
    s.text(s.stage, "Xx ");
    nested(s, PrLayoutBaselineSource_Auto, PrLayoutBaselineSource_Auto);
    s.layout();
    CHECK(s.baseline() == 80);
    // Its own baseline-source applies only to inline-level boxes.
    Stage last;
    last.text(last.stage, "Xx ");
    nested(last, PrLayoutBaselineSource_Auto, PrLayoutBaselineSource_Last);
    last.layout();
    CHECK(last.baseline() == 80);
    // Its first baseline is its first line's.
    Stage first;
    first.text(first.stage, "Xx ");
    nested(first, PrLayoutBaselineSource_First, PrLayoutBaselineSource_Auto);
    first.layout();
    CHECK(first.baseline() == 27);
}

TEST_CASE("a block-level scroll container's margin edge is a last baseline even where it has no line") {
    // An inline-block of one line, then an empty scroll container 30px tall, with `margin` below the scroll container
    // and `padding` below the inline-block's content.
    auto trailing = [](Stage& s, float margin, float padding) {
        PrLayoutBoxStyle outer = inlineBlock(-1, -1);
        outer.padding[PrRectSide_Bottom] = prLayoutLengthPx(padding);
        PrLayoutNode box = s.add(s.stage, outer);
        s.text(s.add(box, block()), "one");
        PrLayoutBoxStyle scroller = clipping(block(30));
        scroller.margin[PrRectSide_Bottom] = prLayoutLengthPx(margin);
        s.add(box, scroller);
        return box;
    };
    SUBCASE("after a line") {
        Stage s;
        s.text(s.stage, "Xx ");
        trailing(s, 0, 0);
        s.layout();
        CHECK(s.baseline() == 70);
        // Laid out again, the scroll container's result is reused and gives the same result.
        s.f.layout(px(401, 300));
        CHECK(verified(s.f.tree));
        CHECK(s.baseline() == 70);
    }
    SUBCASE("by its own margin, not its parent's padding") {
        Stage s;
        s.text(s.stage, "Xx ");
        trailing(s, 5, 10);
        s.layout();
        CHECK(s.baseline() == 75);
    }
    SUBCASE("with no line at all") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutBoxStyle outer = inlineBlock(20, -1);
        outer.padding[PrRectSide_Bottom] = prLayoutLengthPx(10);
        PrLayoutNode box = s.add(s.stage, outer);
        PrLayoutBoxStyle scroller = clipping(block(30));
        scroller.margin[PrRectSide_Bottom] = prLayoutLengthPx(5);
        s.add(box, scroller);
        s.layout();
        CHECK(s.baseline() == 35);
        CHECK(s.top(box) == 0);
    }
}

TEST_CASE("a tree frees what it keeps of the fonts it matched") {
    auto cycle = [] {
        Stage s;
        s.text(s.stage, "Xx");
        s.layout();
    };
    cycle();
    size_t live = prMemoryLiveCount();
    cycle();
    CHECK(prMemoryLiveCount() == live);
}

TEST_CASE("a clamped set's named baselines are each clamped, through nested scroll containers") {
    SUBCASE("the alphabetic baseline, clamped by a nested scroll container") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, sourced(inlineBlock(-1, -1), PrLayoutBaselineSource_First));
        PrLayoutBoxStyle inner = clipping(block(10));
        s.text(s.add(box, inner), "one");
        s.layout();
        CHECK(s.top(box) == 17);
    }
    SUBCASE("two nested ranges compose") {
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, sourced(inlineBlock(-1, -1), PrLayoutBaselineSource_First));
        PrLayoutNode outer = s.add(box, clipping(block(30)));
        PrLayoutBoxStyle innerBox = clipping(block(10));
        innerBox.margin[PrRectSide_Top] = prLayoutLengthPx(15);
        s.text(s.add(outer, innerBox), "one");
        s.layout();
        CHECK(s.top(box) == 2);
    }
    SUBCASE("a named baseline outside the border box where the alphabetic one is inside") {
        // On 4px lines, its first baseline is 18 - 9 below its top, inside its 30px border box, and its hanging
        // baseline is about 12 above that, outside the box, so it clamps to the top.
        double hanging = arimoBaseline(&PrFontMetrics::hangingBaseline, 20);
        REQUIRE(9 + hanging < 0);
        Stage s;
        s.text(s.stage, "Xx ");
        PrLayoutNode box = s.add(s.stage, aligned(sourced(inlineBlock(-1, -1), PrLayoutBaselineSource_First),
            PrLayoutAlignmentBaseline_Hanging));
        PrLayoutFontStyle short4 = font(20, 4);
        s.text(s.add(box, clipping(block(30)), &short4), "one");
        s.layout();
        CHECK(s.top(box) == doctest::Approx(s.baseline() + hanging));
        // Laid out again from its cached result, it keeps the clamp.
        s.f.layout(px(401, 300));
        CHECK(verified(s.f.tree));
        CHECK(s.top(box) == doctest::Approx(s.baseline() + hanging));
    }
}

TEST_CASE("a box whose writing mode is orthogonal to its line's has no baseline for it") {
    Stage s;
    s.text(s.stage, "Xx ");
    PrLayoutFontStyle vertical = font(20, 40);
    vertical.writingMode = PrLayoutWritingMode_VerticalRl;
    PrLayoutNode box = s.add(s.stage, inlineBlock(-1, -1), &vertical);
    s.text(box, "ab");
    PrLayoutNode holder = s.add(s.stage, inlineBlock(-1, -1));
    s.text(s.add(holder, block(), &vertical), "ab");
    s.layout();
    CHECK(s.top(box) + s.height(box) == s.baseline());
    CHECK(s.top(holder) + s.height(holder) == s.baseline());
}

TEST_CASE("a line of normal height is as tall as the faces its text uses on it") {
    auto normal = [](Fixture& f) { return upperAndFallback(f, 20, -1); };
    SUBCASE("capitals alone, and with a word of the fallback face") {
        Stage a(normal);
        a.text(a.stage, "AB");
        a.layout();
        CHECK(a.baseline() == 18);
        CHECK(a.height(a.stage) == 23);
        Stage b(normal);
        b.text(b.stage, "AB ab");
        b.layout();
        CHECK(b.baseline() == 24);
        CHECK(b.height(b.stage) == 30);
    }
    SUBCASE("each line by the faces on it") {
        Stage s(normal, 30);
        s.text(s.stage, "AB ab");
        s.layout();
        CHECK(s.baseline() == 18);
        CHECK(s.height(s.stage) == 53);
    }
    SUBCASE("an explicit line height takes the first face's alone") {
        Stage s([](Fixture& f) { return upperAndFallback(f, 20, 40); });
        s.text(s.stage, "AB ab");
        s.layout();
        CHECK(s.baseline() == 27);
        CHECK(s.height(s.stage) == 40);
    }
    SUBCASE("text-top takes the parent's first face") {
        Stage s(normal);
        s.text(s.stage, "AB ");
        PrLayoutNode span = s.add(s.stage, prLayoutBoxStyleDefault());
        s.text(span, "ab");
        PrLayoutNode box = s.add(span, aligned(square(), PrLayoutAlignmentBaseline_TextTop));
        s.layout();
        CHECK(s.top(box) == 6);
    }
}

TEST_CASE("a box aligned by its leading box takes the faces its text uses on the line, and its descendants move with it") {
    auto normal = [](Fixture& f) { return upperAndFallback(f, 20, -1); };
    auto large = [](Stage& s) {
        PrLayoutFontStyle text = s.style;
        text.font = s.f.textFont(40, {prFontFamilyFromName("Upper"), prFontFamilyFromName("Fallback")});
        return text;
    };
    SUBCASE("middle on a span of fallback text, holding a square") {
        Stage s(normal);
        s.text(s.stage, "AB ");
        PrLayoutFontStyle big = large(s);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), PrLayoutAlignmentBaseline_Middle), &big);
        PrLayoutNode box = s.add(span, square(), &big);
        s.text(span, "ab");
        s.layout();
        CHECK(s.baseline() == doctest::Approx(34.78).epsilon(0.001));
        CHECK(s.top(span) == doctest::Approx(11).epsilon(0.001));
        CHECK(s.top(box) == doctest::Approx(37).epsilon(0.001));
        CHECK(s.height(s.stage) == doctest::Approx(59).epsilon(0.001));
    }
    SUBCASE("the same with capitals only") {
        Stage s(normal);
        s.text(s.stage, "AB ");
        PrLayoutFontStyle big = large(s);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), PrLayoutAlignmentBaseline_Middle), &big);
        PrLayoutNode box = s.add(span, square(), &big);
        s.text(span, "AB");
        s.layout();
        CHECK(s.baseline() == doctest::Approx(27.78).epsilon(0.001));
        CHECK(s.top(box) == doctest::Approx(26).epsilon(0.001));
    }
    SUBCASE("through a nested raise, and not into a subtree of its own") {
        Stage s(normal);
        s.text(s.stage, "AB ");
        s.add(s.stage, inlineBlock(10, 60));
        PrLayoutFontStyle big = large(s);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), PrLayoutAlignmentBaseline_Middle), &big);
        PrLayoutNode raised = s.add(span, aligned(prLayoutBoxStyleDefault(), prLayoutBaselineShiftPx(10)), &big);
        PrLayoutNode inRaised = s.add(raised, square(), &big);
        s.text(raised, "X");
        PrLayoutNode atTop = s.add(span, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Top)), &big);
        PrLayoutNode inTop = s.add(atTop, square(), &big);
        s.text(atTop, "X");
        s.text(span, "ab");
        s.layout();
        // The raised square's bottom is the span's baseline less 10, a baseline that the fallback face moves.
        CHECK(s.top(inRaised) + 10 == doctest::Approx(s.top(span) + 36 - 10));
        // The square at top sits on its own subtree's baseline, at the line's top plus the part of its leading box
        // above the baseline.
        CHECK(s.top(inTop) + 10 == doctest::Approx(s.top(atTop) + 36));
        CHECK(s.top(atTop) + 36 == doctest::Approx(36));
    }
    SUBCASE("a wrapping span, by the faces on each line") {
        Stage s(normal, 120);
        s.text(s.stage, "A ");
        PrLayoutFontStyle big = large(s);
        PrLayoutNode span = s.add(s.stage, aligned(prLayoutBoxStyleDefault(), PrLayoutAlignmentBaseline_Middle), &big);
        PrLayoutNode first = s.add(span, square(), &big);
        s.text(span, "ABC ");
        s.add(span, square(), &big);
        s.text(span, "abc");
        s.layout();
        CHECK(s.top(first) == doctest::Approx(26).epsilon(0.001));
    }
}

TEST_CASE("font metrics and the half-leading snap to target pixels, half pixels at two per px") {
    // Half pixels at two target pixels per px, and whole pixels at one.
    for (float ratio : {1.0f, 2.0f}) {
        CAPTURE(ratio);
        Stage s;
        prLayoutTreeSetTargetPixelRatio(s.f.tree, ratio);
        PrLayoutFontStyle small = font(16, -1);
        PrLayoutNode span = s.add(s.stage, prLayoutBoxStyleDefault(), &small);
        s.text(span, "Xx");
        PrLayoutFontStyle leading = font(20, 25);
        PrLayoutBoxStyle box = block();
        PrLayoutNode lines = s.add(s.stage, box, &leading);
        PrLayoutNode inner = s.add(lines, prLayoutBoxStyleDefault(), &leading);
        s.text(inner, "Xx");
        s.layout();
        CHECK(s.top(span) == doctest::Approx(ratio == 1 ? 13.0 : 12.5));
        CHECK(s.height(span) == doctest::Approx(ratio == 1 ? 17.0 : 18.0));
        CHECK(s.top(inner) - s.top(lines) == doctest::Approx(ratio == 1 ? 1.0 : 1.5));
    }
}

TEST_CASE("a change of a tree's target pixel ratio lays it out again, once, and no other tree") {
    Fixture f;
    Fixture g;
    // Nested blocks and an inline-block sharing a context, in each tree.
    auto build = [&](Fixture& fixture) {
        PrLayoutTree* tree = fixture.tree;
        PrLayoutFontStyle text = prLayoutFontStyleDefault();
        text.font = fixture.textFont(16);
        auto element = [&](PrLayoutNode parent, const PrLayoutBoxStyle& box) {
            PrLayoutNode node = prLayoutNodeCreateElement(tree, parent, fixture.style(box, nullptr, nullptr, &text));
            if (parent == kPrLayoutNodeNone)
                fixture.root = node;
            return node;
        };
        PrLayoutNode root = element(kPrLayoutNodeNone, block());
        PrLayoutNode outer = element(root, block());
        PrLayoutNode inner = element(outer, block());
        prLayoutNodeCreateText(tree, inner, "Xx ");
        PrLayoutNode atomic = element(inner, inlineBlock(-1, -1));
        prLayoutNodeCreateText(tree, atomic, "Xx");
        return inner;
    };
    PrLayoutNode inners[2] = {build(f), build(g)};
    Fixture* fixtures[2] = {&f, &g};
    PrLayoutTree* trees[2] = {f.tree, g.tree};
    for (Fixture* fixture : fixtures)
        fixture->layout(px(400, 300));
    for (float ratio : {1.0f, 2.0f, 2.0f, 1.0f, 1.0f}) {
        CAPTURE(ratio);
        bool changed = ratio != prLayoutTreeTargetPixelRatio(f.tree);
        prLayoutTreeSetTargetPixelRatio(f.tree, ratio);
        for (uint32_t i = 0; i < 2; ++i) {
            CAPTURE(i);
            fixtures[i]->layout(px(400, 300));
            CHECK(verified(trees[i]));
            PrLayoutTreeStats stats = prLayoutTreeStats(trees[i]);
            // A new ratio reuses no block of its tree, and shapes no text. The same ratio again lays out nothing.
            CHECK(stats.inlinesShaped == 0);
            CHECK((stats.blocksLaidOut != 0) == (i == 0 && changed));
            PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(trees[i], inners[i], PrLayoutCoordinates_Layout));
            // 16px Arimo's line: an ascent, descent and line gap of 14, 3 and 1 in whole pixels, and of 14.5, 3.5 and
            // 0.5 in half pixels.
            CHECK(double(bounds.bottom - bounds.top) == doctest::Approx(i == 0 && ratio != 1 ? 18.5 : 18.0));
        }
    }
}

TEST_CASE("a phantom line is zero high: no text, atomic inline, forced break or inline-axis edge holds it") {
    struct Case {
        const char* what;
        PrLayoutBoxStyle span;
        double height;
    };
    auto edges = [](PrLayoutLength margin, PrLayoutLength padding, PrRectSide side) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.margin[side] = margin;
        box.padding[side] = padding;
        return box;
    };
    // Each edge on its own, including a non-zero percentage of a zero base.
    for (Case c : {Case{"padding-top", edges(prLayoutLengthPx(0), prLayoutLengthPx(1), PrRectSide_Top), 0},
        Case{"padding-left", edges(prLayoutLengthPx(0), prLayoutLengthPx(1), PrRectSide_Left), 40},
                Case{"padding-right", edges(prLayoutLengthPx(0), prLayoutLengthPx(1), PrRectSide_Right), 40},
                Case{"edges that cancel", edges(prLayoutLengthPx(-1), prLayoutLengthPx(1), PrRectSide_Left), 40},
                Case{"a percentage", edges(prLayoutLengthPx(0), prLayoutLengthPercent(10), PrRectSide_Left), 40}}) {
        CAPTURE(c.what);
        Stage s(Width{200, false});
        s.add(s.stage, c.span);
        s.layout();
        CHECK(s.height(s.stage) == doctest::Approx(c.height));
    }
}

TEST_CASE("an inline box at top alone on a phantom line takes no baseline from the line") {
    // The second block's line is phantom, so it is not aligned, though its box roots an aligned subtree. Nothing that
    // the line before it aligned, in the first block, places the box.
    Stage s;
    PrLayoutNode first = s.add(s.stage, block(-1));
    s.text(first, "a ");
    PrLayoutNode filled = s.add(first, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Top)));
    s.text(filled, "b");
    PrLayoutNode second = s.add(s.stage, block(-1));
    PrLayoutNode empty = s.add(second, aligned(prLayoutBoxStyleDefault(), keyword(PrLayoutKeyword_Top)));
    s.layout();
    CHECK(s.height(second) == doctest::Approx(0.0));
    CHECK(s.height(empty) == doctest::Approx(0.0));
}

TEST_CASE("the items after a forced break that ends the text are on a line of their own") {
    {
        // An edge makes it a line. With no edge, it is phantom.
        Stage s;
        s.text(s.stage, "a");
        s.lineBreak(s.stage);
        PrLayoutBoxStyle padded = prLayoutBoxStyleDefault();
        padded.padding[PrRectSide_Left] = prLayoutLengthPx(5);
        s.add(s.stage, padded);
        s.layout();
        CHECK(s.height(s.stage) == doctest::Approx(80.0));
        Stage empty;
        empty.text(empty.stage, "a");
        empty.lineBreak(empty.stage);
        empty.add(empty.stage, prLayoutBoxStyleDefault());
        empty.layout();
        CHECK(empty.height(empty.stage) == doctest::Approx(40.0));
    }
    {
        // A float there is placed at that line, below the break.
        Stage s;
        s.text(s.stage, "a");
        s.lineBreak(s.stage);
        PrLayoutBoxStyle side = block(10);
        side.width = prLayoutLengthPx(10);
        side.floating = PrLayoutFloat_Left;
        PrLayoutNode floated = s.add(s.stage, side);
        s.layout();
        CHECK(s.top(floated) == doctest::Approx(40.0));
    }
}

TEST_CASE("a line break's own font and alignment take no part in its line, but its clear does") {
    {
        // A break of 25px text in 5px text, and one alone.
        Stage s([](Fixture&) {
            PrLayoutFontStyle tiny = font(5, -1);
            tiny.lineHeight = prLayoutLineHeightNumber(1);
            return tiny;
        });
        PrLayoutFontStyle big = s.style;
        big.font = s.f.textFont(25);
        s.text(s.stage, "XXX");
        prLayoutNodeCreateLineBreak(s.f.tree, s.stage, s.f.style(prLayoutBoxStyleDefault(), nullptr, nullptr, &big));
        s.text(s.stage, "XX");
        s.layout();
        CHECK(s.height(s.stage) == doctest::Approx(10.0));
    }
    {
        // What follows a clearing break goes below the float, or to the block's end if nothing follows. A float before
        // the break that grows moves it.
        Stage s;
        PrLayoutBoxStyle side = block(60);
        side.width = prLayoutLengthPx(10);
        side.floating = PrLayoutFloat_Left;
        PrLayoutNode floated = s.add(s.stage, side);
        s.text(s.stage, "a");
        PrLayoutBoxStyle clears = prLayoutBoxStyleDefault();
        clears.clear = PrLayoutClear_Left;
        s.lineBreak(s.stage, clears);
        s.layout();
        CHECK(s.height(s.stage) == doctest::Approx(60.0));
        s.text(s.stage, "b");
        s.layout();
        CHECK(s.height(s.stage) == doctest::Approx(100.0));
        side.height = prLayoutLengthPx(90);
        prLayoutNodeSetStyle(s.f.tree, floated, s.f.style(side, nullptr, nullptr, &s.style));
        s.layout();
        CHECK(s.height(s.stage) == doctest::Approx(130.0));
    }
}

TEST_CASE("lines may break before and after an atomic inline, but beside GL, WJ and ZWJ but no-break space") {
    struct Case {
        const char* between;
        double top;
    };
    // Two 80px inline-blocks in 100px, with no marker, whose own opportunity would break first. Each case gives the
    // second's top.
    for (Case c : {Case{" ", 57}, Case{"⁠", 17}, Case{" ", 17}, Case{"‍", 17}, Case{"", 57},
        Case{",", 57}}) {
        CAPTURE(c.between);
        Stage s(Width{100, false});
        s.add(s.stage, inlineBlock(80, 10));
        if (c.between[0] != 0)
            s.text(s.stage, c.between);
        PrLayoutNode second = s.add(s.stage, inlineBlock(80, 10));
        s.layout();
        CHECK(s.top(second) == doctest::Approx(c.top));
    }
    {
        // Not inside a box that does not wrap, and not before the space after one, which ends its line.
        Stage s(Width{100, false});
        PrLayoutFontStyle nowrap = s.style;
        nowrap.textWrapMode = PrLayoutTextWrapMode_Nowrap;
        PrLayoutNode span = s.add(s.stage, prLayoutBoxStyleDefault(), &nowrap);
        s.add(span, inlineBlock(80, 10));
        PrLayoutNode inside = s.add(span, inlineBlock(80, 10));
        s.layout();
        CHECK(s.top(inside) == doctest::Approx(17.0));
        Stage spaced(Width{100, false});
        spaced.add(spaced.stage, inlineBlock(80, 10));
        spaced.text(spaced.stage, " ");
        PrLayoutNode next = spaced.add(spaced.stage, inlineBlock(30, 10));
        spaced.layout();
        CHECK(spaced.top(next) == doctest::Approx(57.0));
        CHECK(spaced.f.bounds(next).left == doctest::Approx(spaced.f.bounds(spaced.stage).left));
    }
    {
        // Not before a forced break, whether a line break node or a preserved newline, after a box that overflows its
        // line. The text after the break is on the second line, as after a box that fits.
        for (bool preserved : {false, true}) {
            CAPTURE(preserved);
            double tops[2] = {};
            for (float width : {80.0f, 120.0f}) {
                Stage s(Width{100, false});
                PrLayoutFontStyle pre = s.style;
                pre.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
                s.add(s.stage, inlineBlock(width, 10));
                if (preserved)
                    s.text(s.add(s.stage, prLayoutBoxStyleDefault(), &pre), "\n");
                else
                    s.lineBreak(s.stage);
                PrLayoutNode after = s.add(s.stage, inlineBlock(10, 10));
                s.layout();
                tops[width > 100] = s.top(after);
            }
            CHECK(tops[1] == doctest::Approx(tops[0]));
        }
    }
}
