// Positions in text: the caret before an offset of a text node's text, the boxes that a range of the text covers, and
// the position nearest a point.

#include "pr/draw_commands.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/mem.h"

#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// A block of 20px Arimo on 40px lines, `width` px wide, holding one text node, with its white space as given.
struct Block {
    Fixture f;
    PrLayoutNode root = kPrLayoutNodeNone;
    PrLayoutNode text = kPrLayoutNodeNone;

    Block(const char* value, float width = 400,
            PrLayoutWhiteSpaceCollapse collapse = PrLayoutWhiteSpaceCollapse_Preserve,
            PrTextDirection direction = PrTextDirection_Ltr,
            PrLayoutWritingMode mode = PrLayoutWritingMode_HorizontalTb) {
        PrLayoutFontStyle font = prLayoutFontStyleDefault();
        font.font = textFontOf(20);
        font.lineHeight = prLayoutLineHeightPx(40);
        font.whiteSpaceCollapse = collapse;
        font.direction = direction;
        font.writingMode = mode;
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPx(width);
        root = f.element(kPrLayoutNodeNone, box, nullptr, nullptr, &font);
        text = f.text(root, value);
        f.layout(px(800, 600));
        CHECK(verified(f.tree));
    }

    // Returns the caret before `offset`, in px.
    PrRect caret(uint32_t offset, PrLayoutTextAffinity affinity = PrLayoutTextAffinity_Downstream) {
        PrLayoutRect rect{};
        REQUIRE(prLayoutNodeTextCaret(f.tree, text, offset, affinity, PrLayoutCoordinates_Viewport, &rect));
        return prLayoutRectToPx(rect);
    }

    // Returns the boxes of the bytes from `start` to `end`, in px.
    std::vector<PrRect> range(uint32_t start, uint32_t end) {
        PrLayoutRect out[16];
        uint32_t count = prLayoutNodeTextRangeBoxes(f.tree, text, start, end, PrLayoutCoordinates_Viewport, out, 16);
        std::vector<PrRect> rects;
        for (uint32_t i = 0; i < count && i < 16; ++i)
            rects.push_back(prLayoutRectToPx(out[i]));
        return rects;
    }

    // Returns the offset nearest `point`, in px.
    uint32_t at(PrVec2 point) {
        PrLayoutTextPosition position{};
        REQUIRE(prLayoutTextPositionFromPoint(f.tree, root, prLayoutVecFromPx(point), &position));
        CHECK(position.node == text);
        return position.offset;
    }
};

// What a list paints, through the lists it embeds, ignoring transforms: its rectangles, the colors of its glyph runs,
// and the clips pushed around them.
struct Painted {
    struct Fill {
        PrRect rect;
        PrColor16 color;
    };
    std::vector<Fill> rects;
    std::vector<PrColor16> glyphColors;
    uint32_t clips = 0;

    void read(const PrDrawList* list) {
        const PrDrawCommand* commands = prDrawListCommands(list);
        for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
            const PrDrawCommand& command = commands[i];
            switch (command.kind) {
                case PrDrawCommandKind_Embed: read(command.embed.list); break;
                case PrDrawCommandKind_Rect: rects.push_back(Fill{command.rect, command.color}); break;
                case PrDrawCommandKind_Glyphs: glyphColors.push_back(command.color); break;
                case PrDrawCommandKind_PushClipRect: ++clips; break;
                default: break;
            }
        }
    }
};

Painted paintOf(PrLayoutTree* tree) {
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(tree, list);
    Painted painted;
    painted.read(list);
    prDrawListRelease(list);
    return painted;
}

bool sameColor(PrColor16 a, PrColor16 b) {
    return memEquals(&a, &b, sizeof(PrColor16));
}

const PrColor16 kYellow = prColor16FromColor(PrColor{1, 1, 0, 1});
const PrColor16 kRed = prColor16FromColor(PrColor{1, 0, 0, 1});
const PrColor16 kBlue = prColor16FromColor(PrColor{0, 0, 1, 1});

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a caret has no width, spans the text across its line, and moves right through ltr text") {
    Block b("abc");
    PrRect box = b.f.bounds(b.text);
    PrRect first = b.caret(0);
    CHECK(first.left == doctest::Approx(box.left));
    CHECK(first.right == first.left);
    CHECK(first.top == doctest::Approx(box.top));
    CHECK(first.bottom == doctest::Approx(box.bottom));
    CHECK(b.caret(1).left > first.left);
    CHECK(b.caret(2).left > b.caret(1).left);
    CHECK(b.caret(3).left == doctest::Approx(box.right).epsilon(0.01));
    // An offset past the text is the end of it.
    CHECK(b.caret(9).left == b.caret(3).left);
}

