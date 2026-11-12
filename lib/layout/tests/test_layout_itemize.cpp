// Itemization: text shaped in runs of one script, face and style across nodes and inline boxes, with a context that
// stops where shaping must.

#include "layout/tests/layout_fixture.h"

namespace pr_impl {

namespace {

// Returns a font of `family`, at 20px.
PrLayoutFontStyle fontOf(PrFontFamily family) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(20, Span<const PrFontFamily>(&family, 1));
    return font;
}

// Returns the glyphs that `build` paints, in a block of the font of `families`, laid out 800 wide.
template <typename Build>
Array<uint16_t> glyphsOf(PrStringView file, PrStringView family, Build build) {
    Fixture f;
    PrLayoutFontStyle font = fontOf(f.family(file, family));
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    build(f, root, font);
    f.layout(px(800, 600));
    CHECK(verified(f.tree));
    return paintedGlyphs(f.tree);
}

// Creates an inline box in `font`, with `padding` on its inline sides.
PrLayoutNode span(Fixture& f, PrLayoutNode parent, const PrLayoutFontStyle& font, float padding = 0) {
    PrLayoutBoxStyle box = inlineBox();
    box.padding[PrRectSide_Left] = prLayoutLengthPx(padding);
    box.padding[PrRectSide_Right] = prLayoutLengthPx(padding);
    return f.element(parent, box, nullptr, nullptr, &font);
}

constexpr const char* kDevanagari = "NotoSansDevanagari[wdth,wght].ttf";
constexpr const char* kArabic = "NotoSansArabic[wdth,wght].ttf";

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("two scripts in one face shape each in its own script") {
    // कि reorders its vowel sign before its consonant in Devanagari, but not in Latin, which the text starts in.
    Array<uint16_t> alone = glyphsOf(kDevanagari, "Noto Sans Devanagari",
        [](Fixture& f, PrLayoutNode root, const PrLayoutFontStyle&) { f.text(root, "कि"); });
    Array<uint16_t> mixed = glyphsOf(kDevanagari, "Noto Sans Devanagari",
        [](Fixture& f, PrLayoutNode root, const PrLayoutFontStyle&) { f.text(root, "ab कि"); });
    REQUIRE(alone.count() == 2);
    REQUIRE(mixed.count() == 5);
    CHECK(mixed[3] == alone[0]);
    CHECK(mixed[4] == alone[1]);
    // Punctuation between them takes the script before it, and a closing bracket takes its opening bracket's script.
    Array<uint16_t> bracketed = glyphsOf(kDevanagari, "Noto Sans Devanagari",
        [](Fixture& f, PrLayoutNode root, const PrLayoutFontStyle&) { f.text(root, "(कि) a"); });
    REQUIRE(bracketed.count() == 6);
    CHECK(bracketed[1] == alone[0]);
    CHECK(bracketed[2] == alone[1]);
}

TEST_CASE("identically styled inline boxes and adjacent text nodes shape as one node") {
    auto joined = [](Fixture& f, PrLayoutNode root, const PrLayoutFontStyle&) { f.text(root, "ععع"); };
    auto nodes = [](Fixture& f, PrLayoutNode root, const PrLayoutFontStyle&) {
        f.text(root, "ع");
        f.text(root, "ع");
        f.text(root, "ع");
    };
    auto spans = [](Fixture& f, PrLayoutNode root, const PrLayoutFontStyle& font) {
        f.text(root, "ع");
        f.text(span(f, root, font), "ع");
        f.text(root, "ع");
    };
    Array<uint16_t> expected = glyphsOf(kArabic, "Noto Sans Arabic", joined);
    REQUIRE(expected.count() == 3);
    // The letters join: initial, medial and final forms differ.
    CHECK(expected[0] != expected[1]);
    CHECK(glyphsOf(kArabic, "Noto Sans Arabic", nodes) == expected);
    CHECK(glyphsOf(kArabic, "Noto Sans Arabic", spans) == expected);
}

TEST_CASE("an inline box's padding interrupts shaping and the joining context across it") {
    // Compared against the same letters split by explicit zero-width non-joiners, which join with nothing.
    auto padded = [](Fixture& f, PrLayoutNode root, const PrLayoutFontStyle& font) {
        f.text(root, "ع");
        f.text(span(f, root, font, 5), "ع");
        f.text(root, "ع");
    };
    auto unjoined = [](Fixture& f, PrLayoutNode root, const PrLayoutFontStyle&) {
        f.text(root, "ع‌ع‌ع");
    };
    Array<uint16_t> expected = glyphsOf(kArabic, "Noto Sans Arabic", unjoined);
    Array<uint16_t> shaped = glyphsOf(kArabic, "Noto Sans Arabic", padded);
    REQUIRE(shaped.count() == 3);
    // Each letter stands alone: the same isolated form three times.
    CHECK(shaped[0] == shaped[1]);
    CHECK(shaped[1] == shaped[2]);
    CHECK(shaped[0] == expected.last());
}
