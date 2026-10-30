// Layout in every writing mode: boxes stacked in vertical block flows, orthogonal flow roots sized in their own axes in
// a space their containing block makes, and lines of text set sideways or upright, aligned by their dominant baselines.

#include "layout/tests/layout_fixture.h"

namespace pr_impl {

namespace {

// A page of horizontal-tb blocks in a viewport of 800 by 600, holding stages of 20px Arimo on 40px lines.
struct Page {
    Fixture f;
    PrLayoutNode root = kPrLayoutNodeNone;

    Page() { root = f.element(kPrLayoutNodeNone, block()); }

    // Returns a font of 20px on 40px lines, in `mode` and `direction`.
    static PrLayoutFontStyle font(PrLayoutWritingMode mode = PrLayoutWritingMode_HorizontalTb,
            PrTextDirection direction = PrTextDirection_Ltr) {
        PrLayoutFontStyle font = prLayoutFontStyleDefault();
        font.font = textFontOf(20);
        font.lineHeight = prLayoutLineHeightPx(40);
        font.writingMode = mode;
        font.direction = direction;
        return font;
    }

    // Creates an element under `parent` styled by `box` and `font`.
    PrLayoutNode styled(PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutFontStyle& style) {
        return f.element(parent, box, nullptr, nullptr, &style);
    }

    // Creates an element under `parent` styled by `box`, in `mode` and `direction`.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box,
            PrLayoutWritingMode mode = PrLayoutWritingMode_HorizontalTb,
            PrTextDirection direction = PrTextDirection_Ltr) {
        PrLayoutFontStyle style = font(mode, direction);
        return f.element(parent, box, nullptr, nullptr, &style);
    }

    // Creates a flow-root stage under the root, `width` wide and `height` tall, each auto if negative.
    PrLayoutNode stage(float width, float height, PrLayoutWritingMode mode = PrLayoutWritingMode_HorizontalTb,
            PrTextDirection direction = PrTextDirection_Ltr) {
        PrLayoutBoxStyle box = block();
        box.display = PrLayoutDisplay_FlowRoot;
        if (width >= 0)
            box.width = prLayoutLengthPx(width);
        if (height >= 0)
            box.height = prLayoutLengthPx(height);
        return add(root, box, mode, direction);
    }

    void layout(float height = 600) {
        f.layout(px(800, height));
        CHECK(verified(f.tree));
    }

    // Returns the node's border box relative to the top left of `from`.
    PrRect rect(PrLayoutNode node, PrLayoutNode from) {
        PrRect r = f.bounds(node);
        PrRect origin = f.bounds(from);
        return PrRect{r.left - origin.left, r.top - origin.top, r.right - origin.left, r.bottom - origin.top};
    }
    double left(PrLayoutNode node, PrLayoutNode from) { return double(rect(node, from).left); }
    double top(PrLayoutNode node, PrLayoutNode from) { return double(rect(node, from).top); }
    double width(PrLayoutNode node) { return double(f.bounds(node).right - f.bounds(node).left); }
    double height(PrLayoutNode node) { return double(f.bounds(node).bottom - f.bounds(node).top); }
};

// Returns a block `width` by `height`, each auto if negative.
PrLayoutBoxStyle sized(float width, float height) {
    PrLayoutBoxStyle box = block();
    if (width >= 0)
        box.width = prLayoutLengthPx(width);
    if (height >= 0)
        box.height = prLayoutLengthPx(height);
    return box;
}

// Returns a float to `side`, 30 wide and 20 tall.
PrLayoutBoxStyle floated(PrLayoutFloat side) {
    PrLayoutBoxStyle box = sized(30, 20);
    box.floating = side;
    return box;
}

constexpr PrLayoutWritingMode kRl = PrLayoutWritingMode_VerticalRl;
constexpr PrLayoutWritingMode kLr = PrLayoutWritingMode_VerticalLr;

// Text long enough to wrap at any measure a case uses.
constexpr const char* kLong = "aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll mmm nnn ooo ppp qqq rrr sss ttt uuu "
    "vvv www xxx yyy zzz";

// Returns a scroll container `width` by `height` with `padding` on each side.
PrLayoutBoxStyle scroller(float width, float height, float padding = 0) {
    PrLayoutBoxStyle box = sized(width, height);
    box.overflowX = PrLayoutOverflow_Scroll;
    box.overflowY = PrLayoutOverflow_Scroll;
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.padding[side] = prLayoutLengthPx(padding);
    return box;
}

// The scroll state of `node`: its smallest and greatest offsets and its scrollable overflow, in pixels.
struct Scroll {
    PrRect range;
    PrRect content;
};
Scroll scrollOf(Page& p, PrLayoutNode node) {
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(p.f.tree, node, &info));
    return Scroll{prLayoutRectToPx(info.range), prLayoutRectToPx(info.content)};
}

// ---------------------------------------------------------------------------------------------------------------------
// Lines in vertical modes

// A glyph as painted: its id and its origin, in the tree's space.
struct Painted {
    uint16_t id;
    double x;
    double y;
    bool turned;                    // true if drawn under a quarter turn
};

// Appends the glyphs that `list` draws under `transform`, including through the lists it embeds.
void appendPainted(const PrDrawList* list, PrDAffine2 transform, std::vector<Painted>& out) {
    std::vector<PrDAffine2> stack{transform};
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        const PrDrawCommand& command = commands[i];
        const PrDAffine2& t = stack.back();
        switch (command.kind) {
            case PrDrawCommandKind_Embed: appendPainted(command.embed.list, t, out); break;
            case PrDrawCommandKind_PushTransformAffine2: {
                const PrDAffine2& m = command.affine;
                stack.push_back(PrDAffine2{
                    t.a * m.a + t.c * m.b, t.b * m.a + t.d * m.b, t.a * m.c + t.c * m.d,
                    t.b * m.c + t.d * m.d, t.a * m.e + t.c * m.f + t.e,
                    t.b * m.e + t.d * m.f + t.f,
                });
                break;
            }
            case PrDrawCommandKind_PopTransform: stack.pop_back(); break;
            case PrDrawCommandKind_Glyphs:
                for (uint32_t g = 0; g < command.glyphs->glyphCount; ++g) {
                    PrVec2 p = command.glyphs->glyphs[g].position;
                    double x = double(p.x);
                    double y = double(p.y);
                    out.push_back(Painted{
                        command.glyphs->glyphs[g].id, t.a * x + t.c * y + t.e,
                        t.b * x + t.d * y + t.f,
                        t.b != 0,
                    });
                }
                break;
            default: break;
        }
    }
}

