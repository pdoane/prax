#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/src/layout_stacking.h"
#include "layout/src/layout_tree.h"
#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

#include <cmath>
#include <vector>

namespace pr_impl {

namespace {

// Stacking (CSS2 Appendix E, css-position-4/#painting-order): stack levels, the stacking contexts that contain them,
// tree order as the logical order of a line's fragments, inline boxes that paint whole across their pieces, and what
// hit testing, painting and changes do with them. Each case is a stage 200px wide, a relative flow-root of 20px Arimo
// on 40px lines that is a stacking context unless the case says otherwise, and checks the engine's numbers for that
// stage.

// Returns a font for 20px text on 40px lines, in `direction`.
PrLayoutFontStyle font(PrTextDirection direction = PrTextDirection_Ltr) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(20);
    font.lineHeight = prLayoutLineHeightPx(40);
    font.direction = direction;
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

PrLayoutBoxStyle positioned(PrLayoutBoxStyle box, PrLayoutPosition position = PrLayoutPosition_Relative) {
    box.position = position;
    return box;
}

// Returns an absolutely positioned block at `left`, `top`, `width` by `height`.
PrLayoutBoxStyle absolute(float left, float top, float width, float height) {
    PrLayoutBoxStyle box = positioned(sized(block(), width, height), PrLayoutPosition_Absolute);
    box.inset[PrRectSide_Left] = prLayoutLengthPx(left);
    box.inset[PrRectSide_Top] = prLayoutLengthPx(top);
    return box;
}

// Returns an inline-block at the top of its line.
PrLayoutBoxStyle inlineBlock(float width, float height) {
    PrLayoutBoxStyle box = sized(prLayoutBoxStyleDefault(), width, height);
    box.display = PrLayoutDisplay_InlineBlock;
    box.baselineShift = prLayoutBaselineShiftKeyword(PrLayoutKeyword_Top);
    return box;
}

PrLayoutBoxStyle floatLeft(float width, float height) {
    PrLayoutBoxStyle box = sized(block(), width, height);
    box.floating = PrLayoutFloat_Left;
    return box;
}

// zIndexed returns a box with a z-index other than auto, and level returns the effects that give its z-index.
PrLayoutBoxStyle zIndexed(PrLayoutBoxStyle box) {
    box.zIndexed = 1;
    return box;
}

PrLayoutEffectStyle level(int32_t z, float opacity = 1) {
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.zIndex = z;
    effects.opacity = opacity;
    return effects;
}

PrLayoutEffectStyle faded(float opacity) {
    return level(0, opacity);
}

// A tree whose root holds a stage: a flow-root 200px wide, `height` high if that is not negative, relative at z-index 0
// if `context` says, in the font of `direction`.
struct Stage {
    Fixture f;
    PrLayoutFontStyle style;
    PrLayoutNode root = kPrLayoutNodeNone;
    PrLayoutNode stage = kPrLayoutNodeNone;

    explicit Stage(float height = -1, bool context = true, PrTextDirection direction = PrTextDirection_Ltr) {
        style = font(direction);
        root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
        PrLayoutBoxStyle box = sized(block(), 200, height);
        box.display = PrLayoutDisplay_FlowRoot;
        if (context) {
            box = zIndexed(positioned(box));
            PrLayoutEffectStyle zero = level(0);
            stage = f.element(root, box, nullptr, &zero, &style);
        } else {
            stage = f.element(root, box, nullptr, nullptr, &style);
        }
    }

    // Creates an element under `parent` styled by `box`, with `effects` if given, in the stage's font.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutEffectStyle* PR_NULLABLE effects = nullptr) {
        return f.element(parent, box, nullptr, effects, &style);
    }
    // Creates an element at z-index `z`.
    PrLayoutNode at(PrLayoutNode parent, const PrLayoutBoxStyle& box, int32_t z) {
        PrLayoutEffectStyle effects = level(z);
        return add(parent, zIndexed(box), &effects);
    }
    PrLayoutNode text(PrLayoutNode parent, const char* value) { return f.text(parent, value); }

    // Sets the style of `node`.
    void set(PrLayoutNode node, const PrLayoutBoxStyle& box, const PrLayoutEffectStyle* PR_NULLABLE effects = nullptr) {
        prLayoutNodeSetStyle(f.tree, node, f.style(box, nullptr, effects, &style));
    }

    // Lays the tree out, and checks the incremental work against a layout from scratch.
    void layout() {
        f.layout(px(400, 400));
        CHECK(verified(f.tree));
    }

    PrRect stageBounds() { return f.bounds(stage); }

    // Returns the topmost element at a point relative to the stage's top left, using a text node's parent for the text
    // node.
    PrLayoutNode hit(float x, float y) {
        PrRect origin = stageBounds();
        PrLayoutNode node = f.hit(PrVec2{origin.left + x, origin.top + y});
        if (node != kPrLayoutNodeNone && prLayoutNodeKind(f.tree, node) == PrLayoutNodeKind_Text)
            return prLayoutNodeParent(f.tree, node);
        return node;
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

    // Returns the pixels of the tree rendered at 400 by 400.
    std::vector<uint8_t> pixels() { return render(f.tree, 400, 400); }
    // Returns the color of the pixel at a point relative to the stage's top left.
    std::string color(const std::vector<uint8_t>& bytes, float x, float y) {
        PrRect origin = stageBounds();
        return pixel(bytes, 400, uint32_t(origin.left + x), uint32_t(origin.top + y));
    }
};

PrLayoutDecorationStyle red() {
    return background(PrColor{1, 0, 0, 1});
}
PrLayoutDecorationStyle blue() {
    return background(PrColor{0, 0, 1, 1});
}

// ---------------------------------------------------------------------------------------------------------------------
// Levels and contexts

// Returns the boxes of the list of the stacking context `context`, in order.
std::vector<PrLayoutNode> listOf(Stage& s, PrLayoutNode context) {
    const LayoutTree* tree = s.f.tree;
    LayoutStackedBoxes list = layoutStackingList(tree, layoutNodeIndex(tree, context));
    std::vector<PrLayoutNode> nodes;
    for (uint32_t i = 0; i < list.count; ++i)
        nodes.push_back(layoutNodeName(tree, list.boxes[i].node));
    return nodes;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a stacking context lists the boxes that paint whole in it by level and tree order, through those at auto") {
    Stage s;
    PrLayoutNode first = s.at(s.stage, positioned(block()), 1);
    PrLayoutNode container = s.add(s.stage, positioned(block()));
    PrLayoutNode under = s.at(container, positioned(block()), -1);
    PrLayoutNode inner = s.add(container, positioned(block()));
    PrLayoutNode context = s.at(s.stage, positioned(block()), 2);
    PrLayoutNode contained = s.add(context, positioned(block()));
    PrLayoutBoxStyle hidden = positioned(block());
    hidden.display = PrLayoutDisplay_None;
    s.add(s.stage, hidden);
    s.layout();
    // The boxes in the positioned box at auto belong to its context, after it; those in the context at 2 are its own.
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{under, container, inner, first, context});
    CHECK(listOf(s, context) == std::vector<PrLayoutNode>{contained});
    CHECK(verified(s.f.tree));

    // Each change below moves boxes in the lists the tree holds, and finds none again.
    auto found = [&] { return prLayoutTreeStats(s.f.tree).stackingListsFound; };
    // A box that stops painting whole leaves the list, one inserted joins it in tree order, and one removed leaves it.
    s.set(inner, block());
    PrLayoutNode added = s.add(s.stage, positioned(block()));
    prLayoutTreeRemove(s.f.tree, first);
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{under, container, added, context});
    CHECK(found() == 0);
    // A box removed alone leaves the list.
    prLayoutTreeRemove(s.f.tree, added);
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{under, container, context});
    prLayoutTreeInsert(s.f.tree, s.stage, added, kPrLayoutNodeNone);
    // A context at auto again lists nothing of its own, and its boxes belong to its context.
    s.set(context, positioned(block()));
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{under, container, context, contained, added});
    CHECK(found() == 0);
    CHECK(verified(s.f.tree));
    // A box inserted before another at its level goes before it in tree order, and a box inserted inside one at auto
    // goes after that box.
    PrLayoutStyleIndex positionedStyle = s.f.style(positioned(block()), nullptr, nullptr, &s.style);
    PrLayoutNode between = prLayoutNodeCreateElement(s.f.tree, kPrLayoutNodeNone, positionedStyle);
    prLayoutTreeInsert(s.f.tree, s.stage, between, context);
    PrLayoutNode nested = s.add(container, positioned(block()));
    s.layout();
    std::vector<PrLayoutNode> all{under, container, nested, between, context, contained, added};
    CHECK(listOf(s, s.stage) == all);
    // A level that changes moves the box among the levels, and back to its place in tree order at 0.
    s.set(container, zIndexed(positioned(block())));
    PrLayoutEffectStyle three = level(3);
    s.set(between, zIndexed(positioned(block())), &three);
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{container, context, contained, added, between});
    CHECK(listOf(s, container) == std::vector<PrLayoutNode>{under, nested});
    s.set(between, positioned(block()));
    s.set(container, positioned(block()));
    s.layout();
    CHECK(listOf(s, s.stage) == all);
    CHECK(found() == 0);
    CHECK(verified(s.f.tree));
}