TEST_CASE("a caret on a line that holds only a preserved break stands at that line's start") {
    Block b("ab\n\ncd");
    PrRect first = b.caret(0);
    PrRect empty = b.caret(3);
    PrRect last = b.caret(4);
    CHECK(empty.left == doctest::Approx(first.left));
    CHECK(empty.top == doctest::Approx(first.top + 40));
    CHECK(last.top == doctest::Approx(first.top + 80));
    // The break that ends the first line puts a caret before it at the end of that line.
    CHECK(b.caret(2).top == doctest::Approx(first.top));
    CHECK(b.caret(2).left > first.left);
    // A break adds no box of its own.
    CHECK(b.range(0, 6).size() == 2);
}

TEST_CASE("at a soft wrap, a caret stands at the next line's start downstream, and the line's end upstream") {
    Block b("aaaa bbbb", 60, PrLayoutWhiteSpaceCollapse_Collapse);
    PrRect down = b.caret(5, PrLayoutTextAffinity_Downstream);
    PrRect up = b.caret(5, PrLayoutTextAffinity_Upstream);
    CHECK(down.top == doctest::Approx(b.caret(0).top + 40));
    CHECK(down.left == doctest::Approx(b.caret(0).left));
    CHECK(up.top == doctest::Approx(b.caret(0).top));
    CHECK(up.left > b.caret(4).left);
}

TEST_CASE("collapsed white space stands within the space it collapsed to") {
    Block b("a    b", 400, PrLayoutWhiteSpaceCollapse_Collapse);
    CHECK(b.caret(1).left < b.caret(3).left);
    CHECK(b.caret(3).left < b.caret(5).left);
    CHECK(b.caret(5).left < b.caret(6).left);
}

TEST_CASE("a range covers one box for each line it reaches, between the carets at its ends") {
    Block b("aaaa bbbb", 60, PrLayoutWhiteSpaceCollapse_Collapse);
    std::vector<PrRect> rects = b.range(2, 7);
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].left == doctest::Approx(b.caret(2).left));
    CHECK(rects[1].left == doctest::Approx(b.caret(5).left));
    CHECK(rects[1].right == doctest::Approx(b.caret(7).left));
    CHECK(rects[1].top == doctest::Approx(rects[0].top + 40));
    CHECK(b.range(3, 3).empty());
}

TEST_CASE("in rtl text, a caret moves left as the offset grows") {
    Block b("שלום", 400, PrLayoutWhiteSpaceCollapse_Preserve, PrTextDirection_Rtl);
    PrRect box = b.f.bounds(b.text);
    // Each Hebrew letter is two bytes.
    CHECK(b.caret(0).left == doctest::Approx(box.right).epsilon(0.01));
    CHECK(b.caret(2).left < b.caret(0).left);
    CHECK(b.caret(4).left < b.caret(2).left);
    CHECK(b.caret(8).left == doctest::Approx(box.left).epsilon(0.01));
    // The nearest position to a point is found the same way.
    CHECK(b.at(PrVec2{box.right - 1, box.top + 5}) == 0);
    CHECK(b.at(PrVec2{box.left + 1, box.top + 5}) == 8);
}

TEST_CASE("on a vertical line, a caret lies across the line and moves down") {
    Block b("abc", 400, PrLayoutWhiteSpaceCollapse_Preserve, PrTextDirection_Ltr, PrLayoutWritingMode_VerticalRl);
    PrRect first = b.caret(0);
    CHECK(first.top == first.bottom);
    CHECK(first.right > first.left);
    CHECK(b.caret(1).top > first.top);
    CHECK(b.caret(3).top > b.caret(2).top);
}

TEST_CASE("the position nearest a point: the nearer edge of the character under it, on the nearest line") {
    Block b("ab\n\ncd");
    PrRect a = b.caret(0);
    PrRect mid = b.caret(1);
    float y = (a.top + a.bottom) / 2;
    CHECK(b.at(PrVec2{a.left + 1, y}) == 0);
    CHECK(b.at(PrVec2{mid.left - 1, y}) == 1);
    CHECK(b.at(PrVec2{mid.left + 1, y}) == 1);
    // Past the end of a line, the end of it.
    CHECK(b.at(PrVec2{a.left + 300, y}) == 2);
    // On the line of the break alone, the break.
    CHECK(b.at(PrVec2{a.left + 300, y + 40}) == 3);
    // Below the last line, on that line.
    CHECK(b.at(PrVec2{a.left + 300, y + 400}) == 6);
}

