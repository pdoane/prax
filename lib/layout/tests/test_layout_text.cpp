// Text on lines: what each white-space-collapse keeps, how text is transformed, where text may wrap, tab stops, the
// language text is shaped in, and the advances of hinted fonts.

#include "pr/font_freetype.h"
#include "pr/font_set.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns a font for text on 20px lines, with white space and wrapping as given. A record inherits nothing, so each
// inline box takes one.
PrLayoutFontStyle textFont(PrLayoutWhiteSpaceCollapse collapse, PrLayoutTextWrapMode wrap = PrLayoutTextWrapMode_Wrap) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.lineHeight = prLayoutLineHeightPx(20);
    font.whiteSpaceCollapse = collapse;
    font.textWrapMode = wrap;
    return font;
}

// Creates and lays out a block of `width` px, styled by `font`, holding `text`, and returns the block.
PrLayoutNode textBlock(Fixture& f, const PrLayoutFontStyle& font, const char* text, float width) {
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    f.text(root, text);
    f.layout(px(width, 600));
    return root;
}

// Returns the height of a block of `width` px styled by `font` holding `text`.
float heightOf(const PrLayoutFontStyle& font, const char* text, float width = 400) {
    Fixture f;
    PrLayoutNode root = textBlock(f, font, text, width);
    CHECK(verified(f.tree));
    return f.bounds(root).bottom;
}

// Returns the width of `text` alone on a line, with its white space preserved.
float widthOf(const char* text) {
    Fixture f;
    PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_Preserve, PrLayoutTextWrapMode_Nowrap);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutNode span = f.element(root, inlineBox(), nullptr, nullptr, &font);
    f.text(span, text);
    f.layout(px(400, 600));
    PrRect bounds = f.bounds(span);
    return bounds.right - bounds.left;
}

PrLayoutBoxStyle withWidth(PrLayoutBoxStyle box, PrLayoutLength width) {
    box.width = width;
    return box;
}

// Returns a font for text on 20px lines that breaks within words as `wordBreak` and `overflowWrap` say.
PrLayoutFontStyle breakingFont(PrTextWordBreak wordBreak, PrTextOverflowWrap overflowWrap) {
    PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    font.wordBreak = wordBreak;
    font.overflowWrap = overflowWrap;
    return font;
}

// The height of a block `width` wide that holds `text` in `font`, and the width of a float in a container `width` wide
// that holds the same. The float's width is its min-content size if that is wider than the container.
struct Broken {
    float height;
    float floatWidth;
};

Broken brokenOf(const PrLayoutFontStyle& font, const char* text, float width) {
    Broken out{};
    {
        Fixture f;
        PrLayoutNode root = textBlock(f, font, text, width);
        CHECK(verified(f.tree));
        out.height = f.bounds(root).bottom;
    }
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutBoxStyle floating = block();
    floating.floating = PrLayoutFloat_Left;
    PrLayoutNode box = f.element(root, floating, nullptr, nullptr, &font);
    f.text(box, text);
    f.layout(px(width, 600));
    CHECK(verified(f.tree));
    out.floatWidth = f.bounds(box).right - f.bounds(box).left;
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a preserved segment break forces a line break, and one that collapses is a space") {
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_Preserve), "a\nb\n\nc") == 80.0f);
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_PreserveBreaks), "a\nb") == 40.0f);
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_BreakSpaces), "a\nb") == 40.0f);
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_Collapse), "a\nb") == 20.0f);
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_PreserveSpaces), "a\nb") == 20.0f);
    // Discarded white space takes no line, and no room.
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_Discard), "a\nb") == 20.0f);
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_Discard), "aaaa bbbb", 1) == 20.0f);
}