TEST_CASE("a box that turns between block and flex moves its children in the lists once") {
    Stage s;
    // A context at 1 holding positioned boxes, which turns into a static flex container, and back.
    PrLayoutNode container = s.at(s.stage, positioned(block()), 1);
    PrLayoutNode child = s.add(container, positioned(block()));
    PrLayoutNode grandchild = s.add(child, positioned(block()));
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{container});
    CHECK(listOf(s, container) == std::vector<PrLayoutNode>{child, grandchild});
    PrLayoutBoxStyle flex = block();
    flex.display = PrLayoutDisplay_Flex;
    s.set(container, flex);
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{child, grandchild});
    s.set(container, zIndexed(positioned(block())));
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{container});
    CHECK(listOf(s, container) == std::vector<PrLayoutNode>{child, grandchild});
    // A positioned box at auto, which is in its context's list with its children, turns into flex and back.
    PrLayoutBoxStyle positionedFlex = positioned(block());
    positionedFlex.display = PrLayoutDisplay_Flex;
    s.set(container, positionedFlex);
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{container, child, grandchild});
    s.set(container, positioned(block()));
    s.layout();
    CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{container, child, grandchild});
    CHECK(prLayoutTreeStats(s.f.tree).stackingListsFound == 0);
}

TEST_CASE("boxes paint by stack level, then in tree order, auto and 0 at one level") {
    SUBCASE("three levels") {
        Stage s(70);
        PrLayoutNode a = s.at(s.stage, absolute(0, 0, 60, 60), 3);
        PrLayoutNode b = s.at(s.stage, absolute(40, 0, 60, 30), 1);
        PrLayoutNode c = s.at(s.stage, absolute(40, 20, 60, 40), 2);
        s.layout();
        CHECK(s.hit(50, 10) == a);
        CHECK(s.hit(80, 25) == c);
        CHECK(s.hit(50, 50) == a);
        CHECK(s.hit(80, 10) == b);
    }
    SUBCASE("auto and 0") {
        Stage s(60);
        s.at(s.stage, absolute(10, 0, 60, 40), 0);
        PrLayoutNode b = s.add(s.stage, absolute(40, 0, 60, 40));
        PrLayoutNode c = s.at(s.stage, absolute(70, 0, 60, 40), 0);
        s.layout();
        CHECK(s.hit(55, 20) == b);
        CHECK(s.hit(85, 20) == c);
    }
    SUBCASE("equal levels in tree order, most negative first and smallest positive first") {
        Stage s(60);
        PrLayoutNode first = s.at(s.stage, absolute(0, 0, 60, 40), 2);
        PrLayoutNode second = s.at(s.stage, absolute(20, 0, 60, 40), 2);
        PrLayoutNode low = s.at(s.stage, absolute(40, 0, 60, 40), -2);
        PrLayoutNode high = s.at(s.stage, absolute(60, 0, 60, 40), -1);
        s.layout();
        CHECK(s.hit(30, 20) == second);
        CHECK(s.hit(10, 20) == first);
        CHECK(s.hit(70, 20) == second);
        CHECK(s.hit(90, 20) == high);
        (void)low;
    }
    SUBCASE("the largest integers, clamped, at one level in tree order") {
        Stage s(60);
        s.at(s.stage, absolute(10, 0, 60, 40), INT32_MAX);
        PrLayoutNode b = s.at(s.stage, absolute(40, 0, 60, 40), INT32_MAX);
        s.layout();
        CHECK(s.hit(55, 20) == b);
    }
}

TEST_CASE("a box at a negative level paints over its context's background and under its flow") {
    SUBCASE("under a block of the context") {
        Stage s(80);
        PrLayoutNode n = s.at(s.stage, absolute(0, 0, 100, 70), -1);
        PrLayoutNode b = s.add(s.stage, block(40));
        s.layout();
        CHECK(s.hit(30, 20) == b);
        CHECK(s.hit(30, 50) == n);
    }
    SUBCASE("under the context's text") {
        Stage s;
        PrLayoutNode text = s.text(s.stage, "xxxxxx");
        s.at(s.stage, absolute(0, 0, 100, 40), -1);
        s.layout();
        CHECK(s.f.hit(PrVec2{s.stageBounds().left + 10, s.stageBounds().top + 20}) == text);
    }
    SUBCASE("under a float") {
        Stage s(60);
        PrLayoutNode fl = s.add(s.stage, floatLeft(60, 40));
        s.at(s.stage, absolute(0, 0, 100, 50), -1);
        s.layout();
        CHECK(s.hit(20, 20) == fl);
    }
    SUBCASE("under its relative parent's background at auto, and over it at 0") {
        Stage s(100);
        PrLayoutNode p = s.add(s.stage, positioned(block(40)));
        PrLayoutNode pc = s.at(p, absolute(0, 0, 60, 30), -1);
        PrLayoutBoxStyle second = positioned(block(40));
        second.margin[PrRectSide_Top] = prLayoutLengthPx(10);
        PrLayoutNode q = s.at(s.stage, second, 0);
        PrLayoutNode qc = s.at(q, absolute(0, 0, 60, 30), -1);
        s.layout();
        CHECK(s.hit(20, 20) == p);
        CHECK(s.hit(20, 70) == qc);
        (void)pc;
    }
    SUBCASE("in an inline-block, under a later block pulled up over it") {
        Stage s(80);
        PrLayoutNode ib = s.add(s.stage, inlineBlock(60, 40));
        s.at(ib, absolute(0, 0, 120, 40), -1);
        PrLayoutBoxStyle later = block(40);
        later.margin[PrRectSide_Top] = prLayoutLengthPx(-30);
        PrLayoutNode b = s.add(s.stage, later);
        s.layout();
        CHECK(s.hit(90, 20) == b);
    }
    SUBCASE("deep in a block after one it reaches under") {
        Stage s(80);
        PrLayoutNode first = s.add(s.stage, block(40));
        PrLayoutNode deep = s.add(s.add(s.stage, block()), block());
        s.at(deep, absolute(0, 0, 60, 60), -1);
        s.layout();
        CHECK(s.hit(20, 20) == first);
    }
}

TEST_CASE("a stacking context contains its descendants' levels, and a positioned box at auto does not") {
    for (bool context : {true, false}) {
        CAPTURE(context);
        Stage s(90);
        PrLayoutBoxStyle parentBox = absolute(20, 10, 110, 80);
        PrLayoutNode parent = context ? s.at(s.stage, parentBox, 1) : s.add(s.stage, parentBox);
        PrLayoutNode child = s.at(parent, absolute(30, 20, 90, 30), 100);
        PrLayoutNode sibling = s.at(s.stage, absolute(90, 20, 90, 50), 2);
        s.layout();
        CHECK(s.hit(100, 40) == (context ? sibling : child));
    }
}

TEST_CASE("z-index applies to positioned boxes alone, and the boxes that make contexts contain their descendants") {
    SUBCASE("a static block at 10, of opacity or not, under a relative block after it") {
        for (float opacity : {1.0f, 0.9f}) {
            CAPTURE(opacity);
            Stage s;
            PrLayoutEffectStyle effects = level(10, opacity);
            s.add(s.stage, zIndexed(block(40)), &effects);
            PrLayoutBoxStyle later = positioned(sized(block(), 100, 40));
            later.margin[PrRectSide_Top] = prLayoutLengthPx(-20);
            PrLayoutNode r = s.add(s.stage, later);
            s.layout();
            CHECK(s.hit(20, 20) == r);
        }
    }
    SUBCASE("opacity, a transform, fixed and sticky positioning each make a context at auto") {
        for (int kind = 0; kind < 4; ++kind) {
            CAPTURE(kind);
            Stage s(90);
            PrLayoutBoxStyle parentBox = absolute(20, 10, 110, 80);
            PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
            if (kind == 0) {
                effects.opacity = 0.9f;
            } else if (kind == 1) {
                parentBox = sized(block(), 110, 80);
                parentBox.margin[PrRectSide_Top] = prLayoutLengthPx(10);
                parentBox.margin[PrRectSide_Left] = prLayoutLengthPx(20);
                parentBox.transformed = 1;
            } else if (kind == 2) {
                parentBox.position = PrLayoutPosition_Fixed;
                PrLayoutBoxStyle stage = sized(block(), 200, 90);
                stage.display = PrLayoutDisplay_FlowRoot;
                stage.transformed = 1;
                s.set(s.stage, stage);
            } else {
                parentBox = sized(block(), 110, 80);
                parentBox.margin[PrRectSide_Top] = prLayoutLengthPx(10);
                parentBox.margin[PrRectSide_Left] = prLayoutLengthPx(20);
                parentBox.position = PrLayoutPosition_Sticky;
            }
            PrLayoutNode parent = s.add(s.stage, parentBox, &effects);
            s.at(parent, absolute(30, 20, 90, 30), 5);
            PrLayoutNode sibling = s.at(s.stage, absolute(90, 20, 90, 50), 1);
            s.layout();
            CHECK(s.hit(100, 40) == sibling);
        }
    }
}

