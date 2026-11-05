#include "pr/font.h"
#include "pr/thread.h"
#include "pr/vec.h"

#include "font/src/sfnt.h"

#include "freetype/freetype.h"
#include "freetype/ftadvanc.h"
#include "freetype/tttables.h"
#include "tools/harness/test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

extern "C" int prTestFontCApi(void);

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

const PrFont* load(const char* name) {
    std::vector<uint8_t> bytes = readFile(name);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Returns the font with its derivation changed by `change`.
template <typename Change>
const PrFont* derive(const PrFont* font, Change change) {
    PrFontDerivation d = prFontDerivationDefault();
    change(d);
    const PrFont* out = prFontCreateDerived(font, &d);
    return out;
}

const PrFont* hinted(const PrFont* font, PrFontHinting hinting) {
    return derive(font, [&](PrFontDerivation& d) {
        d.hinting = hinting;
        d.overrides = PrFontOverrideFlag_Hinting;
    });
}

std::string text(PrRawString string) {
    PrStringView view = prRawStringView(&string);
    std::string result(view.data != nullptr ? view.data : "", view.size);
    prRawStringReset(&string);
    return result;
}

constexpr uint32_t kWght = PR_FONT_TAG('w', 'g', 'h', 't');

// A painter that writes each call as a word.
struct Trace {
    std::string calls;
    std::vector<PrColor> colors;
    uint32_t depth = 0;
    uint32_t deepest = 0;
    bool balanced = true;
};

void push(Trace& t, const char* word) {
    t.calls += word;
    t.calls += ' ';
    if (++t.depth > t.deepest)
        t.deepest = t.depth;
}

void pop(Trace& t, const char* word) {
    t.calls += word;
    t.calls += ' ';
    if (t.depth == 0)
        t.balanced = false;
    else
        --t.depth;
}

PrFontPainter tracer() {
    PrFontPainter p{};
    p.pushTransform = [](void* c, PrAffine2) { push(*static_cast<Trace*>(c), "transform"); };
    p.popTransform = [](void* c) { pop(*static_cast<Trace*>(c), "/transform"); };
    p.pushClipGlyph = [](void* c, uint16_t) { push(*static_cast<Trace*>(c), "clip"); };
    p.popClip = [](void* c) { pop(*static_cast<Trace*>(c), "/clip"); };
    p.pushLayer = [](void* c, PrFontComposite) { push(*static_cast<Trace*>(c), "layer"); };
    p.popLayer = [](void* c) { pop(*static_cast<Trace*>(c), "/layer"); };
    p.fill = [](void* c, const PrFontPaint* paint) {
        const char* kinds[] = {"solid", "linear", "radial", "conic"};
        Trace& t = *static_cast<Trace*>(c);
        t.calls += kinds[paint->kind];
        t.calls += ' ';
    };
    return p;
}

const PrFont* derived(const PrFont* font, const PrFontDerivation& derivation) {
    const PrFont* out = prFontCreateDerived(font, &derivation);
    return out;
}

void put16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value & 0xffff);
}

// Returns the font file with its sbix table replaced by one strike of `ppem` pixels per em, in which glyph `glyph` is
// `image` of the sbix graphic type `type`, with its bottom left corner at the origin.
std::vector<uint8_t> withSbixImage(std::vector<uint8_t> font, uint16_t glyph, uint16_t glyphCount, uint32_t type,
        const std::vector<uint8_t>& image, uint16_t ppem) {
    std::vector<uint8_t> sbix;
    put16(sbix, 1);
    put16(sbix, 1);
    put32(sbix, 1);
    put32(sbix, 12);
    uint32_t data = 4 + (uint32_t(glyphCount) + 1) * 4;
    put16(sbix, ppem);
    put16(sbix, 72);
    for (uint32_t g = 0; g <= glyphCount; ++g)
        put32(sbix, g <= glyph ? data : data + 8 + uint32_t(image.size()));
    put16(sbix, 0);
    put16(sbix, 0);
    put32(sbix, type);
    sbix.insert(sbix.end(), image.begin(), image.end());
    // The table goes at the end of the file, and its record points there.
    while (font.size() % 4 != 0)
        font.push_back(0);
    uint32_t offset = uint32_t(font.size());
    font.insert(font.end(), sbix.begin(), sbix.end());
    uint32_t tables = uint32_t(font[4]) << 8 | font[5];
    for (uint32_t i = 0; i < tables; ++i) {
        uint8_t* record = font.data() + 12 + i * 16;
        if (std::string(reinterpret_cast<const char*>(record), 4) != "sbix")
            continue;
        for (uint32_t k = 0; k < 4; ++k) {
            record[8 + k] = uint8_t(offset >> (24 - 8 * k));
            record[12 + k] = uint8_t(sbix.size() >> (24 - 8 * k));
        }
    }
    return font;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestFontCApi() == 0);
}