// Returns the glyphs that the tree paints, in the order it paints them.
std::vector<Painted> painted(PrLayoutTree* tree) {
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(tree, list);
    std::vector<Painted> out;
    appendPainted(list, PrDAffine2{1, 0, 0, 1, 0, 0}, out);
    prDrawListRelease(list);
    return out;
}

constexpr PrLayoutWritingMode kSwRl = PrLayoutWritingMode_SidewaysRl;
constexpr PrLayoutWritingMode kSwLr = PrLayoutWritingMode_SidewaysLr;

// Returns a font of 20px on 40px lines in `mode`, set as `orientation`.
PrLayoutFontStyle oriented(PrLayoutWritingMode mode, PrLayoutTextOrientation orientation) {
    PrLayoutFontStyle font = Page::font(mode);
    font.textOrientation = orientation;
    return font;
}

// Returns an inline-block `width` by `height`.
PrLayoutBoxStyle inlineBlock(float width, float height) {
    PrLayoutBoxStyle box = sized(width, height);
    box.display = PrLayoutDisplay_InlineBlock;
    return box;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vertical-rl blocks stack from the right, and their stage is as wide as they are") {
    Page p;
    PrLayoutNode stage = p.stage(-1, 200, kRl);
    PrLayoutNode first = p.add(stage, sized(30, -1), kRl);
    PrLayoutNode second = p.add(stage, sized(40, -1), kRl);
    p.layout();
    CHECK(p.left(first, stage) == 40);
    CHECK(p.left(second, stage) == 0);
    CHECK(p.width(stage) == 70);
    CHECK(p.height(first) == 200);
}

TEST_CASE("block-axis margins collapse across a vertical flow") {
    for (PrLayoutWritingMode mode : {kRl, kLr}) {
        CAPTURE(int(mode));
        Page p;
        PrLayoutNode stage = p.stage(200, 200, mode);
        // The first box's block-end margin, 20, and the second's block-start margin, 30, collapse to 30.
        PrRectSide end = mode == kRl ? PrRectSide_Left : PrRectSide_Right;
        PrRectSide start = mode == kRl ? PrRectSide_Right : PrRectSide_Left;
        PrLayoutBoxStyle one = sized(30, -1);
        one.margin[end] = prLayoutLengthPx(20);
        PrLayoutBoxStyle two = sized(40, -1);
        two.margin[start] = prLayoutLengthPx(30);
        PrLayoutNode a = p.add(stage, one, mode);
        PrLayoutNode b = p.add(stage, two, mode);
        p.layout();
        CHECK(p.left(a, stage) == (mode == kRl ? 170 : 0));
        CHECK(p.left(b, stage) == (mode == kRl ? 100 : 60));
    }
}

TEST_CASE("auto inline margins center a block in a vertical flow, and a line is as wide as its height") {
    Page p;
    PrLayoutNode stage = p.stage(200, 200, kRl);
    PrLayoutBoxStyle centered = sized(20, 100);
    centered.margin[PrRectSide_Top] = prLayoutLengthAuto();
    centered.margin[PrRectSide_Bottom] = prLayoutLengthAuto();
    PrLayoutNode box = p.add(stage, centered, kRl);
    PrLayoutNode line = p.add(stage, block(), kRl);
    p.f.text(line, "a");
    p.layout();
    CHECK(p.top(box, stage) == 50);
    CHECK(p.height(box) == 100);
    CHECK(p.width(line) == 40);
    CHECK(p.left(line, stage) == 140);
}

TEST_CASE("percentage margins in a vertical flow resolve against its inline size") {
    Page p;
    PrLayoutNode stage = p.stage(200, 100, kLr);
    PrLayoutBoxStyle one = sized(20, -1);
    one.margin[PrRectSide_Top] = prLayoutLengthPercent(10);
    PrLayoutBoxStyle two = sized(20, -1);
    two.margin[PrRectSide_Left] = prLayoutLengthPercent(10);
    PrLayoutNode a = p.add(stage, one, kLr);
    PrLayoutNode b = p.add(stage, two, kLr);
    p.layout();
    CHECK(p.top(a, stage) == 10);
    CHECK(p.left(b, stage) == 30);
}

TEST_CASE("floats in a vertical flow go to the line-left side, the top") {
    {
        Page p;
        PrLayoutNode stage = p.stage(200, 200, kRl);
        PrLayoutNode left = p.add(stage, floated(PrLayoutFloat_Left), kRl);
        PrLayoutNode right = p.add(stage, floated(PrLayoutFloat_Right), kRl);
        PrLayoutBoxStyle clearing = floated(PrLayoutFloat_Left);
        clearing.clear = PrLayoutClear_Right;
        PrLayoutNode third = p.add(stage, clearing, kRl);
        p.layout();
        CHECK(p.left(left, stage) == 170);
        CHECK(p.top(left, stage) == 0);
        CHECK(p.left(right, stage) == 170);
        CHECK(p.top(right, stage) == 180);
        CHECK(p.left(third, stage) == 140);
        CHECK(p.top(third, stage) == 0);
    }
    {
        Page p;
        PrLayoutNode stage = p.stage(200, 200, kLr);
        PrLayoutNode left = p.add(stage, floated(PrLayoutFloat_Left), kLr);
        p.layout();
        CHECK(p.left(left, stage) == 0);
        CHECK(p.top(left, stage) == 0);
    }
}

TEST_CASE("in vertical-rl rtl, a block and an inline-start float start at the bottom") {
    Page p;
    PrLayoutNode stage = p.stage(200, 200, kRl, PrTextDirection_Rtl);
    PrLayoutNode start = p.add(stage, floated(PrLayoutFloat_InlineStart), kRl, PrTextDirection_Rtl);
    PrLayoutBoxStyle avoider = sized(20, 100);
    avoider.overflowX = PrLayoutOverflow_Hidden;
    avoider.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutNode box = p.add(stage, avoider, kRl, PrTextDirection_Rtl);
    p.layout();
    CHECK(p.top(start, stage) == 180);
    CHECK(p.top(box, stage) == 80);
}