TEST_CASE("flex items paint whole as inline-blocks, in order-modified document order, at their z-index static") {
    PrLayoutBoxStyle flex = sized(block(), 200, 40);
    flex.display = PrLayoutDisplay_Flex;
    // A, 60 by 40, holds text that runs past it. B, 60 by 40, overlaps it by 20px from 40.
    PrLayoutBoxStyle a = sized(block(), 60, 40);
    PrLayoutBoxStyle b = sized(block(), 60, 40);
    b.margin[PrRectSide_Left] = prLayoutLengthPx(-20);
    SUBCASE("B paints whole after A, over A's text") {
        Stage s;
        PrLayoutNode container = s.add(s.stage, flex);
        PrLayoutNode first = s.add(container, a);
        s.text(first, "abcdefghij");
        PrLayoutNode second = s.add(container, b);
        s.layout();
        CHECK(s.hit(50, 10) == second);
        CHECK(s.hit(10, 10) == first);
    }
    SUBCASE("order paints A after B") {
        Stage s;
        PrLayoutNode container = s.add(s.stage, flex);
        PrLayoutFlexStyle later = prLayoutFlexStyleDefault();
        later.order = 1;
        PrLayoutNode first = s.f.element(container, a, nullptr, nullptr, &s.style, &later);
        s.add(container, b);
        s.layout();
        // A is laid out after B, at 40, and over it.
        CHECK(s.hit(50, 10) == first);
    }
    SUBCASE("a static item's z-index applies, and is its context's") {
        Stage s;
        PrLayoutNode container = s.add(s.stage, flex);
        PrLayoutNode first = s.at(container, a, 1);
        PrLayoutNode second = s.add(container, b);
        s.layout();
        CHECK(s.hit(50, 10) == first);
        CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{first});
        // The container turned into a block: z-index applies to no static box, and A paints in flow under B.
        s.set(container, sized(block(), 200, 80));
        s.layout();
        CHECK(listOf(s, s.stage).empty());
        CHECK(s.hit(10, 50) == second);
        // And back.
        s.set(container, flex);
        s.layout();
        CHECK(listOf(s, s.stage) == std::vector<PrLayoutNode>{first});
        CHECK(s.hit(50, 10) == first);
    }
    SUBCASE("a static item at 0 contains a child at -1 over its background") {
        Stage s;
        PrLayoutNode container = s.add(s.stage, flex);
        PrLayoutNode first = s.at(container, a, 0);
        PrLayoutNode child = s.at(first, positioned(sized(block(), 20, 20)), -1);
        s.layout();
        CHECK(s.hit(10, 10) == child);
        CHECK(listOf(s, first) == std::vector<PrLayoutNode>{child});
    }
}

