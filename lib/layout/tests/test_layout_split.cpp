#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A block-level box inside an inline box (CSS2/#anonymous-block-level): the inline box broken around it, the content on
// either side in anonymous block boxes, and the inline box's row around it. Each case is a stage 200px wide, a
// flow-root of 20px Arimo on 40px lines, and checks the engine's numbers for that stage.

// Returns a font for 20px text on lines `lineHeight` tall.
PrLayoutFontStyle font(float lineHeight = 40) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(20);
    font.lineHeight = prLayoutLineHeightPx(lineHeight);
    return font;
}

// Returns an inline-block `width` by `height`.
PrLayoutBoxStyle inlineBlock(float width, float height) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_InlineBlock;
    box.width = prLayoutLengthPx(width);
    box.height = prLayoutLengthPx(height);
    return box;
}

// Returns a block box `width` by `height`, each auto if negative.
PrLayoutBoxStyle sized(PrLayoutBoxStyle box, float width, float height) {
    if (width >= 0)
        box.width = prLayoutLengthPx(width);
    if (height >= 0)
        box.height = prLayoutLengthPx(height);
    return box;
}

// Return a box with block-start and block-end margins, or one with inline-axis padding at its start and end.
PrLayoutBoxStyle margined(PrLayoutBoxStyle box, float top, float bottom) {
    setMargins(box, top, bottom);
    return box;
}
PrLayoutBoxStyle padded(PrLayoutBoxStyle box, float left, float right) {
    box.padding[PrRectSide_Left] = prLayoutLengthPx(left);
    box.padding[PrRectSide_Right] = prLayoutLengthPx(right);
    return box;
}

// Return a box with a border of `width` on every side, or one that floats left.
PrLayoutBoxStyle bordered(PrLayoutBoxStyle box, float width) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.border[side] = prLayoutUnitFromPx(width);
    return box;
}
PrLayoutBoxStyle floatLeft(float width, float height) {
    PrLayoutBoxStyle box = sized(block(), width, height);
    box.floating = PrLayoutFloat_Left;
    return box;
}

// Return a box that establishes a block formatting context, or one that clips and scrolls.
PrLayoutBoxStyle flowRoot(PrLayoutBoxStyle box) {
    box.display = PrLayoutDisplay_FlowRoot;
    return box;
}
PrLayoutBoxStyle scrolling(PrLayoutBoxStyle box) {
    box.overflowX = PrLayoutOverflow_Scroll;
    box.overflowY = PrLayoutOverflow_Scroll;
    return box;
}

// A rectangle: a box's left and top, and its width and height.
struct Bounds {
    double left, top, width, height;
};

// A tree whose root holds a stage: a flow-root `width` wide in the stage's font.
struct Stage {
    Fixture f;
    PrLayoutFontStyle style = font();
    PrLayoutNode root = kPrLayoutNodeNone;
    PrLayoutNode stage = kPrLayoutNodeNone;

    explicit Stage(float width = 200) { build(width); }

    // Creates a stage in a font that `edit` changes, `width` wide and `height` high if that is not negative.
    template <typename Edit>
    requires requires(Edit e, PrLayoutFontStyle& value) { e(value); }
    Stage(Edit edit, float width = 200, float height = -1) {
        edit(style);
        build(width, height);
    }

    void build(float width, float height = -1) {
        root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
        stage = f.element(root, sized(flowRoot(block()), width, height), nullptr, nullptr, &style);
    }

    // Creates an element under `parent` styled by `box`, in the stage's font or `withFont`.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutFontStyle* PR_NULLABLE withFont = nullptr,
            const PrLayoutDecorationStyle* PR_NULLABLE decoration = nullptr) {
        return f.element(parent, box, decoration, nullptr, withFont != nullptr ? withFont : &style);
    }
    PrLayoutNode text(PrLayoutNode parent, const char* value) { return f.text(parent, value); }
    PrLayoutNode lineBreak(PrLayoutNode parent) {
        uint32_t record = f.style(prLayoutBoxStyleDefault(), nullptr, nullptr, &style);
        PrLayoutNode node = prLayoutNodeCreateLineBreak(f.tree, parent, record);
        return node;
    }

    // Create an inline box, a block box holding `value` if it is given, or a zero-size marker on the baseline.
    PrLayoutNode span(PrLayoutNode parent, const PrLayoutBoxStyle& box = inlineBox()) { return add(parent, box); }
    PrLayoutNode div(PrLayoutNode parent, const PrLayoutBoxStyle& box = block(), const char* PR_NULLABLE value = nullptr) {
        PrLayoutNode node = add(parent, box);
        if (value != nullptr)
            text(node, value);
        return node;
    }
    PrLayoutNode marker(PrLayoutNode parent) { return add(parent, inlineBlock(0, 0)); }

    // Lays the tree out, and checks the incremental work against a layout from scratch.
    void layout(float width = 400) {
        f.layout(px(width, 300));
        CHECK(verified(f.tree));
    }

    // Return a box's edges and size, relative to the stage's content box.
    PrRect stageBounds() { return f.bounds(stage); }
    double top(PrLayoutNode node) { return double(f.bounds(node).top - stageBounds().top); }
    double left(PrLayoutNode node) { return double(f.bounds(node).left - stageBounds().left); }
    double width(PrLayoutNode node) { return double(f.bounds(node).right - f.bounds(node).left); }
    double height(PrLayoutNode node) { return double(f.bounds(node).bottom - f.bounds(node).top); }

    // Returns a node's boxes, in content order, relative to the stage's content box.
    std::vector<Bounds> rects(PrLayoutNode node) {
        uint32_t count = prLayoutNodeBoxes(f.tree, node, PrLayoutCoordinates_Viewport, nullptr, 0);
        std::vector<PrLayoutRect> boxes(count);
        prLayoutNodeBoxes(f.tree, node, PrLayoutCoordinates_Viewport, boxes.data(), count);
        PrRect origin = stageBounds();
        std::vector<Bounds> out;
        for (const PrLayoutRect& box : boxes) {
            PrRect r = prLayoutRectToPx(box);
            out.push_back(Bounds{
                double(r.left - origin.left), double(r.top - origin.top), double(r.right - r.left),
                double(r.bottom - r.top),
            });
        }
        return out;
    }

    // Returns the nodes at a point relative to the stage's top left, topmost first.
    std::vector<PrLayoutNode> hits(float x, float y) {
        PrRect origin = stageBounds();
        PrLayoutVec point = prLayoutVecFromPx(PrVec2{origin.left + x, origin.top + y});
        uint32_t count = prLayoutTreeHitTest(f.tree, point, nullptr, 0);
        std::vector<PrLayoutNode> out(count);
        prLayoutTreeHitTest(f.tree, point, out.data(), count);
        return out;
    }
    PrLayoutNode hit(float x, float y) {
        std::vector<PrLayoutNode> all = hits(x, y);
        return all.empty() ? kPrLayoutNodeNone : all[0];
    }
};

