#include "pr/array.h"
#include "pr/float.h"
#include "pr/font_subset.h"
#include "pr/path.h"
#include "pr/string.h"

#include "tools/harness/test.h"

extern "C" int prTestFontSubsetCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

void collect(void* PR_NULLABLE context, PrByteView bytes) {
    Array<uint8_t>& out = *static_cast<Array<uint8_t>*>(context);
    out.append(bytes.data, uint32_t(bytes.size));
}

const PrFont* open(const char* path) {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Returns the glyph's outline as text, each coordinate with six decimals.
String outline(const PrFont* font, uint16_t glyph) {
    PrPath* path = prPathCreate();
    String text;
    if (prFontGlyphOutline(font, glyph, 0, prAffine2Identity(), path)) {
        const PrVec2* points = prPathPoints(path);
        for (uint32_t i = 0; i < prPathPointCount(path); ++i)
            formatTo(text, "{:.6f},{:.6f} ", points[i].x, points[i].y);
    }
    prPathDestroy(path);
    return text;
}

uint32_t read32(const Array<uint8_t>& b, uint32_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

void write32(Array<uint8_t>& b, uint32_t at, uint32_t value) {
    for (uint32_t k = 0; k < 4; ++k)
        b[at + k] = uint8_t(value >> (24 - k * 8));
}

// Returns the font file `file` with its CFF table replaced by `cff`, so that a bare CFF subset opens as a font.
Array<uint8_t> withCff(const Array<uint8_t>& file, const Array<uint8_t>& cff) {
    uint32_t count = uint32_t(file[4]) << 8 | file[5];
    Array<uint8_t> out;
    out.append(file.data(), 12 + count * 16);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t record = 12 + i * 16;
        uint32_t tag = read32(file, record);
        uint32_t offset = read32(file, record + 8);
        uint32_t length = read32(file, record + 12);
        write32(out, record + 8, out.count());
        if (tag == PR_FONT_TAG('C', 'F', 'F', ' ')) {
            write32(out, record + 12, cff.count());
            out.append(cff.span());
        } else {
            out.append(file.data() + offset, length);
        }
        while (out.count() % 4 != 0)
            out.push(0);
    }
    return out;
}

// Returns the font file `file` with the glyph count of its maxp table set to `count`.
Array<uint8_t> withGlyphCount(Array<uint8_t> file, uint32_t count) {
    uint32_t tables = uint32_t(file[4]) << 8 | file[5];
    for (uint32_t i = 0; i < tables; ++i) {
        uint32_t record = 12 + i * 16;
        if (read32(file, record) == PR_FONT_TAG('m', 'a', 'x', 'p')) {
            uint32_t at = read32(file, record + 8) + 4;
            file[at] = uint8_t(count >> 8);
            file[at + 1] = uint8_t(count);
        }
    }
    return file;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the subset header compiles as C") {
    CHECK(prTestFontSubsetCApi() == 0);
}

TEST_CASE("a TrueType subset keeps the glyphs asked for under their ids") {
    const PrFont* font = open(PR_FONTS_DIR "/NotoSans[wdth,wght].ttf");
    CHECK(prFontSubsetFormat(font) == PrFontSubsetFormat_TrueType);
    uint16_t a = prFontCodePointGlyph(font, 'a');
    uint16_t e = prFontCodePointGlyph(font, 'e');
    uint16_t aacute = prFontCodePointGlyph(font, 0xe1);
    uint16_t glyphs[2] = {e, aacute};
    Array<uint8_t> bytes;
    CHECK(prFontSubset(font, glyphs, 2, collect, &bytes) == PrFontSubsetFormat_TrueType);
    CHECK(bytes.count() < prFontData(font).size / 10);
    const PrFont* subset = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &subset) == PrStatus_Ok);
    uint16_t largest = e > aacute ? e : aacute;
    CHECK(prFontGlyphCount(subset) >= uint32_t(largest) + 1);
    CHECK(outline(subset, e) == outline(font, e));
    CHECK(outline(subset, aacute) == outline(font, aacute));
    CHECK(prFontGlyphAdvance(subset, e, 0) == prFontGlyphAdvance(font, e, 0));
    // A glyph that no kept glyph is built from is empty, unless the accent is built from it.
    if (a < largest && outline(subset, a) != "")
        CHECK(outline(subset, a) == outline(font, a));
    uint16_t z = prFontCodePointGlyph(font, 'z');
    if (z < largest)
        CHECK(outline(subset, z) == "");
    prFontRelease(subset);
    prFontRelease(font);
}

