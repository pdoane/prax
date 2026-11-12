// The simple shaper: one glyph per character and pair kerning place Latin, Greek and Cyrillic text as HarfBuzz does.

#include "pr/array.h"
#include "pr/font.h"
#include "pr/string.h"
#include "pr/text_harfbuzz.h"
#include "pr/text_simple.h"

#include "tools/harness/test.h"

extern "C" int prTestTextSimpleCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

Array<uint8_t> readFile(const char* name) {
    return harness::readFile(format("{}/{}", PR_FONTS_DIR, PrStringView(name)));
}

const PrFont* load(const Array<uint8_t>& bytes) {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

Array<PrTextGlyph> shape(PrTextShaper* shaper, const PrTextRun& run) {
    PrTextGlyphBuffer* glyphs = prTextGlyphBufferCreate();
    REQUIRE(prTextShaperShape(shaper, &run, glyphs) == PrStatus_Ok);
    Array<PrTextGlyph> out;
    out.append(prTextGlyphBufferGlyphs(glyphs), prTextGlyphBufferGlyphCount(glyphs));
    prTextGlyphBufferDestroy(glyphs);
    return out;
}

// Returns the position of each glyph along the run: its pen position plus its offset.
Array<PrVec2> positionsOf(const Array<PrTextGlyph>& glyphs) {
    Array<PrVec2> out;
    PrVec2 pen{0, 0};
    for (const PrTextGlyph& g : glyphs) {
        out.push(PrVec2{pen.x + g.offset.x, pen.y + g.offset.y});
        pen = PrVec2{pen.x + g.advance.x, pen.y + g.advance.y};
    }
    out.push(pen);
    return out;
}

// Checks that the simple shaper gives the glyphs and positions that HarfBuzz gives the run.
void checkLikeHarfBuzz(PrTextShaper* simple, PrTextShaper* harfbuzz, const PrTextRun& run) {
    Array<PrTextGlyph> ours = shape(simple, run);
    Array<PrTextGlyph> theirs = shape(harfbuzz, run);
    REQUIRE(ours.count() == theirs.count());
    Array<PrVec2> a = positionsOf(ours);
    Array<PrVec2> b = positionsOf(theirs);
    for (uint32_t i = 0; i < ours.count(); ++i) {
        CAPTURE(i);
        CHECK(ours[i].id == theirs[i].id);
        CHECK(ours[i].cluster == theirs[i].cluster);
    }
    for (uint32_t i = 0; i < a.count(); ++i) {
        CAPTURE(i);
        CHECK(a[i].x == doctest::Approx(b[i].x).epsilon(1e-4));
        CHECK(a[i].y == doctest::Approx(b[i].y).epsilon(1e-4));
    }
}

PrTextRun runOf(const PrFont* font, const char* text) {
    PrTextRun run{};
    run.font = font;
    run.text = PrStringView(text);
    return run;
}

// Returns the bytes of a font with a kern table appended, which kerns the pair of glyphs by `value` units.
Array<uint8_t> withKernTable(const Array<uint8_t>& font, uint16_t left, uint16_t right, int16_t value) {
    auto uint16At = [&](uint32_t at) { return uint32_t(font[at]) << 8 | font[at + 1]; };
    auto uint32At = [&](uint32_t at) { return uint16At(at) << 16 | uint16At(at + 2); };
    uint32_t count = uint16At(4);
    const uint8_t kern[] = {
        0, 0, 0, 1,                                    // version 0, one subtable
        0, 0, 0, 20, 0, 1,                             // version, length, horizontal
        0, 1, 0, 6, 0, 0, 0, 0,                        // one pair
        uint8_t(left >> 8), uint8_t(left), uint8_t(right >> 8), uint8_t(right),
        uint8_t(uint16_t(value) >> 8), uint8_t(uint16_t(value)),
    };
    Array<uint8_t> out;
    out.append(font.data(), 12);
    out[4] = uint8_t((count + 1) >> 8);
    out[5] = uint8_t(count + 1);
    uint32_t directory = 12 + (count + 1) * 16;
    Array<uint8_t> records;
    Array<uint8_t> tables;
    auto record = [&](const uint8_t* tag, uint32_t offset, uint32_t length) {
        records.append(tag, 4);
        for (int k = 0; k < 4; ++k)
            records.push(0);
        const uint32_t values[] = {offset, length};
        for (uint32_t v : values)
            for (int shift = 24; shift >= 0; shift -= 8)
                records.push(uint8_t(v >> shift));
    };
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = 12 + i * 16;
        uint32_t offset = uint32At(at + 8);
        uint32_t length = uint32At(at + 12);
        record(&font[at], directory + tables.count(), length);
        tables.append(font.data() + offset, length);
        while (tables.count() % 4 != 0)
            tables.push(0);
    }
    static const uint8_t kKernTag[4] = {'k', 'e', 'r', 'n'};
    record(kKernTag, directory + tables.count(), uint32_t(sizeof(kern)));
    tables.append(Span(kern));
    out.append(records.span());
    out.append(tables.span());
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestTextSimpleCApi() == 0);
}