TEST_CASE("a box out of order is clipped by the clips around it as in flow") {
    SUBCASE("by a relative clip around it") {
        Stage s(60);
        PrLayoutBoxStyle clip = positioned(sized(block(), 40, 40));
        clip.overflowX = PrLayoutOverflow_Hidden;
        clip.overflowY = PrLayoutOverflow_Hidden;
        PrLayoutNode clipper = s.add(s.stage, clip);
        PrLayoutNode n = s.at(clipper, absolute(0, 0, 100, 40), -1);
        s.layout();
        CHECK(s.hit(60, 20) == s.stage);
        CHECK(s.hit(20, 20) == clipper);
        (void)n;
    }
    SUBCASE("not by a static clip between it and its containing block") {
        Stage s(60);
        PrLayoutBoxStyle clip = sized(block(), 40, 40);
        clip.overflowX = PrLayoutOverflow_Hidden;
        clip.overflowY = PrLayoutOverflow_Hidden;
        PrLayoutNode clipper = s.add(s.stage, clip);
        PrLayoutNode n = s.at(clipper, absolute(0, 0, 100, 40), -1);
        s.layout();
        CHECK(s.hit(60, 20) == n);
    }
    SUBCASE("scrolled with the scroll container it is in, its content entered once in the recording") {
        Stage s(100);
        PrLayoutBoxStyle scroller = positioned(sized(block(), 100, 60));
        scroller.overflowX = PrLayoutOverflow_Scroll;
        scroller.overflowY = PrLayoutOverflow_Scroll;
        PrLayoutNode container = s.add(s.stage, scroller);
        PrLayoutNode low = s.at(container, absolute(0, 20, 50, 30), -1);
        PrLayoutNode high = s.at(container, absolute(0, 40, 50, 30), 1);
        PrLayoutNode tall = s.add(container, block(200));
        s.layout();
        CHECK(s.hit(20, 30) == tall);
        CHECK(s.hit(20, 50) == high);
        prLayoutNodeSetScroll(s.f.tree, container, px(0, 20));
        CHECK(s.hit(20, 25) == high);
        CHECK(s.hit(20, 55) == tall);
        // Out of the scrollport, clipped.
        CHECK(s.hit(20, 75) == s.stage);
        CHECK(verified(s.f.tree));
        (void)low;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Tree order

TEST_CASE("a line's children are in tree order, so equal levels on a bidi line paint in it") {
    // rtl: span A, then span B pulled over it by a negative start margin. Each holds an inline-block.
    for (bool relative : {false, true}) {
        CAPTURE(relative);
        Stage s(-1, false, PrTextDirection_Rtl);
        PrLayoutBoxStyle a = relative ? positioned(inlineBox()) : inlineBox();
        PrLayoutBoxStyle b = a;
        b.margin[PrRectSide_Right] = prLayoutLengthPx(-20);
        PrLayoutNode spanA = s.add(s.stage, a);
        PrLayoutNode inA = s.add(spanA, inlineBlock(40, 20));
        PrLayoutNode spanB = s.add(s.stage, b);
        PrLayoutNode inB = s.add(spanB, inlineBlock(40, 20));
        s.layout();
        CHECK(s.hit(170, 10) == inB);
        (void)inA;
    }
}

TEST_CASE("a float is in tree order at its anchor on its line") {
    // A relative float, then on its line a relative span holding an inline-block pulled back over the float.
    for (int32_t z : {0, 1}) {
        CAPTURE(z);
        Stage s;
        PrLayoutNode fl;
        if (z != 0)
            fl = s.at(s.stage, positioned(floatLeft(60, 40)), z);
        else
            fl = s.add(s.stage, positioned(floatLeft(60, 40)));
        PrLayoutNode span;
        if (z != 0)
            span = s.at(s.stage, positioned(inlineBox()), z);
        else
            span = s.add(s.stage, positioned(inlineBox()));
        PrLayoutBoxStyle pulled = inlineBlock(40, 40);
        pulled.margin[PrRectSide_Left] = prLayoutLengthPx(-30);
        PrLayoutNode i = s.add(span, pulled);
        s.layout();
        CHECK(s.hit(45, 20) == i);
        (void)fl;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Inline boxes

TEST_CASE("a positioned inline box paints whole, after its line's content") {
    SUBCASE("at auto, over an inline-block pulled back over it") {
        Stage s;
        PrLayoutNode span = s.add(s.stage, positioned(inlineBox()));
        s.text(span, "abc");
        PrLayoutBoxStyle pulled = inlineBlock(40, 40);
        pulled.margin[PrRectSide_Left] = prLayoutLengthPx(-40);
        s.add(s.stage, pulled);
        s.layout();
        CHECK(s.hit(15, 20) == span);
    }
    SUBCASE("at 1, over an absolute box at auto") {
        Stage s;
        PrLayoutNode span = s.at(s.stage, positioned(inlineBox()), 1);
        s.text(span, "abc");
        s.add(s.stage, absolute(0, 0, 60, 40));
        s.layout();
        CHECK(s.hit(15, 20) == span);
    }
    SUBCASE("at 1 across two lines, over an absolute box on its second") {
        Stage s;
        s.text(s.stage, "xxxxx ");
        PrLayoutNode span = s.at(s.stage, positioned(inlineBox()), 1);
        s.text(span, "yyyyyy zzzzzz wwwwww");
        s.add(s.stage, absolute(0, 40, 100, 40));
        s.layout();
        CHECK(s.hit(10, 60) == span);
    }
    SUBCASE("a relative span at auto inside one at 1") {
        Stage s;
        PrLayoutNode outer = s.at(s.stage, positioned(inlineBox()), 1);
        s.text(outer, "a");
        PrLayoutNode inner = s.add(outer, positioned(inlineBox()));
        s.text(inner, "bc");
        s.add(s.stage, absolute(0, 0, 60, 40));
        s.layout();
        CHECK(s.hit(15, 20) == inner);
    }
    SUBCASE("at -1, under a later block pulled up over it, which paints over it") {
        Stage s;
        PrLayoutNode line = s.add(s.stage, block());
        PrLayoutNode span = s.at(line, positioned(inlineBox()), -1);
        s.text(span, "abc");
        PrLayoutBoxStyle later = block(40);
        later.margin[PrRectSide_Top] = prLayoutLengthPx(-30);
        PrLayoutDecorationStyle color = blue();
        PrLayoutNode b = s.f.element(s.stage, later, &color, nullptr, &s.style);
        s.layout();
        CHECK(s.hit(15, 20) == b);
        CHECK(s.color(s.pixels(), 15, 20) == "0 0 255 255");
    }
    SUBCASE("static, its z-index applies not") {
        Stage s;
        PrLayoutNode span = s.at(s.stage, inlineBox(), 10);
        s.text(span, "abc");
        PrLayoutBoxStyle pulled = inlineBlock(40, 40);
        pulled.margin[PrRectSide_Left] = prLayoutLengthPx(-40);
        PrLayoutNode ib = s.add(s.stage, pulled);
        s.layout();
        CHECK(s.hit(15, 20) == ib);
    }
}

TEST_CASE("an inline box of opacity paints whole, faded as one group in a layer of its own") {
    SUBCASE("after its line") {
        Stage s;
        PrLayoutEffectStyle half = faded(0.5f);
        PrLayoutNode span = s.add(s.stage, inlineBox(), &half);
        s.text(span, "abc");
        PrLayoutBoxStyle pulled = inlineBlock(40, 40);
        pulled.margin[PrRectSide_Left] = prLayoutLengthPx(-40);
        s.add(s.stage, pulled);
        s.layout();
        CHECK(s.hit(15, 20) == span);
    }
    SUBCASE("containing a child at 5 under a sibling at 1") {
        Stage s;
        PrLayoutEffectStyle half = faded(0.5f);
        PrLayoutNode span = s.add(s.stage, inlineBox(), &half);
        s.text(span, "a");
        PrLayoutNode child = s.at(span, positioned(inlineBox()), 5);
        s.text(child, "bc");
        PrLayoutNode sibling = s.at(s.stage, absolute(0, 0, 60, 40), 1);
        s.layout();
        CHECK(s.hit(15, 20) == sibling);
    }
    SUBCASE("its background, float and block, overlapping, composited once") {
        Stage s(160);
        PrLayoutEffectStyle half = faded(0.5f);
        PrLayoutDecorationStyle orange = red();
        PrLayoutNode span = s.f.element(s.stage, inlineBox(), &orange, &half, &s.style);
        s.text(span, "xxxxxx");
        PrLayoutBoxStyle fl = floatLeft(100, 60);
        fl.margin[PrRectSide_Right] = prLayoutLengthPx(-60);
        s.f.element(span, fl, &orange, nullptr, &s.style);
        PrLayoutBoxStyle pulled = sized(block(), 200, 60);
        pulled.margin[PrRectSide_Top] = prLayoutLengthPx(-40);
        s.f.element(span, pulled, &orange, nullptr, &s.style);
        s.text(span, "y");
        s.layout();
        std::vector<uint8_t> bytes = s.pixels();
        // The float is over the part, and the block over both: they form one group, so the overlap is red at half, not
        // darker.
        CHECK(s.color(bytes, 5, 30) == "128 0 0 128");
        CHECK(s.color(bytes, 50, 50) == "128 0 0 128");
        CHECK(prLayoutTreeStats(s.f.tree).layersRecorded >= 2);
    }
    SUBCASE("its opacity toggled after layout lays nothing out") {
        Stage s;
        PrLayoutEffectStyle half = faded(0.5f);
        PrLayoutDecorationStyle color = red();
        PrLayoutNode span = s.f.element(s.stage, inlineBox(), &color, nullptr, &s.style);
        s.text(span, "a");
        s.layout();
        // A point of its part above the glyph.
        std::vector<double> part = s.rects(span)[0];
        float x = float(part[0] + 2);
        float y = float(part[1] + 2);
        CHECK(s.color(s.pixels(), x, y) == "255 0 0 255");
        prLayoutNodeSetStyle(s.f.tree, span, s.f.style(inlineBox(), &color, &half, &s.style));
        s.layout();
        CHECK(prLayoutTreeStats(s.f.tree).blocksLaidOut == 0);
        CHECK(s.color(s.pixels(), x, y) == "128 0 0 128");
        CHECK(verified(s.f.tree));
        prLayoutNodeSetStyle(s.f.tree, span, s.f.style(inlineBox(), &color, nullptr, &s.style));
        s.layout();
        CHECK(s.color(s.pixels(), x, y) == "255 0 0 255");
        CHECK(verified(s.f.tree));
    }
}

TEST_CASE("an inline box that paints whole paints its rows' blocks and its floats as its own") {
    SUBCASE("a block in a span at 1, over an absolute box at auto") {
        Stage s;
        PrLayoutNode span = s.at(s.stage, positioned(inlineBox()), 1);
        s.text(span, "a");
        PrLayoutNode inner = s.add(span, block(30));
        s.text(span, "b");
        s.add(s.stage, absolute(0, 40, 60, 30));
        s.layout();
        CHECK(s.hit(15, 50) == inner);
    }
    SUBCASE("its line over the block pulled up over it, the block's background first") {
        Stage s;
        PrLayoutNode span = s.at(s.stage, positioned(inlineBox()), 1);
        s.text(span, "xxxx");
        PrLayoutBoxStyle pulled = block(40);
        pulled.margin[PrRectSide_Top] = prLayoutLengthPx(-30);
        s.add(span, pulled);
        s.text(span, "y");
        s.layout();
        CHECK(s.hit(10, 20) == span);
    }
    SUBCASE("a float in a span at 1, over an absolute box at auto") {
        Stage s(60);
        PrLayoutNode span = s.at(s.stage, positioned(inlineBox()), 1);
        s.text(span, "a");
        PrLayoutNode fl = s.add(span, floatLeft(50, 40));
        s.add(s.stage, absolute(0, 0, 60, 30));
        s.layout();
        CHECK(s.hit(15, 20) == fl);
    }
    SUBCASE("a float in a relative span at auto, over an inline-block of the line pulled back over it") {
        Stage s(60);
        PrLayoutNode span = s.add(s.stage, positioned(inlineBox()));
        s.text(span, "a");
        PrLayoutNode fl = s.add(span, floatLeft(50, 40));
        PrLayoutBoxStyle pulled = inlineBlock(60, 40);
        pulled.margin[PrRectSide_Left] = prLayoutLengthPx(-80);
        s.add(s.stage, pulled);
        s.layout();
        CHECK(s.hit(15, 20) == fl);
    }
    SUBCASE("its border over its float, which its floats phase paints first") {
        Stage s;
        PrLayoutBoxStyle spanBox = positioned(inlineBox());
        for (uint32_t side = 0; side < PrRectSide_Count; ++side)
            spanBox.border[side] = prLayoutUnitFromPx(10);
        PrLayoutNode span = s.at(s.stage, spanBox, 0);
        s.text(span, "x");
        PrLayoutBoxStyle fl = floatLeft(50, 40);
        fl.margin[PrRectSide_Right] = prLayoutLengthPx(-10);
        s.add(span, fl);
        s.layout();
        CHECK(s.hit(45, 20) == span);
    }
}

TEST_CASE("an inline box has a part on a line where it starts and where it ends, empty or beside a float alone") {
    SUBCASE("around a block") {
        Stage s;
        PrLayoutNode span = s.add(s.stage, inlineBox());
        s.add(span, block(20));
        s.layout();
        std::vector<std::vector<double>> rects = s.rects(span);
        REQUIRE(rects.size() == 3);
        CHECK(rects[0][2] == 0);
        CHECK(rects[1][2] == 200);
        CHECK(rects[2][2] == 0);
        CHECK(rects[2][1] == 20);
    }
    SUBCASE("beside a float") {
        Stage s;
        PrLayoutNode span = s.add(s.stage, inlineBox());
        s.add(span, floatLeft(20, 20));
        s.layout();
        std::vector<std::vector<double>> rects = s.rects(span);
        REQUIRE(rects.size() == 1);
        CHECK(rects[0][0] == 20);
        CHECK(rects[0][2] == 0);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Changes

TEST_CASE("z-index between integers lays out nothing, and between auto and an integer lays out the box") {
    Stage s(70);
    PrLayoutNode a = s.at(s.stage, absolute(0, 0, 60, 60), 3);
    PrLayoutNode b = s.at(s.stage, absolute(40, 0, 60, 30), 1);
    s.layout();
    CHECK(s.hit(50, 10) == a);
    PrLayoutEffectStyle five = level(5);
    s.set(b, zIndexed(absolute(40, 0, 60, 30)), &five);
    s.layout();
    CHECK(prLayoutTreeStats(s.f.tree).blocksLaidOut == 0);
    CHECK(s.hit(50, 10) == b);
    CHECK(verified(s.f.tree));
    s.set(b, absolute(40, 0, 60, 30));
    s.layout();
    CHECK(prLayoutTreeStats(s.f.tree).blocksLaidOut != 0);
    CHECK(s.hit(50, 10) == a);
    CHECK(verified(s.f.tree));
    // From auto to 5 again, painted once more over a.
    s.set(b, zIndexed(absolute(40, 0, 60, 30)), &five);
    s.layout();
    CHECK(s.hit(50, 10) == b);
    CHECK(verified(s.f.tree));
}

TEST_CASE("a stacking change lays nothing out, and records again the layer that names the box") {
    Stage s(70);
    PrLayoutNode a = s.at(s.stage, absolute(0, 0, 60, 60), 3);
    PrLayoutNode b = s.at(s.stage, absolute(40, 0, 60, 30), 1);
    PrLayoutNode other = s.add(s.stage, block());
    s.text(other, "x");
    s.layout();
    s.pixels();
    CHECK(s.hit(50, 10) == a);
    PrLayoutEffectStyle five = level(5);
    s.set(b, zIndexed(absolute(40, 0, 60, 30)), &five);
    s.f.layout(px(400, 400));
    CHECK(prLayoutTreeStats(s.f.tree).blocksLaidOut == 0);
    s.pixels();
    CHECK(prLayoutTreeStats(s.f.tree).layersRecorded == 1);
    CHECK(s.hit(50, 10) == b);
    CHECK(verified(s.f.tree));
    // A change of what paints at level 0.
    PrLayoutDecorationStyle color = red();
    prLayoutNodeSetStyle(s.f.tree, other, s.f.style(block(), &color, nullptr, &s.style));
    s.f.layout(px(400, 400));
    s.pixels();
    CHECK(s.hit(50, 10) == b);
    CHECK(verified(s.f.tree));
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes out of flow

TEST_CASE("a box at a level inside a positioned box at auto, a float, an inline-block and a box out of flow") {
    // Each holds a box at 1 under the stage's own box at 2 and over a box at auto, both of which it overlaps.
    for (int kind = 0; kind < 4; ++kind) {
        CAPTURE(kind);
        Stage s(100);
        PrLayoutNode holder = kPrLayoutNodeNone;
        if (kind == 0)
            holder = s.add(s.stage, positioned(sized(block(), 100, 50)));
        else if (kind == 1)
            holder = s.add(s.stage, floatLeft(100, 50));
        else if (kind == 2)
            holder = s.add(s.stage, inlineBlock(100, 50));
        else
            holder = s.add(s.stage, absolute(0, 0, 100, 50));
        PrLayoutNode inner = s.at(holder, positioned(sized(block(), 80, 40)), 1);
        PrLayoutNode over = s.at(s.stage, absolute(60, 0, 60, 30), 2);
        PrLayoutNode under = s.add(s.stage, absolute(0, 20, 30, 30));
        s.layout();
        CHECK(s.hit(70, 10) == over);
        CHECK(s.hit(10, 30) == inner);
        CHECK(verified(s.f.tree));
        (void)under;
    }
}

TEST_CASE("a box out of flow held outside its stacking context is in its context's list, where its holder rewraps") {
    // A static stage of opacity holds an absolute box A at auto, which the root holds, with a relative span at 1.
    Fixture f;
    PrLayoutFontStyle style = font();
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
    PrLayoutEffectStyle half = faded(0.5f);
    PrLayoutNode stage = f.element(root, sized(block(), 200, 100), nullptr, &half, &style);
    PrLayoutBoxStyle aBox = absolute(0, 0, 120, -1);
    PrLayoutNode a = f.element(stage, aBox, nullptr, nullptr, &style);
    f.text(a, "xxxx ");
    PrLayoutEffectStyle one = level(1);
    PrLayoutNode span = f.element(a, zIndexed(positioned(inlineBox())), nullptr, &one, &style);
    f.text(span, "yyyy zzzz");
    // A later box at auto in the stage overlaps the span, and is under it.
    PrLayoutNode later = f.element(stage, absolute(0, 0, 200, 100), nullptr, nullptr, &style);
    f.layout(px(400, 400));
    CHECK(verified(f.tree));
    auto hit = [&](float x, float y) {
        PrLayoutNode node = f.hit(PrVec2{x, y});
        return prLayoutNodeKind(f.tree, node) == PrLayoutNodeKind_Text ? prLayoutNodeParent(f.tree, node) : node;
    };
    CHECK(hit(10, 20) == later);
    CHECK(hit(60, 20) == span);
    // A's width changes, and its lines rewrap, while the stage's fragment stays.
    prLayoutNodeSetStyle(f.tree, a, f.style(absolute(0, 0, 60, -1), nullptr, nullptr, &style));
    f.layout(px(400, 400));
    CHECK(verified(f.tree));
    CHECK(hit(10, 60) == span);
    CHECK(hit(10, 20) == later);
    render(f.tree, 400, 400);
    CHECK(verified(f.tree));
}

TEST_CASE("a list keeps tree order at a level: a box, two in a box held outside the context, then another") {
    // The stage, static and with opacity, holds L1, then an absolute box R that the root holds, with E1 and E2 inside,
    // then L2, all at 1, each overlapping the next.
    for (bool nested : {false, true}) {
        CAPTURE(nested);
        Fixture f;
        PrLayoutFontStyle style = font();
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
        PrLayoutEffectStyle half = faded(0.5f);
        PrLayoutNode stage = f.element(root, sized(block(), 300, 100), nullptr, &half, &style);
        PrLayoutEffectStyle one = level(1);
        PrLayoutNode l1 = f.element(stage, zIndexed(absolute(0, 0, 60, 40)), nullptr, &one, &style);
        PrLayoutNode r = f.element(stage, absolute(0, 0, 300, 100), nullptr, nullptr, &style);
        // Nested: R holds a second absolute box at auto, which R holds, so E1 and E2 are in R's list through it.
        PrLayoutNode holder = nested ? f.element(r, absolute(0, 0, 300, 100), nullptr, nullptr, &style) : r;
        PrLayoutNode e1 = f.element(holder, zIndexed(absolute(40, 0, 60, 40)), nullptr, &one, &style);
        PrLayoutNode e2 = f.element(holder, zIndexed(absolute(80, 0, 60, 40)), nullptr, &one, &style);
        PrLayoutNode l2 = f.element(stage, zIndexed(absolute(120, 0, 60, 40)), nullptr, &one, &style);
        f.layout(px(400, 400));
        CHECK(verified(f.tree));
        CHECK(f.hit(PrVec2{50, 20}) == e1);
        CHECK(f.hit(PrVec2{90, 20}) == e2);
        CHECK(f.hit(PrVec2{130, 20}) == l2);
        CHECK(f.hit(PrVec2{10, 20}) == l1);
        CHECK(f.hit(PrVec2{250, 20}) == holder);
    }
}

TEST_CASE("an absolute box at auto holding one going to 1 lists it in its context, held inside the context or outside") {
    for (bool inside : {true, false}) {
        CAPTURE(inside);
        Fixture f;
        PrLayoutFontStyle style = font();
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
        PrLayoutEffectStyle half = faded(0.5f);
        // Inside: the stage is relative and holds A. Outside: the stage is static with opacity, and the root holds A.
        PrLayoutBoxStyle stageBox = sized(block(), 200, 100);
        if (inside)
            stageBox = zIndexed(positioned(stageBox));
        PrLayoutEffectStyle zero = level(0);
        PrLayoutNode stage = f.element(root, stageBox, nullptr, inside ? &zero : &half, &style);
        PrLayoutNode a = f.element(stage, absolute(0, 0, 100, 60), nullptr, nullptr, &style);
        PrLayoutNode b = f.element(a, absolute(0, 0, 50, 30), nullptr, nullptr, &style);
        PrLayoutNode later = f.element(stage, absolute(0, 0, 200, 100), nullptr, nullptr, &style);
        f.layout(px(400, 400));
        CHECK(verified(f.tree));
        CHECK(f.hit(PrVec2{10, 10}) == later);
        // B from auto to 1, after the lists are found.
        PrLayoutEffectStyle one = level(1);
        prLayoutNodeSetStyle(f.tree, b, f.style(zIndexed(absolute(0, 0, 50, 30)), nullptr, &one, &style));
        f.layout(px(400, 400));
        CHECK(verified(f.tree));
        CHECK(f.hit(PrVec2{10, 10}) == b);
        render(f.tree, 400, 400);
        CHECK(verified(f.tree));
        (void)a;
    }
}

TEST_CASE("a layer's recording that embeds a cached recording of a box held outside it depends on the box's list") {
    // A static stage with opacity paints, from its list, a box at 1 that the root holds, whose recording is cached with
    // the relative box of its own list in it. The stage records again for a change of its own text, embedding the box's
    // recording.
    Fixture f;
    PrLayoutFontStyle style = font();
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
    PrLayoutEffectStyle half = faded(0.5f);
    PrLayoutNode stage = f.element(root, sized(block(), 200, 100), nullptr, &half, &style);
    PrLayoutDecorationStyle color = red();
    PrLayoutEffectStyle one = level(1);
    PrLayoutNode box = f.element(stage, zIndexed(absolute(0, 0, 100, 50)), &color, &one, &style);
    PrLayoutNode inner = f.element(box, positioned(sized(block(), 50, 20)), &color, nullptr, &style);
    PrLayoutNode text = f.text(f.element(stage, block(), nullptr, nullptr, &style), "x");
    f.layout(px(400, 400));
    render(f.tree, 400, 400);
    CHECK(verified(f.tree));
    prLayoutNodeSetText(f.tree, text, "more");
    f.layout(px(400, 400));
    render(f.tree, 400, 400);
    CHECK(verified(f.tree));
    CHECK(f.hit(PrVec2{10, 10}) == inner);
    // A box joining the box's list makes the box record again.
    PrLayoutNode later = f.element(box, absolute(0, 0, 100, 50), nullptr, nullptr, &style);
    f.layout(px(400, 400));
    render(f.tree, 400, 400);
    CHECK(verified(f.tree));
    CHECK(f.hit(PrVec2{10, 10}) == later);
}

// ---------------------------------------------------------------------------------------------------------------------
// Hit testing an inline box that paints whole

TEST_CASE("an inline box that paints whole is hit on a later line where the line it opens on is declined") {
    SUBCASE("on its second line") {
        Stage s;
        s.text(s.stage, "xxxxx ");
        PrLayoutNode span = s.add(s.stage, positioned(inlineBox()));
        s.text(span, "yyyyyy zzzzzz wwwwww");
        s.layout();
        CHECK(s.hit(10, 60) == span);
        // A change of effects that makes the span a layer of its own still hits it there.
        PrLayoutEffectStyle half = faded(0.5f);
        prLayoutNodeSetStyle(s.f.tree, span, s.f.style(positioned(inlineBox()), nullptr, &half, &s.style));
        s.layout();
        CHECK(s.hit(10, 60) == span);
    }
    SUBCASE("on a later row and a later anonymous block") {
        Stage s;
        PrLayoutNode span = s.add(s.stage, positioned(inlineBox()));
        s.text(span, "a");
        PrLayoutNode inner = s.add(span, block(30));
        s.text(span, "b");
        s.layout();
        CHECK(s.hit(150, 50) == inner);
        CHECK(s.hit(5, 85) == span);
    }
    SUBCASE("its float, and an absolute box in it held outside the container") {
        Stage s(100);
        PrLayoutNode wrap = s.add(s.stage, block());
        PrLayoutNode span = s.add(wrap, positioned(inlineBox()));
        s.text(span, "a");
        PrLayoutNode fl = s.add(span, floatLeft(30, 80));
        PrLayoutNode box = s.add(s.add(span, inlineBox()), absolute(150, 70, 20, 20));
        s.layout();
        CHECK(s.hit(10, 70) == fl);
        // The span's containing block starts past the float.
        CHECK(s.hit(190, 85) == box);
    }
    SUBCASE("a document of wrapped links that paint nothing whole hits the text") {
        Stage s;
        PrLayoutNode first = kPrLayoutNodeNone;
        for (uint32_t i = 0; i < 50; ++i) {
            PrLayoutNode link = s.add(s.stage, inlineBox());
            s.text(link, "aaaa bbbb cccc ");
            first = first == kPrLayoutNodeNone ? link : first;
        }
        s.layout();
        CHECK(s.hit(5, 20) == first);
    }
}

TEST_CASE("nested inline boxes that paint whole, opening on a declined line, are hit once in their painting order") {
    // An outer relative span O holds a relative span I across three lines, then a relative inline-block X moved up over
    // I's second line. Both spans open on the first line, which a hit on the second line skips.
    SUBCASE("O at 0: X, after I in O, is over it") {
        Stage s;
        PrLayoutNode outer = s.add(s.stage, positioned(inlineBox()));
        PrLayoutNode inner = s.add(outer, positioned(inlineBox()));
        s.text(inner, "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn oooo pppp");
        PrLayoutBoxStyle moved = positioned(inlineBlock(40, 40));
        PrLayoutNode x = s.add(outer, positioned(inlineBlock(40, 40)));
        s.layout();
        std::vector<std::vector<double>> lines = s.rects(inner);
        REQUIRE(lines.size() >= 3);
        moved.inset[PrRectSide_Top] = prLayoutLengthPx(-float(s.rects(x)[0][1] - lines[1][1]));
        s.set(x, moved);
        s.layout();
        std::vector<double> at = s.rects(x)[0];
        REQUIRE(at[1] == lines[1][1]);
        CHECK(s.hit(float(at[0] + 5), float(at[1] + 10)) == x);
    }
    SUBCASE("O at -1: a later block of the stage's flow is over I") {
        Stage s;
        PrLayoutNode outer = s.at(s.stage, positioned(inlineBox()), -1);
        PrLayoutNode inner = s.add(outer, positioned(inlineBox()));
        s.text(inner, "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn oooo pppp");
        PrLayoutBoxStyle pulled = sized(block(), 200, 40);
        PrLayoutNode later = s.add(s.stage, pulled);
        s.layout();
        std::vector<std::vector<double>> lines = s.rects(inner);
        REQUIRE(lines.size() >= 3);
        // The block pulled up over I's second line.
        pulled.margin[PrRectSide_Top] = prLayoutLengthPx(-float(s.rects(later)[0][1] - lines[1][1]));
        s.set(later, pulled);
        s.layout();
        CHECK(s.hit(10, float(lines[1][1] + 10)) == later);
    }
}

TEST_CASE("boxes held outside a declined container hit in the painting order of the opacity inline box around them") {
    // An opacity span, whose container is a 40px block, holds in tree order boxes at 1, auto and -1 that the stage
    // holds, overlapping below the container, and a nested span with opacity holding another box at auto. Each box is
    // further right than the one before.
    Stage s(200);
    PrLayoutNode container = s.add(s.stage, sized(block(), 200, 40));
    PrLayoutEffectStyle half = faded(0.5f);
    PrLayoutNode span = s.add(container, inlineBox(), &half);
    s.text(span, "a");
    PrLayoutNode high = s.at(span, absolute(0, 100, 60, 60), 1);
    PrLayoutNode mid = s.add(span, absolute(20, 100, 60, 60));
    PrLayoutNode low = s.at(span, absolute(40, 100, 60, 60), -1);
    PrLayoutNode nested = s.add(span, inlineBox(), &half);
    s.text(nested, "b");
    PrLayoutNode inNested = s.add(nested, absolute(60, 100, 60, 60));
    s.layout();
    auto hits = [&](float x) {
        PrLayoutNode out[8];
        PrRect o = s.stageBounds();
        uint32_t count = prLayoutTreeHitTest(s.f.tree, prLayoutVecFromPx(PrVec2{o.left + x, o.top + 150}), out, 8);
        return std::vector<PrLayoutNode>(out, out + (count < 8 ? count : 8));
    };
    // Topmost first, each once: the positive level over what paints at 0 in tree order, over the negative level.
    CHECK(hits(50) == std::vector<PrLayoutNode>{high, mid, low, s.stage, s.root});
    CHECK(hits(70) == std::vector<PrLayoutNode>{inNested, mid, low, s.stage, s.root});
    CHECK(hits(110) == std::vector<PrLayoutNode>{inNested, s.stage, s.root});
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

TEST_CASE("what paints out of order is painted where its levels put it") {
    SUBCASE("a span at -1 under a later block pulled over its line") {
        Stage s;
        PrLayoutNode line = s.add(s.stage, block());
        PrLayoutDecorationStyle orange = red();
        PrLayoutEffectStyle under = level(-1);
        PrLayoutNode span = s.f.element(line, zIndexed(positioned(inlineBox())), &orange, &under, &s.style);
        s.text(span, "abcdef");
        PrLayoutBoxStyle later = sized(block(), 150, 40);
        later.margin[PrRectSide_Top] = prLayoutLengthPx(-30);
        PrLayoutDecorationStyle color = blue();
        s.f.element(s.stage, later, &color, nullptr, &s.style);
        s.layout();
        CHECK(s.color(s.pixels(), 30, 30) == "0 0 255 255");
    }
    SUBCASE("a span at 1: its line over the block pulled up over it") {
        Stage s;
        PrLayoutDecorationStyle orange = red();
        PrLayoutEffectStyle one = level(1);
        PrLayoutNode span = s.f.element(s.stage, zIndexed(positioned(inlineBox())), &orange, &one, &s.style);
        s.text(span, "xxxxxx");
        PrLayoutBoxStyle pulled = sized(block(), 150, 40);
        pulled.margin[PrRectSide_Top] = prLayoutLengthPx(-30);
        PrLayoutDecorationStyle color = blue();
        s.f.element(span, pulled, &color, nullptr, &s.style);
        s.text(span, "y");
        s.layout();
        std::vector<double> part = s.rects(span)[0];
        CHECK(s.color(s.pixels(), float(part[0] + part[2] - 2), float(part[1] + part[3] - 2)) == "255 0 0 255");
    }
    SUBCASE("a float in a span of opacity faded with it") {
        Stage s(120);
        PrLayoutEffectStyle half = faded(0.5f);
        PrLayoutNode span = s.add(s.stage, inlineBox(), &half);
        s.text(span, "a");
        PrLayoutDecorationStyle color = blue();
        s.f.element(span, floatLeft(100, 80), &color, nullptr, &s.style);
        s.layout();
        CHECK(s.color(s.pixels(), 50, 60) == "0 0 128 128");
    }
    SUBCASE("layers at levels, played in order in a scene") {
        Stage s(100);
        PrLayoutDecorationStyle r = red();
        PrLayoutDecorationStyle b = blue();
        PrLayoutEffectStyle low = level(-1, 0.5f);
        PrLayoutEffectStyle high = level(1);
        high.layer = true;
        s.f.element(s.stage, zIndexed(absolute(0, 0, 60, 60)), &r, &low, &s.style);
        s.f.element(s.stage, sized(block(), 200, 40), &b, nullptr, &s.style);
        s.f.element(s.stage, zIndexed(absolute(20, 20, 60, 60)), &r, &high, &s.style);
        s.layout();
        std::vector<uint8_t> bytes = s.pixels();
        CHECK(s.color(bytes, 10, 10) == "0 0 255 255");
        CHECK(s.color(bytes, 10, 50) == "128 0 0 128");
        CHECK(s.color(bytes, 30, 30) == "255 0 0 255");
        CHECK(verified(s.f.tree));
    }
}

TEST_CASE("an inline box that paints whole at the start, middle and end of a run of children past kPaintRun") {
    for (uint32_t at : {0u, 20u, 39u}) {
        CAPTURE(at);
        Stage s;
        PrLayoutNode span = kPrLayoutNodeNone;
        for (uint32_t i = 0; i < 40; ++i) {
            if (i == at) {
                span = s.add(s.stage, positioned(inlineBox()));
                s.text(span, "a");
                s.add(span, block(10));
                s.text(span, "b");
            } else {
                s.add(s.stage, block(2));
            }
        }
        PrLayoutDecorationStyle color = red();
        s.layout();
        s.pixels();
        prLayoutNodeSetStyle(s.f.tree, span, s.f.style(positioned(inlineBox()), &color, nullptr, &s.style));
        s.layout();
        s.pixels();
        CHECK(verified(s.f.tree));
    }
}

TEST_CASE("the worked trace: a stacking context's list, its painting order, and what its changes cost") {
    // S, relative at 0, holds a static clip C with N at -1 and a relative P at auto that holds an absolute Q at 2. Then
    // come text with a relative span I at 1 around a block B, with a float F in I, and a block O with opacity holding R
    // at 5.
    Fixture f;
    PrLayoutFontStyle style = font();
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
    PrLayoutEffectStyle zero = level(0);
    PrLayoutNode s = f.element(root, zIndexed(positioned(sized(block(), 200, -1))), nullptr, &zero, &style);
    PrLayoutBoxStyle clip = sized(block(), 200, 40);
    clip.overflowX = PrLayoutOverflow_Hidden;
    clip.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutNode c = f.element(s, clip, nullptr, nullptr, &style);
    PrLayoutEffectStyle under = level(-1);
    PrLayoutNode n = f.element(c, zIndexed(positioned(sized(block(), 200, 10))), nullptr, &under, &style);
    PrLayoutNode p = f.element(c, positioned(sized(block(), 200, 20)), nullptr, nullptr, &style);
    PrLayoutEffectStyle two = level(2);
    PrLayoutNode q = f.element(p, zIndexed(absolute(0, 0, 100, 60)), nullptr, &two, &style);
    f.text(s, "text ");
    PrLayoutEffectStyle one = level(1);
    PrLayoutNode i = f.element(s, zIndexed(positioned(inlineBox())), nullptr, &one, &style);
    f.text(i, "i ");
    PrLayoutNode fl = f.element(i, floatLeft(20, 20), nullptr, nullptr, &style);
    PrLayoutNode b = f.element(i, block(20), nullptr, nullptr, &style);
    f.text(i, " j");
    PrLayoutEffectStyle half = faded(0.5f);
    PrLayoutNode o = f.element(s, block(), nullptr, &half, &style);
    PrLayoutEffectStyle five = level(5);
    PrLayoutNode r = f.element(o, zIndexed(positioned(sized(block(), 50, 20))), nullptr, &five, &style);
    f.layout(px(400, 400));
    CHECK(verified(f.tree));
    render(f.tree, 400, 400);
    // S paints N, I and Q at their levels. R is in O, and none of I's pieces belong to S.
    const LayoutTree* tree = f.tree;
    LayoutNodeIndex owner = layoutNodeIndex(tree, s);
    // The levels other than 0 of S's list, each once.
    auto levelsOfS = [&] {
        LayoutStackedBoxes list = layoutStackingList(tree, owner);
        std::vector<int32_t> levels;
        for (uint32_t i = 0; i < list.count; ++i) {
            if (list.boxes[i].level != 0 && (levels.empty() || levels.back() != list.boxes[i].level))
                levels.push_back(list.boxes[i].level);
        }
        return levels;
    };
    CHECK(levelsOfS() == std::vector<int32_t>{-1, 1, 2});
    // Q is clipped by C, which holds Q's placeholder but not Q, because Q's holder P is in C.
    CHECK(f.hit(PrVec2{10, 50}) != q);
    // Q's level from 2 to -2 lays nothing out, moves Q in S's list rather than finding the list again, and records the
    // layer S is in.
    PrLayoutEffectStyle minus = level(-2);
    prLayoutNodeSetStyle(f.tree, q, f.style(zIndexed(absolute(0, 0, 100, 60)), nullptr, &minus, &style));
    f.layout(px(400, 400));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    render(f.tree, 400, 400);
    CHECK(prLayoutTreeStats(f.tree).stackingListsFound == 0);
    CHECK(levelsOfS() == std::vector<int32_t>{-2, -1, 1});
    CHECK(verified(f.tree));
    // O's opacity becomes 1: R joins S's levels, inserted in its list.
    prLayoutNodeSetStyle(f.tree, o, f.style(block(), nullptr, nullptr, &style));
    f.layout(px(400, 400));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    render(f.tree, 400, 400);
    CHECK(prLayoutTreeStats(f.tree).stackingListsFound == 0);
    CHECK(verified(f.tree));
    CHECK(levelsOfS() == std::vector<int32_t>{-2, -1, 1, 5});
    // Text before I lays S's content out again, which changes no list: S's is the one found before.
    prLayoutNodeSetText(f.tree, prLayoutNodeFirstChild(f.tree, s) == c ? prLayoutNodeNextSibling(f.tree, c) : c, "more text ");
    f.layout(px(400, 400));
    render(f.tree, 400, 400);
    CHECK(prLayoutTreeStats(f.tree).stackingListsFound == 0);
    CHECK(levelsOfS() == std::vector<int32_t>{-2, -1, 1, 5});
    CHECK(verified(f.tree));
    (void)n;
    (void)fl;
    (void)b;
    (void)r;
}

TEST_CASE("changes of what makes a stacking context, and of what is laid out, against a result from scratch") {
    Stage s(100);
    PrLayoutNode block1 = s.add(s.stage, sized(block(), 100, 40));
    PrLayoutNode child = s.at(block1, absolute(0, 0, 50, 50), 1);
    PrLayoutNode sibling = s.at(s.stage, absolute(20, 0, 50, 50), 2);
    PrLayoutNode text = s.text(s.stage, "x");
    s.layout();
    CHECK(s.hit(30, 10) == sibling);
    SUBCASE("opacity crossing 1 on a block contains the child, under the sibling still") {
        PrLayoutEffectStyle half = faded(0.5f);
        s.set(block1, sized(block(), 100, 40), &half);
        s.layout();
        CHECK(prLayoutTreeStats(s.f.tree).blocksLaidOut == 0);
        CHECK(s.hit(10, 10) == child);
        CHECK(s.hit(30, 10) == sibling);
        s.set(block1, sized(block(), 100, 40));
        s.layout();
        CHECK(s.hit(10, 10) == child);
    }
    SUBCASE("a transform appearing on the block contains the child, which it then holds") {
        PrLayoutBoxStyle moved = sized(block(), 100, 40);
        moved.transformed = 1;
        PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
        effects.transform = prLayoutTransformFromAffine2(prAffine2Translate(PrVec2{0, 0}));
        s.set(block1, moved, &effects);
        s.layout();
        CHECK(s.hit(10, 10) == child);
        CHECK(s.hit(30, 10) == sibling);
        s.set(block1, sized(block(), 100, 40));
        s.layout();
        CHECK(s.hit(30, 10) == sibling);
    }
    SUBCASE("content before a positioned box, and the box going between static and relative") {
        prLayoutNodeSetText(s.f.tree, text, "more");
        s.layout();
        CHECK(s.hit(30, 10) == sibling);
        s.set(sibling, zIndexed(sized(block(), 50, 50)));
        s.layout();
        CHECK(s.hit(10, 10) == child);
        s.set(sibling, zIndexed(positioned(sized(block(), 50, 50))));
        s.layout();
        render(s.f.tree, 400, 400);
        CHECK(verified(s.f.tree));
    }
}

TEST_CASE("an atomic inline that paints whole paints its own decoration, at auto, -1, 1 and of opacity") {
    for (int kind = 0; kind < 4; ++kind) {
        CAPTURE(kind);
        Stage s;
        PrLayoutDecorationStyle color = blue();
        PrLayoutEffectStyle effects = kind == 1 ? level(-1) : kind == 2 ? level(1) : faded(kind == 3 ? 0.5f : 1);
        PrLayoutBoxStyle box = inlineBlock(40, 40);
        if (kind != 3)
            box = positioned(box);
        if (kind == 1 || kind == 2)
            box = zIndexed(box);
        s.f.element(s.stage, box, &color, &effects, &s.style);
        s.layout();
        const char* expected = kind == 3 ? "0 0 128 128" : "0 0 255 255";
        CHECK(s.color(s.pixels(), 20, 20) == expected);
    }
}

TEST_CASE("a box split by bidi into parts on one line paints its content once, and its client rects in content order") {
    // An ltr span in an rtl line, holding Hebrew and Latin, split by reordering, with a relative child.
    Stage s(-1, false, PrTextDirection_Rtl);
    PrLayoutNode span = s.add(s.stage, positioned(inlineBox()));
    s.text(span, "ab \xd7\x90\xd7\x91 cd");
    PrLayoutNode inner = s.add(span, positioned(inlineBox()));
    s.text(inner, "ef");
    s.layout();
    std::vector<std::vector<double>> rects = s.rects(span);
    REQUIRE(!rects.empty());
    double width = 0;
    for (const std::vector<double>& r : rects)
        width += r[2];
    CHECK(width > 0);
    CHECK(s.hit(float(s.rects(inner)[0][0] + 2), 20) == inner);
    CHECK(verified(s.f.tree));
}

TEST_CASE("a box split by bidi into parts on one line opens on the part at its start edge") {
    // In an ltr paragraph, a span ends in Hebrew that the Hebrew after it reorders around, so that the span's Latin is
    // on the left and its Hebrew on the right of the text after it. Its first part in content order is the one at its
    // start edge: the leftmost part for an ltr span, and the rightmost for an rtl one.
    for (PrTextDirection direction : {PrTextDirection_Ltr, PrTextDirection_Rtl}) {
        CAPTURE(direction);
        Stage s(-1, false);
        s.text(s.stage, "ab ");
        PrLayoutFontStyle own = font(direction);
        PrLayoutNode span = s.f.element(s.stage, inlineBox(), nullptr, nullptr, &own);
        s.text(span, "cd \xd7\x90\xd7\x91");
        s.text(s.stage, " \xd7\x92\xd7\x93 ef");
        s.layout();
        std::vector<std::vector<double>> rects = s.rects(span);
        REQUIRE(rects.size() > 1);
        for (size_t i = 1; i < rects.size(); ++i) {
            if (direction == PrTextDirection_Ltr)
                CHECK(rects[0][0] < rects[i][0]);
            else
                CHECK(rects[0][0] > rects[i][0]);
        }
    }
}

TEST_CASE("a static float holding a relative box, before a relative span on its line, paints in tree order") {
    // F holds A, relative. B, a relative span on F's line, holds an inline-block pulled back over A. The cases are at
    // -1, 0 and 1.
    for (int32_t z : {-1, 0, 1}) {
        for (bool inWhole : {false, true}) {
            CAPTURE(z);
            CAPTURE(inWhole);
            Stage s(60);
            PrLayoutNode parent = inWhole ? s.add(s.stage, positioned(inlineBox())) : s.stage;
            PrLayoutNode fl = s.add(parent, floatLeft(60, 40));
            PrLayoutNode a;
            if (z != 0)
                a = s.at(fl, positioned(sized(block(), 60, 40)), z);
            else
                a = s.add(fl, positioned(sized(block(), 60, 40)));
            s.add(fl, absolute(0, 0, 10, 10));
            PrLayoutNode b = z != 0 ? s.at(parent, positioned(inlineBox()), z) : s.add(parent, positioned(inlineBox()));
            PrLayoutBoxStyle pulled = inlineBlock(40, 40);
            pulled.margin[PrRectSide_Left] = prLayoutLengthPx(-30);
            PrLayoutNode i = s.add(b, pulled);
            s.layout();
            // Below 0, both are under the float, which is a block of the stage's flow.
            CHECK(s.hit(45, 20) == (z < 0 ? fl : i));
            CHECK(s.hit(20, 30) == (z < 0 ? fl : a));
        }
    }
}

TEST_CASE("a static float in text reaches a box at a level that it places and its stacking context holds") {
    // The stage holds A, an absolute box inside the float F beside the stage's text, clear of F's area.
    for (int32_t z : {-1, 1}) {
        CAPTURE(z);
        Stage s(100);
        s.text(s.stage, "x ");
        PrLayoutNode fl = s.add(s.stage, floatLeft(60, 40));
        PrLayoutDecorationStyle color = blue();
        PrLayoutEffectStyle effects = level(z);
        PrLayoutNode a = s.f.element(fl, zIndexed(absolute(100, 50, 30, 30)), &color, &effects, &s.style);
        s.text(s.stage, "y");
        s.layout();
        CHECK(s.hit(110, 60) == a);
        CHECK(s.color(s.pixels(), 110, 60) == "0 0 255 255");
    }
}

TEST_CASE("a float inside a block inside an inline box of opacity is drawn once, in the box's group") {
    Stage s(120);
    PrLayoutEffectStyle half = faded(0.5f);
    PrLayoutNode span = s.add(s.stage, inlineBox(), &half);
    s.text(span, "a");
    PrLayoutNode inner = s.add(span, block());
    PrLayoutDecorationStyle color = blue();
    s.f.element(inner, floatLeft(60, 60), &color, nullptr, &s.style);
    s.text(span, "b");
    s.layout();
    CHECK(s.color(s.pixels(), 30, 60) == "0 0 128 128");
    CHECK(verified(s.f.tree));
}

TEST_CASE("a box of opacity whose content its scroll containers, clips, escaping boxes and layers divide composites once") {
    // A faded box with a red background holds red content over it, divided from the box's own by each kind of break. If
    // the group composites once, the overlap is red at half, not darker.
    for (int kind = 0; kind < 4; ++kind) {
        CAPTURE(kind);
        Stage s(100);
        PrLayoutDecorationStyle orange = red();
        PrLayoutEffectStyle half = faded(0.5f);
        PrLayoutNode clip = s.stage;
        if (kind == 2) {
            PrLayoutBoxStyle hidden = block();
            hidden.overflowX = PrLayoutOverflow_Clip;
            hidden.overflowY = PrLayoutOverflow_Clip;
            clip = s.add(s.stage, hidden);
        }
        PrLayoutNode box = s.f.element(clip, sized(block(), 100, 40), &orange, &half, &s.style);
        if (kind == 0 || kind == 1) {
            PrLayoutBoxStyle container = sized(block(), 100, 40);
            container.overflowX = kind == 0 ? PrLayoutOverflow_Scroll : PrLayoutOverflow_Clip;
            container.overflowY = container.overflowX;
            PrLayoutNode inner = s.add(box, container);
            s.f.element(inner, sized(block(), 100, 80), &orange, nullptr, &s.style);
        } else if (kind == 2) {
            // An absolute box that the stage holds, past the clip that it escapes.
            s.f.element(box, absolute(0, 0, 100, 40), &orange, nullptr, &s.style);
            s.f.element(box, absolute(0, 60, 100, 40), &orange, nullptr, &s.style);
        } else {
            PrLayoutEffectStyle animated = prLayoutEffectStyleDefault();
            animated.layer = true;
            s.f.element(box, sized(block(), 100, 40), &orange, &animated, &s.style);
        }
        s.layout();
        std::vector<uint8_t> bytes = s.pixels();
        CHECK(s.color(bytes, 50, 20) == "128 0 0 128");
        if (kind == 2)
            CHECK(s.color(bytes, 50, 80) == "128 0 0 128");
        CHECK(verified(s.f.tree));
        // Its opacity is changed.
        PrLayoutEffectStyle most = faded(0.75f);
        prLayoutNodeSetStyle(s.f.tree, box, s.f.style(sized(block(), 100, 40), &orange, &most, &s.style));
        s.layout();
        CHECK(s.color(s.pixels(), 50, 20) == "191 0 0 191");
        CHECK(verified(s.f.tree));
    }
}

TEST_CASE("nested boxes of opacity divided by a scroll container composite each once") {
    // A faded red box holds a faded blue box, which holds a scroll container of blue content over its background.
    Stage s(100);
    PrLayoutDecorationStyle orange = red();
    PrLayoutDecorationStyle color = blue();
    PrLayoutEffectStyle half = faded(0.5f);
    PrLayoutNode outer = s.f.element(s.stage, sized(block(), 100, 40), &orange, &half, &s.style);
    PrLayoutNode inner = s.f.element(outer, sized(block(), 100, 40), &color, &half, &s.style);
    PrLayoutBoxStyle container = sized(block(), 100, 40);
    container.overflowX = PrLayoutOverflow_Scroll;
    container.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode scroller = s.add(inner, container);
    s.f.element(scroller, sized(block(), 100, 80), &color, nullptr, &s.style);
    s.layout();
    // The inner group is blue at half over the outer group's red, and the whole is at half.
    CHECK(s.color(s.pixels(), 50, 20) == "64 0 64 128");
    CHECK(verified(s.f.tree));
}

TEST_CASE("a stacking context's recording that clips, scroll containers and layers divide is cached whole") {
    // A context at 0 holds a box that clips in one axis or both, scrolls, or has a layer of its own, with a relative
    // box of the context's list in it. A change to a later block records the root again, which appends the context's
    // recording, as recording it in place would, without recording the context again.
    for (int kind = 0; kind < 4; ++kind) {
        CAPTURE(kind);
        Stage s(100);
        PrLayoutDecorationStyle color = red();
        PrLayoutNode context = s.at(s.stage, positioned(sized(block(), 100, 60)), 0);
        PrLayoutBoxStyle around = sized(block(), 100, 40);
        PrLayoutEffectStyle* effects = nullptr;
        PrLayoutEffectStyle animated = prLayoutEffectStyleDefault();
        if (kind == 0) {
            around.overflowX = PrLayoutOverflow_Clip;
        } else if (kind == 1) {
            around.overflowX = PrLayoutOverflow_Clip;
            around.overflowY = PrLayoutOverflow_Clip;
        } else if (kind == 2) {
            around.overflowX = PrLayoutOverflow_Scroll;
            around.overflowY = PrLayoutOverflow_Scroll;
        } else {
            animated.layer = true;
            effects = &animated;
        }
        PrLayoutNode inner = s.f.element(context, around, &color, effects, &s.style);
        PrLayoutNode listed = s.f.element(inner, positioned(sized(block(), 50, 20)), &color, nullptr, &s.style);
        s.f.element(context, sized(block(), 100, 20), &color, nullptr, &s.style);
        PrLayoutNode later = s.add(s.stage, block());
        PrLayoutNode text = s.text(later, "x");
        s.layout();
        s.pixels();
        CHECK(verified(s.f.tree));
        prLayoutNodeSetText(s.f.tree, text, "more");
        s.layout();
        s.pixels();
        CHECK(prLayoutTreeStats(s.f.tree).subtreesReused >= 1);
        CHECK(prLayoutTreeStats(s.f.tree).commandsRecorded <= 2);
        CHECK(s.hit(10, 10) == listed);
        CHECK(verified(s.f.tree));
    }
}