TEST_CASE("preserved spaces take room, and collapsible ones around a preserved segment break are removed") {
    CHECK(widthOf("a   b") > widthOf("a b"));
    Fixture f;
    PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_PreserveBreaks);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutNode first = f.element(root, inlineBox(), nullptr, nullptr, &font);
    f.text(first, "a   ");
    f.text(root, "\n   ");
    PrLayoutNode second = f.element(root, inlineBox(), nullptr, nullptr, &font);
    f.text(second, "b");
    f.layout(px(400, 600));
    CHECK(verified(f.tree));
    PrRect a = f.bounds(first);
    PrRect b = f.bounds(second);
    CHECK(double(a.right - a.left) == doctest::Approx(double(widthOf("a"))));
    CHECK(double(b.left) == doctest::Approx(0.0));
    CHECK(b.top > 20.0f);
}

TEST_CASE("text that does not wrap stays on its line past the container's edge, but for a forced break") {
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_Collapse), "aaa bbb ccc", 30) == 60.0f);
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_Collapse, PrLayoutTextWrapMode_Nowrap), "aaa bbb ccc", 30) == 20.0f);
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_Preserve, PrLayoutTextWrapMode_Nowrap), "aaa bbb\nccc", 30) == 40.0f);
}

TEST_CASE("whether text wraps between two boxes is the nearest box's that holds both") {
    Fixture f;
    PrLayoutFontStyle wraps = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    PrLayoutFontStyle nowrap = textFont(PrLayoutWhiteSpaceCollapse_Collapse, PrLayoutTextWrapMode_Nowrap);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &wraps);
    PrLayoutNode kept = f.element(root, inlineBox(), nullptr, nullptr, &nowrap);
    f.text(kept, "aaa bbb");
    PrLayoutNode after = f.element(root, inlineBox(), nullptr, nullptr, &wraps);
    f.text(after, " ccc");
    f.layout(px(widthOf("aaa bbb") + 2, 600));
    CHECK(verified(f.tree));
    // The span's own space does not wrap, and the one after it, in the block's text, does.
    CHECK(f.bounds(kept).bottom < 20.0f);
    CHECK(f.bounds(after).bottom > 20.0f);
    CHECK(double(f.bounds(root).bottom) == doctest::Approx(40.0));

    // If the block does not wrap, the boundary between the span and the text after it does not either.
    Fixture g;
    PrLayoutNode outer = g.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &nowrap);
    PrLayoutNode inner = g.element(outer, inlineBox(), nullptr, nullptr, &wraps);
    g.text(inner, "aaa");
    g.text(outer, "bbb");
    g.layout(px(10, 600));
    CHECK(double(g.bounds(outer).bottom) == doctest::Approx(20.0));
}

TEST_CASE("preserved spaces at a line's end hang where the line may wrap, and break-spaces wraps between them") {
    float width = widthOf("aaa") + 1;
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_Preserve), "aaa      bbb", width) == 40.0f);
    CHECK(heightOf(textFont(PrLayoutWhiteSpaceCollapse_BreakSpaces), "aaa      bbb", width) > 40.0f);
}

TEST_CASE("a preserved tab advances to the next tab stop, eight spaces apart from the block's start") {
    float stop = widthOf("        ");
    for (const char* text : {"\t", "a\t"}) {
        Fixture f;
        PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_Preserve);
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
        f.text(root, text);
        PrLayoutNode after = f.element(root, inlineBox(), nullptr, nullptr, &font);
        f.text(after, "b");
        f.layout(px(400, 600));
        CHECK(verified(f.tree));
        CHECK(double(f.bounds(after).left) == doctest::Approx(double(stop)).epsilon(0.02));
    }
    // A tab that collapses is a space.
    Fixture f;
    PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    f.text(root, "a\t");
    PrLayoutNode after = f.element(root, inlineBox(), nullptr, nullptr, &font);
    f.text(after, "b");
    f.layout(px(400, 600));
    CHECK(double(f.bounds(after).left) == doctest::Approx(double(widthOf("a "))).epsilon(0.02));
}

