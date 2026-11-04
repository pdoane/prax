#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

#include <vector>

namespace pr_impl {

namespace {

// Scroll anchoring (css-scroll-anchoring-1): what a layout does to the offsets of scrolled containers as content moves.
// Each case is a document of 100px blocks in a viewport 100px wide and 200px tall.

float viewportY(Fixture& f) {
    return prLayoutUnitToPx(prLayoutNodeScroll(f.tree, kPrLayoutNodeNone).y);
}

std::vector<PrLayoutScrollAdjustment> taken(PrLayoutTree* tree) {
    std::vector<PrLayoutScrollAdjustment> moves(8);
    moves.resize(prLayoutTreeTakeScrollAdjustments(tree, moves.data(), uint32_t(moves.size())));
    return moves;
}

// A document of `count` blocks of 100px, whose blocks it writes to `out`.
PrLayoutNode blocks(Fixture& f, uint32_t count, std::vector<PrLayoutNode>& out) {
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    for (uint32_t i = 0; i < count; ++i)
        out.push_back(f.element(root, block(100)));
    return root;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("growth above what the viewport shows moves the offset by as much, and the move is reported") {
    Fixture f;
    std::vector<PrLayoutNode> b;
    blocks(f, 10, b);
    f.layout(px(100, 200));
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 250));
    prLayoutNodeSetStyle(f.tree, b[0], f.style(block(160)));
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 310);
    // The block at the top of what showed is still where it was on screen.
    CHECK(f.bounds(b[3]).top == doctest::Approx(50));
    std::vector<PrLayoutScrollAdjustment> moves = taken(f.tree);
    REQUIRE(moves.size() == 1);
    CHECK(moves[0].node == kPrLayoutNodeNone);
    CHECK(prLayoutUnitToPx(moves[0].delta.y) == 60);
    CHECK(taken(f.tree).empty());
    // The scene that follows carries the move, and the one after it carries none.
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    PrLayoutScrollAdjustment carried{};
    CHECK(prLayoutSceneScrollAdjustments(scene, &carried, 1) == 1);
    CHECK(prLayoutUnitToPx(carried.delta.y) == 60);
    prLayoutSceneRelease(scene);
    prLayoutNodeSetStyle(f.tree, b[9], f.style(block(120)));
    f.layout(px(100, 200));
    scene = prLayoutTreeScene(f.tree);
    CHECK(prLayoutSceneScrollAdjustments(scene, nullptr, 0) == 0);
    prLayoutSceneRelease(scene);
    CHECK(verified(f.tree));
}

TEST_CASE("the examination goes on past an empty box that holds children, to the content after it") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode empty = f.element(root, block(0));
    f.element(empty, block(0));
    PrLayoutNode content = f.element(root, block());
    std::vector<PrLayoutNode> b;
    for (uint32_t i = 0; i < 10; ++i)
        b.push_back(f.element(content, block(100)));
    f.layout(px(100, 200));
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 250));
    prLayoutNodeSetStyle(f.tree, b[0], f.style(block(160)));
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 310);
}

TEST_CASE("growth below, a document at its origin, and a layout with nothing to lay out move nothing") {
    Fixture f;
    std::vector<PrLayoutNode> b;
    blocks(f, 10, b);
    f.layout(px(100, 200));
    // At the origin, content grows beneath the reader.
    prLayoutNodeSetStyle(f.tree, b[0], f.style(block(150)));
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 0);
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 250));
    prLayoutNodeSetStyle(f.tree, b[8], f.style(block(150)));
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 250);
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 250);
    CHECK(taken(f.tree).empty());
}

TEST_CASE("a replaced anchor keeps its place, and a new anchor is chosen once the old one is erased") {
    Fixture f;
    std::vector<PrLayoutNode> b;
    PrLayoutNode root = blocks(f, 10, b);
    f.layout(px(100, 200));
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 300));
    // The block at the top of what shows is the anchor. Replacing it with one of the same height moves nothing.
    prLayoutTreeErase(f.tree, b[3]);
    PrLayoutNode again = prLayoutNodeCreateElement(f.tree, kPrLayoutNodeNone, f.style(block(100)));
    prLayoutTreeInsert(f.tree, root, again, b[4]);
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 300);
    // The replacement, laid out now, is the anchor that the next change above it moves.
    prLayoutNodeSetStyle(f.tree, b[1], f.style(block(130)));
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 330);
    CHECK(f.bounds(again).top == doctest::Approx(0));
}

