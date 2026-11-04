#include "pr/draw.h"
#include "pr/draw_cpu.h"
#include "pr/float.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Sticky positioning (css-position-3/#stickypos-insets): a box laid out in flow and moved by its scroll container's
// offset to stay inside the scrollport at its insets, within its containing block. Each case is a document of plain
// blocks, and checks where the box is drawn, hit and measured as its scroll container scrolls.

constexpr float kAuto = kPrF32Nan;

// Returns `box` sticky positioned, at `top`, `right`, `bottom` and `left`, each auto if it is kAuto.
PrLayoutBoxStyle sticky(PrLayoutBoxStyle box, float top, float right, float bottom, float left) {
    box.position = PrLayoutPosition_Sticky;
    const float values[] = {top, right, bottom, left};
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.inset[side] = values[side] == values[side] ? prLayoutLengthPx(values[side]) : prLayoutLengthAuto();
    return box;
}

// Returns a scroll container `width` by `height`.
PrLayoutBoxStyle scroller(float width, float height) {
    PrLayoutBoxStyle box = block(height);
    box.width = prLayoutLengthPx(width);
    box.overflowX = PrLayoutOverflow_Scroll;
    box.overflowY = PrLayoutOverflow_Scroll;
    return box;
}

// Returns the top of the box of `node` in `coordinates`, in px.
float topOf(Fixture& f, PrLayoutNode node, PrLayoutCoordinates coordinates) {
    return prLayoutUnitToPx(prLayoutNodeBounds(f.tree, node, coordinates).top);
}