TEST_CASE("the Unicode cmap subtable is the one FreeType chooses, and maps every code point as FreeType does") {
    const char* names[] = {
        "Arimo.ttf",    "BungeeSpice.ttf",     "ColrTestGlyphs.ttf", "Cousine.ttf",
        "HandwritingCff2.otf", "KalniaGlaze.ttf", "Literata.ttf",   "Nabla.ttf",
        "NotoColorEmoji.ttf",  "NotoFlagsSbix.ttf", "NotoSans.ttf",  "NotoSansArabic.ttf",
        "NotoSansDevanagari.ttf", "NotoSansJP.ttf", "RobotoFlex.ttf", "Tinos.ttf",
        "Twemoji.ttf",         "TwemojiSmileySbix.ttf",
    };
    FT_Library library = nullptr;
    REQUIRE(FT_Init_FreeType(&library) == 0);
    for (const char* name : names) {
        CAPTURE(name);
        std::vector<uint8_t> bytes = readFile(name);
        FT_Face face = nullptr;
        REQUIRE(FT_New_Memory_Face(library, bytes.data(), FT_Long(bytes.size()), 0, &face) == 0);
        REQUIRE(face->charmap != nullptr);
        Sfnt sfnt{};
        REQUIRE(sfntOpen(bytes.data(), bytes.size(), 0, &sfnt));
        SfntCmap cmap{};
        REQUIRE(sfntCmapUnicode(sfnt, &cmap));
        CHECK(cmap.format == uint16_t(FT_Get_CMap_Format(face->charmap)));
        uint32_t differ = 0;
        uint32_t first = 0;
        for (uint32_t c = 0; c <= 0x10ffff; ++c) {
            if (FT_Get_Char_Index(face, c) != sfntCmapGlyph(cmap, c) && differ++ == 0)
                first = c;
        }
        CAPTURE(first);
        CHECK(differ == 0);
        FT_Done_Face(face);
    }
    FT_Done_FreeType(library);
}

TEST_CASE("a font's design advances are FreeType's unhinted ones") {
    const char* names[] = {
        "Arimo.ttf",           "Cousine.ttf",     "HandwritingCff2.otf", "Literata.ttf",
        "NotoSans.ttf",        "NotoSansJP.ttf",  "NotoSansArabic.ttf",  "RobotoFlex.ttf",
        "Tinos.ttf",           "Twemoji.ttf",     "NotoColorEmoji.ttf",  "TwemojiSmileySbix.ttf",
    };
    FT_Library library = nullptr;
    REQUIRE(FT_Init_FreeType(&library) == 0);
    for (const char* name : names) {
        CAPTURE(name);
        std::vector<uint8_t> bytes = readFile(name);
        FT_Face face = nullptr;
        REQUIRE(FT_New_Memory_Face(library, bytes.data(), FT_Long(bytes.size()), 0, &face) == 0);
        const PrFont* font = nullptr;
        REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
        bool scalable = FT_IS_SCALABLE(face) && FT_Set_Char_Size(face, 0, FT_F26Dot6(face->units_per_EM) << 6, 72, 72) == 0;
        uint32_t differ = 0;
        uint32_t first = 0;
        for (FT_Long g = 0; scalable && g < face->num_glyphs; ++g) {
            FT_Fixed advance = 0;
            FT_Get_Advance(face, FT_UInt(g), FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT, &advance);
            float expected = float(advance) / 65536.0f / float(face->units_per_EM);
            if (prFontGlyphAdvance(font, uint16_t(g), 0) != expected && differ++ == 0)
                first = uint32_t(g);
        }
        CAPTURE(first);
        CHECK(differ == 0);
        prFontRelease(font);
        FT_Done_Face(face);
    }
    FT_Done_FreeType(library);
}

TEST_CASE("a single font has one face, and bytes that are not a font none") {
    std::vector<uint8_t> bytes = readFile("NotoSans.ttf");
    CHECK(prFontCount(bytes) == 1);
    // A collection header that counts more faces than its bytes hold.
    const uint8_t collection[] = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
    CHECK(prFontCount(collection) == 0);
    CHECK(prFontCount(PrByteView{bytes.data(), 3}) == 0);
}

TEST_CASE("a font's PostScript name is its name table's, or names the axes of an instance away from its default") {
    const PrFont* font = load("NotoSans.ttf");
    CHECK(text(prFontPostScriptName(font)) == "NotoSans-Regular");
    prFontRelease(font);
    const PrFont* flex = load("RobotoFlex.ttf");
    PrFontVariation bold[2] = {{PR_FONT_TAG('w', 'g', 'h', 't'), 700}, {PR_FONT_TAG('w', 'd', 't', 'h'), 87.5f}};
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = bold;
    derivation.variationCount = 2;
    const PrFont* instance = prFontCreateDerived(flex, &derivation);
    std::string name = text(prFontPostScriptName(instance));
    CHECK(name.rfind("RobotoFlex", 0) == 0);
    CHECK(name.find("_700wght") != std::string::npos);
    CHECK(name.find("_87.5wdth") != std::string::npos);
    prFontRelease(instance);
    prFontRelease(flex);
}

