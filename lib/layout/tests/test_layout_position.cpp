#include "pr/float.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/src/layout_scene.h"
#include "layout/src/layout_tree.h"
#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

#include <cmath>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Absolute and fixed positioning (css-position-3): boxes out of flow placed in their containing blocks once those are
// sized, their static positions, inline boxes as containing blocks, relative positioning of inline boxes, and what
// painting, hit testing, scenes and changes do with them. Each case is a stage 200px wide, a flow-root of 20px Arimo on
// 40px lines, and checks the engine's numbers for that stage.

// Returns a font for 20px text on 40px lines, in `direction` and `writingMode`.
PrLayoutFontStyle font(PrTextDirection direction = PrTextDirection_Ltr,
        PrLayoutWritingMode writingMode = PrLayoutWritingMode_HorizontalTb) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(20);
    font.lineHeight = prLayoutLineHeightPx(40);
    font.direction = direction;
    font.writingMode = writingMode;
    return font;
}

// Returns a box `width` by `height`, each auto if negative.
PrLayoutBoxStyle sized(PrLayoutBoxStyle box, float width, float height) {
    if (width >= 0)
        box.width = prLayoutLengthPx(width);
    if (height >= 0)
        box.height = prLayoutLengthPx(height);
    return box;
}

// Return a box with position `position`, or an absolutely positioned box with no insets, which sits at its static
// position.
PrLayoutBoxStyle positioned(PrLayoutBoxStyle box, PrLayoutPosition position) {
    box.position = position;
    return box;
}
PrLayoutBoxStyle absolute(PrLayoutBoxStyle box = block(), bool staticInline = false) {
    box.position = PrLayoutPosition_Absolute;
    box.staticInline = staticInline;
    return box;
}

// Returns an absolutely positioned 20px square, block-level or inline-level as its display was before it was
// blockified.
PrLayoutBoxStyle square(bool staticInline = false) {
    return absolute(sized(block(), 20, 20), staticInline);
}

// Sets the box's insets, in the order top, right, bottom, left, each auto if NaN.
PrLayoutBoxStyle inset(PrLayoutBoxStyle box, float top, float right, float bottom, float left) {
    const float values[] = {top, right, bottom, left};
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.inset[side] = values[side] == values[side] ? prLayoutLengthPx(values[side]) : prLayoutLengthAuto();
    return box;
}

constexpr float kAuto = kNanF32;

// A box with a border, padding and margins of `width` on every side, each where it is not below zero.
PrLayoutBoxStyle bordered(PrLayoutBoxStyle box, float width) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.border[side] = prLayoutUnitFromPx(width);
    return box;
}
PrLayoutBoxStyle padded(PrLayoutBoxStyle box, float width) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.padding[side] = prLayoutLengthPx(width);
    return box;
}
PrLayoutBoxStyle autoMargins(PrLayoutBoxStyle box) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.margin[side] = prLayoutLengthAuto();
    return box;
}

// Return a box that establishes a block formatting context, an inline-block, or a box that floats left.
PrLayoutBoxStyle flowRoot(PrLayoutBoxStyle box) {
    box.display = PrLayoutDisplay_FlowRoot;
    return box;
}
PrLayoutBoxStyle inlineBlock(float width, float height) {
    PrLayoutBoxStyle box = sized(prLayoutBoxStyleDefault(), width, height);
    box.display = PrLayoutDisplay_InlineBlock;
    return box;
}
PrLayoutBoxStyle floatLeft(float width, float height) {
    PrLayoutBoxStyle box = sized(block(), width, height);
    box.floating = PrLayoutFloat_Left;
    return box;
}

// Returns `box` with a transform other than none. The effects record's matrix does not decide whether a box has one.
PrLayoutBoxStyle transformed(PrLayoutBoxStyle box) {
    box.transformed = 1;
    return box;
}

// A tree whose root holds a stage: a flow-root 200px wide, `height` high if that is not negative, relative if
// `relative` says, in the font of `direction` and `writingMode`, with its lines aligned as `align` says.
struct Stage {
    Fixture f;
    PrLayoutFontStyle style;
    PrLayoutNode root = kPrLayoutNodeNone;
    PrLayoutNode stage = kPrLayoutNodeNone;

    explicit Stage(bool relative = false, float height = -1, PrTextDirection direction = PrTextDirection_Ltr,
            PrLayoutWritingMode writingMode = PrLayoutWritingMode_HorizontalTb,
            PrLayoutTextAlign align = PrLayoutTextAlign_Start) {
        style = font(direction, writingMode);
        style.textAlign = align;
        style.textAlignLast = align;
        root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
        PrLayoutBoxStyle box = sized(flowRoot(block()), 200, height);
        if (relative)
            box.position = PrLayoutPosition_Relative;
        stage = f.element(root, box, nullptr, nullptr, &style);
    }

    // Creates an element under `parent` styled by `box`, in the stage's font or `withFont`.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutFontStyle* PR_NULLABLE withFont = nullptr,
            const PrLayoutEffectStyle* PR_NULLABLE effects = nullptr,
            const PrLayoutDecorationStyle* PR_NULLABLE decoration = nullptr) {
        return f.element(parent, box, decoration, effects, withFont != nullptr ? withFont : &style);
    }
    PrLayoutNode text(PrLayoutNode parent, const char* value) { return f.text(parent, value); }
    PrLayoutNode div(PrLayoutNode parent, const PrLayoutBoxStyle& box = block(), const char* PR_NULLABLE value = nullptr) {
        PrLayoutNode node = add(parent, box);
        if (value != nullptr)
            text(node, value);
        return node;
    }

    // Lays the tree out, and checks the incremental work against a layout from scratch.
    void layout(float width = 400, float height = 300) {
        f.layout(px(width, height));
        CHECK(verified(f.tree));
    }

    // Return a box's edges and size, relative to the stage's border box.
    PrRect stageBounds() { return f.bounds(stage); }
    double top(PrLayoutNode node) { return double(f.bounds(node).top - stageBounds().top); }
    double left(PrLayoutNode node) { return double(f.bounds(node).left - stageBounds().left); }
    double width(PrLayoutNode node) { return double(f.bounds(node).right - f.bounds(node).left); }
    double height(PrLayoutNode node) { return double(f.bounds(node).bottom - f.bounds(node).top); }

    // Returns the topmost node at a point relative to the stage's top left.
    PrLayoutNode hit(float x, float y) {
        PrRect origin = stageBounds();
        return f.hit(PrVec2{origin.left + x, origin.top + y});
    }

    // Returns a node's boxes, in content order, relative to the stage's top left, each as left, top, width and height.
    std::vector<std::vector<double>> rects(PrLayoutNode node) {
        uint32_t count = prLayoutNodeBoxes(f.tree, node, PrLayoutCoordinates_Viewport, nullptr, 0);
        std::vector<PrLayoutRect> boxes(count);
        prLayoutNodeBoxes(f.tree, node, PrLayoutCoordinates_Viewport, boxes.data(), count);
        PrRect origin = stageBounds();
        std::vector<std::vector<double>> out;
        for (const PrLayoutRect& box : boxes) {
            PrRect r = prLayoutRectToPx(box);
            out.push_back({
                double(r.left - origin.left), double(r.top - origin.top), double(r.right - r.left),
                double(r.bottom - r.top),
            });
        }
        return out;
    }
};

// Checks a value to a thousandth.
#define CHECK_NEAR(actual, expected) CHECK((actual) == doctest::Approx(expected).epsilon(1e-3))