// Returns `list` drawn onto `width` by `height` RGBA pixels.
std::vector<uint8_t> drawn(PrDrawList* list, uint32_t width, uint32_t height) {
    std::vector<uint8_t> bytes(size_t(width) * height * 4, 0);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{bytes.data(), width * 4, {width, height}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    return bytes;
}

const PrColor kRed{1, 0, 0, 1};
const PrColor kGreen{0, 1, 0, 1};

// Returns a table part of `display`.
PrLayoutBoxStyle part(PrLayoutDisplay display) {
    PrLayoutBoxStyle box = block();
    box.display = display;
    return box;
}

// Adds a row of one cell 20px square to `parent`, its cell's background `cell` if it is not null, and returns the row.
PrLayoutNode row(Fixture& f, PrLayoutNode parent, const PrLayoutDecorationStyle* PR_NULLABLE cell) {
    PrLayoutNode made = f.element(parent, part(PrLayoutDisplay_TableRow));
    PrLayoutNode td = f.element(made, part(PrLayoutDisplay_TableCell), cell);
    PrLayoutBoxStyle content = block(20);
    content.width = prLayoutLengthPx(20);
    f.element(td, content);
    return made;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a sticky box stays at its inset in the scrollport as its scroll container scrolls, within its containing block") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(50));
    PrLayoutNode holder = f.element(container, block(200));
    PrLayoutDecorationStyle red = background(kRed);
    PrLayoutNode stuck = f.element(holder, sticky(block(20), 10, kAuto, kAuto, kAuto), &red);
    f.element(container, block(500));
    f.layout(px(200, 200));
    CHECK(verified(f.tree));
    // Below the inset, it stays where it was laid out.
    CHECK(f.bounds(stuck).top == 50.0f);
    CHECK(pixel(render(f.tree, 200, 200), 200, 5, 55) == "255 0 0 255");
    // Past it, it stays 10px below the scrollport's top. A scroll records nothing.
    prLayoutNodeSetScroll(f.tree, container, px(0, 100));
    CHECK(f.bounds(stuck).top == 10.0f);
    std::vector<uint8_t> scrolled = render(f.tree, 200, 200);
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    CHECK(pixel(scrolled, 200, 5, 15) == "255 0 0 255");
    CHECK(pixel(scrolled, 200, 5, 5) == "0 0 0 0");
    CHECK(f.hit(PrVec2{5, 15}) == stuck);
    // Its containing block holds it: the holder ends at 250, so the box goes no lower than 230.
    prLayoutNodeSetScroll(f.tree, container, px(0, 300));
    CHECK(f.bounds(stuck).top == -70.0f);
    // Layout coordinates leave it where it was laid out, and positioned coordinates move it.
    CHECK(topOf(f, stuck, PrLayoutCoordinates_Layout) == 50.0f);
    CHECK(topOf(f, stuck, PrLayoutCoordinates_Positioned) == 230.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a bottom inset holds a sticky box up from the viewport's end until it reaches its place in flow") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    f.element(root, block(300));
    PrLayoutNode stuck = f.element(root, sticky(block(20), kAuto, kAuto, 0, kAuto));
    f.element(root, block(1000));
    f.layout(px(200, 100));
    CHECK(f.bounds(stuck).top == 80.0f);
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 250));
    CHECK(f.bounds(stuck).top == 50.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a sticky view rectangle smaller than the box grows past its end side, so the start inset wins") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(200));
    PrLayoutNode stuck = f.element(container, sticky(block(150), 10, kAuto, 10, kAuto));
    f.element(container, block(500));
    f.layout(px(200, 200));
    // Laid out below the scrollport, it is held up only as far as the top inset allows.
    CHECK(f.bounds(stuck).top == 10.0f);
    prLayoutNodeSetScroll(f.tree, container, px(0, 300));
    CHECK(f.bounds(stuck).top == 10.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a sticky box in another moves with it, and stays in it") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(100));
    PrLayoutNode outer = f.element(container, sticky(block(60), 0, kAuto, kAuto, kAuto));
    PrLayoutNode inner = f.element(outer, sticky(block(20), 20, kAuto, kAuto, kAuto));
    f.element(container, block(500));
    f.layout(px(200, 200));
    prLayoutNodeSetScroll(f.tree, container, px(0, 150));
    CHECK(f.bounds(outer).top == 0.0f);
    CHECK(f.bounds(inner).top == 20.0f);
    // The inner box stays in the outer box's content box, whose end is 60px below its top.
    PrLayoutBoxStyle taller = sticky(block(20), 50, kAuto, kAuto, kAuto);
    prLayoutNodeSetStyle(f.tree, inner, f.style(taller));
    f.layout(px(200, 200));
    CHECK(f.bounds(inner).top == 40.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a sticky box moves the boxes out of flow that it holds, and not the fixed boxes the viewport holds") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    f.element(root, block(100));
    PrLayoutNode stuck = f.element(root, sticky(block(20), 0, kAuto, kAuto, kAuto));
    PrLayoutBoxStyle absolute = block(10);
    absolute.width = prLayoutLengthPx(10);
    absolute.position = PrLayoutPosition_Absolute;
    absolute.inset[PrRectSide_Top] = prLayoutLengthPx(5);
    absolute.inset[PrRectSide_Left] = prLayoutLengthPx(50);
    PrLayoutNode held = f.element(stuck, absolute);
    PrLayoutBoxStyle fixed = absolute;
    fixed.position = PrLayoutPosition_Fixed;
    fixed.inset[PrRectSide_Top] = prLayoutLengthPx(50);
    PrLayoutDecorationStyle red = background(kRed);
    PrLayoutNode escaped = f.element(stuck, fixed, &red);
    f.element(root, block(1000));
    f.layout(px(200, 100));
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 300));
    CHECK(f.bounds(stuck).top == 0.0f);
    CHECK(f.bounds(held).top == 5.0f);
    CHECK(f.bounds(escaped).top == 50.0f);
    CHECK(pixel(render(f.tree, 200, 100), 200, 55, 55) == "255 0 0 255");
    CHECK(f.hit(PrVec2{55, 55}) == escaped);
    CHECK(verified(f.tree));
}

TEST_CASE("a sticky box with a layer of its own sticks with its effects, and recording follows no scroll") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(50));
    PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
    faded.opacity = 0.5f;
    PrLayoutDecorationStyle red = background(kRed);
    f.element(container, sticky(block(20), 0, kAuto, kAuto, kAuto), &red, &faded);
    f.element(container, block(500));
    f.layout(px(200, 200));
    CHECK(pixel(render(f.tree, 200, 200), 200, 5, 55) == "128 0 0 128");
    prLayoutNodeSetScroll(f.tree, container, px(0, 200));
    std::vector<uint8_t> scrolled = render(f.tree, 200, 200);
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    CHECK(pixel(scrolled, 200, 5, 5) == "128 0 0 128");
    CHECK(verified(f.tree));
}

TEST_CASE("a scene moves a sticky box by the offsets a player gives, not the tree's") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(50));
    PrLayoutDecorationStyle red = background(kRed);
    f.element(container, sticky(block(20), 0, kAuto, kAuto, kAuto), &red);
    f.element(container, block(500));
    f.layout(px(200, 200));
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    PrLayoutSceneScroll scroll{container, px(0, 200)};
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = &scroll;
    options.scrollCount = 1;
    PrDrawList* list = prDrawListCreate();
    prLayoutScenePlay(scene, list, &options);
    std::vector<uint8_t> played = drawn(list, 200, 200);
    prDrawListRelease(list);
    prLayoutSceneRelease(scene);
    CHECK(pixel(played, 200, 5, 5) == "255 0 0 255");
    CHECK(pixel(played, 200, 5, 55) == "0 0 0 0");
}