// Checks a rectangle's edges and size.
void checkBounds(const Bounds& r, double left, double top, double width, double height) {
    CHECK(r.left == doctest::Approx(left).epsilon(1e-3));
    CHECK(r.top == doctest::Approx(top).epsilon(1e-3));
    CHECK(r.width == doctest::Approx(width).epsilon(1e-3));
    CHECK(r.height == doctest::Approx(height).epsilon(1e-3));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// The box tree

TEST_CASE("a block in an inline box takes a line of its own, between the lines before and after it") {
    Stage s;
    s.text(s.stage, "a ");
    PrLayoutNode span = s.span(s.stage);
    s.text(span, "b");
    PrLayoutNode inner = s.div(span, block(), "c");
    PrLayoutNode marker = s.marker(span);
    s.text(span, "d");
    s.layout();
    CHECK(s.top(inner) == 40);
    CHECK(s.height(inner) == 40);
    CHECK(s.top(marker) == 107);
    CHECK(s.height(s.stage) == 120);
}

TEST_CASE("a block in an inline box sizes against the container's content box, not the inline box's") {
    // Its percentages and auto width are the container's, whatever the inline box's padding.
    Stage s([](PrLayoutFontStyle&) {}, 200, 100);
    PrLayoutNode span = s.span(s.stage, padded(inlineBox(), 20, 0));
    PrLayoutBoxStyle half = block();
    half.width = prLayoutLengthPercent(50);
    half.height = prLayoutLengthPercent(50);
    PrLayoutNode inner = s.div(span, half);
    s.layout();
    CHECK(s.width(inner) == 100);
    CHECK(s.height(inner) == 50);
    CHECK(s.left(inner) == 0);
    CHECK(s.top(inner) == 40);
}

TEST_CASE("a block in an inline box in a right-to-left container takes its margins from the right") {
    Stage s([](PrLayoutFontStyle& f) { f.direction = PrTextDirection_Rtl; }, 100);
    PrLayoutNode span = s.span(s.stage);
    PrLayoutBoxStyle box = sized(block(), 80, 20);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.margin[side] = prLayoutLengthPx(10);
    PrLayoutNode inner = s.div(span, box);
    s.layout();
    CHECK(s.left(inner) == 10);
    CHECK(s.top(inner) == 10);
}

TEST_CASE("white space after a block in an inline box starts its line, and is removed") {
    {
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.div(span, block(), "b");
        s.text(span, " ");
        PrLayoutNode word = s.add(span, inlineBlock(30, 10));
        s.layout();
        CHECK(s.left(word) == 0);
    }
    {
        // Preserved, the newline after the block is a line of its own.
        Stage s([](PrLayoutFontStyle& f) { f.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve; });
        PrLayoutNode span = s.span(s.stage);
        s.text(span, "a\n");
        PrLayoutNode inner = s.div(span, block(), "b");
        s.text(span, "\nc");
        PrLayoutNode marker = s.marker(span);
        s.layout();
        CHECK(s.top(inner) == 40);
        CHECK(s.top(marker) == 147);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Lines with nothing in them

TEST_CASE("the content before a block makes a line only where it holds something") {
    {
        // An empty inline box's start is a phantom line, through which the block's margin collapses.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        PrLayoutNode inner = s.div(span, margined(block(), 20, 0), "x");
        s.layout();
        CHECK(s.top(inner) == 20);
        CHECK(s.height(s.stage) == 60);
    }
    {
        // An inline-axis edge makes a line, before the block or after it.
        Stage s;
        PrLayoutNode span = s.span(s.stage, padded(inlineBox(), 5, 0));
        PrLayoutNode inner = s.div(span, block(), "x");
        s.layout();
        CHECK(s.top(inner) == 40);
        CHECK(s.height(s.stage) == 80);
    }
    {
        Stage s;
        PrLayoutNode span = s.span(s.stage, padded(inlineBox(), 0, 10));
        PrLayoutNode inner = s.div(span, block(), "x");
        s.layout();
        CHECK(s.top(inner) == 0);
        CHECK(s.height(s.stage) == 80);
    }
    {
        // A block-axis edge does not.
        Stage s;
        PrLayoutBoxStyle box = inlineBox();
        box.border[PrRectSide_Top] = prLayoutUnitFromPx(5);
        box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(5);
        PrLayoutNode span = s.span(s.stage, box);
        s.div(span, block(), "x");
        s.layout();
        CHECK(s.height(s.stage) == 40);
    }
    {
        // A line ended by a break before the block adds no line.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.text(span, "a");
        s.lineBreak(span);
        PrLayoutNode inner = s.div(span, block(), "x");
        s.layout();
        CHECK(s.top(inner) == 40);
    }
}

TEST_CASE("an empty block in an inline box keeps a zero-high row between the lines around it") {
    Stage s;
    PrLayoutNode span = s.span(s.stage);
    s.add(span, inlineBlock(30, 10));
    s.div(span);
    s.add(span, inlineBlock(30, 10));
    s.layout();
    CHECK(s.height(s.stage) == 80);
    std::vector<Bounds> rects = s.rects(span);
    REQUIRE(rects.size() == 3);
    checkBounds(rects[0], 0, 9, 30, 22);
    checkBounds(rects[1], 0, 40, 200, 0);
    checkBounds(rects[2], 0, 49, 30, 22);
}

// ---------------------------------------------------------------------------------------------------------------------
// Margins

TEST_CASE("a block in an inline box collapses its margins with the boxes around it") {
    {
        // With a block sibling before it.
        Stage s;
        s.div(s.stage, margined(block(), 0, 20), "a");
        PrLayoutNode span = s.span(s.stage);
        PrLayoutNode inner = s.div(span, margined(block(), 30, 0), "b");
        s.layout();
        CHECK(s.top(inner) == 70);
    }
    {
        // With a block in another inline box before it.
        Stage s;
        PrLayoutNode first = s.span(s.stage);
        s.div(first, margined(block(), 0, 20), "a");
        s.text(s.stage, " ");
        PrLayoutNode second = s.span(s.stage);
        PrLayoutNode inner = s.div(second, margined(block(), 30, 0), "b");
        s.layout();
        CHECK(s.top(inner) == 70);
    }
    {
        // With its parent's block-start margin, and its block-end one.
        Stage s;
        PrLayoutNode parent = s.div(s.stage);
        PrLayoutNode span = s.span(parent);
        PrLayoutNode inner = s.div(span, margined(block(), 20, 0), "x");
        s.layout();
        CHECK(s.top(parent) == 20);
        CHECK(s.top(inner) == 20);
    }
    {
        Stage s;
        PrLayoutNode parent = s.div(s.stage);
        PrLayoutNode span = s.span(parent);
        s.div(span, margined(block(), 0, 20), "x");
        PrLayoutNode next = s.div(s.stage, block(), "y");
        s.layout();
        CHECK(s.height(parent) == 40);
        CHECK(s.top(next) == 60);
    }
    {
        // Through an empty block before it.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.div(span, margined(block(), 20, 0));
        PrLayoutNode inner = s.div(span, margined(sized(block(), -1, 10), 100, 0));
        s.text(span, "text");
        s.layout();
        CHECK(s.top(inner) == 100);
    }
    {
        // Not through a line before it.
        Stage s;
        s.text(s.stage, "a");
        PrLayoutNode span = s.span(s.stage);
        PrLayoutNode inner = s.div(span, margined(block(), 30, 0), "b");
        s.layout();
        CHECK(s.top(inner) == 70);
    }
}

TEST_CASE("an inline box's block-axis margins take no part around a block in it") {
    Stage s;
    PrLayoutNode span = s.span(s.stage, margined(inlineBox(), 50, 50));
    PrLayoutNode inner = s.div(span, block(), "x");
    PrLayoutNode next = s.div(s.stage, block(), "y");
    s.layout();
    CHECK(s.top(inner) == 0);
    CHECK(s.top(next) == 40);
}

// ---------------------------------------------------------------------------------------------------------------------
// Floats and clearance

TEST_CASE("a block in an inline box is placed among the floats as a block sibling is") {
    {
        // Beside a float before it, with its line shortened.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.add(span, floatLeft(60, 20));
        PrLayoutNode inner = s.div(span);
        PrLayoutNode word = s.add(inner, inlineBlock(30, 10));
        s.layout();
        CHECK(s.top(inner) == 0);
        CHECK(s.left(word) == 60);
    }
    {
        // Below the float if it clears.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.add(span, floatLeft(60, 20));
        PrLayoutBoxStyle clears = block();
        clears.clear = PrLayoutClear_Left;
        PrLayoutNode inner = s.div(span, clears, "x");
        s.layout();
        CHECK(s.top(inner) == 20);
    }
    {
        // Beside the float if it avoids floats.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.add(span, floatLeft(60, 20));
        PrLayoutNode inner = s.div(span, flowRoot(block()), "x");
        s.layout();
        CHECK(s.top(inner) == 0);
        CHECK(s.left(inner) == 60);
        CHECK(s.width(inner) == 140);
    }
    {
        // A float after it goes on the line after it.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.div(span, block(), "x");
        PrLayoutNode after = s.add(span, floatLeft(60, 20));
        PrLayoutNode word = s.add(span, inlineBlock(30, 10));
        s.layout();
        CHECK(s.top(after) == 40);
        CHECK(s.left(word) == 60);
        CHECK(s.top(word) == 57);
    }
    {
        // A float on a line before it stays on that line.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.text(span, "a");
        PrLayoutNode before = s.add(span, floatLeft(60, 20));
        PrLayoutNode inner = s.div(span, block(), "x");
        s.layout();
        CHECK(s.top(before) == 0);
        CHECK(s.top(inner) == 40);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Lines around the block

TEST_CASE("the lines around a block are the lines of anonymous blocks: indented, aligned and justified as such") {
    {
        // The first line is indented, and so is the block's own first line, but not the line after the block.
        Stage s([](PrLayoutFontStyle& f) { f.textIndent = prLayoutLengthPx(30); });
        PrLayoutNode span = s.span(s.stage);
        PrLayoutNode first = s.add(span, inlineBlock(30, 10));
        PrLayoutNode inner = s.div(span);
        PrLayoutNode own = s.add(inner, inlineBlock(30, 10));
        PrLayoutNode after = s.add(span, inlineBlock(30, 10));
        s.layout();
        CHECK(s.left(first) == 30);
        CHECK(s.left(own) == 30);
        CHECK(s.left(after) == 0);
    }
    {
        Stage s([](PrLayoutFontStyle& f) { f.textIndent = prLayoutLengthPx(30); });
        PrLayoutNode span = s.span(s.stage);
        PrLayoutNode inner = s.div(span);
        PrLayoutNode own = s.add(inner, inlineBlock(30, 10));
        PrLayoutNode after = s.add(span, inlineBlock(30, 10));
        s.layout();
        CHECK(s.left(own) == 30);
        CHECK(s.left(after) == 0);
    }
    {
        // The line before the block is the last line of its anonymous block: it is not justified, and is aligned by
        // text-align-last.
        Stage s([](PrLayoutFontStyle& f) { f.textAlign = PrLayoutTextAlign_Justify; });
        PrLayoutNode span = s.span(s.stage);
        s.add(span, inlineBlock(30, 10));
        s.text(span, " ");
        PrLayoutNode second = s.add(span, inlineBlock(30, 10));
        s.div(span, block(), "x");
        s.layout();
        CHECK(s.left(second) == doctest::Approx(35.56).epsilon(1e-3));
    }
    {
        Stage s([](PrLayoutFontStyle& f) { f.textAlignLast = PrLayoutTextAlign_Right; });
        PrLayoutNode span = s.span(s.stage);
        PrLayoutNode before = s.add(span, inlineBlock(30, 10));
        s.div(span, block(), "x");
        PrLayoutNode after = s.add(span, inlineBlock(30, 10));
        s.layout();
        CHECK(s.left(before) == 170);
        CHECK(s.left(after) == 170);
    }
}

TEST_CASE("a raised inline box grows the line before a block in it, and moves the block nothing") {
    Stage s;
    PrLayoutBoxStyle raised = inlineBox();
    raised.baselineShift = prLayoutBaselineShiftPx(20);
    PrLayoutNode span = s.span(s.stage, raised);
    s.text(span, "a");
    PrLayoutNode inner = s.div(span, block(), "x");
    PrLayoutNode marker = s.marker(span);
    s.text(span, "b");
    s.layout();
    CHECK(s.top(inner) == 60);
    CHECK(s.top(marker) == 127);
}

TEST_CASE("an inline-block's baseline is its last line in flow, in a block in an inline box too") {
    auto baseline = [](bool text, bool before) {
        Stage s;
        PrLayoutNode marker = s.marker(s.stage);
        PrLayoutFontStyle tight = font(20);
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_InlineBlock;
        box.width = prLayoutLengthPx(100);
        PrLayoutNode ib = s.add(s.stage, box, &tight);
        if (before)
            s.text(ib, "c");
        PrLayoutNode span = s.add(ib, inlineBox(), &tight);
        if (text)
            s.text(s.add(span, block(), &tight), "d");
        else
            s.add(span, bordered(sized(block(), -1, 20), 2), &tight);
        s.layout();
        return std::vector<double>{s.top(ib), s.top(marker)};
    };
    // An empty block has no line, and the line before it is the last.
    CHECK(baseline(false, true) == std::vector<double>{10, 27});
    CHECK(baseline(true, true) == std::vector<double>{0, 37});
    CHECK(baseline(true, false) == std::vector<double>{10, 27});
}

// ---------------------------------------------------------------------------------------------------------------------
// The inline box's rows and parts

TEST_CASE("an inline box's boxes are its parts and its rows, in content order") {
    {
        Stage s;
        s.text(s.stage, "a ");
        PrLayoutNode span = s.span(s.stage);
        s.text(span, "b");
        s.div(span, block(), "c");
        s.text(span, "d");
        s.layout();
        std::vector<Bounds> rects = s.rects(span);
        REQUIRE(rects.size() == 3);
        checkBounds(rects[0], 16.6875, 9, 11.125, 22);
        checkBounds(rects[1], 0, 40, 200, 40);
        checkBounds(rects[2], 0, 89, 11.125, 22);
    }
    {
        // The row spans the container whatever the block's width and margins, and is as tall as its border box.
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.text(span, "b");
        PrLayoutBoxStyle narrow = margined(sized(block(), 100, -1), 10, 10);
        narrow.margin[PrRectSide_Left] = prLayoutLengthPx(30);
        PrLayoutNode inner = s.div(span, narrow, "c");
        s.text(span, "d");
        s.layout();
        std::vector<Bounds> rects = s.rects(span);
        REQUIRE(rects.size() == 3);
        checkBounds(rects[1], 0, 50, 200, 40);
        CHECK(s.top(inner) == 50);
        CHECK(s.left(inner) == 30);
    }
    {
        // Each inline box around the block has a row.
        Stage s;
        PrLayoutNode outer = s.span(s.stage, bordered(inlineBox(), 2));
        s.text(outer, "a");
        PrLayoutNode inner = s.span(outer, bordered(inlineBox(), 2));
        s.text(inner, "b");
        PrLayoutNode box = s.div(inner, block(), "c");
        s.text(inner, "d");
        s.text(outer, "e");
        s.layout();
        std::vector<Bounds> outerRects = s.rects(outer);
        std::vector<Bounds> innerRects = s.rects(inner);
        REQUIRE(outerRects.size() == 3);
        REQUIRE(innerRects.size() == 3);
        checkBounds(outerRects[0], 0, 7, 26.25, 26);
        checkBounds(outerRects[1], 0, 40, 200, 40);
        checkBounds(outerRects[2], 0, 87, 26.25, 26);
        checkBounds(innerRects[0], 13.125, 7, 13.125, 26);
        checkBounds(innerRects[1], 0, 40, 200, 40);
        checkBounds(innerRects[2], 0, 87, 13.125, 26);
        CHECK(s.top(box) == 40);
    }
}

TEST_CASE("an inline box's parts around a block have its edges on their own sides alone") {
    Stage s;
    s.text(s.stage, "x");
    PrLayoutNode span = s.span(s.stage, padded(bordered(inlineBox(), 5), 10, 10));
    s.text(span, "y");
    PrLayoutNode inner = s.div(span, block(), "z");
    s.text(span, "w");
    s.layout();
    CHECK(s.left(inner) == 0);
    CHECK(s.width(inner) == 200);
    std::vector<Bounds> rects = s.rects(span);
    REQUIRE(rects.size() == 3);
    checkBounds(rects[0], 10, 4, 25, 32);
    checkBounds(rects[1], 0, 40, 200, 40);
    checkBounds(rects[2], 0, 84, 29.453125, 32);
}

TEST_CASE("a block's row is hit under the block, and the block's margins are not the inline box's") {
    {
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        PrLayoutBoxStyle box = sized(block(), 100, -1);
        box.margin[PrRectSide_Left] = prLayoutLengthPx(50);
        PrLayoutNode inner = s.div(span, box, "x");
        s.layout();
        CHECK(s.hit(20, 20) == span);
        CHECK(s.hit(100, 20) == inner);
        CHECK(s.hit(180, 20) == span);
    }
    {
        Stage s;
        PrLayoutNode span = s.span(s.stage);
        s.text(span, "b");
        PrLayoutNode inner = s.div(span, margined(block(), 10, 10), "c");
        s.text(span, "d");
        s.layout();
        CHECK(s.hit(100, 45) == s.stage);
        CHECK(s.hit(100, 60) == inner);
    }
    {
        // Every box at a point on the block: the block, each inline box outward, then the container.
        Stage s;
        PrLayoutNode outer = s.span(s.stage);
        PrLayoutNode inner = s.span(outer);
        PrLayoutNode box = s.div(inner, sized(block(), -1, 20));
        s.layout();
        std::vector<PrLayoutNode> hits = s.hits(100, 10);
        REQUIRE(hits.size() >= 4);
        CHECK(hits[0] == box);
        CHECK(hits[1] == inner);
        CHECK(hits[2] == outer);
        CHECK(hits[3] == s.stage);
    }
}

TEST_CASE("an inline box's row holds what its block reaches, for the scroll container around it") {
    auto contentWidth = [](uint32_t spans, bool clips, bool overflows) {
        Stage s;
        PrLayoutNode scroller = s.div(s.stage, scrolling(sized(block(), 100, 100)));
        PrLayoutNode parent = scroller;
        for (uint32_t i = 0; i < spans; ++i)
            parent = s.span(parent);
        PrLayoutBoxStyle box = sized(block(), overflows ? 50 : 300, 20);
        if (clips)
            box.overflowX = box.overflowY = PrLayoutOverflow_Hidden;
        PrLayoutNode inner = s.div(parent, box);
        if (overflows)
            s.add(inner, inlineBlock(300, 10));
        s.layout();
        PrLayoutScrollInfo info{};
        REQUIRE(prLayoutNodeScrollInfo(s.f.tree, scroller, &info));
        return double(prLayoutUnitToPx(info.content.right - info.content.left));
    };
    CHECK(contentWidth(1, false, false) == 300);
    CHECK(contentWidth(3, false, false) == 300);
    // Content that overflows a fixed-size block, and no overflow if the block clips it.
    CHECK(contentWidth(1, false, true) == 300);
    CHECK(contentWidth(1, true, true) == 100);
}

TEST_CASE("the lines and floats around a block in an inline box keep their overflow, for the scroll container") {
    // Where the oversized content is: before the block, after it, or a float in a segment with no text.
    enum Where { Before, After, Float };
    for (Where where : {Before, After, Float}) {
        CAPTURE(int(where));
        Stage s;
        PrLayoutNode scroller = s.div(s.stage, scrolling(sized(block(), 100, 100)));
        PrLayoutNode span = s.span(scroller);
        PrLayoutNode wide = kPrLayoutNodeNone;
        if (where == Before)
            wide = s.add(span, inlineBlock(300, 10));
        s.div(span, sized(block(), -1, 10));
        if (where == After)
            wide = s.add(span, inlineBlock(300, 10));
        if (where == Float)
            wide = s.add(span, floatLeft(300, 10));
        s.layout();
        PrLayoutScrollInfo info{};
        REQUIRE(prLayoutNodeScrollInfo(s.f.tree, scroller, &info));
        CHECK(prLayoutUnitToPx(info.content.right) == 300.0f);
        CHECK(prLayoutUnitToPx(info.range.right) == 200.0f);
        // Scrolled to its far edge, the content's end is at the scrollport's end.
        prLayoutNodeSetScroll(s.f.tree, scroller, px(200, 0));
        s.layout();
        CHECK(s.left(wide) + s.width(wide) == 100);
    }
}

TEST_CASE("content reached through a row scrolls into view") {
    Stage s;
    PrLayoutNode scroller = s.div(s.stage, scrolling(sized(block(), 100, 100)));
    PrLayoutNode span = s.span(scroller);
    PrLayoutNode inner = s.div(span, sized(block(), 300, 20));
    PrLayoutNode end = s.add(inner, inlineBlock(10, 10));
    s.add(inner, inlineBlock(280, 10));
    PrLayoutNode last = s.add(inner, inlineBlock(10, 10));
    s.layout();
    double before = s.left(last);
    prLayoutNodeSetScroll(s.f.tree, scroller, px(200, 0));
    s.layout();
    CHECK(s.left(last) == before - 200);
    CHECK(s.left(end) == -200);
}

TEST_CASE("an inline box's border is open where a block splits it, on the sides its line's direction gives") {
    auto edges = [](PrTextDirection direction) {
        Stage s([&](PrLayoutFontStyle& f) { f.direction = direction; });
        PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
        PrLayoutBoxStyle box = inlineBox();
        box.border[PrRectSide_Left] = prLayoutUnitFromPx(10);
        box.border[PrRectSide_Right] = prLayoutUnitFromPx(10);
        decoration.borderStyle[PrRectSide_Left] = PrDrawBorderStyle_Solid;
        decoration.borderStyle[PrRectSide_Right] = PrDrawBorderStyle_Solid;
        decoration.borderColor[PrRectSide_Left] = prColor16FromColor(PrColor{1, 0, 0, 1});
        decoration.borderColor[PrRectSide_Right] = prColor16FromColor(PrColor{0, 0, 1, 1});
        PrLayoutNode span = s.add(s.stage, box, nullptr, &decoration);
        s.add(span, inlineBlock(30, 10));
        s.div(span, sized(block(), -1, 20));
        s.add(span, inlineBlock(30, 10));
        s.layout();
        std::vector<Bounds> rects = s.rects(span);
        REQUIRE(rects.size() == 3);
        std::vector<uint8_t> pixels = render(s.f.tree, 400, 300);
        PrRect origin = s.stageBounds();
        auto at = [&](const Bounds& r, bool right) {
            double x = double(origin.left) + (right ? r.left + r.width - 5 : r.left + 5);
            return pixel(pixels, 400, uint32_t(x), uint32_t(double(origin.top) + r.top + r.height / 2));
        };
        return std::vector<std::string>{
            at(rects[0], false), at(rects[0], true), at(rects[2], false),
            at(rects[2], true),
        };
    };
    std::string red = "255 0 0 255";
    std::string blue = "0 0 255 255";
    // Left to right: the first part opens at its left with the left border, and the second closes at its right with the
    // right border.
    std::vector<std::string> ltr = edges(PrTextDirection_Ltr);
    CHECK(ltr[0] == red);
    CHECK(ltr[1] != blue);
    CHECK(ltr[2] != red);
    CHECK(ltr[3] == blue);
    // Right to left: the first part opens at its right with the right border, and the second closes at its left with
    // the left border.
    std::vector<std::string> rtl = edges(PrTextDirection_Rtl);
    CHECK(rtl[0] != red);
    CHECK(rtl[1] == blue);
    CHECK(rtl[2] == red);
    CHECK(rtl[3] != blue);
}

// ---------------------------------------------------------------------------------------------------------------------
// Segments of a context

TEST_CASE("an inline box's edges at a block go with the content on their own side of it") {
    // A start edge of 20px before the block goes with the words before it, and an end edge of 30px after the block goes
    // with the word after it.
    auto build = [](Stage& s, PrLayoutNode parent) {
        PrLayoutNode first = s.add(parent, inlineBlock(20, 10));
        s.text(parent, " ");
        PrLayoutNode second = s.add(parent, inlineBlock(20, 10));
        PrLayoutNode span = s.span(parent, padded(inlineBox(), 20, 30));
        s.div(span, sized(block(), 0, 10));
        PrLayoutNode after = s.add(parent, inlineBlock(40, 10));
        return std::vector<PrLayoutNode>{first, second, span, after};
    };
    {
        Stage s;
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_InlineBlock;
        PrLayoutNode shrink = s.add(s.stage, box);
        build(s, shrink);
        s.layout();
        CHECK(s.width(shrink) == doctest::Approx(70).epsilon(1e-3));
    }
    {
        // 65.56px fits a line 70px wide with the start edge, which it would not with an end edge of 30px.
        Stage s(70);
        std::vector<PrLayoutNode> nodes = build(s, s.stage);
        s.layout();
        CHECK(s.top(nodes[1]) == 17);
        std::vector<Bounds> rects = s.rects(nodes[2]);
        REQUIRE(rects.size() == 3);
        CHECK(rects[0].width == 20);
        CHECK(rects[2].width == 30);
        CHECK(s.left(nodes[3]) == 30);
    }
}

TEST_CASE("an inline box's edges between two blocks make a line of their own") {
    Stage s;
    PrLayoutNode span = s.span(s.stage);
    s.div(span, sized(block(), -1, 20));
    PrLayoutNode between = s.span(span, padded(inlineBox(), 20, 0));
    PrLayoutNode last = s.div(span, sized(block(), -1, 20));
    s.layout();
    CHECK(s.top(last) == 60);
    std::vector<Bounds> rects = s.rects(between);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].width == 20);
    // Measured, the line is as wide as the edge.
    Stage m;
    PrLayoutBoxStyle shrink = prLayoutBoxStyleDefault();
    shrink.display = PrLayoutDisplay_InlineBlock;
    PrLayoutNode box = m.add(m.stage, shrink);
    PrLayoutNode inner = m.span(box);
    m.div(inner, sized(block(), 5, 20));
    m.span(inner, padded(inlineBox(), 20, 0));
    m.div(inner, sized(block(), 5, 20));
    m.layout();
    CHECK(m.width(box) == 20);
}

TEST_CASE("the text after a block breaks and measures as a context of its own") {
    // Returns the min-content width of an inline-block of `make`'s content.
    auto minContent = [](auto make) {
        Stage s;
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_InlineBlock;
        box.width = prLayoutLengthMinContent();
        PrLayoutNode shrink = s.add(s.stage, box);
        make(s, shrink);
        s.layout();
        return s.width(shrink);
    };
    // A hyphen that starts a segment starts a word, after which no line breaks (UAX14 LB20a).
    double alone = minContent([](Stage& s, PrLayoutNode parent) { s.text(parent, "-B"); });
    double split = minContent([](Stage& s, PrLayoutNode parent) {
        PrLayoutNode span = s.span(parent);
        s.text(span, "A");
        s.div(span);
        s.text(span, "-B");
    });
    double letter = minContent([](Stage& s, PrLayoutNode parent) { s.text(parent, "A"); });
    CHECK(split == std::max(alone, letter));
    // Regional indicators pair within a segment, not with one before a block.
    double pair = minContent([](Stage& s, PrLayoutNode parent) { s.text(parent, "\xF0\x9F\x87\xB8\xF0\x9F\x87\xAB"); });
    double cut = minContent([](Stage& s, PrLayoutNode parent) {
        PrLayoutNode span = s.span(parent);
        s.text(span, "\xF0\x9F\x87\xBA");
        s.div(span);
        s.text(span, "\xF0\x9F\x87\xB8\xF0\x9F\x87\xAB");
    });
    CHECK(cut == pair);
}

// ---------------------------------------------------------------------------------------------------------------------
// Intrinsic sizes

TEST_CASE("a run with a block in an inline box is measured a segment and a block at a time") {
    auto widthOf = [](auto make, bool min = false) {
        Stage s;
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_InlineBlock;
        if (min)
            box.width = prLayoutLengthMinContent();
        PrLayoutNode shrink = s.add(s.stage, box);
        make(s, shrink);
        s.layout();
        return s.width(shrink);
    };
    CHECK(widthOf([](Stage& s, PrLayoutNode parent) {
        PrLayoutNode span = s.span(parent);
        s.text(span, "aa ");
        s.div(span, sized(block(), 150, 10));
        s.text(span, " bb");
    }) == 150);
    // The inline box's edges are on the lines, not around the block.
    CHECK(widthOf([](Stage& s, PrLayoutNode parent) {
        PrLayoutNode span = s.span(parent, padded(inlineBox(), 20, 20));
        s.div(span, sized(block(), 100, 10));
    }) == 100);
    // A float in a segment adds to that segment's line, not to the block after it.
    CHECK(widthOf([](Stage& s, PrLayoutNode parent) {
        PrLayoutNode span = s.span(parent);
        s.add(span, floatLeft(60, 10));
        s.div(span, flowRoot(sized(block(), 100, 10)));
    }) == 100);
    CHECK(widthOf([](Stage& s, PrLayoutNode parent) {
        PrLayoutNode span = s.span(parent);
        s.text(span, "aaaa ");
        s.div(span, sized(block(), 50, 10));
        s.text(span, " b");
    }, true) == 50);
}

// ---------------------------------------------------------------------------------------------------------------------
// Changes

TEST_CASE("a change around a block in an inline box lays out what it reaches") {
    // The stage holds text, a span of a block between words, and words after.
    struct Built {
        PrLayoutNode span, inner, text;
    };
    auto build = [](Stage& s) {
        s.text(s.stage, "a ");
        PrLayoutNode span = s.span(s.stage);
        s.text(span, "b");
        PrLayoutNode inner = s.div(span);
        PrLayoutNode text = s.text(inner, "c");
        s.text(span, "d");
        return Built{span, inner, text};
    };
    {
        // Inside the block: its own context is shaped again, and the run's is not.
        Stage s;
        Built b = build(s);
        s.layout();
        prLayoutNodeSetText(s.f.tree, b.text, "changed");
        s.layout();
        PrLayoutTreeStats stats = prLayoutTreeStats(s.f.tree);
        CHECK(stats.inlinesShaped == 1);
        CHECK(stats.blocksLaidOut == 3);
    }
    {
        // Its margin: its layout reads its own change, and nothing is shaped.
        Stage s;
        Built b = build(s);
        s.layout();
        prLayoutNodeSetStyle(s.f.tree, b.inner, s.f.style(margined(block(), 10, 10), nullptr, nullptr, &s.style));
        s.layout();
        CHECK(prLayoutTreeStats(s.f.tree).inlinesShaped == 0);
        CHECK(s.top(b.inner) == 50);
    }
    {
        // Its display, from block to inline and back: the run is collected again.
        Stage s;
        Built b = build(s);
        s.layout();
        prLayoutNodeSetStyle(s.f.tree, b.inner, s.f.style(inlineBox(), nullptr, nullptr, &s.style));
        s.layout();
        CHECK(prLayoutTreeStats(s.f.tree).inlinesShaped >= 1);
        CHECK(s.height(s.stage) == 40);
        prLayoutNodeSetStyle(s.f.tree, b.inner, s.f.style(block(), nullptr, nullptr, &s.style));
        s.layout();
        CHECK(s.height(s.stage) == 120);
    }
    {
        // The span's padding: the run is shaped again, and the block reused.
        Stage s;
        Built b = build(s);
        s.layout();
        prLayoutNodeSetStyle(s.f.tree, b.span, s.f.style(padded(inlineBox(), 5, 5), nullptr, nullptr, &s.style));
        s.layout();
        PrLayoutTreeStats stats = prLayoutTreeStats(s.f.tree);
        CHECK(stats.inlinesShaped == 1);
        CHECK(stats.blocksLaidOut == 2);
    }
    {
        // Inserting a block into the span, and removing it.
        Stage s;
        Built b = build(s);
        s.layout();
        prLayoutNodeCreateElement(s.f.tree, b.span, s.f.style(sized(block(), -1, 30), nullptr, nullptr, &s.style));
        s.layout();
        CHECK(s.height(s.stage) == 150);
        // Without the first, the text on either side of it is one line.
        prLayoutTreeErase(s.f.tree, b.inner);
        s.layout();
        CHECK(s.height(s.stage) == 70);
    }
    {
        // The container's width.
        Stage s;
        build(s);
        s.layout();
        s.layout(300);
        s.layout(400);
    }
}

TEST_CASE("a block in an inline box is laid out again where what it read of the floats changes") {
    {
        // Content before it moves it below a float, which it no longer avoids.
        Stage s;
        s.add(s.stage, floatLeft(60, 50));
        PrLayoutNode span = s.span(s.stage);
        PrLayoutNode before = s.add(span, inlineBlock(30, 10));
        PrLayoutNode inner = s.div(span, flowRoot(block()), "x");
        s.layout();
        CHECK(s.width(inner) == 140);
        prLayoutNodeSetStyle(s.f.tree, before, s.f.style(inlineBlock(30, 60), nullptr, nullptr, &s.style));
        s.layout();
        CHECK(s.top(inner) >= 50);
        CHECK(s.width(inner) == 200);
    }
    {
        // A float before it grows, leaving it less room.
        Stage s;
        PrLayoutNode wide = s.add(s.stage, floatLeft(60, 50));
        PrLayoutNode span = s.span(s.stage);
        PrLayoutNode inner = s.div(span, flowRoot(block()), "x");
        s.layout();
        CHECK(s.width(inner) == 140);
        prLayoutNodeSetStyle(s.f.tree, wide, s.f.style(floatLeft(80, 50), nullptr, nullptr, &s.style));
        s.layout();
        CHECK(s.width(inner) == 120);
    }
}

TEST_CASE("a change beside blocks in an inline box records what it would with the blocks outside it") {
    // The inline box's rows are new with each layout of their container and paint nothing, so they have no recordings
    // of their own: the blocks in them replay theirs.
    auto recorded = [](bool split) {
        Stage s;
        PrLayoutNode other = s.div(s.stage);
        PrLayoutNode text = s.text(other, "a");
        PrLayoutNode parent = split ? s.span(s.stage) : s.stage;
        for (int i = 0; i < 20; ++i) {
            PrLayoutDecorationStyle decoration = background(PrColor{0, 0, 1, 1});
            s.text(s.add(parent, sized(block(), -1, 10), nullptr, &decoration), "x");
        }
        s.layout();
        render(s.f.tree, 400, 300);
        prLayoutNodeSetText(s.f.tree, text, "b");
        s.layout();
        render(s.f.tree, 400, 300);
        PrLayoutTreeStats stats = prLayoutTreeStats(s.f.tree);
        return std::vector<uint32_t>{stats.subtreesRecorded, stats.subtreesReused, stats.commandsRecorded};
    };
    CHECK(recorded(true) == recorded(false));
}