// Checks a rectangle's left, top, width and height.
void checkBox(const std::vector<double>& r, double left, double top, double width, double height) {
    REQUIRE(r.size() == 4);
    CHECK_NEAR(r[0], left);
    CHECK_NEAR(r[1], top);
    CHECK_NEAR(r[2], width);
    CHECK_NEAR(r[3], height);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// The static position

TEST_CASE("a box out of flow takes no room, and the margins around its placeholder collapse past it") {
    Stage s;
    PrLayoutBoxStyle before = block();
    setMargins(before, 0, 20);
    s.div(s.stage, before, "a");
    PrLayoutNode box = s.add(s.stage, square());
    PrLayoutBoxStyle after = block();
    setMargins(after, 30, 0);
    PrLayoutNode next = s.div(s.stage, after, "b");
    s.layout();
    CHECK(s.top(box) == 60);
    CHECK(s.top(next) == 70);
}

TEST_CASE("a block-level static position starts past the margins before it, through a margin collapsing with its parent") {
    Stage s;
    PrLayoutBoxStyle outer = block();
    setMargins(outer, 20, 0);
    PrLayoutNode parent = s.div(s.stage, outer);
    PrLayoutNode box = s.add(parent, square());
    s.text(parent, "a");
    s.layout();
    CHECK(s.top(box) == 20);
    CHECK(s.top(parent) == 20);
}

TEST_CASE("a box out of flow's own margins apply past its static position") {
    Stage s;
    s.div(s.stage, block(), "a");
    PrLayoutBoxStyle margined = square();
    margined.margin[PrRectSide_Top] = prLayoutLengthPx(10);
    margined.margin[PrRectSide_Left] = prLayoutLengthPx(10);
    PrLayoutNode box = s.add(s.stage, margined);
    s.layout();
    CHECK(s.top(box) == 50);
    CHECK(s.left(box) == 10);
}

TEST_CASE("an inline-level static position is its place on its line, after alignment, at the line's top") {
    SUBCASE("after text") {
        Stage s;
        s.text(s.stage, "abc");
        PrLayoutNode box = s.add(s.stage, square(true));
        s.text(s.stage, "def");
        s.layout();
        CHECK_NEAR(s.left(box), 32.25);
        CHECK(s.top(box) == 0);
    }
    SUBCASE("in a centered line") {
        Stage s(false, -1, PrTextDirection_Ltr, PrLayoutWritingMode_HorizontalTb, PrLayoutTextAlign_Center);
        s.text(s.stage, "abc");
        PrLayoutNode box = s.add(s.stage, square(true));
        s.text(s.stage, "def");
        s.layout();
        CHECK_NEAR(s.left(box), 102.219);
        CHECK(s.top(box) == 0);
    }
    SUBCASE("beside a float, which a block-level static position ignores") {
        Stage s;
        s.add(s.stage, floatLeft(60, 60));
        s.text(s.stage, "ab");
        PrLayoutNode inlineLevel = s.add(s.stage, square(true));
        s.text(s.stage, "cd");
        s.layout();
        CHECK_NEAR(s.left(inlineLevel), 82.25);
        CHECK(s.top(inlineLevel) == 0);
    }
    SUBCASE("at a line's end, before the space it breaks at") {
        Stage s;
        s.text(s.stage, "xxxxxx yyyyyy");
        PrLayoutNode box = s.add(s.stage, square(true));
        s.text(s.stage, " zz");
        s.layout();
        CHECK_NEAR(s.left(box), 125.563);
        CHECK(s.top(box) == 0);
    }
}

TEST_CASE("a block-level box in a line goes to the next line where content comes before it") {
    Stage s;
    s.text(s.stage, "abc");
    PrLayoutNode box = s.add(s.stage, square());
    s.text(s.stage, "def");
    s.layout();
    CHECK(s.left(box) == 0);
    CHECK(s.top(box) == 40);
}

TEST_CASE("a block-level static position ignores floats and the alignment of lines") {
    Stage s(false, -1, PrTextDirection_Ltr, PrLayoutWritingMode_HorizontalTb, PrLayoutTextAlign_Center);
    s.add(s.stage, floatLeft(60, 60));
    PrLayoutNode box = s.add(s.stage, square());
    s.div(s.stage, block(), "x");
    s.layout();
    CHECK(s.left(box) == 0);
    CHECK(s.top(box) == 0);
}

TEST_CASE("a line holding only a box out of flow is a phantom line, and lines run across it as though it were not there") {
    Stage s;
    PrLayoutNode only = s.div(s.stage);
    s.add(only, square(true));
    s.layout();
    CHECK(s.height(only) == 0);

    // A word around it is one word, with no wrap opportunity at it.
    Stage t;
    PrLayoutBoxStyle shrink = inlineBlock(-1, -1);
    shrink.width = prLayoutLengthMinContent();
    PrLayoutNode word = t.add(t.stage, shrink);
    t.text(word, "abc");
    t.add(word, square(true));
    t.text(word, "def");
    t.layout();
    CHECK_NEAR(t.width(word), 60.063);
}

TEST_CASE("the static position gives the start edges of its static context, whatever its containing block's direction") {
    SUBCASE("rtl, block-level: its right edge") {
        Stage s(false, -1, PrTextDirection_Rtl);
        s.div(s.stage, block(), "a");
        PrLayoutNode box = s.add(s.stage, square());
        s.layout();
        CHECK(s.left(box) == 180);
        CHECK(s.top(box) == 40);
    }
    SUBCASE("rtl, inline-level: its right edge at its place") {
        Stage s(false, -1, PrTextDirection_Rtl);
        s.text(s.stage, "abc");
        PrLayoutNode box = s.add(s.stage, square(true));
        s.text(s.stage, "def");
        s.layout();
        CHECK_NEAR(s.left(box), 152.188);
    }
    SUBCASE("between runs of two directions, it resolves as a neutral: the paragraph's direction") {
        for (PrTextDirection direction : {PrTextDirection_Ltr, PrTextDirection_Rtl}) {
            CAPTURE(direction);
            bool rtl = direction == PrTextDirection_Rtl;
            Stage s(false, -1, direction);
            s.text(s.stage, rtl ? "אבג" : "abc");
            PrLayoutNode box = s.add(s.stage, square(true));
            s.text(s.stage, rtl ? "abc" : "אבג");
            s.layout();
            // Where the run of the paragraph's direction ends: after "abc", and before "אבג" from the right.
            CHECK(std::abs(s.left(box) - (rtl ? 147.016 : 32.25)) < 0.05);
        }
    }
    SUBCASE("inside a run of the other direction, it takes the run's") {
        Stage s;
        s.text(s.stage, "x אבג");
        PrLayoutNode box = s.add(s.stage, square(true));
        s.text(s.stage, "דהו");
        s.layout();
        CHECK(std::abs(s.left(box) - 44.517) < 0.05);
    }
    SUBCASE("an rtl static block in an ltr containing block: the box's right edge, its width fitting its content") {
        Stage s(true);
        PrLayoutFontStyle rtl = font(PrTextDirection_Rtl);
        PrLayoutNode context = s.add(s.stage, block(), &rtl);
        s.add(context, block(), &rtl);
        PrLayoutNode box = s.add(context, absolute(), &rtl);
        s.text(box, "abc");
        s.layout();
        CHECK_NEAR(s.left(box), 167.75);
        CHECK_NEAR(s.width(box), 32.25);
    }
    SUBCASE("an ltr static block in an rtl containing block: its left edge") {
        Stage s(true, -1, PrTextDirection_Rtl);
        PrLayoutFontStyle ltr = font();
        PrLayoutNode context = s.add(s.stage, block(), &ltr);
        s.add(context, block(), &ltr);
        PrLayoutNode box = s.add(context, absolute(), &ltr);
        s.text(box, "abc");
        s.layout();
        CHECK(s.left(box) == 0);
        CHECK_NEAR(s.width(box), 32.25);
    }
    SUBCASE("vertical-rl: its right edge, past the blocks before it") {
        Stage s(false, 200, PrTextDirection_Ltr, PrLayoutWritingMode_VerticalRl);
        s.div(s.stage, block(), "a");
        PrLayoutNode box = s.add(s.stage, square());
        s.layout();
        CHECK(s.left(box) == 140);
        CHECK(s.top(box) == 0);
    }
}

TEST_CASE("a static position is reached through static blocks, with their padding") {
    Stage s(true);
    PrLayoutBoxStyle inner = block();
    inner.padding[PrRectSide_Top] = prLayoutLengthPx(10);
    inner.padding[PrRectSide_Left] = prLayoutLengthPx(20);
    PrLayoutNode first = s.add(s.stage, inner);
    PrLayoutNode second = s.add(first, inner);
    PrLayoutNode box = s.add(second, square());
    s.layout();
    CHECK(s.left(box) == 40);
    CHECK(s.top(box) == 20);
}

// ---------------------------------------------------------------------------------------------------------------------
// Containing blocks

TEST_CASE("an inline box is the containing block of the boxes out of flow in it: its padding box, first part to last") {
    SUBCASE("on one line, with a border and padding") {
        Stage s;
        s.text(s.stage, "x");
        PrLayoutNode span = s.add(s.stage, padded(bordered(positioned(inlineBox(), PrLayoutPosition_Relative), 5), 10));
        s.text(span, "yy");
        PrLayoutNode box = s.add(span, inset(absolute(), 0, 0, 0, 0), nullptr);
        s.layout();
        CHECK(s.left(box) == 15);
        CHECK(s.top(box) == -1);
        CHECK(s.width(box) == 40);
        CHECK(s.height(box) == 42);
    }
    SUBCASE("across two lines, from its first part's start to its last part's end") {
        Stage s;
        s.text(s.stage, "xxxxx ");
        PrLayoutBoxStyle span = positioned(inlineBox(), PrLayoutPosition_Relative);
        span.padding[PrRectSide_Left] = prLayoutLengthPx(10);
        span.padding[PrRectSide_Right] = prLayoutLengthPx(10);
        PrLayoutNode inlineNode = s.add(s.stage, span);
        s.text(inlineNode, "yyy zzzzzzz wwwww");
        PrLayoutNode box = s.add(inlineNode, inset(absolute(), 0, 0, 0, 0));
        PrLayoutNode corner = s.add(inlineNode, inset(square(), kAuto, 0, 0, kAuto));
        s.layout();
        CHECK_NEAR(s.left(box), 55.563);
        CHECK(s.top(box) == 9);
        CHECK_NEAR(s.width(box), 26.656);
        CHECK(s.height(box) == 62);
        CHECK_NEAR(s.left(corner), 62.219);
        CHECK(s.top(corner) == 51);
    }
    SUBCASE("around a block, where its parts on lines are its first and last") {
        Stage s;
        PrLayoutBoxStyle span = positioned(inlineBox(), PrLayoutPosition_Relative);
        span.padding[PrRectSide_Left] = prLayoutLengthPx(10);
        span.padding[PrRectSide_Right] = prLayoutLengthPx(10);
        PrLayoutNode inlineNode = s.add(s.stage, span);
        s.text(inlineNode, "a");
        s.add(inlineNode, sized(block(), -1, 20));
        s.text(inlineNode, "b");
        PrLayoutNode box = s.add(inlineNode, inset(absolute(), 0, 0, 0, 0));
        s.layout();
        CHECK(s.left(box) == 0);
        CHECK(s.top(box) == 9);
        CHECK_NEAR(s.width(box), 21.125);
        CHECK(s.height(box) == 82);
    }
    SUBCASE("before a positioned or transformed block container around it") {
        for (bool transform : {false, true}) {
            CAPTURE(transform);
            Stage s(!transform);
            PrLayoutNode host = transform ? s.add(s.stage, transformed(padded(block(), 7))) : s.stage;
            s.text(host, "x");
            PrLayoutNode span = s.add(host, padded(positioned(inlineBox(), PrLayoutPosition_Relative), 10));
            s.text(span, "yy");
            PrLayoutNode box = s.add(span, inset(square(), 0, kAuto, kAuto, 0));
            s.layout();
            std::vector<std::vector<double>> parts = s.rects(span);
            REQUIRE(parts.size() == 1);
            CHECK_NEAR(s.left(box), parts[0][0]);
            CHECK_NEAR(s.top(box), parts[0][1]);
        }
    }
}

TEST_CASE("an empty inline box on a phantom line is zero high at the line's top, and so is the block it makes") {
    Stage s;
    PrLayoutNode outer = s.div(s.stage);
    PrLayoutNode wrapper = s.add(outer, inlineBox());
    PrLayoutNode span = s.add(wrapper, positioned(inlineBox(), PrLayoutPosition_Relative));
    s.text(span, " ");
    PrLayoutNode box = s.add(span, inset(square(), 0, kAuto, kAuto, 0));
    s.text(span, " ");
    s.layout();
    CHECK(s.height(outer) == 0);
    std::vector<std::vector<double>> rects = s.rects(span);
    REQUIRE(rects.size() == 1);
    checkBox(rects[0], 0, 0, 0, 0);
    CHECK(s.top(box) == 0);
    CHECK(s.left(box) == 0);
}

TEST_CASE("a transform makes a box the containing block of absolute and fixed boxes, but not on an inline box, and a "
        "filter does on any box") {
    SUBCASE("a transformed block") {
        Stage s(true, 80);
        PrLayoutBoxStyle holder = transformed(sized(block(), 120, 60));
        holder.margin[PrRectSide_Left] = prLayoutLengthPx(60);
        PrLayoutNode node = s.add(s.stage, holder);
        PrLayoutNode abs = s.add(node, inset(square(), 0, 0, kAuto, kAuto));
        PrLayoutBoxStyle fixedBox = inset(square(), 0, 0, kAuto, kAuto);
        fixedBox.position = PrLayoutPosition_Fixed;
        PrLayoutNode fixed = s.add(node, fixedBox);
        s.layout();
        CHECK(s.left(abs) == 160);
        CHECK(s.top(abs) == 0);
        CHECK(s.left(fixed) == 160);
        CHECK(s.top(fixed) == 0);
        CHECK(prLayoutNodeHoldsAbsolute(s.f.tree, node));
        CHECK(prLayoutNodeHoldsFixed(s.f.tree, node));
    }
    SUBCASE("a transformed inline box holds nothing; an inline-block and a replaced box do") {
        Stage s(true);
        PrLayoutNode span = s.add(s.stage, transformed(inlineBox()));
        s.text(span, "x");
        PrLayoutNode abs = s.add(span, inset(square(), 0, kAuto, kAuto, 0));
        PrLayoutNode block = s.add(s.stage, transformed(inlineBlock(30, 10)));
        PrLayoutNode image = replaced(s.f, s.stage, transformed(prLayoutBoxStyleDefault()), natural(10, 10));
        s.layout();
        CHECK(s.left(abs) == 0);
        CHECK(s.top(abs) == 0);
        CHECK_FALSE(prLayoutNodeHoldsAbsolute(s.f.tree, span));
        CHECK(prLayoutNodeHoldsFixed(s.f.tree, block));
        CHECK(prLayoutNodeHoldsFixed(s.f.tree, image));
    }
    SUBCASE("a filtered inline box holds both, as a positioned inline box holds absolute boxes") {
        Stage s(true);
        s.text(s.stage, "ab");
        PrLayoutBoxStyle filtered = inlineBox();
        filtered.filtered = 1;
        PrLayoutNode span = s.add(s.stage, filtered);
        s.text(span, "x");
        PrLayoutNode abs = s.add(span, inset(square(), 0, kAuto, kAuto, 0));
        PrLayoutBoxStyle fixedBox = inset(square(), 0, kAuto, kAuto, 0);
        fixedBox.position = PrLayoutPosition_Fixed;
        PrLayoutNode fixed = s.add(span, fixedBox);
        s.layout();
        CHECK(s.left(abs) > 0);
        CHECK(s.left(fixed) == s.left(abs));
        CHECK(prLayoutNodeHoldsAbsolute(s.f.tree, span));
        CHECK(prLayoutNodeHoldsFixed(s.f.tree, span));
    }
}

TEST_CASE("a fixed box no transform holds is placed in the viewport, and stays as the document scrolls") {
    Stage s(true);
    s.add(s.stage, block(1000));
    PrLayoutBoxStyle fixedBox = inset(square(), 10, kAuto, kAuto, 10);
    fixedBox.position = PrLayoutPosition_Fixed;
    PrLayoutNode fixed = s.add(s.stage, fixedBox);
    PrLayoutNode abs = s.add(s.root, inset(square(), 10, kAuto, kAuto, 50));
    s.layout(400, 300);
    checkRect(s.f.bounds(fixed), 10, 10, 30, 30);
    checkRect(s.f.bounds(abs), 50, 10, 70, 30);
    // The part of a fixed box that lies outside the viewport cannot be scrolled to, and the document's scroll moves
    // only what is not fixed.
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(s.f.tree, kPrLayoutNodeNone, &info));
    prLayoutNodeSetScroll(s.f.tree, kPrLayoutNodeNone, px(0, 100));
    checkRect(s.f.bounds(fixed), 10, 10, 30, 30);
    checkRect(s.f.bounds(abs), 50, -90, 70, -70);
    CHECK(s.f.hit(PrVec2{20, 20}) == fixed);
    CHECK(verified(s.f.tree));
}