TEST_CASE("a sticky box with no insets stays where it was laid out") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(50));
    PrLayoutNode unset = f.element(container, sticky(block(20), kAuto, kAuto, kAuto, kAuto));
    f.element(container, block(500));
    f.layout(px(200, 200));
    prLayoutNodeSetScroll(f.tree, container, px(0, 60));
    CHECK(f.bounds(unset).top == -10.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a sticky inline box moves its parts, its text and the blocks in it, which are hit where they are drawn") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(50));
    PrLayoutNode line = f.element(container, block(200));
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(20);
    font.lineHeight = prLayoutLineHeightPx(20);
    PrLayoutNode span = f.element(line, sticky(inlineBox(), 10, kAuto, kAuto, kAuto), nullptr, nullptr, &font);
    PrLayoutNode text = f.text(span, "a");
    PrLayoutDecorationStyle red = background(kRed);
    PrLayoutBoxStyle inner = block(20);
    inner.width = prLayoutLengthPx(20);
    PrLayoutNode held = f.element(span, inner, &red);
    f.element(container, block(500));
    f.layout(px(200, 200));
    float spanTop = f.bounds(span).top;
    float textTop = f.bounds(text).top;
    float heldTop = f.bounds(held).top;
    // Scrolled 100px, the box stays 10px below the scrollport's top, and what is in it moves as far.
    prLayoutNodeSetScroll(f.tree, container, px(0, 100));
    float moved = f.bounds(span).top - (spanTop - 100);
    CHECK(f.bounds(span).top == 10.0f);
    CHECK(f.bounds(text).top == textTop - 100 + moved);
    CHECK(f.bounds(held).top == heldTop - 100 + moved);
    float drawnAt = f.bounds(held).top + 5;
    CHECK(pixel(render(f.tree, 200, 200), 200, 5, uint32_t(drawnAt)) == "255 0 0 255");
    CHECK(f.hit(PrVec2{5, drawnAt}) == held);
    CHECK(verified(f.tree));
}

TEST_CASE("a sticky row group paints the backgrounds of its tracks behind its cells, over the rows it passes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(50));
    PrLayoutNode table = f.element(container, part(PrLayoutDisplay_Table));
    PrLayoutDecorationStyle green = background(kGreen);
    PrLayoutBoxStyle head = sticky(part(PrLayoutDisplay_TableHeaderGroup), 0, kAuto, kAuto, kAuto);
    PrLayoutNode thead = f.element(table, head, &green);
    row(f, thead, nullptr);
    PrLayoutNode tbody = f.element(table, part(PrLayoutDisplay_TableRowGroup));
    PrLayoutDecorationStyle red = background(kRed);
    for (int i = 0; i < 5; ++i)
        row(f, tbody, &red);
    f.element(container, block(500));
    f.layout(px(200, 200));
    CHECK(pixel(render(f.tree, 200, 200), 200, 5, 55) == "0 255 0 255");
    // Scrolled past it, the head stays at the top over the body's cells, until the table's end holds it.
    prLayoutNodeSetScroll(f.tree, container, px(0, 80));
    CHECK(f.bounds(thead).top == 0.0f);
    std::vector<uint8_t> scrolled = render(f.tree, 200, 200);
    CHECK(pixel(scrolled, 200, 5, 5) == "0 255 0 255");
    CHECK(pixel(scrolled, 200, 5, 25) == "255 0 0 255");
    prLayoutNodeSetScroll(f.tree, container, px(0, 200));
    CHECK(f.bounds(thead).top == -50.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a sticky cell paints its row's background behind it, and paints it again when the row's changes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, scroller(100, 100));
    f.element(container, block(50));
    PrLayoutNode table = f.element(container, part(PrLayoutDisplay_Table));
    PrLayoutDecorationStyle green = background(kGreen);
    PrLayoutNode first = f.element(table, part(PrLayoutDisplay_TableRow), &green);
    PrLayoutNode cell = f.element(first, sticky(part(PrLayoutDisplay_TableCell), 0, kAuto, kAuto, kAuto));
    PrLayoutBoxStyle content = block(20);
    content.width = prLayoutLengthPx(20);
    f.element(cell, content);
    PrLayoutDecorationStyle red = background(kRed);
    for (int i = 0; i < 5; ++i)
        row(f, table, &red);
    f.element(container, block(500));
    f.layout(px(200, 200));
    prLayoutNodeSetScroll(f.tree, container, px(0, 80));
    CHECK(f.bounds(cell).top == 0.0f);
    CHECK(pixel(render(f.tree, 200, 200), 200, 5, 5) == "0 255 0 255");
    PrLayoutDecorationStyle blue = background(PrColor{0, 0, 1, 1});
    prLayoutNodeSetStyle(f.tree, first, f.style(part(PrLayoutDisplay_TableRow), &blue));
    f.layout(px(200, 200));
    CHECK(pixel(render(f.tree, 200, 200), 200, 5, 5) == "0 0 255 255");
    CHECK(verified(f.tree));
}
