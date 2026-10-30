// Bidirectional text: levels resolved by paragraph with unicode-bidi's controls, lines reordered and placed from
// line-left, and inline boxes split by reordering, with their edges placed by their own direction.

#include "layout/tests/layout_fixture.h"

namespace pr_impl {

namespace {

// A page holding stages of 20px Arimo on 40px lines, 200px wide unless a case says otherwise.
struct Page {
    Fixture f;
    PrLayoutNode root = kPrLayoutNodeNone;

    Page() { root = f.element(kPrLayoutNodeNone, block()); }

    // Returns a font of 20px on 40px lines in `direction`.
    static PrLayoutFontStyle font(PrTextDirection direction = PrTextDirection_Ltr) {
        PrLayoutFontStyle font = prLayoutFontStyleDefault();
        font.font = textFontOf(20);
        font.lineHeight = prLayoutLineHeightPx(40);
        font.direction = direction;
        return font;
    }

    // Creates an element under `parent` styled by `box` and `font`.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutFontStyle& style) {
        return f.element(parent, box, nullptr, nullptr, &style);
    }

    // Creates a stage `width` wide, styled by `style` and `bidi`.
    PrLayoutNode stage(const PrLayoutFontStyle& style, float width = 200, PrTextBidi bidi = PrTextBidi_Normal) {
        PrLayoutBoxStyle box = block();
        box.display = PrLayoutDisplay_FlowRoot;
        box.width = prLayoutLengthPx(width);
        box.unicodeBidi = bidi;
        return add(root, box, style);
    }

    // Creates a span under `parent` holding `text`, in `direction` with `bidi`.
    PrLayoutNode span(PrLayoutNode parent, const char* text, PrTextDirection direction = PrTextDirection_Ltr,
            PrTextBidi bidi = PrTextBidi_Normal) {
        PrLayoutBoxStyle box = inlineBox();
        box.unicodeBidi = bidi;
        PrLayoutNode node = add(parent, box, font(direction));
        if (text != nullptr)
            f.text(node, text);
        return node;
    }

    void layout() {
        f.layout(px(800, 600));
        CHECK(verified(f.tree));
    }

    // Returns the left of the node's first box, relative to `stage`'s left.
    double left(PrLayoutNode node, PrLayoutNode stage) {
        return double(f.bounds(node).left - f.bounds(stage).left);
    }