TEST_CASE("a forced break ends a line of the max-content size, and text that does not wrap is its min-content") {
    Fixture f;
    PrLayoutFontStyle pre = textFont(PrLayoutWhiteSpaceCollapse_Preserve);
    PrLayoutFontStyle nowrap = textFont(PrLayoutWhiteSpaceCollapse_Collapse, PrLayoutTextWrapMode_Nowrap);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode lines = f.element(root, withWidth(block(), prLayoutLengthMaxContent()), nullptr, nullptr, &pre);
    f.text(lines, "aa\nbbbb");
    PrLayoutNode kept = f.element(root, withWidth(block(), prLayoutLengthMinContent()), nullptr, nullptr, &nowrap);
    f.text(kept, "aa bb");
    f.layout(px(400, 600));
    CHECK(verified(f.tree));
    PrRect l = f.bounds(lines);
    PrRect k = f.bounds(kept);
    CHECK(double(l.right - l.left) == doctest::Approx(double(widthOf("bbbb"))).epsilon(0.02));
    CHECK(double(k.right - k.left) == doctest::Approx(double(widthOf("aa bb"))).epsilon(0.02));
}

TEST_CASE("a space collapsed into another keeps the opportunity its own box gives") {
    Fixture f;
    PrLayoutFontStyle wraps = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    PrLayoutFontStyle nowrap = textFont(PrLayoutWhiteSpaceCollapse_Collapse, PrLayoutTextWrapMode_Nowrap);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &nowrap);
    PrLayoutNode kept = f.element(root, inlineBox(), nullptr, nullptr, &nowrap);
    f.text(kept, "aaa ");
    PrLayoutNode collapsed = f.element(root, inlineBox(), nullptr, nullptr, &wraps);
    f.text(collapsed, " ");
    f.text(root, "bbb");
    f.layout(px(widthOf("aaa") + 2, 600));
    CHECK(verified(f.tree));
    CHECK(double(f.bounds(root).bottom) == doctest::Approx(40.0));
}

TEST_CASE("text is shaped in its font group's locale, and a new locale shapes it again") {
    Fixture f;
    std::string path = std::string(PR_FONTS_DIR) + "/NotoSans[wdth,wght].ttf";
    const PrFont* noto = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &noto) == PrStatus_Ok);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    prFontSetAdd(f.fonts, &descriptor, noto);
    prFontRelease(noto);
    PrFontFamily family = prFontFamilyFromName("Noto Sans");
    PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_Collapse, PrLayoutTextWrapMode_Nowrap);
    font.font = f.textFont(16, {family});
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutNode span = f.element(root, inlineBox(), nullptr, nullptr, &font);
    // Catalan's l with a middle dot and an l, which Noto Sans sets as one glyph in Catalan.
    f.text(span, "l\xc2\xb7l");
    auto width = [&]() {
        f.layout(px(400, 600));
        PrRect bounds = f.bounds(span);
        return bounds.right - bounds.left;
    };
    float unknown = width();
    font.font = f.textFont(16, {family}, prLocaleFromTag("ca"));
    prLayoutNodeSetStyle(f.tree, span, f.style(inlineBox(), nullptr, nullptr, &font));
    CHECK(width() < unknown);
    CHECK(verified(f.tree));
}