TEST_CASE("an absolutely positioned box in a scroll container held outside it escapes its clip and its scrolling") {
    Stage s(true, 100);
    PrLayoutBoxStyle scroller = sized(block(), 100, 60);
    scroller.margin[PrRectSide_Left] = prLayoutLengthPx(50);
    scroller.overflowX = PrLayoutOverflow_Scroll;
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode container = s.add(s.stage, scroller);
    s.add(container, block(30));
    PrLayoutNode placed = s.add(container, inset(square(), 10, kAuto, kAuto, 10));
    PrLayoutNode atStatic = s.add(container, square());
    s.add(container, block(100));
    s.layout();
    prLayoutNodeSetScroll(s.f.tree, container, px(0, 30));
    CHECK(s.left(placed) == 10);
    CHECK(s.top(placed) == 10);
    CHECK(s.left(atStatic) == 50);
    CHECK(s.top(atStatic) == 30);
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(s.f.tree, container, &info));
    CHECK(prLayoutUnitToPx(info.content.bottom - info.content.top) == 130.0f);
    CHECK(s.hit(15, 15) == placed);
    CHECK(verified(s.f.tree));
}

TEST_CASE("an absolutely positioned box adds to the scrollable overflow of the scroll container that holds it") {
    Stage s(false, 100);
    PrLayoutBoxStyle scroller = positioned(sized(block(), 100, 60), PrLayoutPosition_Relative);
    scroller.overflowX = PrLayoutOverflow_Scroll;
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode container = s.add(s.stage, scroller);
    s.add(container, inset(square(), 150, kAuto, kAuto, 130));
    s.layout();
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(s.f.tree, container, &info));
    CHECK(prLayoutUnitToPx(info.content.bottom) == 170.0f);
    CHECK(prLayoutUnitToPx(info.content.right) == 150.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sizes and placement

TEST_CASE("an inset-modified containing block below zero gives way at the inset that is not the anchor") {
    Stage s(true);
    PrLayoutNode box = s.add(s.stage, inset(absolute(), kAuto, 150, kAuto, 150));
    s.text(box, "abc");
    s.layout();
    CHECK(s.left(box) == 150);
    CHECK(s.width(box) == 0);
}

TEST_CASE("auto margins share what the inset-modified containing block leaves, safe in the inline axis") {
    SUBCASE("below zero in the block axis, centered") {
        Stage s(true, 40);
        PrLayoutBoxStyle box = inset(sized(absolute(), 20, 80), 0, kAuto, 0, kAuto);
        box.margin[PrRectSide_Top] = prLayoutLengthAuto();
        box.margin[PrRectSide_Bottom] = prLayoutLengthAuto();
        PrLayoutNode node = s.add(s.stage, box);
        s.layout();
        CHECK(s.top(node) == -20);
    }
    SUBCASE("csswg-drafts#11478: below zero in both axes") {
        Stage s(true, 100);
        PrLayoutBoxStyle stage = positioned(sized(flowRoot(block()), 100, 100), PrLayoutPosition_Relative);
        PrLayoutNode inner = s.add(s.stage, stage);
        PrLayoutNode node = s.add(inner, bordered(autoMargins(inset(absolute(), 90, 90, 90, 90)), 10));
        PrLayoutBoxStyle negative = bordered(inset(absolute(), 90, 90, 90, 90), 10);
        for (uint32_t side = 0; side < PrRectSide_Count; ++side)
            negative.margin[side] = prLayoutLengthPx(-50);
        PrLayoutNode moved = s.add(inner, negative);
        s.layout();
        CHECK(s.left(node) == 90);
        CHECK(s.top(node) == 80);
        CHECK(s.width(node) == 20);
        CHECK(s.height(node) == 20);
        CHECK(s.left(moved) == 40);
        CHECK(s.top(moved) == 40);
    }
}

TEST_CASE("an auto size stretches between insets, and its percentages resolve against what it stretches to") {
    Stage s(true, 80);
    PrLayoutNode box = s.add(s.stage, inset(sized(absolute(), 40, -1), 10, kAuto, 10, kAuto));
    PrLayoutBoxStyle half = block();
    half.height = prLayoutLengthPercent(50);
    PrLayoutNode child = s.add(box, half);
    s.layout();
    CHECK(s.height(box) == 60);
    CHECK(s.height(child) == 30);

    // Padding and borders shrink the content box, and a limit clamps the stretched size.
    PrLayoutBoxStyle edged = padded(bordered(inset(sized(absolute(), 40, -1), 0, kAuto, 0, kAuto), 5), 10);
    PrLayoutNode inner = s.add(s.stage, edged);
    PrLayoutNode innerChild = s.add(inner, half);
    PrLayoutBoxStyle limited = inset(sized(absolute(), 40, -1), 0, kAuto, 0, kAuto);
    limited.maxHeight = prLayoutLengthPx(50);
    limited.boxSizing = PrLayoutBoxSizing_BorderBox;
    limited = bordered(limited, 5);
    PrLayoutNode clamped = s.add(s.stage, limited);
    s.layout();
    CHECK(s.height(inner) == 80);
    CHECK(s.height(innerChild) == 25);
    CHECK(s.height(clamped) == 50);
}

TEST_CASE("a stretched size between auto insets follows its static position as content before it changes") {
    Stage s(true, 100);
    PrLayoutNode filler = s.div(s.stage, block(20));
    PrLayoutBoxStyle stretched = sized(absolute(), 20, -1);
    stretched.height = prLayoutLengthStretch();
    PrLayoutNode box = s.add(s.stage, stretched);
    s.layout();
    CHECK(s.top(box) == 20);
    CHECK(s.height(box) == 80);
    prLayoutNodeSetStyle(s.f.tree, filler, s.f.style(block(40), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.top(box) == 40);
    CHECK(s.height(box) == 60);
}

TEST_CASE("an auto width fits the content where an inset is auto, and stretches where neither is") {
    Stage s(true);
    PrLayoutNode box = s.add(s.stage, inset(absolute(), kAuto, 0, kAuto, 0));
    s.text(box, "abc");
    s.layout();
    CHECK(s.width(box) == 200);
    PrLayoutBoxStyle fitted = inset(absolute(), kAuto, kAuto, kAuto, 0);
    prLayoutNodeSetStyle(s.f.tree, box, s.f.style(fitted, nullptr, nullptr, &s.style));
    s.layout();
    CHECK_NEAR(s.width(box), 32.25);
}

TEST_CASE("a replaced box out of flow keeps its natural size between insets") {
    Stage s(true, 40);
    PrLayoutNode image = replaced(s.f, s.stage, inset(absolute(prLayoutBoxStyleDefault()), kAuto, 0, kAuto, 0),
        natural(30, 20));
    s.layout();
    CHECK(s.width(image) == 30);
    CHECK(s.left(image) == 0);
}

TEST_CASE("an orthogonal box out of flow fits what its containing block leaves, its percentages of the whole") {
    SUBCASE("vertical-rl text in a horizontal stage") {
        Stage s(true, 80);
        PrLayoutFontStyle vertical = font(PrTextDirection_Ltr, PrLayoutWritingMode_VerticalRl);
        PrLayoutNode box = s.add(s.stage, inset(absolute(), 0, kAuto, kAuto, 0), &vertical);
        s.text(box, "abc def ghi jkl");
        s.layout();
        CHECK(s.width(box) == 80);
        CHECK(s.height(box) == 80);
    }
    SUBCASE("in a vertical-rl stage, its auto block size of its text") {
        Stage s(true, 100, PrTextDirection_Ltr, PrLayoutWritingMode_VerticalRl);
        PrLayoutNode box = s.add(s.stage, inset(absolute(), kAuto, kAuto, kAuto, 10));
        s.text(box, "abc");
        s.layout();
        CHECK(s.left(box) == 10);
        CHECK(s.width(box) == 40);
        CHECK_NEAR(s.height(box), 32.25);
    }
}

TEST_CASE("a box out of flow adds nothing to its parent's intrinsic sizes") {
    Stage s;
    PrLayoutNode shrink = s.add(s.stage, inlineBlock(-1, -1));
    s.text(shrink, "a");
    s.add(shrink, absolute(sized(block(), 150, 10)));
    s.layout();
    CHECK_NEAR(s.width(shrink), 11.125);
}

// ---------------------------------------------------------------------------------------------------------------------
// Relative positioning of inline boxes

TEST_CASE("a relative inline box moves its parts and what they hold, and nothing else of the line") {
    SUBCASE("across lines") {
        Stage s;
        s.text(s.stage, "xxxxx ");
        PrLayoutBoxStyle moved = inset(positioned(inlineBox(), PrLayoutPosition_Relative), 10, kAuto, kAuto, 10);
        PrLayoutNode span = s.add(s.stage, moved);
        s.text(span, "yyy zzzzzzz wwwww");
        PrLayoutNode marker = s.add(span, inlineBlock(30, 10));
        s.layout();
        std::vector<std::vector<double>> rects = s.rects(span);
        REQUIRE(rects.size() == 2);
        checkBox(rects[0], 65.563, 19, 105.563, 22);
        checkBox(rects[1], 10, 59, 102.219, 22);
        CHECK_NEAR(s.left(marker), 82.219);
        CHECK(s.top(marker) == 67);
    }
    SUBCASE("around a block, which moves with its rows") {
        Stage s;
        PrLayoutBoxStyle moved = inset(positioned(inlineBox(), PrLayoutPosition_Relative), 10, kAuto, kAuto, 10);
        PrLayoutNode span = s.add(s.stage, moved);
        s.text(span, "a");
        PrLayoutNode inner = s.div(span, block(), "x");
        PrLayoutNode marker = s.add(span, inlineBlock(30, 10));
        s.layout();
        CHECK(s.top(inner) == 50);
        CHECK(s.left(inner) == 10);
        CHECK(s.top(marker) == 107);
        CHECK(s.left(marker) == 10);
    }
    SUBCASE("a percentage of the block container") {
        Stage s(false, 80);
        PrLayoutBoxStyle span = positioned(inlineBox(), PrLayoutPosition_Relative);
        span.inset[PrRectSide_Top] = prLayoutLengthPercent(25);
        PrLayoutNode node = s.add(s.stage, span);
        s.text(node, "abc");
        s.layout();
        CHECK(s.top(node) == 29);
    }
    SUBCASE("hit where it is") {
        Stage s;
        PrLayoutBoxStyle moved = inset(positioned(inlineBox(), PrLayoutPosition_Relative), kAuto, kAuto, kAuto, 50);
        PrLayoutNode node = s.add(s.stage, moved);
        s.text(node, "abc");
        s.layout();
        CHECK(s.hit(10, 20) == s.stage);
        CHECK(s.hit(60, 20) != s.stage);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting and hit testing

TEST_CASE("a box out of flow paints where its placeholder is in tree order, out of the clips between") {
    SUBCASE("among the positioned boxes of its stacking context") {
        Stage s(true, 80);
        PrLayoutNode outer = s.div(s.stage);
        PrLayoutNode r = s.add(outer, positioned(sized(block(), 60, 20), PrLayoutPosition_Relative));
        PrLayoutNode x = s.add(outer, inset(sized(absolute(), 120, 60), 0, kAuto, kAuto, 20));
        PrLayoutNode later = s.add(s.stage, positioned(sized(block(), 150, 40), PrLayoutPosition_Relative));
        s.layout();
        CHECK(s.hit(110, 30) == later);
        CHECK(s.hit(30, 10) == x);
        CHECK(s.hit(5, 10) == r);
    }
    SUBCASE("through a static clip it is not held by") {
        Stage s(true, 120);
        PrLayoutBoxStyle clip = sized(block(), 40, 40);
        clip.overflowX = PrLayoutOverflow_Hidden;
        clip.overflowY = PrLayoutOverflow_Hidden;
        PrLayoutNode clipper = s.add(s.stage, clip);
        PrLayoutNode box = s.add(clipper, inset(sized(absolute(), 100, 100), 0, kAuto, kAuto, 0));
        s.layout();
        CHECK(s.hit(20, 20) == box);
        CHECK(s.hit(80, 80) == box);
    }
    SUBCASE("through an opacity box with a clip it is not held by") {
        Stage s(true, 80);
        PrLayoutBoxStyle clip = sized(block(), 40, 40);
        clip.overflowX = PrLayoutOverflow_Hidden;
        clip.overflowY = PrLayoutOverflow_Hidden;
        PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
        faded.opacity = 0.5f;
        PrLayoutNode clipper = s.add(s.stage, clip, nullptr, &faded);
        PrLayoutNode box = s.add(clipper, inset(sized(absolute(), 80, 60), 0, kAuto, kAuto, 100));
        s.layout();
        CHECK(s.hit(150, 30) == box);
    }
    SUBCASE("a fixed box in an absolute box whose holder is outside the point, clipped or not") {
        for (bool clips : {false, true}) {
            CAPTURE(clips);
            Stage s(false, 100);
            PrLayoutBoxStyle wrapStyle = sized(block(), 40, 40);
            if (clips) {
                wrapStyle.overflowX = PrLayoutOverflow_Hidden;
                wrapStyle.overflowY = PrLayoutOverflow_Hidden;
            }
            PrLayoutNode wrap = s.add(s.stage, wrapStyle);
            PrLayoutNode holder = s.add(wrap, positioned(sized(block(), 20, 20), PrLayoutPosition_Relative));
            PrLayoutNode box = s.add(holder, inset(sized(absolute(), 60, 60), 0, kAuto, kAuto, 0));
            PrLayoutBoxStyle fixedBox = inset(square(), 0, kAuto, kAuto, 300);
            fixedBox.position = PrLayoutPosition_Fixed;
            PrLayoutNode fixed = s.add(box, fixedBox);
            s.layout();
            checkRect(s.f.bounds(fixed), 300, 0, 320, 20);
            CHECK(s.f.hit(PrVec2{310, 10}) == fixed);
            // The absolute box stays in the clip around its holder.
            if (clips)
                CHECK(s.hit(50, 50) != box);
            else
                CHECK(s.hit(50, 50) == box);
        }
    }
}

TEST_CASE("a box out of flow escapes an opacity layer's clip and scrolling when painted and played") {
    Fixture f;
    PrLayoutBoxStyle stage = positioned(sized(block(), 100, 100), PrLayoutPosition_Relative);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode holder = f.element(root, stage);
    PrLayoutBoxStyle scroller = sized(block(), 40, 40);
    scroller.overflowX = PrLayoutOverflow_Scroll;
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode container = f.element(holder, scroller);
    PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
    faded.opacity = 0.5f;
    PrLayoutNode layer = f.element(container, block(100), nullptr, &faded);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    f.element(layer, inset(sized(absolute(), 20, 20), 60, kAuto, kAuto, 60), &red);
    f.layout(px(100, 100));
    CHECK(verified(f.tree));
    std::vector<uint8_t> before = render(f.tree, 100, 100);
    // Faded, and outside the container's clip. The colors are premultiplied.
    CHECK(pixel(before, 100, 70, 70) == "128 0 0 128");
    // A scroll of the container moves nothing of the box, and records nothing.
    prLayoutNodeSetScroll(f.tree, container, px(0, 30));
    std::vector<uint8_t> after = render(f.tree, 100, 100);
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    CHECK(pixel(after, 100, 70, 70) == "128 0 0 128");
    CHECK(verified(f.tree));
}

TEST_CASE("a fixed box in a fading body stays put as the document scrolls, and a scroll records nothing") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
    faded.opacity = 0.5f;
    PrLayoutNode body = f.element(root, block(), nullptr, &faded);
    f.element(body, block(400));
    PrLayoutBoxStyle fixedBox = inset(sized(absolute(), 20, 20), 0, kAuto, kAuto, 0);
    fixedBox.position = PrLayoutPosition_Fixed;
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    f.element(body, fixedBox, &red);
    f.layout(px(50, 50));
    CHECK(verified(f.tree));
    CHECK(pixel(render(f.tree, 50, 50), 50, 10, 10) == "128 0 0 128");
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 100));
    std::vector<uint8_t> scrolled = render(f.tree, 50, 50);
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    CHECK(pixel(scrolled, 50, 10, 10) == "128 0 0 128");
    CHECK(verified(f.tree));
}

TEST_CASE("a fixed box in an absolute box, both held outside a fading layer, paints in the layer") {
    for (bool ownLayer : {false, true}) {
        CAPTURE(ownLayer);
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
        faded.opacity = 0.5f;
        PrLayoutNode body = f.element(root, block(), nullptr, &faded);
        PrLayoutNode box = f.element(body, inset(sized(absolute(), 50, 50), 10, kAuto, kAuto, 10), nullptr,
            ownLayer ? &faded : nullptr);
        PrLayoutBoxStyle fixedBox = inset(square(), 0, kAuto, kAuto, 0);
        fixedBox.position = PrLayoutPosition_Fixed;
        PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
        f.element(box, fixedBox, &red);
        f.layout(px(100, 100));
        CHECK(verified(f.tree));
        CHECK(pixel(render(f.tree, 100, 100), 100, 5, 5) == (ownLayer ? "64 0 0 64" : "128 0 0 128"));
        // A scroll of the document moves nothing of it, and records nothing.
        prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 10));
        std::vector<uint8_t> scrolled = render(f.tree, 100, 100);
        CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
        CHECK(pixel(scrolled, 100, 5, 5) == (ownLayer ? "64 0 0 64" : "128 0 0 128"));
        CHECK(verified(f.tree));
    }
}

