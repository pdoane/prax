// A line's content in its line box: text-align and text-align-last, justification, and text-indent. Each case is a
// stage of 20px Arimo on 40px lines, with boxes 30px wide standing for words. The numbers are the engine's at one
// target pixel per px.

#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns a font for text on 40px lines, aligned as given, which a stage sets in 20px text.
PrLayoutFontStyle aligned(PrLayoutTextAlign align, PrLayoutTextAlign last = PrLayoutTextAlign_Auto) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.lineHeight = prLayoutLineHeightPx(40);
    font.textAlign = align;
    font.textAlignLast = last;
    return font;
}

// Returns an inline-block `width` by 10px, standing for a word.
PrLayoutBoxStyle word(float width = 30) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_InlineBlock;
    box.width = prLayoutLengthPx(width);
    box.height = prLayoutLengthPx(10);
    return box;
}

// Returns a float on `side`, `width` by `height`.
PrLayoutBoxStyle floating(PrLayoutFloat side, float width, float height) {
    PrLayoutBoxStyle box = block();
    box.floating = side;
    box.width = prLayoutLengthPx(width);
    box.height = prLayoutLengthPx(height);
    return box;
}

// A block `width` wide in `font`, set in 20px text, under a root. The test adds its children.
struct Stage {
    Fixture f;
    PrLayoutFontStyle font;
    PrLayoutNode stage = kPrLayoutNodeNone;

    explicit Stage(const PrLayoutFontStyle& style, float width = 200) : font(style) {
        font.font = f.textFont(20);
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPx(width);
        stage = f.element(root, box, nullptr, nullptr, &font);
    }

    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutFontStyle* PR_NULLABLE withFont = nullptr) {
        return f.element(parent, box, nullptr, nullptr, withFont != nullptr ? withFont : &font);
    }
    PrLayoutNode add(const PrLayoutBoxStyle& box) {
        return add(stage, box);
    }
    PrLayoutNode text(const char* value, PrLayoutNode parent = kPrLayoutNodeNone) {
        return f.text(parent != kPrLayoutNodeNone ? parent : stage, value);
    }
    PrLayoutNode lineBreak(PrLayoutNode parent = kPrLayoutNodeNone) {
        return prLayoutNodeCreateLineBreak(f.tree, parent != kPrLayoutNodeNone ? parent : stage,
            f.style(prLayoutBoxStyleDefault()));
    }

    void layout() {
        f.layout(px(400, 600));
        CHECK(verified(f.tree));
    }

    // Return a box's left and top relative to the stage's, and its width.
    double left(PrLayoutNode node) {
        return double(f.bounds(node).left - f.bounds(stage).left);
    }
    double top(PrLayoutNode node) {
        return double(f.bounds(node).top - f.bounds(stage).top);
    }
    double width(PrLayoutNode node) {
        PrRect bounds = f.bounds(node);
        return double(bounds.right - bounds.left);
    }
};