TEST_CASE("a text node in a record of its own sets its text in it, reaches its line height, and returns to its parent's") {
    Fixture f;
    PrLayoutFontStyle normal = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &normal);
    PrLayoutNode text = f.text(root, "a\nb");
    f.layout(px(400, 600));
    CHECK(f.bounds(root).bottom == 20.0f);
    CHECK(prLayoutNodeStyle(f.tree, text) == kPrLayoutStyleNone);

    // Its own record preserves the break and sets 40px lines, which reach past the container's 20px strut.
    PrLayoutFontStyle own = textFont(PrLayoutWhiteSpaceCollapse_Preserve);
    own.lineHeight = prLayoutLineHeightPx(40);
    PrLayoutStyleIndex style = f.style(inlineBox(), nullptr, nullptr, &own);
    prLayoutNodeSetStyle(f.tree, text, style);
    f.layout(px(400, 600));
    CHECK(f.bounds(root).bottom == 80.0f);
    CHECK(prLayoutNodeStyle(f.tree, text) == style);
    CHECK(verified(f.tree));

    prLayoutNodeSetStyle(f.tree, text, kPrLayoutStyleNone);
    f.layout(px(400, 600));
    CHECK(f.bounds(root).bottom == 20.0f);
    CHECK(verified(f.tree));

    // Its record's wrap mode holds between its words, where the container's mode would wrap them.
    prLayoutNodeSetText(f.tree, text, "wrapped words");
    f.layout(px(10, 600));
    CHECK(f.bounds(root).bottom == 40.0f);
    PrLayoutFontStyle nowrap = textFont(PrLayoutWhiteSpaceCollapse_Collapse, PrLayoutTextWrapMode_Nowrap);
    prLayoutNodeSetStyle(f.tree, text, f.style(inlineBox(), nullptr, nullptr, &nowrap));
    f.layout(px(10, 600));
    CHECK(f.bounds(root).bottom == 20.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("text in a fully hinted font advances as hinted at the tree's target pixel ratio, and again at a new one") {
    PrFontHinter* hinter = prFontFreeTypeCreate();
    {
        Fixture f;
        prFontSetSetHinting(f.fonts, PrFontHinting_Full, hinter);
        const float size = 11;
        const char* text = "aaaaaaaa";
        PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_Preserve, PrLayoutTextWrapMode_Nowrap);
        font.font = f.textFont(size);
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
        PrLayoutNode span = f.element(root, inlineBox(), nullptr, nullptr, &font);
        f.text(span, text);
        // The width that the font's advances give at a ratio: hinted to the target pixels of that ratio, or as
        // designed.
        const PrFont* file = nullptr;
        REQUIRE(prFontCreateFromFile(std::string(PR_FONTS_DIR) + "/Arimo-Regular.ttf", 0, nullptr, &file) ==
            PrStatus_Ok);
        PrFontDerivation hinted = prFontDerivationDefault();
        hinted.hinting = PrFontHinting_Full;
        hinted.hinter = hinter;
        hinted.overrides = PrFontOverrideFlag_Hinting;
        const PrFont* arimo = prFontCreateDerived(file, &hinted);
        prFontRelease(file);
        uint16_t m = prFontCodePointGlyph(arimo, 'a');
        auto expected = [&](float ratio) { return 8 * prFontGlyphAdvance(arimo, m, size * ratio) * size; };
        CHECK(expected(1) != doctest::Approx(8 * prFontGlyphAdvance(arimo, m, 0) * size));
        CHECK(expected(1) != doctest::Approx(expected(2)));
        for (float ratio : {1.0f, 2.0f, 1.0f}) {
            CAPTURE(ratio);
            prLayoutTreeSetTargetPixelRatio(f.tree, ratio);
            f.layout(px(400, 600));
            PrRect bounds = f.bounds(span);
            CHECK(bounds.right - bounds.left == doctest::Approx(expected(ratio)).epsilon(1e-4));
        }
        prFontRelease(arimo);
    }
    prFontHinterDestroy(hinter);
}