TEST_CASE("a kept recording replayed inside one being made passes on the boxes out of flow it holds") {
    // A holder of a fixed content width holds a red box at its right, inside static wrappers. It is recorded once, then
    // with only the outer wrapper changed, then with the holder's padding grown, which moves the box and keeps the
    // wrappers.
    for (bool runs : {false, true}) {
        CAPTURE(runs);
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutBoxStyle holderStyle = positioned(sized(block(), 100, 100), PrLayoutPosition_Relative);
        PrLayoutNode holder = f.element(root, holderStyle);
        PrLayoutNode outer = f.element(holder, block());
        PrLayoutNode inner = outer;
        if (runs) {
            for (uint32_t i = 0; i < 40; ++i) {
                PrLayoutNode child = f.element(outer, block(1));
                if (i == 4)
                    inner = child;
            }
        } else {
            inner = f.element(outer, block());
        }
        PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
        f.element(inner, inset(square(), 0, 0, kAuto, kAuto), &red);
        f.layout(px(200, 120));
        CHECK(pixel(render(f.tree, 200, 120), 200, 90, 10) == "255 0 0 255");
        PrLayoutDecorationStyle blue = background(PrColor{0, 0, 1, 1});
        prLayoutNodeSetStyle(f.tree, outer, f.style(block(), &blue));
        f.layout(px(200, 120));
        CHECK(pixel(render(f.tree, 200, 120), 200, 90, 10) == "255 0 0 255");
        holderStyle.padding[PrRectSide_Right] = prLayoutLengthPx(50);
        prLayoutNodeSetStyle(f.tree, holder, f.style(holderStyle));
        f.layout(px(200, 120));
        std::vector<uint8_t> moved = render(f.tree, 200, 120);
        CHECK(pixel(moved, 200, 140, 10) == "255 0 0 255");
        CHECK(pixel(moved, 200, 90, 10) != "255 0 0 255");
        CHECK(verified(f.tree));
    }
}

