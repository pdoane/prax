// The FreeType hinter: hinted outlines fit their size's pixel grid, full hinting fits advances to whole pixels, and a
// font set hints the faces that it matches by it.

#include "pr/float.h"
#include "pr/font.h"
#include "pr/font_freetype.h"
#include "pr/font_set.h"
#include "pr/string.h"

#include "tools/harness/test.h"

extern "C" int prTestFontFreeTypeCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

const PrFont* load(const char* name) {
    String path = format("{}/{}", PR_FONTS_DIR, name);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Returns the font hinted by `hinter`.
const PrFont* hinted(const PrFont* font, PrFontHinter* hinter, PrFontHinting hinting) {
    PrFontDerivation d = prFontDerivationDefault();
    d.hinting = hinting;
    d.hinter = hinter;
    d.overrides = PrFontOverrideFlag_Hinting;
    return prFontCreateDerived(font, &d);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestFontFreeTypeCApi() == 0);
}

TEST_CASE("a hinted outline fits its size's pixel grid") {
    PrFontHinter* hinter = prFontFreeTypeCreate();
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    uint16_t x = prFontCodePointGlyph(font, 'x');
    const float ppem = 13;
    float design = prFontGlyphBounds(font, x).top * ppem;
    CHECK(prF32Abs(design - prF32Round(design)) > 0.02f);
    const PrFontHinting hintings[] = {PrFontHinting_Light, PrFontHinting_Full};
    for (PrFontHinting hinting : hintings) {
        const PrFont* fitted = hinted(font, hinter, hinting);
        CHECK(prFontHinting(fitted) == hinting);
        CHECK(prFontHinter(fitted) == hinter);
        PrPath* path = prPathCreate();
        REQUIRE(prFontGlyphOutline(fitted, x, ppem, prAffine2Scale(PrVec2{ppem, ppem}), path));
        float top = prPathBounds(path).top;
        CHECK(prF32Abs(top - prF32Round(top)) < 0.02f);
        CHECK(prF32Abs(top - design) < 1.0f);
        // A derivation of a hinted font stays hinted.
        PrFontDerivation none = prFontDerivationDefault();
        const PrFont* again = prFontCreateDerived(fitted, &none);
        CHECK(prFontHinting(again) == hinting);
        prFontRelease(again);
        prPathDestroy(path);
        prFontRelease(fitted);
    }
    prFontRelease(font);
    prFontHinterDestroy(hinter);
}

TEST_CASE("full hinting fits advances to whole pixels, and the others keep the design's") {
    PrFontHinter* hinter = prFontFreeTypeCreate();
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    const uint32_t codePoints[] = {'a', 'm', 'i'};
    for (uint32_t c : codePoints) {
        uint16_t glyph = prFontCodePointGlyph(font, c);
        float design = prFontGlyphAdvance(font, glyph, 0);
        CHECK(prFontGlyphAdvance(font, glyph, 11) == design);
        const PrFont* light = hinted(font, hinter, PrFontHinting_Light);
        CHECK(prFontGlyphAdvance(light, glyph, 11) == design);
        prFontRelease(light);
        const PrFont* full = hinted(font, hinter, PrFontHinting_Full);
        CHECK(prFontGlyphAdvance(full, glyph, 0) == design);
        float pixels = prFontGlyphAdvance(full, glyph, 11) * 11;
        prFontRelease(full);
        CHECK(pixels == doctest::Approx(prF32Round(pixels)).epsilon(1e-5));
        CHECK(prF32Abs(pixels - design * 11) <= 1.0f);
    }
    prFontRelease(font);
    prFontHinterDestroy(hinter);
}

TEST_CASE("a hinter hints each instance of a variable font at its own axis values") {
    PrFontHinter* hinter = prFontFreeTypeCreate();
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    PrFontVariation heavy{PR_FONT_TAG('w', 'g', 'h', 't'), 900};
    PrFontDerivation d = prFontDerivationDefault();
    d.variations = &heavy;
    d.variationCount = 1;
    const PrFont* bold = prFontCreateDerived(font, &d);
    const PrFont* regular = hinted(font, hinter, PrFontHinting_Full);
    const PrFont* black = hinted(bold, hinter, PrFontHinting_Full);
    uint16_t m = prFontCodePointGlyph(font, 'm');
    CHECK(prFontGlyphAdvance(black, m, 16) > prFontGlyphAdvance(regular, m, 16));
    prFontRelease(black);
    prFontRelease(regular);
    prFontRelease(bold);
    prFontRelease(font);
    prFontHinterDestroy(hinter);
}

TEST_CASE("a set hints the faces that it matches, and a set over it hints them the same unless it sets its own") {
    PrFontHinter* hinter = prFontFreeTypeCreate();
    PrFontSet* host = prFontSetCreate(nullptr);
    String path = format("{}/NotoSans[wdth,wght].ttf", PR_FONTS_DIR);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    REQUIRE(prFontSetAddFile(host, &descriptor, path) > 0);
    CHECK(prFontSetHinting(host, nullptr) == PrFontHinting_None);
    prFontSetSetHinting(host, PrFontHinting_Full, hinter);
    PrFontHinter* reported = nullptr;
    CHECK(prFontSetHinting(host, &reported) == PrFontHinting_Full);
    CHECK(reported == hinter);
    PrFontSet* document = prFontSetCreate(host);
    PrFontSet* designed = prFontSetCreate(host);
    prFontSetSetHinting(designed, PrFontHinting_Light, nullptr);
    CHECK(prFontSetHinting(designed, nullptr) == PrFontHinting_None);
    PrFontStyle style = prFontStyleDefault();
    PrFontMatch* match = prFontMatchCreate();
    struct Expected {
        PrFontSet* set;
        PrFontHinting hinting;
        PrFontHinter* hinter;
    };
    const Expected cases[] = {
        {host, PrFontHinting_Full, hinter}, {document, PrFontHinting_Full, hinter},
        {designed, PrFontHinting_None, nullptr},
    };
    for (const Expected& expected : cases) {
        prFontMatchClear(match);
        prFontSetMatch(expected.set, prFontFamilyFromName("Noto Sans"), &style, match);
        REQUIRE(prFontMatchFaceCount(match) > 0);
        const PrFont* font = prFontMatchFace(match, 0).font;
        REQUIRE(font != nullptr);
        CHECK(prFontHinting(font) == expected.hinting);
        CHECK(prFontHinter(font) == expected.hinter);
    }
    prFontMatchDestroy(match);
    prFontSetDestroy(designed);
    prFontSetDestroy(document);
    prFontSetDestroy(host);
    prFontHinterDestroy(hinter);
}

TEST_CASE("a face whose descriptor sets its hinting keeps it whatever its set's hinting") {
    PrFontHinter* hinter = prFontFreeTypeCreate();
    PrFontSet* set = prFontSetCreate(nullptr);
    String path = format("{}/NotoSans[wdth,wght].ttf", PR_FONTS_DIR);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    descriptor.derivation.overrides = PrFontOverrideFlag_Hinting;
    descriptor.derivation.hinting = PrFontHinting_None;
    REQUIRE(prFontSetAddFile(set, &descriptor, path) > 0);
    prFontSetSetHinting(set, PrFontHinting_Full, hinter);
    PrFontStyle style = prFontStyleDefault();
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, prFontFamilyFromName("Noto Sans"), &style, match);
    REQUIRE(prFontMatchFaceCount(match) > 0);
    CHECK(prFontHinting(prFontMatchFace(match, 0).font) == PrFontHinting_None);
    prFontMatchDestroy(match);
    prFontSetDestroy(set);
    prFontHinterDestroy(hinter);
}