TEST_CASE("an orthogonal flow root fits its content in its containing block's block size") {
    // Two lines of text in a stage 100 tall: the box is as tall as the stage, and as wide as its lines.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, 100);
        PrLayoutNode box = p.add(stage, block(), kRl);
        p.f.text(box, "aaa bbb ccc ddd");
        p.layout();
        CHECK(p.height(box) == 100);
        CHECK(p.width(box) == 80);
        CHECK(p.left(box, stage) == 0);
    }
    // Text shorter than the stage: the box takes its max-content size.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, 100);
        PrLayoutNode box = p.add(stage, block(), kRl);
        p.f.text(box, "ab");
        p.layout();
        CHECK(p.height(box) == doctest::Approx(22.25).epsilon(0.01));
        CHECK(p.width(box) == 40);
    }
    // A fixed inline size wraps the text there.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, -1);
        PrLayoutNode box = p.add(stage, sized(-1, 80), kRl);
        p.f.text(box, "aaa bbb ccc ddd");
        p.layout();
        CHECK(p.width(box) == 80);
    }
}

TEST_CASE("an orthogonal flow root in an indefinite block size falls back to its limit, a scroller or the viewport") {
    // The initial containing block's height.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, -1);
        PrLayoutNode box = p.add(stage, block(), kRl);
        p.f.text(box, kLong);
        p.layout();
        CHECK(p.height(box) == 600);
        p.layout(500);
        CHECK(p.height(box) == 500);
    }
    // The containing block's max-height.
    {
        Page p;
        PrLayoutBoxStyle limited = sized(200, -1);
        limited.display = PrLayoutDisplay_FlowRoot;
        limited.maxHeight = prLayoutLengthPx(120);
        PrLayoutNode stage = p.add(p.root, limited);
        PrLayoutNode box = p.add(stage, block(), kRl);
        p.f.text(box, kLong);
        p.layout();
        CHECK(p.height(box) == 120);
    }
    // The nearest scroll container, through a block of auto height, and its max-height if that is smaller.
    for (float maxHeight : {-1.0f, 100.0f}) {
        CAPTURE(maxHeight);
        Page p;
        PrLayoutBoxStyle scroller = sized(200, 150);
        scroller.overflowX = PrLayoutOverflow_Scroll;
        scroller.overflowY = PrLayoutOverflow_Scroll;
        if (maxHeight >= 0)
            scroller.maxHeight = prLayoutLengthPx(maxHeight);
        PrLayoutNode stage = p.add(p.root, scroller);
        PrLayoutNode between = p.add(stage, block());
        PrLayoutNode box = p.add(between, block(), kRl);
        p.f.text(box, kLong);
        p.layout();
        CHECK(p.height(box) == (maxHeight >= 0 ? double(maxHeight) : 150.0));
        // The viewport no longer bounds it, but the scroller's own size does.
        p.layout(500);
        CHECK(p.height(box) == (maxHeight >= 0 ? double(maxHeight) : 150.0));
    }
}

TEST_CASE("an orthogonal flow root's percentages cross axes") {
    // Its height is its inline size, of the stage's block size; its width is its block size, of the stage's inline
    // size.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, 100);
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPercent(50);
        box.height = prLayoutLengthPercent(50);
        PrLayoutNode node = p.add(stage, box, kRl);
        p.layout();
        CHECK(p.height(node) == 50);
        CHECK(p.width(node) == 100);
    }
    // An inline size of an indefinite block size is a percentage of the fallback.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, -1);
        PrLayoutBoxStyle box = block();
        box.height = prLayoutLengthPercent(50);
        PrLayoutNode node = p.add(stage, box, kRl);
        p.f.text(node, "ab");
        p.layout();
        CHECK(p.height(node) == 300);
    }
    // Its margins resolve against its containing block's inline size, in either axis.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, -1);
        PrLayoutBoxStyle box = sized(20, 20);
        box.margin[PrRectSide_Top] = prLayoutLengthPercent(10);
        PrLayoutNode node = p.add(stage, box, kRl);
        p.layout();
        CHECK(p.top(node, stage) == 20);
    }
}

TEST_CASE("an orthogonal flow root is placed as a block-level box in its containing block's flow") {
    // Its block-start margin collapses with the margin before it.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, -1);
        PrLayoutBoxStyle before = sized(-1, 10);
        before.margin[PrRectSide_Bottom] = prLayoutLengthPx(30);
        p.add(stage, before);
        PrLayoutBoxStyle box = sized(-1, 40);
        box.margin[PrRectSide_Top] = prLayoutLengthPx(20);
        PrLayoutNode node = p.add(stage, box, kRl);
        p.f.text(node, "a");
        p.layout();
        CHECK(p.top(node, stage) == 40);
    }
    // Auto inline margins center it by its block size.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, -1);
        PrLayoutBoxStyle box = sized(40, 40);
        box.margin[PrRectSide_Left] = prLayoutLengthAuto();
        box.margin[PrRectSide_Right] = prLayoutLengthAuto();
        PrLayoutNode node = p.add(stage, box, kRl);
        p.layout();
        CHECK(p.left(node, stage) == 80);
    }
    // A horizontal box in a vertical stage is the reverse case.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, 200, kRl);
        PrLayoutNode node = p.add(stage, block());
        p.f.text(node, "aaa bbb ccc ddd eee fff ggg hhh iii jjj");
        p.layout();
        CHECK(p.width(node) == 200);
        CHECK(p.height(node) == 80);
        CHECK(p.left(node, stage) == 0);
    }
}

TEST_CASE("an orthogonal flow root contributes its block size to its containing block's intrinsic sizes") {
    // An inline-block, a float and a min-content box around a 60 tall box of three lines are as wide as its lines.
    auto around = [](PrLayoutBoxStyle outer) {
        Page p;
        PrLayoutNode stage = p.stage(200, -1);
        PrLayoutNode wrapper = p.add(stage, outer);
        PrLayoutNode box = p.add(wrapper, sized(-1, 60), kRl);
        p.f.text(box, "aaa bbb ccc");
        p.layout();
        CHECK(p.width(box) == 120);
        return p.width(wrapper);
    };
    PrLayoutBoxStyle inlineBlock = prLayoutBoxStyleDefault();
    inlineBlock.display = PrLayoutDisplay_InlineBlock;
    CHECK(around(inlineBlock) == 120);
    PrLayoutBoxStyle floating = block();
    floating.floating = PrLayoutFloat_Left;
    CHECK(around(floating) == 120);
    PrLayoutBoxStyle minContent = block();
    minContent.width = prLayoutLengthMinContent();
    CHECK(around(minContent) == 120);
    // With its height auto, it is laid out against the fallback, on one line.
    {
        Page p;
        PrLayoutNode stage = p.stage(200, -1);
        PrLayoutNode wrapper = p.add(stage, inlineBlock);
        PrLayoutNode box = p.add(wrapper, block(), kRl);
        p.f.text(box, "aaa bbb ccc");
        p.layout();
        CHECK(p.width(wrapper) == 40);
    }
}