TEST_CASE("a font reads its names, metrics and glyphs") {
    const PrFont* font = load("NotoSans.ttf");
    CHECK(text(prFontFamilyName(font)) == "Noto Sans");
    PrFontMetrics m = prFontMetrics(font);
    CHECK(m.unitsPerEm == 1000);
    CHECK(m.ascent == doctest::Approx(1.069));
    CHECK(m.descent == doctest::Approx(0.293));
    CHECK(m.capHeight == doctest::Approx(0.714));
    CHECK(m.xHeight == doctest::Approx(0.536));
    CHECK(m.averageAdvance == doctest::Approx(0.570));
    CHECK(m.maxAdvance == doctest::Approx(2.840));
    CHECK((m.fromFont & PrFontMetricFlag_AverageAdvance) != 0);
    CHECK(m.underlinePosition > 0.0f);
    CHECK(m.strikeoutPosition < 0.0f);
    uint16_t h = prFontCodePointGlyph(font, 'H');
    CHECK(h != 0);
    CHECK(prFontCodePointGlyph(font, 0x10ffff) == 0);
    CHECK(prFontGlyphAdvance(font, h, 0) == doctest::Approx(0.742).epsilon(0.01));
    // H stands on the baseline and reaches up to the cap height. y is down.
    PrRect bounds = prFontGlyphBounds(font, h);
    CHECK(bounds.bottom == doctest::Approx(0));
    CHECK(bounds.top == doctest::Approx(-m.capHeight));
    PrPath* path = prPathCreate();
    CHECK(prFontGlyphOutline(font, h, 0, prAffine2Scale(PrVec2{100, 100}), path));
    CHECK(prPathBounds(path).top == doctest::Approx(-71.4));
    prPathClear(path);
    CHECK_FALSE(prFontGlyphOutline(font, prFontCodePointGlyph(font, ' '), 0, prAffine2Identity(), path));
    CHECK(prPathVerbCount(path) == 0);
    CHECK_FALSE(prFontGlyphOutline(font, 0xffff, 0, prAffine2Identity(), path));
    // Synthetic bold grows every side, and the bounds, by the given amount. Synthetic oblique is applied by the code
    // that places glyphs.
    const PrFont* bolder = derive(font, [](PrFontDerivation& d) {
        d.embolden = 0.05f;
        d.oblique = 0.2f;
    });
    CHECK(prFontEmbolden(bolder) == 0.05f);
    CHECK(prFontOblique(bolder) == 0.2f);
    prPathClear(path);
    CHECK(prFontGlyphOutline(bolder, h, 0, prAffine2Identity(), path));
    PrRect bold = prPathBounds(path);
    CHECK(bold.top == doctest::Approx(bounds.top - 0.05f).epsilon(0.01));
    CHECK(bold.left == doctest::Approx(bounds.left - 0.05f).epsilon(0.01));
    CHECK(prFontGlyphBounds(bolder, h).left == doctest::Approx(prFontGlyphBounds(font, h).left - 0.05f));
    CHECK(prFontGlyphAdvance(bolder, h, 0) == prFontGlyphAdvance(font, h, 0));
    prFontRelease(bolder);
    prPathDestroy(path);
    prFontRelease(font);
}

TEST_CASE("a variation is a font of its own, sharing the bytes") {
    const PrFont* font = load("NotoSans.ttf");
    REQUIRE(prFontAxisCount(font) == 2);
    uint32_t weight = prFontAxis(font, 0).tag == kWght ? 0 : 1;
    PrFontAxis axis = prFontAxis(font, weight);
    CHECK(axis.min == 100.0f);
    CHECK(axis.defaultValue == 400.0f);
    CHECK(axis.max == 900.0f);
    CHECK(prFontAxisValue(font, weight) == 400.0f);
    PrFontVariation heavy{kWght, 2000};
    const PrFont* black = nullptr;
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = &heavy;
    derivation.variationCount = 1;
    black = prFontCreateDerived(font, &derivation);
    CHECK(prFontAxisValue(black, weight) == 900.0f);
    CHECK(prFontId(black) != prFontId(font));
    CHECK(prFontData(black).data == prFontData(font).data);
    uint16_t o = prFontCodePointGlyph(font, 'o');
    CHECK(prFontGlyphAdvance(black, o, 0) > prFontGlyphAdvance(font, o, 0));
    PrRect regular = prFontGlyphBounds(font, o);
    PrRect bold = prFontGlyphBounds(black, o);
    CHECK(prRectWidth(bold) > prRectWidth(regular));
    prFontRelease(font);
    // The variation holds its own reference to the bytes.
    CHECK(prFontGlyphAdvance(black, o, 0) > 0.0f);
    prFontRelease(black);
}