    // Returns the node's boxes, each relative to `stage`'s top left, in the order that geometry lists them.
    std::vector<PrRect> rects(PrLayoutNode node, PrLayoutNode stage) {
        PrLayoutRect out[16];
        uint32_t count = prLayoutNodeBoxes(f.tree, node, PrLayoutCoordinates_Viewport, out, 16);
        PrRect origin = f.bounds(stage);
        std::vector<PrRect> rects;
        for (uint32_t i = 0; i < count && i < 16; ++i) {
            PrRect r = prLayoutRectToPx(out[i]);
            rects.push_back(PrRect{
                r.left - origin.left, r.top - origin.top, r.right - origin.left,
                r.bottom - origin.top,
            });
        }
        return rects;
    }
};

constexpr PrTextDirection kLtr = PrTextDirection_Ltr;
constexpr PrTextDirection kRtl = PrTextDirection_Rtl;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("English in an rtl block reads left to right, at the right, its final ! at the left") {
    Page p;
    PrLayoutNode stage = p.stage(Page::font(kRtl));
    PrLayoutNode hello = p.span(stage, "Hello,");
    p.f.text(stage, " ");
    PrLayoutNode world = p.span(stage, "world");
    PrLayoutNode mark = p.span(stage, "!");
    p.layout();
    CHECK(p.left(hello, stage) == doctest::Approx(95.5).epsilon(0.001));
    CHECK(p.left(world, stage) == doctest::Approx(152.2).epsilon(0.001));
    CHECK(p.left(mark, stage) == doctest::Approx(89.94).epsilon(0.001));
}

TEST_CASE("a Hebrew word among English reads right to left, in either paragraph direction") {
    for (PrTextDirection direction : {kLtr, kRtl}) {
        CAPTURE(int(direction));
        Page p;
        PrLayoutNode stage = p.stage(Page::font(direction));
        PrLayoutNode one = p.span(stage, "one");
        p.f.text(stage, " ");
        PrLayoutNode shalom = p.span(stage, "שלום");
        p.f.text(stage, " ");
        PrLayoutNode two = p.span(stage, "two");
        p.layout();
        bool ltr = direction == PrTextDirection_Ltr;
        CHECK(p.left(one, stage) == doctest::Approx(ltr ? 0 : 166.63).epsilon(0.001));
        CHECK(p.left(shalom, stage) == doctest::Approx(ltr ? 38.94 : 117.1).epsilon(0.001));
        CHECK(p.left(two, stage) == doctest::Approx(ltr ? 88.47 : 80.41).epsilon(0.001));
    }
}

TEST_CASE("unicode-bidi on a span: normal changes nothing, embed embeds, bidi-override reverses every character") {
    const PrTextBidi values[] = {PrTextBidi_Normal, PrTextBidi_Embed, PrTextBidi_BidiOverride};
    for (uint32_t k = 0; k < 3; ++k) {
        PrTextBidi bidi = values[k];
        CAPTURE(int(bidi));
        Page p;
        PrLayoutNode stage = p.stage(Page::font());
        p.f.text(stage, "one ");
        PrLayoutNode outer = p.span(stage, nullptr, kRtl, bidi);
        PrLayoutNode two = p.span(outer, "two", kRtl);
        p.f.text(outer, " ");
        PrLayoutNode three = p.span(outer, "three", kRtl);
        PrLayoutNode mark = p.span(outer, "!", kRtl);
        p.f.text(stage, " four");
        p.layout();
        double expected[3][3] = {{38.94, 75.63, 121.22}, {44.5, 81.19, 38.94}, {95.66, 44.5, 38.94}};
        CHECK(p.left(two, stage) == doctest::Approx(expected[k][0]).epsilon(0.001));
        CHECK(p.left(three, stage) == doctest::Approx(expected[k][1]).epsilon(0.001));
        CHECK(p.left(mark, stage) == doctest::Approx(expected[k][2]).epsilon(0.001));
    }
}

TEST_CASE("a block's bidi-override overrides its paragraph") {
    Page p;
    PrLayoutNode stage = p.stage(Page::font(kRtl), 200, PrTextBidi_BidiOverride);
    PrLayoutNode abc = p.span(stage, "abc", kRtl);
    p.f.text(stage, " ");
    PrLayoutNode def = p.span(stage, "def", kRtl);
    p.f.text(stage, " ");
    PrLayoutNode ghi = p.span(stage, "ghi", kRtl);
    p.layout();
    CHECK(p.left(abc, stage) == doctest::Approx(167.75).epsilon(0.001));
    CHECK(p.left(def, stage) == doctest::Approx(134.38).epsilon(0.001));
    CHECK(p.left(ghi, stage) == doctest::Approx(102.11).epsilon(0.001));
}

TEST_CASE("an inline box split by reordering has a part for each piece, the edges on its first and last") {
    // The example of css-writing-modes-4: an em of Latin and Hebrew, which Hebrew outside it splits, bordered 2px on
    // its left and 6px on its right. Its start edge is on the left of its Latin, and its end edge on the right of its
    // Hebrew.
    auto build = [](Page& p, float width, PrTextBidi bidi, PrTextDirection direction) {
        PrLayoutNode stage = p.stage(Page::font(), width);
        p.f.text(stage, "here is ");
        PrLayoutBoxStyle box = inlineBox();
        box.border[PrRectSide_Left] = prLayoutUnitFromPx(2);
        box.border[PrRectSide_Right] = prLayoutUnitFromPx(6);
        box.unicodeBidi = bidi;
        PrLayoutNode em = p.add(stage, box, Page::font(direction));
        p.f.text(em, "some עברית");
        p.f.text(stage, " טקסט.");
        return std::pair{stage, em};
    };
    SUBCASE("on one line") {
        Page p;
        auto [stage, em] = build(p, 400, PrTextBidi_Normal, PrTextDirection_Ltr);
        p.layout();
        std::vector<PrRect> rects = p.rects(em, stage);
        REQUIRE(rects.size() == 2);
        CHECK(double(rects[0].left) == doctest::Approx(65.59).epsilon(0.001));
        CHECK(double(rects[0].right - rects[0].left) == doctest::Approx(56.47).epsilon(0.001));
        CHECK(double(rects[1].left) == doctest::Approx(178.66).epsilon(0.001));
        CHECK(double(rects[1].right - rects[1].left) == doctest::Approx(59.31).epsilon(0.001));
    }
    SUBCASE("wrapped, its pieces side by side, still two parts, as csswg-drafts#1509 resolved") {
        Page p;
        auto [stage, em] = build(p, 200, PrTextBidi_Normal, PrTextDirection_Ltr);
        p.layout();
        std::vector<PrRect> rects = p.rects(em, stage);
        REQUIRE(rects.size() == 2);
        CHECK(double(rects[0].left) == doctest::Approx(65.59).epsilon(0.001));
        CHECK(double(rects[1].left) == doctest::Approx(122.06).epsilon(0.001));
        CHECK(double(rects[0].right) == doctest::Approx(double(rects[1].left)).epsilon(0.001));
    }
    SUBCASE("an isolate, which nothing outside splits") {
        Page p;
        auto [stage, em] = build(p, 400, PrTextBidi_Isolate, kRtl);
        p.layout();
        std::vector<PrRect> rects = p.rects(em, stage);
        REQUIRE(rects.size() == 1);
        CHECK(double(rects[0].left) == doctest::Approx(65.59).epsilon(0.001));
        CHECK(double(rects[0].right - rects[0].left) == doctest::Approx(115.77).epsilon(0.001));
    }
}

TEST_CASE("a box's edges go by its own direction across lines, whatever its parent's") {
    // An rtl span of English in an ltr block, and an ltr span in an rtl block, padded 10px on the left and 20px on the
    // right. An rtl box's start is its right side, on its first line, and an ltr box's start is its left side.
    for (PrTextDirection outer : {kLtr, kRtl}) {
        CAPTURE(int(outer));
        Page p;
        PrLayoutNode stage = p.stage(Page::font(outer));
        PrLayoutBoxStyle box = inlineBox();
        box.padding[PrRectSide_Left] = prLayoutLengthPx(10);
        box.padding[PrRectSide_Right] = prLayoutLengthPx(20);
        PrLayoutNode span = p.add(stage, box, Page::font(outer == kRtl ? PrTextDirection_Ltr : kRtl));
        PrLayoutNode first = p.span(span, "the", outer == kRtl ? PrTextDirection_Ltr : kRtl);
        p.f.text(span, " element runs on across two lines to end");
        p.layout();
        std::vector<PrRect> rects = p.rects(span, stage);
        REQUIRE(rects.size() == 3);
        if (outer == PrTextDirection_Ltr) {
            CHECK(double(rects[0].right - rects[0].left) == doctest::Approx(196.8).epsilon(0.001));
            CHECK(p.left(first, stage) == 0);
        } else {
            CHECK(double(rects[0].left) == doctest::Approx(13.2).epsilon(0.002));
            CHECK(p.left(first, stage) == doctest::Approx(23.2).epsilon(0.001));
        }
    }
}

TEST_CASE("a forced break ends a paragraph, and the controls of the boxes open across it open again") {
    SUBCASE("a br, after which the ! of an ltr paragraph is at its right") {
        Page p;
        PrLayoutNode stage = p.stage(Page::font());
        p.f.text(stage, "abc ");
        PrLayoutNode hebrew = p.span(stage, "אבג");
        PrLayoutNode mark = p.span(stage, "!");
        PrLayoutBoxStyle br = inlineBox();
        prLayoutNodeCreateLineBreak(p.f.tree, stage, p.f.style(br));
        p.f.text(stage, "דהו");
        p.layout();
        CHECK(p.left(hebrew, stage) == doctest::Approx(37.81).epsilon(0.001));
        CHECK(p.left(mark, stage) == doctest::Approx(70.8).epsilon(0.001));
    }
    SUBCASE("an rtl override across a preserved newline overrides both paragraphs") {
        Page p;
        PrLayoutFontStyle pre = Page::font();
        pre.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
        PrLayoutNode stage = p.stage(pre);
        PrLayoutFontStyle rtl = pre;
        rtl.direction = kRtl;
        PrLayoutBoxStyle box = inlineBox();
        box.unicodeBidi = PrTextBidi_BidiOverride;
        PrLayoutNode span = p.add(stage, box, rtl);
        PrLayoutNode a = p.add(span, inlineBox(), rtl);
        p.f.text(a, "a");
        PrLayoutNode b = p.add(span, inlineBox(), rtl);
        p.f.text(b, "b\n");
        PrLayoutNode c = p.add(span, inlineBox(), rtl);
        p.f.text(c, "c");
        PrLayoutNode d = p.add(span, inlineBox(), rtl);
        p.f.text(d, "d");
        p.layout();
        CHECK(p.left(a, stage) == doctest::Approx(11.13).epsilon(0.001));
        CHECK(p.left(b, stage) == 0);
        CHECK(p.left(c, stage) == doctest::Approx(11.13).epsilon(0.001));
        CHECK(p.left(d, stage) == 0);
    }
}

TEST_CASE("plaintext takes each paragraph's direction from its text, and aligns it so") {
    Page p;
    PrLayoutNode stage = p.stage(Page::font(), 200, PrTextBidi_Plaintext);
    PrLayoutNode hebrew = p.span(stage, "אבג");
    PrLayoutBoxStyle br = inlineBox();
    prLayoutNodeCreateLineBreak(p.f.tree, stage, p.f.style(br));
    PrLayoutNode english = p.span(stage, "abc");
    p.layout();
    CHECK(p.left(hebrew, stage) == doctest::Approx(167.02).epsilon(0.001));
    CHECK(p.left(english, stage) == 0);
}

TEST_CASE("a tab in rtl text keeps the advance its stop gave it in the text's order, and moves with it") {
    Page p;
    PrLayoutFontStyle pre = Page::font(kRtl);
    pre.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
    PrLayoutNode stage = p.stage(pre, 400);
    PrLayoutNode first = p.add(stage, inlineBox(), pre);
    p.f.text(first, "אבג");
    p.f.text(stage, "\t");
    PrLayoutNode second = p.add(stage, inlineBox(), pre);
    p.f.text(second, "abc");
    p.f.text(stage, "\t");
    PrLayoutNode third = p.add(stage, inlineBox(), pre);
    p.f.text(third, "דהו");
    p.layout();
    // The Hebrew starts the line at the right, and each tab reaches its stop, measured from the line's start in text
    // order. Stops are eight spaces of 20px Arimo apart, at 44.45 and 88.91. The English ends at the first stop from
    // the right, and the second Hebrew word at the second.
    PrRect a = p.f.bounds(first);
    PrRect b = p.f.bounds(second);
    PrRect c = p.f.bounds(third);
    PrRect s = p.f.bounds(stage);
    CHECK(double(s.right - a.right) == doctest::Approx(0).epsilon(0.001));
    CHECK(double(s.right - b.right) == doctest::Approx(44.45).epsilon(0.001));
    CHECK(double(s.right - c.right) == doctest::Approx(88.91).epsilon(0.001));
}

TEST_CASE("an rtl line justifies over its opportunities in visual order, and hangs its trailing space at its left") {
    SUBCASE("justified") {
        Page p;
        PrLayoutFontStyle font = Page::font(kRtl);
        font.textAlign = PrLayoutTextAlign_Justify;
        PrLayoutNode stage = p.stage(font);
        PrLayoutNode first = p.span(stage, "first", kRtl);
        p.f.text(stage, " and then some more ");
        PrLayoutNode words = p.span(stage, "words", kRtl);
        p.f.text(stage, " to wrap here");
        p.layout();
        CHECK(p.left(first, stage) == 0);
        CHECK(p.left(words, stage) == doctest::Approx(59.25).epsilon(0.001));
    }
    SUBCASE("aligned left") {
        Page p;
        PrLayoutFontStyle font = Page::font(kRtl);
        font.textAlign = PrLayoutTextAlign_Left;
        PrLayoutNode stage = p.stage(font);
        PrLayoutNode first = p.span(stage, "first", kRtl);
        p.f.text(stage, " and then some more words to wrap");
        p.layout();
        CHECK(p.left(first, stage) == 0);
    }
}

TEST_CASE("text in left-to-right scripts alone resolves no levels, and a change of the container's unicode-bidi does") {
    Page p;
    PrLayoutNode stage = p.stage(Page::font());
    PrLayoutNode first = p.span(stage, "abc");
    p.f.text(stage, " def");
    p.layout();
    CHECK(prLayoutTreeStats(p.f.tree).inlinesShaped == 1);
    CHECK(prLayoutTreeStats(p.f.tree).inlinesLeveled == 0);
    CHECK(p.left(first, stage) == 0);
    // An rtl override of the block reverses the line, which is shaped again to resolve it.
    PrLayoutBoxStyle box = block();
    box.display = PrLayoutDisplay_FlowRoot;
    box.width = prLayoutLengthPx(200);
    box.unicodeBidi = PrTextBidi_BidiOverride;
    PrLayoutFontStyle rtl = Page::font(kRtl);
    prLayoutNodeSetStyle(p.f.tree, stage, p.f.style(box, nullptr, nullptr, &rtl));
    p.layout();
    CHECK(prLayoutTreeStats(p.f.tree).inlinesLeveled == 1);
    CHECK(p.left(first, stage) > 100);
}

TEST_CASE("an isolate's initiator and terminator keep its content from joining the text around it") {
    // In an ltr paragraph, ab isolated after Hebrew is at level 2 and the Hebrew at 1. The isolate's controls, at level
    // 0 between them, keep the two from reversing together, so ab stays at the right.
    Page p;
    PrLayoutNode stage = p.stage(Page::font());
    PrLayoutNode hebrew = p.span(stage, "לום");
    PrLayoutNode isolate = p.span(stage, "ab", PrTextDirection_Ltr, PrTextBidi_Isolate);
    p.layout();
    CHECK(p.left(hebrew, stage) == 0);
    CHECK(p.left(isolate, stage) > p.left(hebrew, stage));
}

TEST_CASE("a change of unicode-bidi alone, on the container or an inline box, resolves the levels again") {
    SUBCASE("the container") {
        Page p;
        PrLayoutNode stage = p.stage(Page::font(kRtl));
        PrLayoutNode abc = p.span(stage, "abc", kRtl);
        p.f.text(stage, " ");
        PrLayoutNode def = p.span(stage, "def", kRtl);
        p.layout();
        CHECK(p.left(abc, stage) < p.left(def, stage));
        PrLayoutBoxStyle box = block();
        box.display = PrLayoutDisplay_FlowRoot;
        box.width = prLayoutLengthPx(200);
        box.unicodeBidi = PrTextBidi_BidiOverride;
        PrLayoutFontStyle rtl = Page::font(kRtl);
        prLayoutNodeSetStyle(p.f.tree, stage, p.f.style(box, nullptr, nullptr, &rtl));
        p.layout();
        CHECK(prLayoutTreeStats(p.f.tree).inlinesShaped == 1);
        CHECK(p.left(abc, stage) > p.left(def, stage));
    }
    SUBCASE("an inline box") {
        Page p;
        PrLayoutNode stage = p.stage(Page::font());
        p.f.text(stage, "one ");
        PrLayoutNode outer = p.span(stage, nullptr, kRtl);
        PrLayoutNode two = p.span(outer, "two", kRtl);
        p.f.text(outer, " ");
        PrLayoutNode three = p.span(outer, "three", kRtl);
        p.layout();
        CHECK(p.left(two, stage) < p.left(three, stage));
        PrLayoutBoxStyle box = inlineBox();
        box.unicodeBidi = PrTextBidi_BidiOverride;
        PrLayoutFontStyle rtl = Page::font(kRtl);
        prLayoutNodeSetStyle(p.f.tree, outer, p.f.style(box, nullptr, nullptr, &rtl));
        p.layout();
        CHECK(prLayoutTreeStats(p.f.tree).inlinesShaped == 1);
        CHECK(p.left(two, stage) > p.left(three, stage));
    }
}