TEST_CASE("an orthogonal flow root lays out again where its containing block's sizes change, and not otherwise") {
    Page p;
    PrLayoutNode stage = p.stage(200, 100);
    PrLayoutNode box = p.add(stage, block(), kRl);
    p.f.text(box, "aaa bbb ccc ddd");
    p.layout();
    CHECK(p.width(box) == 80);
    // A new viewport height lays out nothing: the root, whose percentages it is the base of, has none, and the stage
    // and the box are reused.
    p.layout(500);
    CHECK(prLayoutTreeStats(p.f.tree).blocksLaidOut == 0);
    // A taller stage wraps the text once.
    PrLayoutBoxStyle taller = sized(200, 200);
    taller.display = PrLayoutDisplay_FlowRoot;
    PrLayoutFontStyle font = Page::font();
    prLayoutNodeSetStyle(p.f.tree, stage, p.f.style(taller, nullptr, nullptr, &font));
    p.layout(500);
    CHECK(p.width(box) == 40);
}

TEST_CASE("the fallback an orthogonal flow root read is compared where it is reused") {
    // Two scroll containers around a block of auto height around the box. Each change is checked against a layout from
    // scratch, and the box takes the nearest scroller's height.
    Page p;
    auto scroller = [](float height, bool scrolls, float maxHeight = -1) {
        PrLayoutBoxStyle box = sized(200, height);
        if (maxHeight >= 0)
            box.maxHeight = prLayoutLengthPx(maxHeight);
        box.overflowX = scrolls ? PrLayoutOverflow_Scroll : PrLayoutOverflow_Visible;
        box.overflowY = box.overflowX;
        return box;
    };
    PrLayoutFontStyle font = Page::font();
    PrLayoutNode outer = p.add(p.root, scroller(300, true));
    PrLayoutNode inner = p.add(outer, scroller(200, true));
    PrLayoutNode between = p.add(inner, block());
    PrLayoutNode box = p.add(between, block(), kRl);
    p.f.text(box, kLong);
    p.layout();
    CHECK(p.height(box) == 200);
    // The nearest scroller's height changes, through the unchanged block between.
    prLayoutNodeSetStyle(p.f.tree, inner, p.f.style(scroller(150, true), nullptr, nullptr, &font));
    p.layout();
    CHECK(p.height(box) == 150);
    // A smaller max-height wins over its height.
    prLayoutNodeSetStyle(p.f.tree, inner, p.f.style(scroller(150, true, 90), nullptr, nullptr, &font));
    p.layout();
    CHECK(p.height(box) == 90);
    // It stops scrolling, so the outer one is nearest.
    prLayoutNodeSetStyle(p.f.tree, inner, p.f.style(scroller(150, false), nullptr, nullptr, &font));
    p.layout();
    CHECK(p.height(box) == 300);
    // The outer one stops too, so the viewport is nearest, and then the viewport's extent changes.
    prLayoutNodeSetStyle(p.f.tree, outer, p.f.style(scroller(300, false), nullptr, nullptr, &font));
    p.layout();
    CHECK(p.height(box) == 600);
    p.layout(400);
    CHECK(p.height(box) == 400);
    // A scroller of auto height falls back to the viewport, and reads its extent.
    prLayoutNodeSetStyle(p.f.tree, inner, p.f.style(scroller(-1, true), nullptr, nullptr, &font));
    p.layout(400);
    CHECK(p.height(box) == 400);
    p.layout(350);
    CHECK(p.height(box) == 350);
}

TEST_CASE("an orthogonal flow root in a box out of flow lays out again where the viewport's extent changes") {
    // The box is absolutely positioned in a relatively positioned block of auto height, so the root falls back to the
    // viewport's height. Two floats 50 tall share a line only where the viewport is 100 tall.
    Page p;
    PrLayoutNode stage = p.stage(10, 10);
    PrLayoutBoxStyle relative = block();
    relative.position = PrLayoutPosition_Relative;
    PrLayoutNode holder = p.add(stage, relative);
    PrLayoutBoxStyle absolute = block();
    absolute.position = PrLayoutPosition_Absolute;
    PrLayoutNode positioned = p.add(holder, absolute);
    PrLayoutNode box = p.add(positioned, sized(100, -1), kRl);
    PrLayoutBoxStyle floating = sized(100, 50);
    floating.floating = PrLayoutFloat_Left;
    PrLayoutNode first = p.add(box, floating, kRl);
    PrLayoutNode second = p.add(box, floating, kRl);
    p.layout(50);
    CHECK(p.left(second, first) == -100);
    CHECK(p.top(second, first) == 0);
    p.layout(100);
    CHECK(p.left(second, first) == 0);
    CHECK(p.top(second, first) == 50);
}

TEST_CASE("an orthogonal flow root in a box out of flow falls back to the scroller that holds the box") {
    // The box is absolutely positioned in a block of auto height in a positioned scroll container, which holds it.
    Page p;
    PrLayoutBoxStyle scroll = scroller(200, 150);
    scroll.position = PrLayoutPosition_Relative;
    PrLayoutNode holder = p.add(p.root, scroll);
    PrLayoutNode between = p.add(holder, block());
    PrLayoutBoxStyle absolute = block();
    absolute.position = PrLayoutPosition_Absolute;
    PrLayoutNode positioned = p.add(between, absolute);
    PrLayoutNode box = p.add(positioned, block(), kRl);
    p.f.text(box, kLong);
    p.layout();
    CHECK(p.height(box) == 150);
    // The viewport no longer bounds it, but the scroller's own size does.
    p.layout(500);
    CHECK(p.height(box) == 150);
    scroll.height = prLayoutLengthPx(120);
    PrLayoutFontStyle font = Page::font();
    prLayoutNodeSetStyle(p.f.tree, holder, p.f.style(scroll, nullptr, nullptr, &font));
    p.layout(500);
    CHECK(p.height(box) == 120);
}