TEST_CASE("a CFF subset keeps the glyphs asked for under their ids") {
    const PrFont* font = open(PR_FONTS_DIR "/SFNT-CFF-Reference.otf");
    CHECK(prFontSubsetFormat(font) == PrFontSubsetFormat_Cff);
    uint32_t count = prFontGlyphCount(font);
    REQUIRE(count > 3);
    uint16_t kept = uint16_t(count - 2);
    Array<uint8_t> cff;
    CHECK(prFontSubset(font, &kept, 1, collect, &cff) == PrFontSubsetFormat_Cff);
    PrByteView data = prFontData(font);
    Array<uint8_t> file;
    file.append(data.data, uint32_t(data.size));
    Array<uint8_t> rebuilt = withCff(file, cff);
    const PrFont* subset = nullptr;
    REQUIRE(prFontCreate(rebuilt, 0, nullptr, &subset) == PrStatus_Ok);
    CHECK(outline(subset, kept) == outline(font, kept));
    CHECK(outline(subset, 0) == outline(font, 0));
    for (uint16_t g = 1; g < kept; ++g)
        CHECK(outline(subset, g) == "");
    prFontRelease(subset);
    prFontRelease(font);
}

TEST_CASE("a font whose file does not hold its outlines is subset as CFF, with the outlines that it draws") {
    // The subset is a bare CFF table, which opens in a CFF font's file in place of its own, with the glyph count of the
    // subset.
    const PrFont* container = open(PR_FONTS_DIR "/SFNT-CFF-Reference.otf");
    PrByteView data = prFontData(container);
    Array<uint8_t> file;
    file.append(data.data, uint32_t(data.size));
    auto reopened = [&](const PrFont* font, uint16_t glyph) {
        Array<uint8_t> cff;
        CHECK(prFontSubset(font, &glyph, 1, collect, &cff) == PrFontSubsetFormat_Cff);
        Array<uint8_t> rebuilt = withGlyphCount(withCff(file, cff), uint32_t(glyph) + 1);
        const PrFont* subset = nullptr;
        REQUIRE(prFontCreate(rebuilt, 0, nullptr, &subset) == PrStatus_Ok);
        return subset;
    };
    // Each point of an outline is rounded to a thousandth of an em.
    auto near = [](const PrFont* a, const PrFont* b, uint16_t glyph) {
        PrPath* pa = prPathCreate();
        PrPath* pb = prPathCreate();
        prFontGlyphOutline(a, glyph, 0, prAffine2Identity(), pa);
        prFontGlyphOutline(b, glyph, 0, prAffine2Identity(), pb);
        bool same = prPathPointCount(pa) == prPathPointCount(pb) && prPathPointCount(pa) != 0;
        for (uint32_t i = 0; same && i < prPathPointCount(pa); ++i) {
            PrVec2 d = prPathPoints(pa)[i] - prPathPoints(pb)[i];
            same = prF32Abs(d.x) <= 0.001f && prF32Abs(d.y) <= 0.001f;
        }
        prPathDestroy(pa);
        prPathDestroy(pb);
        return same;
    };

    // CFF2 outlines, which are cubic as CFF's are.
    const PrFont* cff2 = open(PR_FONTS_DIR "/noto_handwriting-cff2_colr_1.otf");
    CHECK(prFontSubsetFormat(cff2) == PrFontSubsetFormat_Cff);
    uint16_t glyph = 1;
    while (glyph < prFontGlyphCount(cff2) && outline(cff2, glyph).isEmpty())
        ++glyph;
    const PrFont* subset = reopened(cff2, glyph);
    CHECK(near(subset, cff2, glyph));
    prFontRelease(subset);
    prFontRelease(cff2);

    // Synthetic bold: the subset holds the emboldened outline.
    const PrFont* font = open(PR_FONTS_DIR "/NotoSans[wdth,wght].ttf");
    PrFontDerivation bold = prFontDerivationDefault();
    bold.embolden = 0.02f;
    const PrFont* derived = prFontCreateDerived(font, &bold);
    CHECK(prFontSubsetFormat(derived) == PrFontSubsetFormat_Cff);
    uint16_t dot = prFontCodePointGlyph(font, '.');
    subset = reopened(derived, dot);
    PrRect drawn = prFontGlyphBounds(derived, dot);
    PrRect held = prFontGlyphBounds(subset, dot);
    CHECK(prF32Abs(held.left - drawn.left) <= 0.002f);
    CHECK(prF32Abs(held.right - drawn.right) <= 0.002f);
    CHECK(held.right - held.left > prFontGlyphBounds(font, dot).right - prFontGlyphBounds(font, dot).left);
    prFontRelease(subset);
    prFontRelease(derived);
    prFontRelease(font);
    prFontRelease(container);
}