// Returns a font for the stage's children, which inherit nothing: the stage's own font, indented by `indent`.
PrLayoutFontStyle indented(PrLayoutFontStyle font, PrLayoutLength indent, PrLayoutTextIndentFlags flags = 0) {
    font.textIndent = indent;
    font.textIndentFlags = flags;
    return font;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("text-align places each line's content in its line box, by its start or end, or its line-left or right") {
    {
        Stage s(aligned(PrLayoutTextAlign_Center));
        PrLayoutNode span = s.add(inlineBox());
        s.text("ab", span);
        s.text(" ");
        PrLayoutNode box = s.add(word());
        s.layout();
        CHECK(s.left(span) == doctest::Approx(71.09375));
        CHECK(s.left(box) == doctest::Approx(98.90625));
    }
    {
        // Centering halves the free space, rounded down to a unit.
        Stage s(aligned(PrLayoutTextAlign_Center));
        PrLayoutNode box = s.add(word(1.0f / 64));
        s.layout();
        CHECK(s.left(box) == doctest::Approx(99.984375));
    }
    {
        // A collapsible space before a forced break is removed, and takes no room.
        Stage s(aligned(PrLayoutTextAlign_Right));
        PrLayoutNode box = s.add(word());
        s.text(" ");
        s.lineBreak();
        s.text("x");
        s.layout();
        CHECK(s.left(box) == doctest::Approx(170.0));
    }
    // Start and end follow the container's direction; left and right do not.
    for (PrTextDirection direction : {PrTextDirection_Ltr, PrTextDirection_Rtl}) {
        CAPTURE(int(direction));
        bool rtl = direction == PrTextDirection_Rtl;
        for (PrLayoutTextAlign align : {PrLayoutTextAlign_Start, PrLayoutTextAlign_End, PrLayoutTextAlign_Left,
            PrLayoutTextAlign_Right}) {
            CAPTURE(int(align));
            PrLayoutFontStyle font = aligned(align);
            font.direction = direction;
            Stage s(font);
            PrLayoutNode box = s.add(word());
            s.layout();
            bool atRight = align == PrLayoutTextAlign_Right || (align == PrLayoutTextAlign_Start && rtl) ||
                (align == PrLayoutTextAlign_End && !rtl);
            CHECK(s.left(box) == doctest::Approx(atRight ? 170.0 : 0.0));
        }
    }
    {
        // Beside a float, the line is aligned in the space that the float leaves. Content wider than its line is at its
        // start.
        Stage s(aligned(PrLayoutTextAlign_Center));
        s.add(floating(PrLayoutFloat_Left, 60, 10));
        PrLayoutNode box = s.add(word());
        s.layout();
        CHECK(s.left(box) == doctest::Approx(115.0));
        Stage wide(aligned(PrLayoutTextAlign_Center));
        PrLayoutNode over = wide.add(word(300));
        wide.layout();
        CHECK(wide.left(over) == doctest::Approx(0.0));
    }
}

TEST_CASE("preserved white space a line ends in hangs at a soft wrap, and past the line's end otherwise") {
    PrLayoutFontStyle font = aligned(PrLayoutTextAlign_Right);
    font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
    {
        // At the end of the paragraph, spaces that fit count, and those past the edge hang.
        Stage s(font);
        PrLayoutNode box = s.add(word());
        s.text("   ");
        s.layout();
        CHECK(s.left(box) == doctest::Approx(153.328125));
        Stage over(font);
        PrLayoutNode wide = over.add(word(190));
        over.text("   ");
        over.layout();
        CHECK(over.left(wide) == doctest::Approx(0.0));
    }
    {
        // At a soft wrap they all hang.
        Stage s(font, 100);
        PrLayoutNode first = s.add(word(60));
        s.text("   ");
        PrLayoutNode second = s.add(word(60));
        s.layout();
        CHECK(s.left(first) == doctest::Approx(40.0));
        CHECK(s.left(second) == doctest::Approx(40.0));
    }
}

TEST_CASE("text-align-last aligns the last line and each line a forced break ends") {
    Stage s(aligned(PrLayoutTextAlign_Left, PrLayoutTextAlign_Center), 100);
    PrLayoutNode first = s.add(word());
    s.lineBreak();
    s.add(word());
    s.text(" ");
    s.add(word());
    s.text(" ");
    PrLayoutNode third = s.add(word());
    s.text(" ");
    PrLayoutNode fourth = s.add(word());
    s.layout();
    CHECK(s.left(first) == doctest::Approx(35.0));
    CHECK(s.left(third) == doctest::Approx(17.21875));
    CHECK(s.left(fourth) == doctest::Approx(52.78125));
}

TEST_CASE("justification spreads a line's free space over its word separators, filling it exactly") {
    {
        // Three 20px words of four in 105px.
        Stage s(aligned(PrLayoutTextAlign_Justify), 105);
        s.add(word(20));
        s.text(" ");
        PrLayoutNode second = s.add(word(20));
        s.text(" ");
        PrLayoutNode third = s.add(word(20));
        s.text(" ");
        s.add(word());
        s.layout();
        CHECK(s.left(second) == doctest::Approx(42.5));
        CHECK(s.left(third) == doctest::Approx(85.0));
    }
    {
        // 3.3125px over three spaces: each gets a share of whole units, with the first ones a unit more, and the line
        // is exactly full.
        Stage s(aligned(PrLayoutTextAlign_Justify), 100);
        s.add(word(20));
        PrLayoutNode words[3];
        for (PrLayoutNode& w : words) {
            s.text(" ");
            w = s.add(word(20));
        }
        s.text(" ");
        s.add(word(90));
        s.layout();
        CHECK(s.left(words[0]) == doctest::Approx(26.671875));
        CHECK(s.left(words[1]) == doctest::Approx(53.34375));
        CHECK(s.left(words[2]) == doctest::Approx(80.0));
    }
    {
        // Text, with a no-break space also counting as a word separator.
        for (const char* gap : {" ", " "}) {
            CAPTURE(gap);
            Stage s(aligned(PrLayoutTextAlign_Justify), 150);
            PrLayoutNode spans[4];
            const char* words[] = {"one", "two", "three", "four"};
            for (uint32_t i = 0; i < 4; ++i) {
                if (i != 0)
                    s.text(i == 1 ? gap : " ");
                spans[i] = s.add(inlineBox());
                s.text(words[i], spans[i]);
            }
            s.text(" five six seven");
            s.layout();
            CHECK(s.left(spans[1]) == doctest::Approx(53.34375));
            CHECK(s.left(spans[2]) == doctest::Approx(104.40625));
            CHECK(s.top(spans[3]) == doctest::Approx(40.0 + 9.0));
        }
    }
    {
        // An inline box around a separator widens with its share.
        Stage s(aligned(PrLayoutTextAlign_Justify), 120);
        s.add(word());
        PrLayoutBoxStyle padded = inlineBox();
        padded.padding[PrRectSide_Left] = prLayoutLengthPx(5);
        padded.padding[PrRectSide_Right] = prLayoutLengthPx(5);
        PrLayoutNode span = s.add(padded);
        s.text(" ", span);
        PrLayoutNode after = s.add(word());
        s.text(" ");
        s.add(word(100));
        s.layout();
        CHECK(s.left(span) == doctest::Approx(30.0));
        CHECK(s.width(span) == doctest::Approx(60.0));
        CHECK(s.left(after) == doctest::Approx(90.0));
    }
}

TEST_CASE("a justified line that cannot stretch is aligned as text-align-last says, and centered for justify") {
    // As css-text-3/#justify-limits describes.
    struct Case {
        PrLayoutTextAlign last;
        double left;
    };
    for (Case c : {Case{PrLayoutTextAlign_Center, 5.0}, Case{PrLayoutTextAlign_End, 10.0}}) {
        CAPTURE(int(c.last));
        Stage s(aligned(PrLayoutTextAlign_Justify, c.last), 100);
        PrLayoutNode only = s.add(word(90));
        s.text(" ");
        s.add(word());
        s.layout();
        CHECK(s.left(only) == doctest::Approx(c.left));
    }
    Stage s(aligned(PrLayoutTextAlign_Justify, PrLayoutTextAlign_Justify), 100);
    PrLayoutNode last = s.add(word());
    s.layout();
    CHECK(s.left(last) == doctest::Approx(35.0));
    // A line ended by a forced break is not stretched, but start-aligned, and so is a line with no room.
    Stage broken(aligned(PrLayoutTextAlign_Justify));
    broken.add(word());
    broken.text(" ");
    PrLayoutNode second = broken.add(word());
    broken.lineBreak();
    broken.text("x");
    broken.layout();
    CHECK(broken.left(second) == doctest::Approx(35.5625));
}

TEST_CASE("text-justify decides which opportunities a justified line has, by the text's own or its common box's") {
    {
        // None has no opportunities, and a span with none keeps its own separator unstretched.
        PrLayoutFontStyle none = aligned(PrLayoutTextAlign_Justify);
        none.textJustify = PrLayoutTextJustify_None;
        Stage s(none, 100);
        s.add(word());
        s.text(" ");
        PrLayoutNode second = s.add(word());
        s.text(" ");
        s.add(word());
        s.layout();
        CHECK(s.left(second) == doctest::Approx(35.5625));
        Stage mixed(aligned(PrLayoutTextAlign_Justify), 105);
        none.font = mixed.font.font;
        mixed.add(word(20));
        mixed.text(" ");
        PrLayoutNode span = mixed.add(mixed.stage, inlineBox(), &none);
        PrLayoutNode inside = mixed.add(span, word(20));
        mixed.text(" ", span);
        PrLayoutNode third = mixed.add(word(20));
        mixed.text(" ");
        mixed.add(word());
        mixed.layout();
        CHECK(mixed.left(inside) == doctest::Approx(59.4375));
        CHECK(mixed.left(third) == doctest::Approx(85.0));
    }
    {
        // Inter-character stretches the gaps between characters, across inline box edges too.
        PrLayoutFontStyle chars = aligned(PrLayoutTextAlign_Justify, PrLayoutTextAlign_Justify);
        chars.textJustify = PrLayoutTextJustify_InterCharacter;
        Stage plain(chars, 100);
        plain.text("ab");
        PrLayoutNode c = plain.add(inlineBox());
        plain.text("c", c);
        plain.text("d");
        plain.layout();
        Stage split(chars, 100);
        PrLayoutNode letters[4];
        const char* text[] = {"a", "b", "c", "d"};
        for (uint32_t i = 0; i < 4; ++i) {
            letters[i] = split.add(inlineBox());
            split.text(text[i], letters[i]);
        }
        split.layout();
        CHECK(plain.left(c) == doctest::Approx(60.0));
        CHECK(split.left(letters[2]) == doctest::Approx(60.0));
        // A gap widens only the boxes around both of its characters. It does not widen a span that ends or starts at
        // it, but does widen one that holds both, as `bc` does in `a`, `bc` and `d`.
        Stage natural(aligned(PrLayoutTextAlign_Start), 100);
        PrLayoutNode alone = natural.add(inlineBox());
        natural.text("b", alone);
        natural.layout();
        Stage nested(chars, 100);
        nested.text("a");
        PrLayoutNode both = nested.add(inlineBox());
        PrLayoutNode b = nested.add(both, inlineBox());
        nested.text("b", b);
        PrLayoutNode inner = nested.add(both, inlineBox());
        nested.text("c", inner);
        nested.text("d");
        nested.layout();
        CHECK(split.width(letters[1]) == doctest::Approx(natural.width(alone)));
        CHECK(nested.width(b) == doctest::Approx(natural.width(alone)));
        CHECK(nested.left(both) == doctest::Approx(nested.left(b)));
        CHECK(nested.left(both) + nested.width(both) == doctest::Approx(nested.left(inner) + nested.width(inner)));
        CHECK(nested.width(both) > nested.width(b) + nested.width(inner));
        // An empty span between two characters holds neither, so the gap between them keeps its unjustified width,
        // whether the span is undecorated, padded or nested.
        PrLayoutBoxStyle padded = inlineBox();
        padded.padding[PrRectSide_Left] = prLayoutLengthPx(2);
        padded.padding[PrRectSide_Right] = prLayoutLengthPx(2);
        double widths[2][3] = {};
        for (bool justified : {false, true}) {
            Stage s(justified ? chars : aligned(PrLayoutTextAlign_Start), 100);
            s.text("a");
            PrLayoutNode bare = s.add(inlineBox());
            s.text("b");
            PrLayoutNode pad = s.add(padded);
            s.text("c");
            PrLayoutNode outer = s.add(padded);
            s.add(outer, padded);
            s.text("d");
            s.layout();
            widths[justified][0] = s.width(bare);
            widths[justified][1] = s.width(pad);
            widths[justified][2] = s.width(outer);
        }
        for (uint32_t i = 0; i < 3; ++i) {
            CAPTURE(i);
            CHECK(widths[1][i] == doctest::Approx(widths[0][i]));
        }
        // A unit that two text nodes split, before a combining mark or an emoji modifier, has no gap inside it. The
        // second node sits where it does unjustified, and all of the line's free space goes before `x`.
        struct Split {
            const char* first;
            const char* rest;
        };
        for (Split cut : {Split{"e", "\u0301"}, Split{"\U0001F44D", "\U0001F3FD"}}) {
            CAPTURE(cut.rest);
            double lefts[2] = {};
            for (bool justified : {false, true}) {
                Stage s(justified ? chars : aligned(PrLayoutTextAlign_Start), 100);
                s.text(cut.first);
                PrLayoutNode rest = s.text(cut.rest);
                s.text("x");
                s.layout();
                lefts[justified] = s.left(rest);
            }
            CHECK(lefts[1] == doctest::Approx(lefts[0]));
        }
    }
}

TEST_CASE("only the content past a line's last preserved tab stretches, so that the tab keeps its stop") {
    // What precedes the tab keeps its position, and the tab keeps its stop.
    PrLayoutFontStyle font = aligned(PrLayoutTextAlign_Justify);
    font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
    Stage s(font, 200);
    s.text("x y\t");
    PrLayoutNode after = s.add(inlineBox());
    s.text("one", after);
    s.text(" two three four five six seven");
    s.layout();
    // The stop is eight spaces of the font from the block's start.
    double stop = 8 * 5.556640625;
    CHECK(s.left(after) == doctest::Approx(stop).epsilon(0.002));
}

TEST_CASE("text-indent indents the first formatted line, and with each-line or hanging others") {
    PrLayoutFontStyle base = aligned(PrLayoutTextAlign_Start);
    {
        // The first line is indented, not the next. A negative indent outdents.
        Stage s(indented(base, prLayoutLengthPx(30)), 100);
        PrLayoutNode first = s.add(word());
        s.text(" ");
        s.add(word());
        s.text(" ");
        PrLayoutNode second = s.add(word());
        s.layout();
        CHECK(s.left(first) == doctest::Approx(30.0));
        CHECK(s.left(second) == doctest::Approx(0.0));
        Stage out(indented(base, prLayoutLengthPx(-10)));
        PrLayoutNode box = out.add(word());
        out.layout();
        CHECK(out.left(box) == doctest::Approx(-10.0));
    }
    {
        // A percentage of the container's own content width.
        Stage s(base);
        PrLayoutBoxStyle narrow = block();
        narrow.width = prLayoutLengthPx(100);
        PrLayoutFontStyle percent = indented(base, prLayoutLengthPercent(20));
        PrLayoutNode inner = s.add(s.stage, narrow, &percent);
        PrLayoutNode box = s.add(inner, word());
        s.layout();
        CHECK(s.left(box) == doctest::Approx(20.0));
    }
    {
        // With each-line, each line after a forced break is indented. With hanging, every line except the first is.
        Stage each(indented(base, prLayoutLengthPx(30), PrLayoutTextIndentFlag_EachLine), 100);
        each.add(word());
        each.lineBreak();
        PrLayoutNode afterBreak = each.add(word());
        each.text(" ");
        each.add(word());
        each.text(" ");
        PrLayoutNode afterWrap = each.add(word());
        each.layout();
        CHECK(each.left(afterBreak) == doctest::Approx(30.0));
        CHECK(each.left(afterWrap) == doctest::Approx(0.0));
        Stage hanging(indented(base, prLayoutLengthPx(30), PrLayoutTextIndentFlag_Hanging), 100);
        PrLayoutNode first = hanging.add(word());
        hanging.text(" ");
        hanging.add(word());
        hanging.text(" ");
        PrLayoutNode second = hanging.add(word());
        hanging.layout();
        CHECK(hanging.left(first) == doctest::Approx(0.0));
        CHECK(hanging.left(second) == doctest::Approx(30.0));
    }
    {
        // The anonymous block after a block child is not indented, empty or not, but a line beside a float that comes
        // first is.
        PrLayoutFontStyle font = indented(base, prLayoutLengthPx(30));
        Stage s(font);
        PrLayoutNode before = s.add(word());
        s.add(block());
        PrLayoutNode after = s.add(word());
        s.layout();
        CHECK(s.left(before) == doctest::Approx(30.0));
        CHECK(s.left(after) == doctest::Approx(0.0));
        Stage floats(font);
        floats.add(floating(PrLayoutFloat_Left, 60, 10));
        PrLayoutNode beside = floats.add(word());
        floats.layout();
        CHECK(floats.left(beside) == doctest::Approx(90.0));
    }
    {
        // The line breaks and aligns in the space that the indent leaves, and in rtl from the right.
        Stage wrap(indented(base, prLayoutLengthPx(80)), 100);
        PrLayoutNode one = wrap.add(word());
        wrap.text(" ");
        PrLayoutNode two = wrap.add(word());
        wrap.text(" ");
        PrLayoutNode three = wrap.add(word());
        wrap.layout();
        CHECK(wrap.left(one) == doctest::Approx(80.0));
        CHECK(wrap.top(two) == doctest::Approx(57.0));
        CHECK(wrap.left(three) == doctest::Approx(35.5625));
        Stage center(indented(aligned(PrLayoutTextAlign_Center), prLayoutLengthPx(40)));
        PrLayoutNode centered = center.add(word());
        center.layout();
        CHECK(center.left(centered) == doctest::Approx(105.0));
        Stage negative(indented(aligned(PrLayoutTextAlign_Center), prLayoutLengthPx(-20)));
        PrLayoutNode out = negative.add(word());
        negative.layout();
        CHECK(negative.left(out) == doctest::Approx(75.0));
        PrLayoutFontStyle rtl = indented(base, prLayoutLengthPx(30));
        rtl.direction = PrTextDirection_Rtl;
        Stage right(rtl);
        PrLayoutNode box = right.add(word());
        right.layout();
        CHECK(right.left(box) == doctest::Approx(140.0));
    }
}

TEST_CASE("text-indent widens the first piece of each indented line in intrinsic sizes, a percentage as zero") {
    struct Case {
        PrLayoutLength indent;
        PrLayoutTextIndentFlags flags;
        bool minContent;
        bool blockFirst;
        double width;
    };
    for (Case c : {Case{prLayoutLengthPx(30), 0, false, false, 60}, Case{prLayoutLengthPx(-10), 0, false, false, 20},
        Case{prLayoutLengthPercent(50), 0, false, false, 30}, Case{prLayoutLengthPx(30), 0, true, false, 60},
                Case{prLayoutLengthPx(30), PrLayoutTextIndentFlag_Hanging, false, false, 50},
                Case{prLayoutLengthPx(30), PrLayoutTextIndentFlag_EachLine, false, false, 60},
                Case{prLayoutLengthPx(30), 0, false, true, 30}}) {
        CAPTURE(c.width);
        Stage s(aligned(PrLayoutTextAlign_Start));
        PrLayoutFontStyle font = indented(aligned(PrLayoutTextAlign_Start), c.indent, c.flags);
        font.font = s.font.font;
        PrLayoutBoxStyle shrink = word();
        shrink.width = c.minContent ? prLayoutLengthMinContent() : prLayoutLengthAuto();
        shrink.height = prLayoutLengthAuto();
        PrLayoutNode box = s.add(s.stage, shrink, &font);
        if (c.blockFirst)
            s.add(box, block(), &font);
        bool lines = c.flags != 0;
        s.add(box, word(lines && c.flags == PrLayoutTextIndentFlag_EachLine ? 10 : 30), &font);
        if (c.minContent) {
            s.text(" ", box);
            s.add(box, word(50), &font);
        }
        if (lines) {
            s.lineBreak(box);
            s.add(box, word(c.flags == PrLayoutTextIndentFlag_Hanging ? 20 : 30), &font);
        }
        s.layout();
        CHECK(s.width(box) == doctest::Approx(c.width));
    }
}

TEST_CASE("a change of alignment moves a line's content and keeps its shaping") {
    Stage s(aligned(PrLayoutTextAlign_Start));
    PrLayoutNode span = s.add(inlineBox());
    s.text("ab", span);
    s.layout();
    CHECK(s.left(span) == doctest::Approx(0.0));
    PrLayoutFontStyle centered = aligned(PrLayoutTextAlign_Center);
    centered.font = s.font.font;
    PrLayoutStyle record = prLayoutStyleRecord(s.f.tree, prLayoutNodeStyle(s.f.tree, s.stage));
    record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(s.f.tree, &centered);
    prLayoutNodeSetStyle(s.f.tree, s.stage, prLayoutStyleIntern(s.f.tree, &record));
    s.layout();
    CHECK(s.left(span) > 80.0);
    // The font group holds the alignment, so its context is shaped again; only its container is laid out.
    CHECK(prLayoutTreeStats(s.f.tree).inlinesShaped == 1);
}