TEST_CASE("an intrinsic size that read the fallback is measured again where it changes") {
    // A shrink-to-fit float around the box, whose height is auto, in a scroller: the float's width is the width of the
    // box's lines.
    Page p;
    PrLayoutBoxStyle scroll = sized(200, 100);
    scroll.overflowX = PrLayoutOverflow_Scroll;
    scroll.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode scroller = p.add(p.root, scroll);
    PrLayoutNode between = p.add(scroller, block());
    PrLayoutBoxStyle floating = block();
    floating.floating = PrLayoutFloat_Left;
    PrLayoutNode wrapper = p.add(between, floating);
    PrLayoutNode box = p.add(wrapper, block(), kRl);
    p.f.text(box, "aaa bbb ccc ddd");
    p.layout();
    CHECK(p.width(wrapper) == 80);
    scroll.height = prLayoutLengthPx(200);
    PrLayoutFontStyle font = Page::font();
    prLayoutNodeSetStyle(p.f.tree, scroller, p.f.style(scroll, nullptr, nullptr, &font));
    p.layout();
    CHECK(p.width(wrapper) == 40);
}

TEST_CASE("a scroll container scrolls from its writing mode's start corner, with offsets below zero toward it") {
    // A 300 child in a 100 scroller: in rtl it overflows left, in vertical-rl it overflows left, and in vertical-rl rtl
    // it overflows up.
    struct Case {
        PrLayoutWritingMode mode;
        PrTextDirection direction;
        PrRect range;
    };
    const Case cases[] = {
        {PrLayoutWritingMode_HorizontalTb, PrTextDirection_Ltr, {0, 0, 200, 0}},
        {PrLayoutWritingMode_HorizontalTb, PrTextDirection_Rtl, {-200, 0, 0, 0}},
        {kRl, PrTextDirection_Ltr, {-200, 0, 0, 0}},
        {kRl, PrTextDirection_Rtl, {0, -200, 0, 0}},
        {kLr, PrTextDirection_Ltr, {0, 0, 200, 0}},
    };
    for (const Case& c : cases) {
        CAPTURE(int(c.mode));
        CAPTURE(int(c.direction));
        Page p;
        PrLayoutNode box = p.add(p.root, scroller(100, 100), c.mode, c.direction);
        bool horizontal = c.mode == PrLayoutWritingMode_HorizontalTb;
        bool inlineOverflow = horizontal || c.direction == PrTextDirection_Rtl;
        PrLayoutNode child =
            p.add(box, inlineOverflow == horizontal ? sized(300, 10) : sized(10, 300), c.mode, c.direction);
        p.layout();
        Scroll s = scrollOf(p, box);
        CHECK(s.range.left == c.range.left);
        CHECK(s.range.top == c.range.top);
        CHECK(s.range.right == c.range.right);
        CHECK(s.range.bottom == c.range.bottom);
        // Scrolled as far as it goes, the child's far edge is at the scrollport's far edge.
        prLayoutNodeSetScroll(p.f.tree, box, px(c.range.left + c.range.right, c.range.top + c.range.bottom));
        p.layout();
        PrRect port = p.f.bounds(box);
        PrRect seen = prLayoutRectToPx(prLayoutNodeBounds(p.f.tree, child, PrLayoutCoordinates_Viewport));
        if (c.range.left < 0)
            CHECK(seen.left == port.left);
        if (c.range.right > 0)
            CHECK(seen.right == port.right);
        if (c.range.top < 0)
            CHECK(seen.top == port.top);
    }
}

TEST_CASE("a scroll container's padding shows past its in-flow children, from each origin, and not past a descendant") {
    // 100 wide with 20 padding around a 300 child: it scrolls through 340, toward its end in either direction. If the
    // child is in a wrapper 100 wide, the scroller's padding is past the wrapper, and the child reaches 320.
    for (bool wrapped : {false, true}) {
        for (PrTextDirection direction : {PrTextDirection_Ltr, PrTextDirection_Rtl}) {
            CAPTURE(wrapped);
            CAPTURE(int(direction));
            Page p;
            PrLayoutNode box = p.add(p.root, scroller(100, 50, 20), PrLayoutWritingMode_HorizontalTb, direction);
            PrLayoutNode parent = wrapped ? p.add(box, block(), PrLayoutWritingMode_HorizontalTb, direction) : box;
            p.add(parent, sized(300, 10), PrLayoutWritingMode_HorizontalTb, direction);
            p.layout();
            Scroll s = scrollOf(p, box);
            CHECK(s.content.right - s.content.left == (wrapped ? 320.0f : 340.0f));
            CHECK(s.range.right - s.range.left == (wrapped ? 180.0f : 200.0f));
            CHECK(s.content.bottom - s.content.top == 90.0f);
        }
    }
    // A last child's block-end margin is in-flow content, and the padding is past it: 20 + 100 + 30 + 20.
    Page p;
    PrLayoutNode box = p.add(p.root, scroller(100, 50, 20));
    PrLayoutBoxStyle last = sized(-1, 100);
    last.margin[PrRectSide_Bottom] = prLayoutLengthPx(30);
    p.add(box, last);
    p.layout();
    Scroll s = scrollOf(p, box);
    CHECK(s.content.bottom - s.content.top == 170.0f);
}

TEST_CASE("a clipping child's content stays clipped, and its whole border box reachable") {
    // A 300 wide child with a 10 border, clipping its 500 wide content, directly in the scroller and in a wrapper.
    for (bool wrapped : {false, true}) {
        for (PrTextDirection direction : {PrTextDirection_Ltr, PrTextDirection_Rtl}) {
            CAPTURE(wrapped);
            CAPTURE(int(direction));
            Page p;
            PrLayoutNode box = p.add(p.root, scroller(100, 100), PrLayoutWritingMode_HorizontalTb, direction);
            PrLayoutNode parent = wrapped ? p.add(box, sized(100, -1), PrLayoutWritingMode_HorizontalTb, direction) : box;
            PrLayoutBoxStyle clipping = sized(300, 40);
            clipping.boxSizing = PrLayoutBoxSizing_BorderBox;
            clipping.overflowX = PrLayoutOverflow_Hidden;
            clipping.overflowY = PrLayoutOverflow_Hidden;
            for (uint32_t side = 0; side < PrRectSide_Count; ++side)
                clipping.border[side] = prLayoutUnitFromPx(side == PrRectSide_Left ? 10.0f : 4.0f);
            PrLayoutNode child = p.add(parent, clipping, PrLayoutWritingMode_HorizontalTb, direction);
            p.add(child, sized(500, 10), PrLayoutWritingMode_HorizontalTb, direction);
            p.layout();
            Scroll s = scrollOf(p, box);
            CHECK(s.content.right - s.content.left == 300.0f);
        }
    }
}