TEST_CASE("a query finds a box out of flow where its holder placed it, and one beside many placeholders where it is") {
    Stage s(true);
    PrLayoutNode crowd = s.div(s.stage);
    for (uint32_t i = 0; i < 50; ++i)
        s.add(crowd, inset(square(), float(i), kAuto, kAuto, 0));
    PrLayoutNode quiet = s.div(s.stage, block(), "a");
    PrLayoutNode inner = s.add(s.div(s.stage), inset(square(), 30, kAuto, kAuto, 40));
    s.layout();
    CHECK(s.left(inner) == 40);
    CHECK(s.top(inner) == 30);
    CHECK(s.top(quiet) == 0);
    CHECK(verified(s.f.tree));
}

// ---------------------------------------------------------------------------------------------------------------------
// Changes

TEST_CASE("a change inside a box out of flow lays out the box and its holder, and reuses the blocks between") {
    Stage s(true);
    PrLayoutNode outer = s.div(s.stage);
    PrLayoutNode middle = s.div(outer);
    for (uint32_t i = 0; i < 4; ++i)
        s.div(middle, block(10));
    PrLayoutNode box = s.add(middle, inset(absolute(), 0, kAuto, kAuto, 0));
    PrLayoutNode inside = s.text(box, "a");
    s.layout();
    prLayoutNodeSetText(s.f.tree, inside, "abc");
    s.layout();
    PrLayoutTreeStats stats = prLayoutTreeStats(s.f.tree);
    // The root, the stage that holds it, and the box are laid out; the static blocks between are reused.
    CHECK(stats.blocksLaidOut == 3);
    CHECK_NEAR(s.width(box), 32.25);
    // The layout leaves no node marked, not even the nodes between the box and its holder that it did not reach.
    const LayoutTree* tree = s.f.tree;
    for (uint32_t i = 0; i < tree->nodes.count; ++i)
        CHECK(tree->nodes.data[i].dirty == 0);
}