TEST_CASE("Latin, Greek and Cyrillic text kerns as HarfBuzz kerns it") {
    PrTextShaper* simple = prTextSimpleCreate();
    PrTextShaper* harfbuzz = prTextHarfBuzzCreate();
    const char* const files[] = {
        "NotoSans[wdth,wght].ttf", "NotoSerif[wdth,wght].ttf", "Tinos-Regular.ttf", "Arimo-Regular.ttf",
    };
    for (const char* file : files) {
        CAPTURE(file);
        Array<uint8_t> bytes = readFile(file);
        const PrFont* font = load(bytes);
        const char* const texts[] = {"AVATAR To We Ty yo P. LT", "Ταυτότητα ΑΥΤΟ", "Тест ТЬМА Гоу"};
        for (const char* text : texts) {
            CAPTURE(text);
            checkLikeHarfBuzz(simple, harfbuzz, runOf(font, text));
        }
        prFontRelease(font);
    }
    prTextShaperDestroy(harfbuzz);
    prTextShaperDestroy(simple);
}

TEST_CASE("a variable font kerns at its instance, as HarfBuzz varies its kerning") {
    PrTextShaper* simple = prTextSimpleCreate();
    PrTextShaper* harfbuzz = prTextHarfBuzzCreate();
    Array<uint8_t> bytes = readFile("NotoSans[wdth,wght].ttf");
    const PrFont* font = load(bytes);
    const float weights[] = {100.0f, 900.0f};
    for (float weight : weights) {
        PrFontVariation variation{PR_FONT_TAG('w', 'g', 'h', 't'), weight};
        PrFontDerivation d = prFontDerivationDefault();
        d.variations = &variation;
        d.variationCount = 1;
        const PrFont* instance = prFontCreateDerived(font, &d);
        checkLikeHarfBuzz(simple, harfbuzz, runOf(instance, "AVATAR To We Ty yo"));
        prFontRelease(instance);
    }
    prFontRelease(font);
    prTextShaperDestroy(harfbuzz);
    prTextShaperDestroy(simple);
}

TEST_CASE("a run without kerning advances by the font's advances alone") {
    PrTextShaper* simple = prTextSimpleCreate();
    Array<uint8_t> bytes = readFile("NotoSans[wdth,wght].ttf");
    const PrFont* font = load(bytes);
    PrTextRun run = runOf(font, "AVATAR");
    float kerned = positionsOf(shape(simple, run)).last().x;
    run.kerning = PrTextKerning_None;
    for (const PrTextGlyph& g : shape(simple, run))
        CHECK(g.advance.x == doctest::Approx(prFontGlyphAdvance(font, g.id, 0)));
    // The font kerns these pairs closer.
    CHECK(kerned < positionsOf(shape(simple, run)).last().x - 0.05f);
    prFontRelease(font);
    prTextShaperDestroy(simple);
}

TEST_CASE("a right-to-left run comes out in visual order, its clusters descending") {
    PrTextShaper* simple = prTextSimpleCreate();
    PrTextShaper* harfbuzz = prTextHarfBuzzCreate();
    Array<uint8_t> bytes = readFile("NotoSansHebrew[wdth,wght].ttf");
    const PrFont* font = load(bytes);
    PrTextRun run = runOf(font, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d");
    run.direction = PrTextDirection_Rtl;
    Array<PrTextGlyph> glyphs = shape(simple, run);
    REQUIRE(glyphs.count() == 4);
    CHECK(glyphs[0].cluster > glyphs[3].cluster);
    checkLikeHarfBuzz(simple, harfbuzz, run);
    prFontRelease(font);
    prTextShaperDestroy(harfbuzz);
    prTextShaperDestroy(simple);
}

TEST_CASE("a font whose GPOS does not kern kerns by its kern table") {
    PrTextShaper* simple = prTextSimpleCreate();
    Array<uint8_t> ahem = readFile("Ahem.ttf");
    const PrFont* plain = load(ahem);
    uint16_t a = prFontCodePointGlyph(plain, 'A');
    uint16_t b = prFontCodePointGlyph(plain, 'B');
    Array<uint8_t> kerned = withKernTable(ahem, a, b, -100);
    const PrFont* font = load(kerned);
    Array<PrTextGlyph> glyphs = shape(simple, runOf(font, "AB"));
    REQUIRE(glyphs.count() == 2);
    float units = float(prFontMetrics(font).unitsPerEm);
    CHECK(glyphs[0].advance.x == doctest::Approx(prFontGlyphAdvance(font, a, 0) - 100 / units));
    CHECK(glyphs[1].advance.x == doctest::Approx(prFontGlyphAdvance(font, b, 0)));
    prFontRelease(font);
    prFontRelease(plain);
    prTextShaperDestroy(simple);
}

TEST_CASE("a vertical run stands each glyph at its vertical origin") {
    PrTextShaper* simple = prTextSimpleCreate();
    Array<uint8_t> bytes = readFile("NotoSansJP-VF.otf");
    const PrFont* font = load(bytes);
    PrTextRun run = runOf(font, "\xe6\xbc\xa2\xe5\xad\x97");
    run.axis = PrTextAxis_Vertical;
    for (const PrTextGlyph& g : shape(simple, run)) {
        PrVec2 origin = prFontGlyphVerticalOrigin(font, g.id);
        CHECK(g.offset.x == doctest::Approx(-origin.x));
        CHECK(g.offset.y == doctest::Approx(-origin.y));
        CHECK(g.advance.y == doctest::Approx(prFontGlyphVerticalAdvance(font, g.id)));
    }
    prFontRelease(font);
    prTextShaperDestroy(simple);
}