TEST_CASE("overflow is a rectangle, so a child of another writing mode passes up what it reaches as it is") {
    // An rtl scroller holding a vertical-lr block whose line overflows it leftward through an anonymous block.
    Page p;
    PrLayoutNode box = p.add(p.root, scroller(100, 100), PrLayoutWritingMode_HorizontalTb, PrTextDirection_Rtl);
    PrLayoutNode vertical = p.add(box, sized(-1, 50), kLr, PrTextDirection_Rtl);
    PrLayoutBoxStyle wide = prLayoutBoxStyleDefault();
    wide.display = PrLayoutDisplay_InlineBlock;
    wide.width = prLayoutLengthPx(300);
    wide.height = prLayoutLengthPx(10);
    wide.baselineShift = prLayoutBaselineShiftKeyword(PrLayoutKeyword_Top);
    p.add(vertical, wide, kLr, PrTextDirection_Rtl);
    p.add(vertical, block(), kLr, PrTextDirection_Rtl);
    p.layout();
    Scroll s = scrollOf(p, box);
    // The vertical box is as wide as its one line, 300, at the scroller's right edge, so it reaches 200 left of it.
    CHECK(s.range.left == -200.0f);
    CHECK(s.range.right == 0.0f);
}

TEST_CASE("a box that does not scroll lets its content overflow toward its inline end in rtl") {
    Page p;
    PrLayoutNode box = p.add(p.root, sized(100, 50), PrLayoutWritingMode_HorizontalTb, PrTextDirection_Rtl);
    PrLayoutNode child = p.add(box, sized(200, 10), PrLayoutWritingMode_HorizontalTb, PrTextDirection_Rtl);
    p.layout();
    CHECK(p.left(child, box) == -100);
}

TEST_CASE("Latin in a vertical line is set sideways on the line's central baseline, and lines stack from line-over") {
    for (PrLayoutWritingMode mode : {kRl, kLr, kSwRl}) {
        CAPTURE(int(mode));
        Page p;
        PrLayoutNode stage = p.stage(-1, 200, mode);
        PrLayoutNode first = p.add(stage, inlineBox(), mode);
        p.f.text(first, "aaa");
        p.f.text(stage, " bbb ccc ddd eee fff ggg hhh ");
        PrLayoutNode last = p.add(stage, inlineBox(), mode);
        p.f.text(last, "iii");
        p.layout();
        // Arimo's content area is 22 across the 40px line, which centers it, 9 from each edge of the line.
        CHECK(p.width(first) == 22);
        CHECK(p.top(first, stage) == 0);
        CHECK(p.width(stage) == 80);
        CHECK(p.left(first, stage) == (mode == kLr ? 9 : 49));
        CHECK(p.left(last, stage) == (mode == kLr ? 49 : 9));
    }
}

TEST_CASE("sideways glyphs turn clockwise from line-over, but in sideways-lr, which reads from the bottom") {
    for (PrLayoutWritingMode mode : {kRl, kLr, kSwRl, kSwLr}) {
        CAPTURE(int(mode));
        Page p;
        PrLayoutNode stage = p.stage(-1, 200, mode);
        PrLayoutNode span = p.add(stage, inlineBox(), mode);
        p.f.text(span, "abc");
        p.layout();
        CHECK(p.left(span, stage) == 9);
        CHECK(p.width(span) == 22);
        CHECK(p.height(span) == doctest::Approx(32.25).epsilon(0.001));
        // sideways-lr's line starts at the bottom.
        CHECK(p.top(span, stage) == doctest::Approx(mode == kSwLr ? 167.75 : 0).epsilon(0.001));
        std::vector<Painted> glyphs = painted(p.f.tree);
        REQUIRE(glyphs.size() == 3);
        for (const Painted& g : glyphs)
            CHECK(g.turned);
        // The alphabetic baseline is the ascent, 18, from line-over: the right side, or the left side for sideways-lr.
        CHECK(glyphs[0].x == doctest::Approx(mode == kSwLr ? 27 : 13));
        if (mode == kSwLr)
            CHECK(glyphs[1].y < glyphs[0].y);
        else
            CHECK(glyphs[1].y > glyphs[0].y);
    }
}

TEST_CASE("text-orientation upright sets Latin upright, advancing by the ascent and descent where a face has no vmtx") {
    Page p;
    PrLayoutFontStyle font = oriented(kRl, PrLayoutTextOrientation_Upright);
    PrLayoutNode stage = p.styled(p.root, sized(-1, 200), font);
    PrLayoutNode span = p.styled(stage, inlineBox(), font);
    p.f.text(span, "abc");
    p.layout();
    CHECK(p.left(span, stage) == 9);
    CHECK(p.width(span) == 22);
    CHECK(p.height(span) == 66);
    std::vector<Painted> glyphs = painted(p.f.tree);
    REQUIRE(glyphs.size() == 3);
    for (uint32_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK_FALSE(glyphs[i].turned);
        if (i != 0)
            CHECK(glyphs[i].y > glyphs[i - 1].y + 15);
    }
    // Each glyph's middle is on the central baseline, 20 from the stage's left.
    CHECK(glyphs[0].x < 20);
    CHECK(glyphs[0].x > 9);
}

TEST_CASE("Japanese is set upright and Latin among it sideways, each centered on the central baseline") {
    Page p;
    PrLayoutFontStyle font = Page::font(kRl);
    font.font = textFontOf(20, {p.f.family("NotoSansJP.ttf", "Noto Sans JP")});
    PrLayoutNode stage = p.styled(p.root, sized(-1, 200), font);
    PrLayoutNode kanji = p.styled(stage, inlineBox(), font);
    p.f.text(kanji, "日本");
    PrLayoutNode latin = p.styled(stage, inlineBox(), font);
    p.f.text(latin, "abc");
    p.f.text(stage, "語");
    p.layout();
    // Noto Sans JP's content area is 29 across the line, 23 and 6, with the half-leading above it floored to 5.
    CHECK(p.left(kanji, stage) == 6);
    CHECK(p.width(kanji) == 29);
    CHECK(p.height(kanji) == 40);
    CHECK(p.left(latin, stage) == 6);
    CHECK(p.top(latin, stage) == 40);
    CHECK(p.height(latin) == doctest::Approx(33.828).epsilon(0.001));
    std::vector<Painted> glyphs = painted(p.f.tree);
    REQUIRE(glyphs.size() == 6);
    CHECK_FALSE(glyphs[0].turned);
    CHECK_FALSE(glyphs[1].turned);
    CHECK(glyphs[1].y - glyphs[0].y == doctest::Approx(20));
    CHECK(glyphs[2].turned);
    CHECK_FALSE(glyphs[5].turned);
}