TEST_CASE("each variation keeps its own advances once found") {
    const PrFont* font = load("NotoSans.ttf");
    uint16_t o = prFontCodePointGlyph(font, 'o');
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variationCount = 1;
    PrFontVariation light{kWght, 200};
    derivation.variations = &light;
    const PrFont* thin = prFontCreateDerived(font, &derivation);
    PrFontVariation heavy{kWght, 900};
    derivation.variations = &heavy;
    const PrFont* black = prFontCreateDerived(font, &derivation);
    float thinFirst = prFontGlyphAdvance(thin, o, 0);
    float blackFirst = prFontGlyphAdvance(black, o, 0);
    CHECK(blackFirst > thinFirst);
    CHECK(prFontGlyphAdvance(thin, o, 0) == thinFirst);
    CHECK(prFontGlyphAdvance(black, o, 0) == blackFirst);
    prFontRelease(black);
    prFontRelease(thin);
    prFontRelease(font);
}

TEST_CASE("CFF2 outlines read as cubics") {
    const PrFont* font = load("HandwritingCff2.otf");
    bool cubic = false;
    PrPath* path = prPathCreate();
    for (uint16_t g = 0; g < prFontGlyphCount(font) && !cubic; ++g) {
        prPathClear(path);
        prFontGlyphOutline(font, g, 0, prAffine2Identity(), path);
        for (uint32_t i = 0; i < prPathVerbCount(path); ++i)
            cubic = cubic || prPathVerbs(path)[i] == PrPathVerb_Cubic;
    }
    CHECK(cubic);
    prPathDestroy(path);
    prFontRelease(font);
}

TEST_CASE("color glyphs paint as balanced pushes and pops") {
    const PrFont* font = load("ColrTestGlyphs.ttf");
    CHECK(prFontPaletteCount(font) >= 1);
    PrFontPainter painter = tracer();
    uint32_t colored = 0;
    bool kinds[4] = {};
    for (uint16_t g = 0; g < prFontGlyphCount(font); ++g) {
        Trace trace;
        bool has = prFontGlyphHasColor(font, g);
        painter.context = &trace;
        CHECK(prFontGlyphPaint(font, g, nullptr, PrColor{0, 0, 0, 1}, &painter) == has);
        if (!has)
            continue;
        ++colored;
        CHECK(trace.balanced);
        CHECK(trace.depth == 0);
        for (uint32_t k = 0; k < 4; ++k) {
            const char* names[] = {"solid ", "linear ", "radial ", "conic "};
            kinds[k] = kinds[k] || trace.calls.find(names[k]) != std::string::npos;
        }
    }
    CHECK(colored > 50);
    CHECK((kinds[0] && kinds[1] && kinds[2] && kinds[3]));
    painter.context = nullptr;
    CHECK_FALSE(prFontGlyphPaint(font, 0, nullptr, PrColor{0, 0, 0, 1}, &painter));
    prFontRelease(font);
}

TEST_CASE("one state of a font's face is one font, while it is open") {
    const PrFont* font = load("NotoSans.ttf");
    PrFontVariation heavy{kWght, 700};
    PrFontDerivation bold = prFontDerivationDefault();
    bold.variations = &heavy;
    bold.variationCount = 1;
    PrFontDerivation half = prFontDerivationDefault();
    half.sizeAdjust = 0.5f;
    const PrFont* a = prFontCreateDerived(font, &bold);
    const PrFont* b = prFontCreateDerived(font, &bold);
    CHECK(a == b);
    // The state, not the path to it: the same variation reached from the derived font.
    CHECK(prFontCreateDerived(a, &bold) == a);
    prFontRelease(a);
    const PrFont* c = prFontCreateDerived(font, &half);
    CHECK(c != a);
    PrFontDerivation none = prFontDerivationDefault();
    CHECK(prFontCreateDerived(font, &none) == font);
    prFontRelease(font);
    // Fonts made by separate calls to create are separate, whatever their state.
    const PrFont* again = load("NotoSans.ttf");
    CHECK(again != font);
    prFontRelease(again);
    prFontRelease(c);
    prFontRelease(b);
    prFontRelease(a);
    prFontRelease(font);
}