TEST_CASE("a change of what holds a box out of flow lays out the holders before and after") {
    Stage s(true);
    PrLayoutBoxStyle inner = sized(block(), 100, 50);
    inner.margin[PrRectSide_Left] = prLayoutLengthPx(30);
    PrLayoutNode holder = s.add(s.stage, inner);
    PrLayoutNode box = s.add(holder, inset(square(), 0, 0, kAuto, kAuto));
    s.layout();
    CHECK(s.left(box) == 180);
    prLayoutNodeSetStyle(s.f.tree, holder,
        s.f.style(positioned(inner, PrLayoutPosition_Relative), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.left(box) == 110);
    prLayoutNodeSetStyle(s.f.tree, holder, s.f.style(inner, nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.left(box) == 180);
    prLayoutNodeSetStyle(s.f.tree, holder, s.f.style(transformed(inner), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.left(box) == 110);
}

TEST_CASE("a transform appearing or going lays out nothing where nothing out of flow is in its box") {
    Stage s(true);
    PrLayoutBoxStyle card = sized(block(), 100, 50);
    PrLayoutNode quiet = s.add(s.stage, card);
    s.text(s.div(quiet), "a");
    PrLayoutNode holder = s.add(s.stage, card);
    PrLayoutNode box = s.add(holder, inset(square(), 0, 0, kAuto, kAuto));
    s.layout();
    prLayoutNodeSetStyle(s.f.tree, quiet, s.f.style(transformed(card), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(prLayoutTreeStats(s.f.tree).blocksLaidOut == 0);
    prLayoutNodeSetStyle(s.f.tree, quiet, s.f.style(card, nullptr, nullptr, &s.style));
    s.layout();
    CHECK(prLayoutTreeStats(s.f.tree).blocksLaidOut == 0);
    // A box out of flow in it moves to it.
    CHECK(s.left(box) == 180);
    prLayoutNodeSetStyle(s.f.tree, holder, s.f.style(transformed(card), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.left(box) == 80);
}

TEST_CASE("a change of what a placeholder reads lays out its parent, through a static block kept between") {
    Stage s(true);
    PrLayoutNode wrapper = s.div(s.stage);
    PrLayoutNode inner = s.div(wrapper);
    s.text(inner, "abc");
    PrLayoutNode box = s.add(inner, square(true));
    s.layout();
    CHECK_NEAR(s.left(box), 32.25);
    CHECK(s.top(box) == 0);
    prLayoutNodeSetStyle(s.f.tree, box, s.f.style(square(false), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.left(box) == 0);
    CHECK(s.top(box) == 40);
    PrLayoutBoxStyle none = square(false);
    none.display = PrLayoutDisplay_None;
    prLayoutNodeSetStyle(s.f.tree, box, s.f.style(none, nullptr, nullptr, &s.style));
    s.layout();
    CHECK(prLayoutNodeBoxes(s.f.tree, box, PrLayoutCoordinates_Layout, nullptr, 0) == 0);
    prLayoutNodeSetStyle(s.f.tree, box, s.f.style(square(true), nullptr, nullptr, &s.style));
    s.layout();
    CHECK_NEAR(s.left(box), 32.25);
}

TEST_CASE("content before a placeholder, a holder's size, insets, position and the viewport each move a box") {
    Stage s(true);
    PrLayoutNode word = s.text(s.stage, "a");
    PrLayoutNode inline_ = s.add(s.stage, square(true));
    PrLayoutNode corner = s.add(s.stage, inset(square(), kAuto, 0, 0, kAuto));
    PrLayoutBoxStyle fixedBox = inset(square(), kAuto, 0, 0, kAuto);
    fixedBox.position = PrLayoutPosition_Fixed;
    PrLayoutNode fixed = s.add(s.stage, fixedBox);
    PrLayoutNode filler = s.div(s.stage, block(40));
    s.layout();
    double before = s.left(inline_);
    prLayoutNodeSetText(s.f.tree, word, "abc");
    s.layout();
    CHECK(s.left(inline_) > before);
    CHECK(s.top(corner) == 60);
    prLayoutNodeSetStyle(s.f.tree, filler, s.f.style(block(80), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.top(corner) == 100);
    prLayoutNodeSetStyle(s.f.tree, corner, s.f.style(inset(square(), kAuto, 10, 0, kAuto), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.left(corner) == 170);
    PrLayoutBoxStyle moved = inset(square(), kAuto, 10, 0, kAuto);
    moved.position = PrLayoutPosition_Fixed;
    prLayoutNodeSetStyle(s.f.tree, corner, s.f.style(moved, nullptr, nullptr, &s.style));
    s.layout(400, 300);
    checkRect(s.f.bounds(corner), 370, 280, 390, 300);
    s.layout(500, 200);
    checkRect(s.f.bounds(fixed), 480, 180, 500, 200);
    // Back in flow, it takes room before the filler.
    prLayoutNodeSetStyle(s.f.tree, corner, s.f.style(block(10), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.top(corner) == 40);
    CHECK(s.top(filler) == 50);
}

TEST_CASE("a fixed box in an absolute box, both held by the viewport, and a cached absolute box still lists it") {
    Stage s;
    PrLayoutNode abs = s.add(s.stage, inset(sized(absolute(), 50, 50), 10, kAuto, kAuto, 10));
    PrLayoutBoxStyle fixedBox = inset(square(), 0, kAuto, kAuto, 0);
    fixedBox.position = PrLayoutPosition_Fixed;
    PrLayoutNode fixed = s.add(abs, fixedBox);
    PrLayoutNode sibling = s.div(s.stage, block(), "a");
    s.layout();
    checkRect(s.f.bounds(fixed), 0, 0, 20, 20);
    // A change elsewhere keeps the absolute box's result, which still lists the fixed box.
    prLayoutNodeSetStyle(s.f.tree, sibling, s.f.style(block(30), nullptr, nullptr, &s.style));
    s.layout();
    checkRect(s.f.bounds(fixed), 0, 0, 20, 20);
    checkRect(s.f.bounds(abs), 10, 10, 60, 60);
}

// ---------------------------------------------------------------------------------------------------------------------
// Combinations

TEST_CASE("a transformed block, a relative block, a clipping static block and nested relative spans hold their own") {
    // T holds the fixed boxes F and G, R holds the absolute box B, and S lays out A against Q, the innermost positioned
    // span around A. S clips A, whose containing block is inside S, and does not clip F, whose holder is outside.
    Stage s;
    // T's padding keeps R's margin from collapsing through it.
    PrLayoutBoxStyle tStyle = transformed(block());
    tStyle.padding[PrRectSide_Top] = prLayoutLengthPx(5);
    PrLayoutNode t = s.add(s.stage, tStyle);
    PrLayoutBoxStyle relative = positioned(block(), PrLayoutPosition_Relative);
    relative.margin[PrRectSide_Top] = prLayoutLengthPx(10);
    PrLayoutNode r = s.add(t, relative);
    PrLayoutBoxStyle clipped = block();
    clipped.overflowX = PrLayoutOverflow_Hidden;
    clipped.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutNode sNode = s.add(r, clipped);
    s.text(sNode, "text ");
    PrLayoutNode p = s.add(sNode, positioned(inlineBox(), PrLayoutPosition_Relative));
    PrLayoutNode before = s.text(p, "p ");
    PrLayoutNode q = s.add(p, positioned(inlineBox(), PrLayoutPosition_Relative));
    s.text(q, "q");
    PrLayoutNode a = s.add(q, inset(sized(absolute(block(), true), 40, 40), 30, kAuto, kAuto, 0));
    PrLayoutBoxStyle fixedBox = inset(square(true), 60, 0, kAuto, kAuto);
    fixedBox.position = PrLayoutPosition_Fixed;
    PrLayoutNode fixed = s.add(q, fixedBox);
    PrLayoutBoxStyle fixedTop = inset(square(true), 0, kAuto, kAuto, kAuto);
    fixedTop.position = PrLayoutPosition_Fixed;
    PrLayoutNode g = s.add(a, fixedTop);
    PrLayoutBoxStyle half = inset(square(), 0, kAuto, kAuto, kAuto);
    half.inset[PrRectSide_Left] = prLayoutLengthPercent(50);
    PrLayoutNode b = s.add(sNode, half);
    s.layout();
    auto checkPlaces = [&](double bTop) {
        std::vector<std::vector<double>> parts = s.rects(q);
        REQUIRE(parts.size() == 1);
        CHECK_NEAR(s.left(a), parts[0][0]);
        CHECK_NEAR(s.top(a), parts[0][1] + 30);
        CHECK(s.left(fixed) == 180);
        CHECK(s.top(fixed) == 60);
        CHECK(s.top(g) == 0);
        CHECK_NEAR(s.left(g), s.left(a));
        CHECK(s.left(b) == 100);
        CHECK(s.top(b) == bTop);
    };
    checkPlaces(15);
    // S clips A below its line, and not F.
    CHECK(s.hit(float(s.left(a)) + 5, 65) != a);
    CHECK(s.hit(190, 70) == fixed);

    // A change inside G lays out T and G, and reuses R, S and A.
    PrLayoutBoxStyle grown = inset(sized(absolute(block(), true), 30, 30), 0, kAuto, kAuto, kAuto);
    grown.position = PrLayoutPosition_Fixed;
    prLayoutNodeSetStyle(s.f.tree, g, s.f.style(grown, nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.width(g) == 30);
    PrLayoutTreeStats stats = prLayoutTreeStats(s.f.tree);
    CHECK(stats.blocksLaidOut == 4);
    checkPlaces(15);

    // Text before Q moves Q's part, and A with it, and G's static position in A.
    prLayoutNodeSetText(s.f.tree, before, "ppp ");
    s.layout();
    checkPlaces(15);

    // R is static: T holds B, at T's own top.
    PrLayoutBoxStyle unpositioned = block();
    unpositioned.margin[PrRectSide_Top] = prLayoutLengthPx(10);
    prLayoutNodeSetStyle(s.f.tree, r, s.f.style(unpositioned, nullptr, nullptr, &s.style));
    s.layout();
    checkPlaces(0);
}

TEST_CASE("a positioned span around a block holds what the block passes up, and the block holds it once positioned") {
    Stage s(true);
    PrLayoutBoxStyle spanStyle = positioned(inlineBox(), PrLayoutPosition_Relative);
    spanStyle.padding[PrRectSide_Left] = prLayoutLengthPx(10);
    PrLayoutNode span = s.add(s.stage, spanStyle);
    s.text(span, "a");
    PrLayoutBoxStyle inner = block(20);
    inner.margin[PrRectSide_Left] = prLayoutLengthPx(30);
    PrLayoutNode blockNode = s.add(span, inner);
    s.text(span, "b");
    PrLayoutNode box = s.add(blockNode, inset(square(), 0, kAuto, kAuto, 0));
    auto setBlock = [&](const PrLayoutBoxStyle& style) {
        prLayoutNodeSetStyle(s.f.tree, blockNode, s.f.style(style, nullptr, nullptr, &s.style));
        s.layout();
    };
    auto heldBySpan = [&]() {
        std::vector<std::vector<double>> parts = s.rects(span);
        REQUIRE(!parts.empty());
        CHECK_NEAR(s.left(box), parts[0][0]);
        CHECK_NEAR(s.top(box), parts[0][1]);
    };
    auto heldByBlock = [&]() {
        CHECK(s.left(box) == s.left(blockNode));
        CHECK(s.top(box) == s.top(blockNode));
    };
    s.layout();
    heldBySpan();
    setBlock(positioned(inner, PrLayoutPosition_Relative));
    heldByBlock();
    setBlock(transformed(inner));
    heldByBlock();
    setBlock(inner);
    heldBySpan();
    // With the span static, the stage holds the box, at its corner.
    prLayoutNodeSetStyle(s.f.tree, span,
        s.f.style(positioned(spanStyle, PrLayoutPosition_Static), nullptr, nullptr, &s.style));
    s.layout();
    CHECK(s.left(box) == 0);
    CHECK(s.top(box) == 0);
    prLayoutNodeSetStyle(s.f.tree, span, s.f.style(spanStyle, nullptr, nullptr, &s.style));
    s.layout();
    heldBySpan();
}

TEST_CASE("an inline box's containing block moves with its relative offset, and is visual across bidi") {
    SUBCASE("moved relatively") {
        for (bool moved : {false, true}) {
            CAPTURE(moved);
            Stage s;
            s.text(s.stage, "x");
            PrLayoutBoxStyle spanStyle = positioned(inlineBox(), PrLayoutPosition_Relative);
            if (moved)
                spanStyle = inset(spanStyle, 10, kAuto, kAuto, 10);
            PrLayoutNode span = s.add(s.stage, spanStyle);
            s.text(span, "yy");
            PrLayoutNode box = s.add(span, inset(absolute(), 0, 0, 0, 0));
            s.layout();
            CHECK(s.left(box) == 10 + (moved ? 10 : 0));
            CHECK(s.top(box) == 9 + (moved ? 10 : 0));
            CHECK(s.width(box) == 20);
        }
    }
    SUBCASE("Latin then Hebrew in an rtl line: from its visually first part to its last") {
        Stage s(false, -1, PrTextDirection_Rtl);
        s.text(s.stage, "א ");
        PrLayoutNode span = s.add(s.stage, positioned(inlineBox(), PrLayoutPosition_Relative));
        s.text(span, "abc אבג");
        PrLayoutBoxStyle covering = inset(absolute(), 30, 0, 0, 0);
        PrLayoutNode box = s.add(span, covering);
        s.text(s.stage, " ב");
        s.layout();
        CHECK(std::abs(s.left(box) - 111.09) < 0.05);
        CHECK(std::abs(s.width(box) - 70.79) < 0.05);
    }
}

TEST_CASE("a stretched size clamps through either box-sizing, never below the box's padding and borders") {
    Stage s(true, 80);
    auto stretched = [&](PrLayoutBoxSizing sizing, float min, float max) {
        PrLayoutBoxStyle box = padded(bordered(inset(sized(absolute(), 40, -1), 0, kAuto, 0, kAuto), 5), 10);
        box.boxSizing = sizing;
        if (min >= 0)
            box.minHeight = prLayoutLengthPx(min);
        if (max >= 0)
            box.maxHeight = prLayoutLengthPx(max);
        return s.add(s.stage, box);
    };
    PrLayoutNode contentMax = stretched(PrLayoutBoxSizing_ContentBox, -1, 30);
    PrLayoutNode borderMax = stretched(PrLayoutBoxSizing_BorderBox, -1, 40);
    PrLayoutNode contentMin = stretched(PrLayoutBoxSizing_ContentBox, 100, -1);
    PrLayoutNode borderMin = stretched(PrLayoutBoxSizing_BorderBox, 100, -1);
    PrLayoutNode floored = stretched(PrLayoutBoxSizing_BorderBox, -1, 10);
    s.layout();
    CHECK(s.height(contentMax) == 60);
    CHECK(s.height(borderMax) == 40);
    CHECK(s.height(contentMin) == 130);
    CHECK(s.height(borderMin) == 100);
    CHECK(s.height(floored) == 30);
}

TEST_CASE("a scene's input finds an escaped scroll container and slot outside the clip of the layer they paint in") {
    // A holder outside a 40px scroll container holds a box in a fading layer inside it, at 60, 60. The container is a
    // scroll container or a slot of embedded content.
    for (bool slot : {false, true}) {
        CAPTURE(slot);
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutNode holder = f.element(root, positioned(sized(block(), 100, 100), PrLayoutPosition_Relative));
        PrLayoutBoxStyle scroller = sized(block(), 40, 40);
        scroller.overflowX = PrLayoutOverflow_Scroll;
        scroller.overflowY = PrLayoutOverflow_Scroll;
        PrLayoutNode container = f.element(holder, scroller);
        PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
        faded.opacity = 0.5f;
        PrLayoutNode layer = f.element(container, block(100), nullptr, &faded);
        PrLayoutBoxStyle popupStyle = inset(sized(absolute(), 30, 30), 60, kAuto, kAuto, 60);
        PrLayoutNode popup = kPrLayoutNodeNone;
        if (slot) {
            popup = replaced(f, layer, popupStyle, prLayoutNaturalSizeNone());
            PrLayoutContent embedded{};
            embedded.kind = PrLayoutContentKind_Embed;
            embedded.embed = 7;
            prLayoutNodeSetContent(f.tree, popup, &embedded);
        } else {
            popupStyle.overflowY = PrLayoutOverflow_Scroll;
            popup = f.element(layer, popupStyle);
            f.element(popup, block(100));
        }
        f.layout(px(100, 100));
        CHECK(verified(f.tree));
        const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
        const LayoutScene* s = scene;
        Array<PrLayoutVec> offsets;
        layoutSceneScrollOffsets(s, nullptr, 0, offsets);
        LayoutScrollHit hit = layoutSceneScrollAt(s, px(75, 75), nullptr, 0, offsets.data);
        if (slot) {
            REQUIRE(hit.slot != kPaintScrollNone);
            CHECK(s->slots.data[hit.slot].node == popup);
            CHECK(hit.point == px(15, 15));
        } else {
            REQUIRE(hit.scroll != kPaintScrollNone);
            CHECK(s->scrolls.data[hit.scroll].node == popup);
        }
        // Inside the container's clip, the hit is the container.
        LayoutScrollHit inside = layoutSceneScrollAt(s, px(20, 20), nullptr, 0, offsets.data);
        REQUIRE(inside.scroll != kPaintScrollNone);
        CHECK(s->scrolls.data[inside.scroll].node == container);
        offsets.reset();
        prLayoutSceneRelease(scene);
    }
}

TEST_CASE("a recording that escapes stays current as a layer appears and goes above it, and as a clip between goes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode holder = f.element(root, positioned(sized(block(), 100, 100), PrLayoutPosition_Relative));
    PrLayoutBoxStyle middleStyle = block();
    PrLayoutNode middle = f.element(holder, middleStyle);
    PrLayoutBoxStyle clipStyle = sized(block(), 40, 40);
    clipStyle.overflowX = PrLayoutOverflow_Hidden;
    clipStyle.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutNode clip = f.element(middle, clipStyle);
    PrLayoutNode wrapper = f.element(clip, block());
    // Enough boxes that the wrapper's recording is cached.
    for (uint32_t i = 0; i < 40; ++i)
        f.element(wrapper, block(1));
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    f.element(wrapper, inset(sized(absolute(), 20, 20), 60, kAuto, kAuto, 60), &red);
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    f.element(clip, sized(block(), 100, 10), &green);
    f.layout(px(100, 100));
    CHECK(pixel(render(f.tree, 100, 100), 100, 70, 70) == "255 0 0 255");

    // An opacity layer appears above the clip, and then goes.
    PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
    faded.opacity = 0.5f;
    prLayoutNodeSetStyle(f.tree, middle, f.style(middleStyle, nullptr, &faded));
    f.layout(px(100, 100));
    CHECK(pixel(render(f.tree, 100, 100), 100, 70, 70) == "128 0 0 128");
    CHECK(verified(f.tree));
    prLayoutNodeSetStyle(f.tree, middle, f.style(middleStyle));
    f.layout(px(100, 100));
    CHECK(pixel(render(f.tree, 100, 100), 100, 70, 70) == "255 0 0 255");
    CHECK(verified(f.tree));

    // The clip is removed: what spilled out of it shows, and the escaped box stays.
    CHECK(pixel(render(f.tree, 100, 100), 100, 70, 45) == "0 0 0 0");
    prLayoutNodeSetStyle(f.tree, clip, f.style(sized(block(), 40, 40)));
    f.layout(px(100, 100));
    std::vector<uint8_t> unclipped = render(f.tree, 100, 100);
    CHECK(pixel(unclipped, 100, 70, 70) == "255 0 0 255");
    CHECK(pixel(unclipped, 100, 70, 45) == "0 255 0 255");
    CHECK(verified(f.tree));
}
