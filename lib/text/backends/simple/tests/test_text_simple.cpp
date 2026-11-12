// The simple shaper: one glyph per character and pair kerning place Latin, Greek and Cyrillic text as HarfBuzz does.

#include "pr/font.h"
#include "pr/text_harfbuzz.h"
#include "pr/text_simple.h"

#include "tools/harness/test.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" int prTestTextSimpleCApi(void);

namespace pr_impl {

namespace {

std::vector<uint8_t> readFile(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    uint8_t buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
    return bytes;
}

const PrFont* load(const std::vector<uint8_t>& bytes) {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

std::vector<PrTextGlyph> shape(PrTextShaper* shaper, const PrTextRun& run) {
    PrTextGlyphBuffer* glyphs = prTextGlyphBufferCreate();
    REQUIRE(prTextShaperShape(shaper, &run, glyphs) == PrStatus_Ok);
    std::vector<PrTextGlyph> out(prTextGlyphBufferGlyphs(glyphs),
        prTextGlyphBufferGlyphs(glyphs) + prTextGlyphBufferGlyphCount(glyphs));
    prTextGlyphBufferDestroy(glyphs);
    return out;
}

// Returns the position of each glyph along the run: its pen position plus its offset.
std::vector<PrVec2> positionsOf(const std::vector<PrTextGlyph>& glyphs) {
    std::vector<PrVec2> out;
    PrVec2 pen{0, 0};
    for (const PrTextGlyph& g : glyphs) {
        out.push_back(PrVec2{pen.x + g.offset.x, pen.y + g.offset.y});
        pen = PrVec2{pen.x + g.advance.x, pen.y + g.advance.y};
    }
    out.push_back(pen);
    return out;
}

// Checks that the simple shaper gives the glyphs and positions that HarfBuzz gives the run.
void checkLikeHarfBuzz(PrTextShaper* simple, PrTextShaper* harfbuzz, const PrTextRun& run) {
    std::vector<PrTextGlyph> ours = shape(simple, run);
    std::vector<PrTextGlyph> theirs = shape(harfbuzz, run);
    REQUIRE(ours.size() == theirs.size());
    std::vector<PrVec2> a = positionsOf(ours);
    std::vector<PrVec2> b = positionsOf(theirs);
    for (size_t i = 0; i < ours.size(); ++i) {
        CAPTURE(i);
        CHECK(ours[i].id == theirs[i].id);
        CHECK(ours[i].cluster == theirs[i].cluster);
    }
    for (size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        CHECK(a[i].x == doctest::Approx(b[i].x).epsilon(1e-4));
        CHECK(a[i].y == doctest::Approx(b[i].y).epsilon(1e-4));
    }
}

PrTextRun runOf(const PrFont* font, const char* text) {
    PrTextRun run{};
    run.font = font;
    run.text = PrStringView{text, std::strlen(text)};
    return run;
}

// Returns the bytes of a font with a kern table appended, which kerns the pair of glyphs by `value` units.
std::vector<uint8_t> withKernTable(const std::vector<uint8_t>& font, uint16_t left, uint16_t right, int16_t value) {
    auto uint16At = [&](size_t at) { return uint32_t(font[at]) << 8 | font[at + 1]; };
    auto uint32At = [&](size_t at) { return uint16At(at) << 16 | uint16At(at + 2); };
    uint32_t count = uint16At(4);
    std::vector<uint8_t> kern = {
        0, 0, 0, 1,                                    // version 0, one subtable
        0, 0, 0, 20, 0, 1,                             // version, length, horizontal
        0, 1, 0, 6, 0, 0, 0, 0,                        // one pair
        uint8_t(left >> 8), uint8_t(left), uint8_t(right >> 8), uint8_t(right),
        uint8_t(uint16_t(value) >> 8), uint8_t(uint16_t(value)),
    };
    std::vector<uint8_t> out(font.begin(), font.begin() + 12);
    out[4] = uint8_t((count + 1) >> 8);
    out[5] = uint8_t(count + 1);
    size_t directory = 12 + (count + 1) * 16;
    std::vector<uint8_t> records;
    std::vector<uint8_t> tables;
    auto record = [&](const uint8_t* tag, uint32_t offset, uint32_t length) {
        records.insert(records.end(), tag, tag + 4);
        for (int k = 0; k < 4; ++k)
            records.push_back(0);
        for (uint32_t v : {offset, length})
            for (int shift = 24; shift >= 0; shift -= 8)
                records.push_back(uint8_t(v >> shift));
    };
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = 12 + i * 16;
        uint32_t offset = uint32At(at + 8);
        uint32_t length = uint32At(at + 12);
        record(&font[at], uint32_t(directory + tables.size()), length);
        tables.insert(tables.end(), font.begin() + ptrdiff_t(offset), font.begin() + ptrdiff_t(offset + length));
        while (tables.size() % 4 != 0)
            tables.push_back(0);
    }
    static const uint8_t kKernTag[4] = {'k', 'e', 'r', 'n'};
    record(kKernTag, uint32_t(directory + tables.size()), uint32_t(kern.size()));
    tables.insert(tables.end(), kern.begin(), kern.end());
    out.insert(out.end(), records.begin(), records.end());
    out.insert(out.end(), tables.begin(), tables.end());
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
    for (const char* file : {"NotoSans[wdth,wght].ttf", "NotoSerif[wdth,wght].ttf", "Tinos-Regular.ttf", "Arimo-Regular.ttf"}) {
        CAPTURE(file);
        std::vector<uint8_t> bytes = readFile(file);
        const PrFont* font = load(bytes);
        for (const char* text : {"AVATAR To We Ty yo P. LT", "Ταυτότητα ΑΥΤΟ", "Тест ТЬМА Гоу"}) {
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
    std::vector<uint8_t> bytes = readFile("NotoSans[wdth,wght].ttf");
    const PrFont* font = load(bytes);
    for (float weight : {100.0f, 900.0f}) {
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
    std::vector<uint8_t> bytes = readFile("NotoSans[wdth,wght].ttf");
    const PrFont* font = load(bytes);
    PrTextRun run = runOf(font, "AVATAR");
    float kerned = positionsOf(shape(simple, run)).back().x;
    run.kerning = PrTextKerning_None;
    for (const PrTextGlyph& g : shape(simple, run))
        CHECK(g.advance.x == doctest::Approx(prFontGlyphAdvance(font, g.id, 0)));
    // The font kerns these pairs closer.
    CHECK(kerned < positionsOf(shape(simple, run)).back().x - 0.05f);
    prFontRelease(font);
    prTextShaperDestroy(simple);
}

TEST_CASE("a right-to-left run comes out in visual order, its clusters descending") {
    PrTextShaper* simple = prTextSimpleCreate();
    PrTextShaper* harfbuzz = prTextHarfBuzzCreate();
    std::vector<uint8_t> bytes = readFile("NotoSansHebrew[wdth,wght].ttf");
    const PrFont* font = load(bytes);
    PrTextRun run = runOf(font, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d");
    run.direction = PrTextDirection_Rtl;
    std::vector<PrTextGlyph> glyphs = shape(simple, run);
    REQUIRE(glyphs.size() == 4);
    CHECK(glyphs[0].cluster > glyphs[3].cluster);
    checkLikeHarfBuzz(simple, harfbuzz, run);
    prFontRelease(font);
    prTextShaperDestroy(harfbuzz);
    prTextShaperDestroy(simple);
}

TEST_CASE("a font whose GPOS does not kern kerns by its kern table") {
    PrTextShaper* simple = prTextSimpleCreate();
    std::vector<uint8_t> ahem = readFile("Ahem.ttf");
    const PrFont* plain = load(ahem);
    uint16_t a = prFontCodePointGlyph(plain, 'A');
    uint16_t b = prFontCodePointGlyph(plain, 'B');
    std::vector<uint8_t> kerned = withKernTable(ahem, a, b, -100);
    const PrFont* font = load(kerned);
    std::vector<PrTextGlyph> glyphs = shape(simple, runOf(font, "AB"));
    REQUIRE(glyphs.size() == 2);
    float units = float(prFontMetrics(font).unitsPerEm);
    CHECK(glyphs[0].advance.x == doctest::Approx(prFontGlyphAdvance(font, a, 0) - 100 / units));
    CHECK(glyphs[1].advance.x == doctest::Approx(prFontGlyphAdvance(font, b, 0)));
    prFontRelease(font);
    prFontRelease(plain);
    prTextShaperDestroy(simple);
}

TEST_CASE("a vertical run stands each glyph at its vertical origin") {
    PrTextShaper* simple = prTextSimpleCreate();
    std::vector<uint8_t> bytes = readFile("NotoSansJP-VF.otf");
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