TEST_CASE("text in another face is centered on the line's central baseline by its own") {
    Page p;
    PrLayoutNode stage = p.stage(-1, 200, kRl);
    PrLayoutFontStyle jp = Page::font(kRl);
    jp.font = textFontOf(20, {p.f.family("NotoSansJP.ttf", "Noto Sans JP")});
    PrLayoutNode kanji = p.styled(stage, inlineBox(), jp);
    p.f.text(kanji, "日本");
    PrLayoutNode latin = p.add(stage, inlineBox(), kRl);
    p.f.text(latin, "abc");
    p.layout();
    // Noto Sans JP's BASE table puts its central baseline 7.6 above its alphabetic baseline, 0.9 below the middle of
    // its content area, so its 29 across sit 0.4 further left than centered. The line is 40.4 wide.
    CHECK(p.left(latin, stage) == 9);
    CHECK(p.width(latin) == 22);
    CHECK(p.left(kanji, stage) == doctest::Approx(6.40625));
    CHECK(p.width(kanji) == 29);
}

TEST_CASE("text-orientation sideways turns Japanese, set on its alphabetic baseline") {
    Page p;
    PrLayoutFontStyle font = oriented(kRl, PrLayoutTextOrientation_Sideways);
    font.font = textFontOf(20, {p.f.family("NotoSansJP.ttf", "Noto Sans JP")});
    PrLayoutNode stage = p.styled(p.root, sized(-1, 200), font);
    PrLayoutNode span = p.styled(stage, inlineBox(), font);
    p.f.text(span, "日本語");
    p.layout();
    CHECK(p.left(span, stage) == 6);
    CHECK(p.width(span) == 29);
    CHECK(p.height(span) == 60);
    for (const Painted& g : painted(p.f.tree))
        CHECK(g.turned);
}

TEST_CASE("atomic inlines in a vertical line align by their central baselines, the middle where they have none") {
    SUBCASE("an inline-block and a replaced box, by the middle of their margin boxes") {
        Page p;
        PrLayoutNode stage = p.stage(-1, 200, kRl);
        p.f.text(stage, "a");
        PrLayoutNode empty = p.add(stage, inlineBlock(10, 30), kRl);
        PrLayoutBoxStyle inlineImage = sized(10, 30);
        inlineImage.display = PrLayoutDisplay_Inline;
        PrLayoutNode image = replaced(p.f, stage, inlineImage, natural(10, 30));
        p.f.text(stage, "b");
        p.layout();
        CHECK(p.left(empty, stage) == 15);
        CHECK(p.top(empty, stage) == doctest::Approx(11.125).epsilon(0.001));
        CHECK(p.left(image, stage) == 15);
    }
    SUBCASE("an inline-block of 40px text, by its line's central baseline") {
        Page p;
        PrLayoutNode stage = p.stage(-1, 200, kRl);
        p.f.text(stage, "a");
        PrLayoutFontStyle large = Page::font(kRl);
        large.font = textFontOf(40);
        PrLayoutNode text = p.styled(stage, inlineBlock(-1, -1), large);
        p.f.text(text, "b");
        p.f.text(stage, "c");
        p.layout();
        CHECK(p.left(text, stage) == 0);
        CHECK(p.width(text) == 40);
    }
}

TEST_CASE("central baselines of two sizes align") {
    Page p;
    PrLayoutFontStyle line = Page::font(kRl);
    line.font = textFontOf(40);
    line.lineHeight = prLayoutLineHeightPx(60);
    PrLayoutNode stage = p.styled(p.root, sized(-1, 200), line);
    PrLayoutNode letter = p.styled(stage, inlineBox(), line);
    p.f.text(letter, "a");
    PrLayoutFontStyle small = Page::font(kRl);
    small.lineHeight = prLayoutLineHeightPx(20);
    PrLayoutNode box = p.styled(stage, inlineBlock(-1, -1), small);
    p.f.text(box, "b");
    p.layout();
    CHECK(p.left(letter, stage) == 8);
    CHECK(p.left(box, stage) == 20);
}

TEST_CASE("vertical-align top and lengths go toward line-over, the right but in sideways-lr") {
    for (PrLayoutWritingMode mode : {kRl, kLr, kSwLr}) {
        CAPTURE(int(mode));
        Page p;
        PrLayoutNode stage = p.stage(-1, 200, mode);
        p.f.text(stage, "a");
        PrLayoutNode tall = p.add(stage, inlineBlock(60, 10), mode);
        PrLayoutBoxStyle top = inlineBlock(10, 10);
        top.baselineShift = prLayoutBaselineShiftKeyword(PrLayoutKeyword_Top);
        PrLayoutNode raised = p.add(stage, top, mode);
        p.layout();
        CHECK(p.left(tall, stage) == 0);
        CHECK(p.left(raised, stage) == (mode == kSwLr ? 0 : 50));
    }
    Page p;
    PrLayoutNode stage = p.stage(-1, 200, kRl);
    PrLayoutNode plain = p.add(stage, inlineBox(), kRl);
    p.f.text(plain, "a");
    PrLayoutBoxStyle shifted = inlineBox();
    shifted.baselineShift = prLayoutBaselineShiftPx(10);
    PrLayoutNode moved = p.add(stage, shifted, kRl);
    p.f.text(moved, "b");
    p.layout();
    CHECK(p.left(plain, stage) == 9);
    CHECK(p.left(moved, stage) == 19);
}