TEST_CASE("threads that derive one state at once share one font" * doctest::skip(!prThreadSupported())) {
    const PrFont* font = load("NotoSans.ttf");
    std::vector<std::thread> threads;
    std::vector<const PrFont*> made(8);
    for (uint32_t t = 0; t < 8; ++t) {
        threads.emplace_back([font, t, &made] {
            PrFontVariation heavy{kWght, 600};
            PrFontDerivation d = prFontDerivationDefault();
            d.variations = &heavy;
            d.variationCount = 1;
            // Each thread makes and drops the state, then keeps it, so that a lookup meets fonts being freed.
            for (int i = 0; i < 50; ++i)
                prFontRelease(prFontCreateDerived(font, &d));
            made[t] = prFontCreateDerived(font, &d);
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    for (uint32_t t = 1; t < 8; ++t)
        CHECK(made[t] == made[0]);
    for (const PrFont* each : made)
        prFontRelease(each);
    prFontRelease(font);
}

TEST_CASE("a font is read from several threads at once" * doctest::skip(!prThreadSupported())) {
    const PrFont* font = load("NotoSans.ttf");
    std::vector<std::thread> threads;
    std::vector<float> widths(4);
    for (uint32_t t = 0; t < 4; ++t) {
        threads.emplace_back([font, t, &widths] {
            PrPath* path = prPathCreate();
            float sum = 0;
            for (uint16_t g = 1; g < 400; ++g) {
                prPathClear(path);
                prFontGlyphOutline(font, g, t == 0 ? 13.0f : 0, prAffine2Identity(), path);
                sum += prFontGlyphAdvance(font, g, 0);
            }
            widths[t] = sum;
            prPathDestroy(path);
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    CHECK(widths[0] == widths[3]);
    prFontRelease(font);
}

TEST_CASE("baselines come from BASE, and the rest are related to them") {
    const PrFont* font = load("NotoSansJP.ttf");
    PrFontMetrics m = prFontMetrics(font);
    // BASE gives ideo at -120 and the ink edges at -67 and 827. idtp is ideo plus an em.
    CHECK(m.ideographicUnder == doctest::Approx(0.12));
    CHECK(m.ideographicOver == doctest::Approx(-0.88));
    CHECK(m.ideographicInkUnder == doctest::Approx(0.067));
    CHECK(m.ideographicInkOver == doctest::Approx(-0.827));
    CHECK(m.centralBaseline == doctest::Approx(-0.38));
    CHECK(m.alphabeticBaseline == doctest::Approx(0));
    CHECK((m.fromFont & PrFontMetricFlag_Ideographic) != 0);
    CHECK((m.fromFont & PrFontMetricFlag_IdeographicInk) != 0);
    CHECK((m.fromFont & PrFontMetricFlag_Hanging) == 0);
    CHECK((m.fromFont & PrFontMetricFlag_Vertical) != 0);
    prFontRelease(font);

    // A Latin font has no BASE. Its ideographic baselines are its ascent and descent, and its central baseline midway.
    font = load("NotoSans.ttf");
    m = prFontMetrics(font);
    CHECK((m.fromFont & PrFontMetricFlag_Ideographic) == 0);
    CHECK(m.ideographicUnder == doctest::Approx(m.descent));
    CHECK(m.ideographicOver == doctest::Approx(-m.ascent));
    CHECK(m.centralBaseline == doctest::Approx((m.descent - m.ascent) / 2));
    CHECK((m.fromFont & PrFontMetricFlag_Vertical) == 0);
    CHECK(m.mathBaseline < 0.0f);
    prFontRelease(font);
}

TEST_CASE("subscripts, superscripts and heights come from OS/2, varied by MVAR") {
    const PrFont* font = load("Literata.ttf");
    PrFontMetrics m = prFontMetrics(font);
    CHECK(m.subscriptPosition == doctest::Approx(0.075));
    CHECK(m.subscriptSize == doctest::Approx(0.6));
    CHECK(m.superscriptPosition == doctest::Approx(-0.35));
    CHECK(m.superscriptSize == doctest::Approx(0.6));
    CHECK(m.xHeight == doctest::Approx(0.507));
    CHECK((m.fromFont & PrFontMetricFlag_Subscript) != 0);
    CHECK((m.fromFont & PrFontMetricFlag_XHeight) != 0);
    // The x-height moves with the optical size, and with the weight.
    PrFontVariation display{PR_FONT_TAG('o', 'p', 's', 'z'), 72};
    PrFontDerivation d = prFontDerivationDefault();
    d.variations = &display;
    d.variationCount = 1;
    const PrFont* large = derived(font, d);
    CHECK(prFontMetrics(large).xHeight == doctest::Approx(0.513).epsilon(0.002));
    PrFontVariation heavy[2] = {{PR_FONT_TAG('o', 'p', 's', 'z'), 12}, {PR_FONT_TAG('w', 'g', 'h', 't'), 900}};
    d.variations = heavy;
    d.variationCount = 2;
    const PrFont* bold = derived(font, d);
    CHECK(prFontMetrics(bold).xHeight == doctest::Approx(0.517).epsilon(0.002));
    prFontRelease(bold);
    prFontRelease(large);
    prFontRelease(font);
}

TEST_CASE("size-adjust scales outlines and metrics, and overrides replace metrics") {
    const PrFont* font = load("NotoSans.ttf");
    uint16_t x = prFontCodePointGlyph(font, 'x');
    PrFontDerivation d = prFontDerivationDefault();
    d.sizeAdjust = 0.5f;
    d.ascent = 0.9f;
    d.overrides = PrFontOverrideFlag_Ascent;
    const PrFontFeature smallCaps{PR_FONT_TAG('s', 'm', 'c', 'p'), 1};
    d.features = &smallCaps;
    d.featureCount = 1;
    const PrFont* half = derived(font, d);
    CHECK(prFontSizeAdjust(half) == 0.5f);
    CHECK(prFontGlyphAdvance(half, x, 0) == doctest::Approx(prFontGlyphAdvance(font, x, 0) / 2));
    CHECK(prFontGlyphBounds(half, x).top == doctest::Approx(prFontGlyphBounds(font, x).top / 2));
    PrFontMetrics m = prFontMetrics(half);
    CHECK(m.xHeight == doctest::Approx(prFontMetrics(font).xHeight / 2));
    // An override is in ems of the used size, which the size-adjust also scales.
    CHECK(m.ascent == doctest::Approx(0.45));
    CHECK(m.descent == doctest::Approx(prFontMetrics(font).descent / 2));
    REQUIRE(prFontFeatureCount(half) == 1);
    CHECK(prFontFeatures(half)[0].tag == smallCaps.tag);
    PrPath* path = prPathCreate();
    CHECK(prFontGlyphOutline(half, x, 0, prAffine2Identity(), path));
    CHECK(prPathBounds(path).top == doctest::Approx(prFontGlyphBounds(half, x).top));
    // A derivation of a derivation compounds.
    PrFontDerivation again = prFontDerivationDefault();
    again.sizeAdjust = 0.5f;
    const PrFont* quarter = derived(half, again);
    CHECK(prFontSizeAdjust(quarter) == 0.25f);
    CHECK(prFontFeatureCount(quarter) == 1);
    prFontRelease(quarter);
    prPathDestroy(path);
    prFontRelease(half);
    prFontRelease(font);
}

TEST_CASE("vertical metrics place an ideograph in its em box") {
    const PrFont* font = load("NotoSansJP.ttf");
    uint16_t kan = prFontCodePointGlyph(font, 0x6f22);
    CHECK(prFontGlyphVerticalAdvance(font, kan) == doctest::Approx(1));
    PrVec2 origin = prFontGlyphVerticalOrigin(font, kan);
    CHECK(origin.x == doctest::Approx(0.5));
    CHECK(origin.y == doctest::Approx(-0.88).epsilon(0.01));
    prFontRelease(font);
}

TEST_CASE("a font is aliased where a derivation says so, and its derivations stay aliased") {
    const PrFont* font = load("NotoSans.ttf");
    CHECK_FALSE(prFontIsAliased(font));
    const PrFont* aliased = derive(font, [](PrFontDerivation& d) {
        d.aliased = true;
        d.overrides = PrFontOverrideFlag_Aliased;
    });
    CHECK(prFontIsAliased(aliased));
    const PrFont* again = derive(aliased, [](PrFontDerivation&) {});
    CHECK(prFontIsAliased(again));
    prFontRelease(again);
    prFontRelease(aliased);
    prFontRelease(font);
}

TEST_CASE("Ahem is aliased however it is opened, unless a derivation says otherwise") {
    // From bytes, as the @font-face of a page loads it, and from a file, as a host installs it.
    const PrFont* bytes = load("Ahem.ttf");
    CHECK(prFontIsAliased(bytes));
    std::string path = std::string(PR_FONTS_DIR) + "/Ahem.ttf";
    const PrFont* file = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &file) == PrStatus_Ok);
    CHECK(prFontIsAliased(file));
    // An instance that a producer derives takes the setting of its base, and a derivation can turn it off.
    const PrFont* half = derive(bytes, [](PrFontDerivation& d) { d.sizeAdjust = 0.5f; });
    CHECK(prFontIsAliased(half));
    const PrFont* smooth = derive(bytes, [](PrFontDerivation& d) {
        d.aliased = false;
        d.overrides = PrFontOverrideFlag_Aliased;
    });
    CHECK_FALSE(prFontIsAliased(smooth));
    prFontRelease(smooth);
    prFontRelease(half);
    prFontRelease(file);
    prFontRelease(bytes);
}

TEST_CASE("palettes are marked for light and dark backgrounds") {
    const PrFont* font = load("ColrTestGlyphs.ttf");
    REQUIRE(prFontPaletteCount(font) == 3);
    CHECK(prFontPaletteFlags(font, 0) == 0);
    CHECK(prFontPaletteFlags(font, 1) == PrFontPaletteFlag_Dark);
    CHECK(prFontPaletteFlags(font, 2) == PrFontPaletteFlag_Light);
    CHECK(prFontPaletteSelect(font, PrFontPaletteFlag_Light) == 2);
    CHECK(prFontPaletteSelect(font, PrFontPaletteFlag_Dark) == 1);
    CHECK(prFontPaletteSelect(font, 0) == 0);
    prFontRelease(font);
}

TEST_CASE("a palette's colors are overridden entry by entry") {
    const PrFont* font = load("ColrTestGlyphs.ttf");
    // The stops of the first sweep: linen (7), blue (4), red (0), dark slate gray (8).
    Trace stops;
    PrFontPainter painter = tracer();
    painter.fill = [](void* c, const PrFontPaint* paint) {
        for (uint32_t i = 0; i < paint->stopCount; ++i)
            static_cast<Trace*>(c)->colors.push_back(paint->stops[i].color);
    };
    PrFontPaletteColor green{4, PrColor{0, 1, 0, 1}};
    PrFontPalette palette{0, &green, 1};
    painter.context = &stops;
    REQUIRE(prFontGlyphPaint(font, 12, &palette, PrColor{0, 0, 0, 1}, &painter));
    REQUIRE(stops.colors.size() == 4);
    CHECK(stops.colors[1].g == 1.0f);
    CHECK(stops.colors[1].b == 0.0f);
    CHECK(stops.colors[2].r == 1.0f);
    prFontRelease(font);
}

TEST_CASE("a face describes its weight, width and slope, as ranges where axes vary them") {
    const PrFont* font = load("NotoSans.ttf");
    PrFontInfo d = prFontInfo(font);
    CHECK(d.weightMin == 100.0f);
    CHECK(d.weightMax == 900.0f);
    CHECK(d.widthMin == 62.5f);
    CHECK(d.widthMax == 100.0f);
    CHECK(d.slope == PrFontSlope_Normal);
    prFontRelease(font);
    font = load("RobotoFlex.ttf");
    d = prFontInfo(font);
    CHECK(text(prFontFamilyName(font)) == "Roboto Flex");
    CHECK(d.obliqueMin == 0.0f);
    CHECK(d.obliqueMax == 10.0f);
    CHECK(d.opticalSizeMin == 8.0f);
    CHECK(d.opticalSizeMax == 144.0f);
    CHECK(d.widthMin == 25.0f);
    prFontRelease(font);
}

TEST_CASE("without a hinter, a font of any hinting draws and advances as designed") {
    const PrFont* font = load("NotoSans.ttf");
    uint16_t x = prFontCodePointGlyph(font, 'x');
    for (PrFontHinting hinting : {PrFontHinting_Light, PrFontHinting_Full}) {
        const PrFont* asked = hinted(font, hinting);
        CHECK(prFontHinting(asked) == PrFontHinting_None);
        CHECK(prFontHinter(asked) == nullptr);
        CHECK(prFontGlyphAdvance(asked, x, 11) == prFontGlyphAdvance(font, x, 0));
        PrPath* fitted = prPathCreate();
        PrPath* design = prPathCreate();
        REQUIRE(prFontGlyphOutline(asked, x, 13, prAffine2Scale(PrVec2{13, 13}), fitted));
        REQUIRE(prFontGlyphOutline(font, x, 0, prAffine2Scale(PrVec2{13, 13}), design));
        CHECK(prPathBounds(fitted).top == prPathBounds(design).top);
        prPathDestroy(design);
        prPathDestroy(fitted);
        prFontRelease(asked);
    }
    prFontRelease(font);
}

TEST_CASE("a font made with a derivation is the font derived from its default instance") {
    std::vector<uint8_t> bytes = readFile("NotoSans.ttf");
    PrFontVariation heavy{PR_FONT_TAG('w', 'g', 'h', 't'), 800};
    PrFontDerivation d = prFontDerivationDefault();
    d.variations = &heavy;
    d.variationCount = 1;
    d.sizeAdjust = 0.9f;
    d.embolden = 0.01f;
    const PrFont* direct = nullptr;
    REQUIRE(prFontCreate(bytes, 0, &d, &direct) == PrStatus_Ok);
    const PrFont* plain = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &plain) == PrStatus_Ok);
    const PrFont* derived = prFontCreateDerived(plain, &d);
    CHECK(prFontAxisValue(direct, 0) == 800.0f);
    CHECK(prFontAxisValue(direct, 0) == prFontAxisValue(derived, 0));
    CHECK(prFontSizeAdjust(direct) == prFontSizeAdjust(derived));
    CHECK(prFontEmbolden(direct) == prFontEmbolden(derived));
    CHECK(prFontMetrics(direct).xHeight == prFontMetrics(derived).xHeight);
    uint16_t o = prFontCodePointGlyph(direct, 'o');
    CHECK(prFontGlyphAdvance(direct, o, 0) == prFontGlyphAdvance(derived, o, 0));
    prFontRelease(derived);
    prFontRelease(plain);
    prFontRelease(direct);
}

TEST_CASE("an sbix glyph's image goes where its origin offset puts it") {
    const PrFont* font = load("TwemojiSmileySbix.ttf");
    PrFontGlyphImage image{};
    REQUIRE(prFontGlyphImage(font, 2, 20, &image));
    CHECK(image.pixelsPerEm == 109.0f);
    CHECK(image.extent.x == 128);
    CHECK(image.extent.y == 128);
    REQUIRE(image.bytes.size > 8);
    CHECK(image.bytes.data[1] == 'P');
    CHECK(std::string(image.type.data, image.type.size) == "image/png");
    // The bottom left corner 4 pixels right of the origin and 27 below the baseline.
    CHECK(image.bounds.left == doctest::Approx(4.0 / 109));
    CHECK(image.bounds.bottom == doctest::Approx(27.0 / 109));
    CHECK(image.bounds.top == doctest::Approx((27.0 - 128) / 109));
    CHECK_FALSE(prFontGlyphImage(font, 0, 20, &image));
    // A size-adjust scales the placement of the image.
    const PrFont* half = derive(font, [](PrFontDerivation& d) { d.sizeAdjust = 0.5f; });
    REQUIRE(prFontGlyphImage(half, 2, 20, &image));
    CHECK(image.bounds.left == doctest::Approx(2.0 / 109));
    prFontRelease(half);
    prFontRelease(font);
}

TEST_CASE("an sbix strike's JPEG and TIFF images are sized from their headers") {
    std::vector<uint8_t> file = readFile("TwemojiSmileySbix.ttf");
    const PrFont* original = nullptr;
    REQUIRE(prFontCreate(file, 0, nullptr, &original) == PrStatus_Ok);
    uint16_t glyphs = uint16_t(prFontGlyphCount(original));
    prFontRelease(original);
    // The start of image and the frame header of a JPEG, 64 wide and 48 tall.
    std::vector<uint8_t> jpeg = {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x04, 0x00, 0x00, 0xff, 0xc0, 0x00, 0x0b,
        0x08, 0x00, 0x30, 0x00, 0x40, 0x01, 0x01, 0x11, 0x00,
    };
    // The header of a little-endian TIFF and a directory of its width, 20, and length, 10.
    std::vector<uint8_t> tiff = {
        'I', 'I', 42, 0, 8, 0, 0, 0, 2, 0,
        0, 1, 3, 0, 1, 0, 0, 0, 20, 0, 0, 0,
        1, 1, 4, 0, 1, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0,
    };
    struct Case {
        uint32_t type;
        const std::vector<uint8_t>& bytes;
        const char* mime;
        uint32_t width;
        uint32_t height;
    };
    for (const Case& c : {Case{0x6a706720, jpeg, "image/jpeg", 64, 48}, Case{0x74696666, tiff, "image/tiff", 20, 10}}) {
        std::vector<uint8_t> bytes = withSbixImage(file, 2, glyphs, c.type, c.bytes, 100);
        const PrFont* font = nullptr;
        REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
        PrFontGlyphImage image{};
        REQUIRE(prFontGlyphImage(font, 2, 100, &image));
        CHECK(std::string(image.type.data, image.type.size) == c.mime);
        CHECK(image.bytes.size == c.bytes.size());
        CHECK(image.extent.x == c.width);
        CHECK(image.extent.y == c.height);
        CHECK(image.bounds.top == doctest::Approx(-float(c.height) / 100));
        CHECK_FALSE(prFontGlyphImage(font, 3, 100, &image));
        prFontRelease(font);
    }
}

TEST_CASE("a CBDT glyph's image comes from the strike best for the size") {
    const PrFont* font = load("NotoColorEmojiCbdt.ttf");
    uint16_t nine = prFontCodePointGlyph(font, '9');
    REQUIRE(nine != 0);
    PrFontGlyphImage image{};
    REQUIRE(prFontGlyphImage(font, nine, 50, &image));
    CHECK(image.pixelsPerEm == 109.0f);
    CHECK(image.bytes.data[1] == 'P');
    REQUIRE(prFontGlyphImage(font, nine, 150, &image));
    CHECK(image.pixelsPerEm == 210.0f);
    REQUIRE(prFontGlyphImage(font, nine, 400, &image));
    CHECK(image.pixelsPerEm == 210.0f);
    // Whether or not the image is wider than tall, its extent in ems is its pixels over the pixels per em of the
    // strike.
    CHECK(prRectWidth(image.bounds) == doctest::Approx(float(image.extent.x) / 210));
    CHECK(image.bounds.top < 0.0f);
    CHECK_FALSE(prFontGlyphHasColor(font, nine));
    prFontRelease(font);
}

TEST_CASE("a CBDT image whose length runs past its table has no image") {
    std::vector<uint8_t> bytes = readFile("NotoColorEmojiCbdt.ttf");
    // The PNG of each image follows its length. A length that wraps past 2^32 when added to its position fails.
    const uint8_t signature[] = {0x89, 'P', 'N', 'G'};
    uint32_t corrupted = 0;
    for (size_t i = 4; i + 4 <= bytes.size(); ++i) {
        if (std::equal(signature, signature + 4, bytes.begin() + ptrdiff_t(i))) {
            bytes[i - 4] = bytes[i - 3] = bytes[i - 2] = 0xff;
            bytes[i - 1] = 0xf8;
            ++corrupted;
        }
    }
    REQUIRE(corrupted != 0);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    PrFontGlyphImage image{};
    CHECK_FALSE(prFontGlyphImage(font, prFontCodePointGlyph(font, '9'), 50, &image));
    prFontRelease(font);
}