TEST_CASE("the first formatted line takes its own records: its line height, its fonts and where it breaks") {
    Fixture f;
    PrLayoutFontStyle normal = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &normal);
    PrLayoutNode span = f.element(root, inlineBox(), nullptr, nullptr, &normal);
    f.text(span, "aa bb");
    f.text(root, " cc dd");
    f.layout(px(400, 600));
    CHECK(f.bounds(root).bottom == 20.0f);
    float narrow = f.bounds(span).right - f.bounds(span).left;

    // The container's record for the line sets 40px lines, and the span's sets its text at twice the size.
    PrLayoutFontStyle tall = normal;
    tall.lineHeight = prLayoutLineHeightPx(40);
    prLayoutNodeSetFirstLineStyle(f.tree, root, f.style(block(), nullptr, nullptr, &tall));
    PrLayoutFontStyle big = normal;
    big.font = textFontOf(32);
    prLayoutNodeSetFirstLineStyle(f.tree, span, f.style(inlineBox(), nullptr, nullptr, &big));
    f.layout(px(400, 600));
    CHECK(f.bounds(root).bottom == 40.0f);
    float wide = f.bounds(span).right - f.bounds(span).left;
    CHECK(wide > narrow * 1.5f);
    CHECK(verified(f.tree));

    // Too narrow for the first line in its own fonts: it breaks after the span, and the next line, in the container's
    // own records, is 20px.
    f.layout(px(wide + 5, 600));
    CHECK(f.bounds(root).bottom == 60.0f);
    CHECK(verified(f.tree));

    // Without the records, the line is as before.
    prLayoutNodeSetFirstLineStyle(f.tree, root, kPrLayoutStyleNone);
    prLayoutNodeSetFirstLineStyle(f.tree, span, kPrLayoutStyleNone);
    f.layout(px(400, 600));
    CHECK(f.bounds(root).bottom == 20.0f);
    CHECK(f.bounds(span).right - f.bounds(span).left == doctest::Approx(narrow));
    CHECK(verified(f.tree));
}

TEST_CASE("a float sizes to its first formatted line in its own fonts") {
    Fixture f;
    PrLayoutFontStyle normal = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &normal);
    PrLayoutBoxStyle floating = block();
    floating.floating = PrLayoutFloat_Left;
    PrLayoutNode box = f.element(root, floating, nullptr, nullptr, &normal);
    f.text(box, "aa bb");
    f.layout(px(400, 600));
    float plain = f.bounds(box).right - f.bounds(box).left;
    PrLayoutFontStyle big = normal;
    big.font = textFontOf(32);
    prLayoutNodeSetFirstLineStyle(f.tree, box, f.style(floating, nullptr, nullptr, &big));
    f.layout(px(400, 600));
    CHECK(f.bounds(box).right - f.bounds(box).left > plain * 1.5f);
    CHECK(verified(f.tree));
}

TEST_CASE("overflow-wrap breaks a word that fits no line, and anywhere also narrows the min-content size") {
    const char* word = "abcdefghijklmnop";
    Broken normal = brokenOf(breakingFont(PrTextWordBreak_Normal, PrTextOverflowWrap_Normal), word, 30);
    CHECK(normal.height == 20.0f);
    CHECK(normal.floatWidth > 30.0f);
    Broken anywhere = brokenOf(breakingFont(PrTextWordBreak_Normal, PrTextOverflowWrap_Anywhere), word, 30);
    CHECK(anywhere.height > 40.0f);
    CHECK(anywhere.floatWidth <= 30.0f);
    Broken breakWord = brokenOf(breakingFont(PrTextWordBreak_Normal, PrTextOverflowWrap_BreakWord), word, 30);
    CHECK(breakWord.height == anywhere.height);
    CHECK(breakWord.floatWidth == normal.floatWidth);
    // word-break: break-word is overflow-wrap: anywhere.
    Broken legacy = brokenOf(breakingFont(PrTextWordBreak_BreakWord, PrTextOverflowWrap_Normal), word, 30);
    CHECK(legacy.height == anywhere.height);
    CHECK(legacy.floatWidth == anywhere.floatWidth);
    // Text that does not wrap breaks nowhere.
    PrLayoutFontStyle nowrap = breakingFont(PrTextWordBreak_Normal, PrTextOverflowWrap_Anywhere);
    nowrap.textWrapMode = PrLayoutTextWrapMode_Nowrap;
    CHECK(brokenOf(nowrap, word, 30).height == 20.0f);
    nowrap.overflowWrap = PrTextOverflowWrap_BreakWord;
    CHECK(brokenOf(nowrap, word, 30).height == 20.0f);
    // A word that fits no line breaks at the last opportunity that fits, after the words before it.
    PrLayoutFontStyle wraps = breakingFont(PrTextWordBreak_Normal, PrTextOverflowWrap_BreakWord);
    Broken after = brokenOf(wraps, "ab abcdefghijklmnop", 30);
    CHECK(after.height == breakWord.height + 20.0f);
}