TEST_CASE("an inline box's padding and border in a vertical line go around its content area") {
    Page p;
    PrLayoutNode stage = p.stage(-1, 200, kRl);
    p.f.text(stage, "a");
    PrLayoutBoxStyle box = inlineBox();
    for (PrRectSide side = 0; side < 4; ++side) {
        box.padding[side] = prLayoutLengthPx(10);
        box.border[side] = prLayoutUnitFromPx(3);
    }
    PrLayoutNode span = p.add(stage, box, kRl);
    p.f.text(span, "b");
    p.f.text(stage, "c");
    p.layout();
    CHECK(p.left(span, stage) == -4);
    CHECK(p.top(span, stage) == doctest::Approx(11.125).epsilon(0.001));
    CHECK(p.width(span) == 48);
    CHECK(p.height(span) == doctest::Approx(37.125).epsilon(0.001));
}

TEST_CASE("a span set on its alphabetic baseline attaches by the line's central baseline") {
    for (bool sideways : {true, false}) {
        CAPTURE(sideways);
        Page p;
        PrLayoutNode stage = p.stage(-1, 200, kRl);
        PrLayoutNode letter = p.add(stage, inlineBox(), kRl);
        p.f.text(letter, "a");
        PrLayoutFontStyle large = Page::font(kRl);
        large.font = textFontOf(40);
        if (sideways)
            large.textOrientation = PrLayoutTextOrientation_Sideways;
        else
            large.dominantBaseline = PrLayoutDominantBaseline_Alphabetic;
        PrLayoutNode span = p.styled(stage, inlineBox(), large);
        p.f.text(span, "b");
        p.layout();
        // Its 44px content area is centered on the line's central baseline, not set on its alphabetic one.
        CHECK(p.left(span, stage) == -2);
        CHECK(p.width(span) == 44);
        CHECK(p.left(letter, stage) == 9);
    }
}

TEST_CASE("a justified upright line spreads its glyphs down the line") {
    Page p;
    PrLayoutFontStyle font = oriented(kRl, PrLayoutTextOrientation_Upright);
    font.textAlign = PrLayoutTextAlign_Justify;
    font.textJustify = PrLayoutTextJustify_InterCharacter;
    PrLayoutNode stage = p.styled(p.root, sized(-1, 200), font);
    p.f.text(stage, "ab cd ef gh ij");
    p.layout();
    std::vector<Painted> glyphs = painted(p.f.tree);
    REQUIRE(glyphs.size() >= 4);
    // A first line of eight glyphs and a space fills 200, so each step is more than the 22 that a glyph advances.
    CHECK(glyphs[1].y - glyphs[0].y > 22);
    CHECK(glyphs[1].x == doctest::Approx(glyphs[0].x).epsilon(0.05));
}

TEST_CASE("orientation alone splits a run: a digit among Han takes its script and is set sideways") {
    for (PrLayoutTextOrientation orientation : {PrLayoutTextOrientation_Mixed, PrLayoutTextOrientation_Upright}) {
        CAPTURE(int(orientation));
        Page p;
        PrLayoutFontStyle font = oriented(kRl, orientation);
        font.font = textFontOf(20, {p.f.family("NotoSansJP.ttf", "Noto Sans JP")});
        PrLayoutNode stage = p.styled(p.root, sized(-1, 200), font);
        p.f.text(stage, "日1本");
        p.layout();
        std::vector<Painted> glyphs = painted(p.f.tree);
        REQUIRE(glyphs.size() == 3);
        CHECK_FALSE(glyphs[0].turned);
        CHECK(glyphs[1].turned == (orientation == PrLayoutTextOrientation_Mixed));
        CHECK_FALSE(glyphs[2].turned);
    }
}

TEST_CASE("a change of text-orientation shapes the text again") {
    Page p;
    PrLayoutFontStyle mixed = Page::font(kRl);
    PrLayoutNode stage = p.styled(p.root, sized(-1, 200), mixed);
    PrLayoutNode span = p.styled(stage, inlineBox(), mixed);
    p.f.text(span, "abc");
    p.layout();
    CHECK(p.height(span) == doctest::Approx(32.25).epsilon(0.001));
    PrLayoutFontStyle upright = oriented(kRl, PrLayoutTextOrientation_Upright);
    prLayoutNodeSetStyle(p.f.tree, stage, p.f.style(sized(-1, 200), nullptr, nullptr, &upright));
    prLayoutNodeSetStyle(p.f.tree, span, p.f.style(inlineBox(), nullptr, nullptr, &upright));
    p.layout();
    CHECK(p.height(span) == 66);
    CHECK(prLayoutTreeStats(p.f.tree).inlinesShaped >= 1);
}

TEST_CASE("a vertical-only script keeps its intrinsic orientation under upright, and the spaces between its words too") {
    // Mongolian is set sideways under both mixed and upright, and Latin is upright under upright.
    Page p;
    PrLayoutFontStyle font = oriented(kLr, PrLayoutTextOrientation_Upright);
    PrLayoutNode stage = p.styled(p.root, sized(-1, 200), font);
    p.f.text(stage, "\xe1\xa0\xae\xe1\xa0\xa3 \xe1\xa0\xa8" "a");
    p.layout();
    std::vector<Painted> glyphs = painted(p.f.tree);
    REQUIRE(glyphs.size() == 5);
    for (uint32_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(glyphs[i].turned);
    }
    CHECK_FALSE(glyphs[4].turned);
}

TEST_CASE("an orthogonal flow root's min block size resolves a cyclic percentage against zero in its contribution") {
    // In a float that shrinks to fit, a vertical box's min-width is its min block size, whose percentage base is the
    // float's width, which the contribution decides.
    for (bool raw : {true, false}) {
        CAPTURE(raw);
        Page p;
        PrLayoutBoxStyle outer = block();
        outer.floating = PrLayoutFloat_Left;
        PrLayoutNode wrapper = p.add(p.root, outer);
        PrLayoutBoxStyle inner = sized(40, 20);
        PrLayoutCalcNode nodes[] = {
            prLayoutCalcOperation(PrLayoutCalcOp_Sum, 2),
            prLayoutCalcLeaf(PrLayoutCalcLeaf_Px, 160),
            prLayoutCalcLeaf(PrLayoutCalcLeaf_Percent, 0),
        };
        inner.minWidth = raw ? prLayoutLengthPercent(200) : prLayoutLengthIntern(p.f.tree, nodes, 3);
        PrLayoutNode box = p.add(wrapper, inner, kRl);
        p.layout();
        // 200% is zero in the contribution, so the float is 40 wide and the box then takes 200% of that. The 0% of
        // calc(160px + 0%) is zero there too, so the float holds 160.
        CHECK(p.width(wrapper) == (raw ? 40 : 160));
        CHECK(p.width(box) == (raw ? 80 : 160));
    }
}