TEST_CASE("overflow-anchor: none on the container or on the anchor's subtree keeps the offset") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(200);
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode s = f.element(root, scroller);
    PrLayoutNode first = f.element(s, block(100));
    PrLayoutBoxStyle optedOut = block();
    optedOut.overflowAnchor = PrLayoutOverflowAnchor_None;
    PrLayoutNode wrap = f.element(s, block());
    for (uint32_t i = 0; i < 5; ++i)
        f.element(wrap, block(100));
    f.layout(px(100, 400));
    prLayoutNodeSetScroll(f.tree, s, px(0, 150));
    prLayoutNodeSetStyle(f.tree, first, f.style(block(120)));
    f.layout(px(100, 400));
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, s).y) == 170);
    // With the subtree opted out, nothing in it is an anchor.
    prLayoutNodeSetStyle(f.tree, wrap, f.style(optedOut));
    prLayoutNodeSetScroll(f.tree, s, px(0, 150));
    prLayoutNodeSetStyle(f.tree, first, f.style(block(140)));
    f.layout(px(100, 400));
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, s).y) == 150);
    // And with the container opted out, it has no anchor.
    prLayoutNodeSetStyle(f.tree, wrap, f.style(block()));
    PrLayoutBoxStyle quiet = scroller;
    quiet.overflowAnchor = PrLayoutOverflowAnchor_None;
    prLayoutNodeSetStyle(f.tree, s, f.style(quiet));
    f.layout(px(100, 400));
    prLayoutNodeSetScroll(f.tree, s, px(0, 150));
    prLayoutNodeSetStyle(f.tree, first, f.style(block(100)));
    f.layout(px(100, 400));
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, s).y) == 150);
}

TEST_CASE("a change of size on the path from the anchor to its container suppresses the adjustment, in the same viewport") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode before = f.element(root, block(100));
    PrLayoutNode wrap = f.element(root, block());
    for (uint32_t i = 0; i < 5; ++i)
        f.element(wrap, block(100));
    f.layout(px(100, 200));
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 250));
    // The wrapper's margin moves the anchor inside it, and suppresses the adjustment
    // (css-scroll-anchoring-1/#suppression-triggers).
    PrLayoutBoxStyle margined = block();
    setMargins(margined, 40, 0);
    prLayoutNodeSetStyle(f.tree, wrap, f.style(margined));
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 250);
    // A change off the path adjusts again, once a new anchor is chosen.
    prLayoutNodeSetStyle(f.tree, before, f.style(block(150)));
    f.layout(px(100, 200));
    CHECK(viewportY(f) == 300);
    // In a new viewport extent, as a page's media queries change its margins, the same change adjusts.
    setMargins(margined, 70, 0);
    prLayoutNodeSetStyle(f.tree, wrap, f.style(margined));
    f.layout(px(100, 300));
    CHECK(viewportY(f) == 330);
}

TEST_CASE("a narrower viewport keeps the reader at the same text, which wraps onto more lines above it") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode first = f.element(root, block());
    f.text(first, "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen");
    PrLayoutNode marker = f.element(root, block(50));
    f.element(root, block(2000));
    f.layout(px(400, 200));
    float before = f.bounds(marker).top + viewportY(f);
    // The marker is the first box that shows, so it is the anchor. Were the text above partly in view, the text would
    // be the anchor, whose block-start edge stays put as it wraps.
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, before));
    f.layout(px(400, 200));
    CHECK(f.bounds(marker).top == doctest::Approx(0));
    f.layout(px(150, 200));
    CHECK(f.bounds(marker).top + viewportY(f) > before);
    CHECK(f.bounds(marker).top == doctest::Approx(0));
    CHECK(verified(f.tree));
}

TEST_CASE("a vertical-rl document is anchored from its right edge, and a narrower viewport keeps its offset") {
    Fixture f;
    PrLayoutFontStyle vertical = prLayoutFontStyleDefault();
    vertical.writingMode = PrLayoutWritingMode_VerticalRl;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &vertical);
    PrLayoutBoxStyle column = block();
    column.width = prLayoutLengthPx(100);
    PrLayoutNode first = f.element(root, column, nullptr, nullptr, &vertical);
    f.element(root, column, nullptr, nullptr, &vertical);
    PrLayoutBoxStyle rest = block();
    rest.width = prLayoutLengthPx(1000);
    f.element(root, rest, nullptr, nullptr, &vertical);
    f.layout(px(300, 100));
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(-100, 0));
    f.layout(px(300, 100));
    f.layout(px(100, 100));
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, kPrLayoutNodeNone).x) == -100);
    // Growth at the block start, on the right, moves the offset left.
    column.width = prLayoutLengthPx(130);
    prLayoutNodeSetStyle(f.tree, first, f.style(column, nullptr, nullptr, &vertical));
    f.layout(px(100, 100));
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, kPrLayoutNodeNone).x) == -130);
}