TEST_CASE("word-break: break-all breaks between letters, and keep-all keeps East Asian words whole") {
    Broken normal = brokenOf(breakingFont(PrTextWordBreak_Normal, PrTextOverflowWrap_Normal), "ab cdefgh", 1);
    Broken all = brokenOf(breakingFont(PrTextWordBreak_BreakAll, PrTextOverflowWrap_Normal), "ab cdefgh", 1);
    CHECK(all.floatWidth < normal.floatWidth / 3);
    const char* japanese = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x96\x87";
    Broken ideographs = brokenOf(breakingFont(PrTextWordBreak_Normal, PrTextOverflowWrap_Normal), japanese, 1);
    Broken kept = brokenOf(breakingFont(PrTextWordBreak_KeepAll, PrTextOverflowWrap_Normal), japanese, 1);
    CHECK(kept.floatWidth > ideographs.floatWidth * 3);
}

TEST_CASE("text is shaped with its font group's feature set") {
    Fixture f;
    PrLayoutFontStyle normal = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &normal);
    PrLayoutNode span = f.element(root, inlineBox(), nullptr, nullptr, &normal);
    f.text(span, "small capitals");
    f.layout(px(400, 600));
    float plain = f.bounds(span).right - f.bounds(span).left;
    PrTextFeatureSetDesc desc{
        .caps = PrTextCaps_AllSmall,
        .kerning = PrTextKerning_Auto,
        .synthesis = PrTextSynthesisFlag_SmallCaps,
    };
    PrTextFeatureSet* caps = prTextFeatureSetCreate(&desc);
    PrLayoutFontStyle capped = normal;
    capped.features = caps;
    prLayoutNodeSetStyle(f.tree, span, f.style(inlineBox(), nullptr, nullptr, &capped));
    prTextFeatureSetRelease(caps);
    f.layout(px(400, 600));
    CHECK(f.bounds(span).right - f.bounds(span).left != doctest::Approx(plain));
    CHECK(verified(f.tree));
}

TEST_CASE("text beside a box of other font features is shaped with its own features") {
    Fixture f;
    PrLayoutFontStyle normal = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    PrTextFeatureSetDesc desc{
        .caps = PrTextCaps_AllSmall,
        .kerning = PrTextKerning_Auto,
        .synthesis = PrTextSynthesisFlag_SmallCaps,
    };
    PrTextFeatureSet* caps = prTextFeatureSetCreate(&desc);
    PrLayoutFontStyle capped = normal;
    capped.features = caps;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &normal);
    PrLayoutNode first = f.element(root, inlineBox(), nullptr, nullptr, &capped);
    f.text(first, "the");
    PrLayoutNode rest = f.element(root, inlineBox(), nullptr, nullptr, &normal);
    f.text(rest, "following");
    prTextFeatureSetRelease(caps);
    f.layout(px(400, 600));
    CHECK(f.bounds(rest).right - f.bounds(rest).left == doctest::Approx(widthOf("following")));
    CHECK(verified(f.tree));
}