TEST_CASE("a highlight fills its range's box behind the text, and paints the glyphs it covers in its color") {
    Block b("abcdef");
    PrLayoutHighlight highlight{.start = 2, .end = 4, .color = kRed, .background = kYellow};
    prLayoutNodeSetHighlights(b.f.tree, b.text, &highlight, 1);
    Painted painted = paintOf(b.f.tree);
    std::vector<PrRect> range = b.range(2, 4);
    REQUIRE(range.size() == 1);
    bool filled = false;
    for (const Painted::Fill& r : painted.rects)
        filled = filled || (sameColor(r.color, kYellow) && r.rect.left == doctest::Approx(range[0].left) &&
            r.rect.right == doctest::Approx(range[0].right));
    CHECK(filled);
    PrLayoutHighlight out[2];
    CHECK(prLayoutNodeHighlights(b.f.tree, b.text, out, 2) == 1);

    prLayoutNodeSetHighlights(b.f.tree, b.text, nullptr, 0);
    Painted plain = paintOf(b.f.tree);
    CHECK(plain.glyphColors.size() == 1);
    // The text before, inside and after the highlight, each clipped to its stretch.
    REQUIRE(painted.glyphColors.size() == 3);
    CHECK(sameColor(painted.glyphColors[1], kRed));
    CHECK(painted.clips == plain.clips + 3);
    CHECK(prLayoutNodeHighlights(b.f.tree, b.text, nullptr, 0) == 0);
}

TEST_CASE("the caret paints a bar 1 px wide at its position, over the text") {
    Block b("ab\n\ncd");
    PrLayoutCaret caret{.position = {b.text, 3}, .color = kBlue};
    prLayoutTreeSetCaret(b.f.tree, &caret);
    Painted painted = paintOf(b.f.tree);
    PrRect at = b.caret(3);
    uint32_t bars = 0;
    for (const Painted::Fill& r : painted.rects) {
        if (!sameColor(r.color, kBlue))
            continue;
        ++bars;
        CHECK(r.rect.left == doctest::Approx(at.left));
        CHECK(r.rect.right == doctest::Approx(at.left + 1));
        CHECK(r.rect.top == doctest::Approx(at.top));
    }
    CHECK(bars == 1);
    PrLayoutCaret kept{};
    CHECK(prLayoutTreeCaret(b.f.tree, &kept));
    CHECK(kept.position.offset == 3);

    // Moving the caret repaints the text it leaves and the text it comes to.
    caret.position.offset = 0;
    prLayoutTreeSetCaret(b.f.tree, &caret);
    bars = 0;
    for (const Painted::Fill& r : paintOf(b.f.tree).rects) {
        if (sameColor(r.color, kBlue)) {
            ++bars;
            CHECK(r.rect.left == doctest::Approx(b.caret(0).left));
        }
    }
    CHECK(bars == 1);
    prLayoutTreeSetCaret(b.f.tree, nullptr);
    CHECK(!prLayoutTreeCaret(b.f.tree, &kept));
    for (const Painted::Fill& r : paintOf(b.f.tree).rects)
        CHECK(!sameColor(r.color, kBlue));
}

TEST_CASE("a frame's highlights and caret last until a frame that does not set them") {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(20);
    PrLayoutStyleIndex style = f.style(block(), nullptr, nullptr, &font);
    PrLayoutHighlight highlight{.start = 0, .end = 2, .background = kYellow};
    PrLayoutNode text = kPrLayoutNodeNone;
    auto frame = [&](bool marks) {
        prLayoutTreeBeginFrame(f.tree);
        PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, style);
        text = prLayoutTreeFrameText(f.tree, root, 0, "abcd");
        if (marks) {
            prLayoutTreeFrameSetHighlights(f.tree, text, &highlight, 1);
            PrLayoutCaret caret{.position = {text, 1}, .color = kBlue};
            prLayoutTreeFrameSetCaret(f.tree, &caret);
        }
        PrLayoutRoot laid{.node = root, .extent = px(400, 400)};
        prLayoutTreeLayout(f.tree, &laid, 1);
    };
    frame(true);
    frame(true);
    PrLayoutCaret caret{};
    CHECK(prLayoutNodeHighlights(f.tree, text, nullptr, 0) == 1);
    CHECK(prLayoutTreeCaret(f.tree, &caret));
    frame(false);
    CHECK(prLayoutNodeHighlights(f.tree, text, nullptr, 0) == 0);
    CHECK(!prLayoutTreeCaret(f.tree, &caret));
}