TEST_CASE("synthesized small capitals paint at the em that shaping scaled them to") {
    Fixture f;
    PrTextFeatureSetDesc desc{
        .caps = PrTextCaps_Small,
        .kerning = PrTextKerning_Auto,
        .synthesis = PrTextSynthesisFlag_SmallCaps,
    };
    PrTextFeatureSet* caps = prTextFeatureSetCreate(&desc);
    PrLayoutFontStyle capped = textFont(PrLayoutWhiteSpaceCollapse_Collapse);
    capped.features = caps;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &capped);
    f.text(root, "Tht");
    prTextFeatureSetRelease(caps);
    f.layout(px(400, 600));
    CHECK(verified(f.tree));
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(f.tree, list);
    std::vector<DrawnGlyph> drawn;
    appendGlyphs(list, prDrawTransformFromDAffine2(PrDAffine2{1, 0, 0, 1, 0, 0}), drawn);
    prDrawListRelease(list);
    // The capital keeps the em of the text, and the lowercase letters are capitals at a smaller em.
    REQUIRE(drawn.size() == 3);
    CHECK(drawn[0].size == 16.0f);
    CHECK(drawn[1].size < 16.0f);
    CHECK(drawn[2].size == drawn[1].size);
    CHECK(drawn[2].id == drawn[0].id);
}

TEST_CASE("text is transformed after its white space collapses and before it is shaped") {
    auto transformedWidth = [](const char* text, PrTextTransform transform) {
        Fixture f;
        PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_Collapse, PrLayoutTextWrapMode_Nowrap);
        font.textTransform = transform;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
        PrLayoutNode span = f.element(root, inlineBox(), nullptr, nullptr, &font);
        f.text(span, text);
        f.layout(px(400, 600));
        CHECK(verified(f.tree));
        PrRect bounds = f.bounds(span);
        return bounds.right - bounds.left;
    };
    // A full mapping lengthens the text: the sharp s is two capitals.
    CHECK(transformedWidth("straße", PrTextTransform_Uppercase) == doctest::Approx(widthOf("STRASSE")));
    CHECK(transformedWidth("MiXeD", PrTextTransform_Lowercase) == doctest::Approx(widthOf("mixed")));
    CHECK(transformedWidth("the miXed text", PrTextTransform_Capitalize) == doctest::Approx(widthOf("The MiXed Text")));
    CHECK(transformedWidth("a   b", PrTextTransform_Uppercase) == doctest::Approx(widthOf("A B")));
}

TEST_CASE("capitalize reads words across inline boxes, and a lengthened text keeps the boxes after it in place") {
    Fixture f;
    PrLayoutFontStyle font = textFont(PrLayoutWhiteSpaceCollapse_Collapse, PrLayoutTextWrapMode_Nowrap);
    font.textTransform = PrTextTransform_Capitalize;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutNode first = f.element(root, inlineBox(), nullptr, nullptr, &font);
    f.text(first, "mi");
    PrLayoutNode inWord = f.element(root, inlineBox(), nullptr, nullptr, &font);
    f.text(inWord, "xed ");
    PrLayoutNode wordStart = f.element(root, inlineBox(), nullptr, nullptr, &font);
    f.text(wordStart, "xed");
    f.layout(px(400, 600));
    auto width = [&](PrLayoutNode node) { return f.bounds(node).right - f.bounds(node).left; };
    CHECK(width(first) == doctest::Approx(widthOf("Mi")));
    CHECK(width(inWord) == doctest::Approx(widthOf("xed ")).epsilon(0.002));
    CHECK(width(wordStart) == doctest::Approx(widthOf("Xed")));
    // Uppercase lengthens the first box's text, and the boxes after it move along with it.
    font.textTransform = PrTextTransform_Uppercase;
    prLayoutNodeSetStyle(f.tree, first, f.style(inlineBox(), nullptr, nullptr, &font));
    prLayoutNodeSetText(f.tree, prLayoutNodeFirstChild(f.tree, first), "ß");
    f.layout(px(400, 600));
    CHECK(width(first) == doctest::Approx(widthOf("SS")));
    CHECK(f.bounds(inWord).left == doctest::Approx(f.bounds(first).right));
    CHECK(width(inWord) == doctest::Approx(widthOf("xed ")).epsilon(0.002));
    CHECK(verified(f.tree));
}
