#include "pr/array.h"
#include "pr/font.h"
#include "pr/mem.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/vec.h"

#include "font/src/sfnt.h"

#include "freetype/freetype.h"
#include "freetype/ftadvanc.h"
#include "freetype/tttables.h"
#include "tools/harness/test.h"

extern "C" int prTestFontCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

// Returns the bytes of the font file `name`, in the fonts directory.
Array<uint8_t> fontFile(PrStringView name) {
    return harness::readFile(format("{}/{}", PR_FONTS_DIR, name));
}

Array<uint8_t> newCmMathBytes() {
    return fontFile("NewCMMath-Regular.otf");
}

const PrFont* fromBytes(const Array<uint8_t>& bytes) {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Returns the byte offset in the file of the Unicode Variation Sequences subtable (cmap format 14) of its first face.
uint32_t sequencesOffset(const Array<uint8_t>& bytes) {
    Sfnt sfnt{};
    REQUIRE(sfntOpen(bytes.data(), bytes.count(), 0, &sfnt));
    SfntTable cmap = sfntTable(sfnt, PR_FONT_TAG('c', 'm', 'a', 'p'));
    for (uint32_t i = 0; i < cmap.uint16At(2); ++i) {
        uint32_t record = 4 + i * 8;
        SfntTable subtable = cmap.from(cmap.uint32At(record + 4));
        if (cmap.uint16At(record) == 0 && cmap.uint16At(record + 2) == 5 && subtable.uint16At(0) == 14)
            return uint32_t(subtable.data - bytes.data());
    }
    FAIL("the font has no format 14 subtable");
    return 0;
}

void poke24(Array<uint8_t>& bytes, uint32_t at, uint32_t value) {
    bytes[at] = uint8_t(value >> 16);
    bytes[at + 1] = uint8_t(value >> 8);
    bytes[at + 2] = uint8_t(value);
}

void poke32(Array<uint8_t>& bytes, uint32_t at, uint32_t value) {
    bytes[at] = uint8_t(value >> 24);
    poke24(bytes, at + 1, value & 0xffffff);
}

uint32_t peek24(const Array<uint8_t>& bytes, uint32_t at) {
    return uint32_t(bytes[at]) << 16 | uint32_t(bytes[at + 1]) << 8 | bytes[at + 2];
}

uint32_t peek32(const Array<uint8_t>& bytes, uint32_t at) {
    return uint32_t(bytes[at]) << 24 | peek24(bytes, at + 1);
}

const PrFont* load(const char* name) {
    Array<uint8_t> bytes = fontFile(name);
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

String text(PrRawString string) {
    PrStringView view = prRawStringView(&string);
    String result(view.size != 0 ? view : PrStringView(""));
    prRawStringReset(&string);
    return result;
}

constexpr uint32_t kWght = PR_FONT_TAG('w', 'g', 'h', 't');

// A painter that writes each call as a word.
struct Trace {
    String calls;
    Array<PrColor> colors;
    uint32_t depth = 0;
    uint32_t deepest = 0;
    bool balanced = true;
};

void push(Trace& t, const char* word) {
    t.calls.append(word);
    t.calls.append(' ');
    if (++t.depth > t.deepest)
        t.deepest = t.depth;
}

void pop(Trace& t, const char* word) {
    t.calls.append(word);
    t.calls.append(' ');
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
        t.calls.append(kinds[paint->kind]);
        t.calls.append(' ');
    };
    return p;
}

const PrFont* derived(const PrFont* font, const PrFontDerivation& derivation) {
    const PrFont* out = prFontCreateDerived(font, &derivation);
    return out;
}

void put16(Array<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value >> 8));
    out.push(uint8_t(value));
}

void put32(Array<uint8_t>& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value & 0xffff);
}

// Returns the font file with its sbix table replaced by one strike of `ppem` pixels per em, in which glyph `glyph` is
// `image` of the sbix graphic type `type`, with its bottom left corner at the origin.
Array<uint8_t> withSbixImage(Array<uint8_t> font, uint16_t glyph, uint16_t glyphCount, uint32_t type,
        const Array<uint8_t>& image, uint16_t ppem) {
    Array<uint8_t> sbix;
    put16(sbix, 1);
    put16(sbix, 1);
    put32(sbix, 1);
    put32(sbix, 12);
    uint32_t data = 4 + (uint32_t(glyphCount) + 1) * 4;
    put16(sbix, ppem);
    put16(sbix, 72);
    for (uint32_t g = 0; g <= glyphCount; ++g)
        put32(sbix, g <= glyph ? data : data + 8 + image.count());
    put16(sbix, 0);
    put16(sbix, 0);
    put32(sbix, type);
    sbix.append(image.span());
    // The table goes at the end of the file, and its record points there.
    while (font.count() % 4 != 0)
        font.push(0);
    uint32_t offset = font.count();
    font.append(sbix.span());
    uint32_t tables = uint32_t(font[4]) << 8 | font[5];
    for (uint32_t i = 0; i < tables; ++i) {
        uint8_t* record = font.data() + 12 + i * 16;
        if (PrStringView(reinterpret_cast<const char*>(record), 4) != "sbix")
            continue;
        for (uint32_t k = 0; k < 4; ++k) {
            record[8 + k] = uint8_t(offset >> (24 - 8 * k));
            record[12 + k] = uint8_t(sbix.count() >> (24 - 8 * k));
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
        "Arimo-Regular.ttf",    "BungeeSpice.ttf",     "ColrTestGlyphs.ttf", "Cousine-Regular.ttf",
        "noto_handwriting-cff2_colr_1.otf", "KalniaGlaze.ttf", "Literata.ttf",   "Nabla.ttf",
        "NotoColorEmoji.subset.multiple_size_tables.ttf",  "NotoFlagsSbix.ttf", "NotoSans[wdth,wght].ttf",  "NotoSansArabic[wdth,wght].ttf",
        "NotoSansDevanagari[wdth,wght].ttf", "NotoSansJP-VF.otf", "RobotoFlex.ttf", "Tinos-Regular.ttf",
        "Twemoji.ttf",         "TwemojiSmileySbix.ttf",
    };
    FT_Library library = nullptr;
    REQUIRE(FT_Init_FreeType(&library) == 0);
    for (const char* name : names) {
        CAPTURE(name);
        Array<uint8_t> bytes = fontFile(name);
        FT_Face face = nullptr;
        REQUIRE(FT_New_Memory_Face(library, bytes.data(), FT_Long(bytes.count()), 0, &face) == 0);
        REQUIRE(face->charmap != nullptr);
        Sfnt sfnt{};
        REQUIRE(sfntOpen(bytes.data(), bytes.count(), 0, &sfnt));
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

TEST_CASE("variation sequences map as FreeType maps them") {
    const char* const names[] = {
        "NewCMMath-Regular.otf", "NotoSansJP-VF.otf", "cmap14_font1.otf", "NotoSansMongolian-Regular.ttf",
        "NotoColorEmoji.subset.multiple_size_tables.ttf", "NotoSansMath-Regular.ttf", "NotoSans[wdth,wght].ttf",
    };
    FT_Library library = nullptr;
    REQUIRE(FT_Init_FreeType(&library) == 0);
    uint32_t listed = 0;
    for (const char* name : names) {
        String path = format("{}/{}", PR_FONTS_DIR, name);
        CAPTURE(path);
        Array<uint8_t> bytes = harness::readFile(path);
        FT_Face face = nullptr;
        REQUIRE(FT_New_Memory_Face(library, bytes.data(), FT_Long(bytes.count()), 0, &face) == 0);
        const PrFont* font = fromBytes(bytes);
        uint32_t differ = 0;
        uint32_t firstBase = 0;
        uint32_t firstSelector = 0;
        auto check = [&](uint32_t base, uint32_t selector) {
            int isDefault = FT_Face_GetCharVariantIsDefault(face, base, selector);
            FT_UInt glyph = FT_Face_GetCharVariantIndex(face, base, selector);
            PrFontSequenceMapping expected = PrFontSequenceMapping_None;
            if (isDefault == 1 && glyph != 0)
                expected = PrFontSequenceMapping_Default;
            else if (isDefault == 0 && glyph != 0)
                expected = PrFontSequenceMapping_Variant;
            uint16_t expectedGlyph = expected != PrFontSequenceMapping_None ? uint16_t(glyph) : 0;
            PrFontSequenceGlyph found = prFontVariationSequenceGlyph(font, base, selector);
            if ((found.mapping != expected || found.glyph != expectedGlyph) && differ++ == 0) {
                firstBase = base;
                firstSelector = selector;
            }
        };
        // FreeType returns each list in a buffer of the face's, which its next list reuses, so the selectors are copied
        // before their code points are asked for.
        Array<FT_UInt32> selectors;
        for (FT_UInt32* at = FT_Face_GetVariantSelectors(face); at != nullptr && *at != 0; ++at)
            selectors.push(*at);
        for (uint32_t s = 0; s < selectors.count(); ++s) {
            FT_UInt32* bases = FT_Face_GetCharsOfVariant(face, selectors[s]);
            for (uint32_t b = 0; bases != nullptr && bases[b] != 0; ++b, ++listed) {
                // The code points beside a listed one, which the font may not list.
                check(bases[b], selectors[s]);
                check(bases[b] + 1, selectors[s]);
                check(bases[b] - 1, selectors[s]);
            }
            check('A', selectors[s]);
        }
        check('A', 0xFE00);
        CAPTURE(firstBase);
        CAPTURE(firstSelector);
        CHECK(differ == 0);
        prFontRelease(font);
        FT_Done_Face(face);
    }
    CHECK(listed > 0);
    FT_Done_FreeType(library);
}

// New Computer Modern Math gives the script capitals a roundhand form after U+FE01, and the empty set a variant form
// after U+FE00.
TEST_CASE("a variation sequence maps to its own glyph, its base's glyph, or nothing") {
    const PrFont* font = fromBytes(newCmMathBytes());
    uint16_t script = prFontCodePointGlyph(font, 0x1D49C);
    REQUIRE(script != 0);
    PrFontSequenceGlyph roundhand = prFontVariationSequenceGlyph(font, 0x1D49C, 0xFE01);
    CHECK(roundhand.mapping == PrFontSequenceMapping_Variant);
    CHECK(roundhand.glyph != 0);
    CHECK(roundhand.glyph != script);
    PrFontSequenceGlyph emptySet = prFontVariationSequenceGlyph(font, 0x2205, 0xFE00);
    CHECK(emptySet.mapping == PrFontSequenceMapping_Variant);
    CHECK(emptySet.glyph != 0);
    CHECK(emptySet.glyph != prFontCodePointGlyph(font, 0x2205));
    // A selector that the font lists for other characters, and selectors that it lists for none.
    CHECK(prFontVariationSequenceGlyph(font, 'A', 0xFE01).mapping == PrFontSequenceMapping_None);
    CHECK(prFontVariationSequenceGlyph(font, 0x1D49C, 0xE01EF).mapping == PrFontSequenceMapping_None);
    CHECK(prFontVariationSequenceGlyph(font, 0x1D49C, 0x10FFFF).glyph == 0);
    prFontRelease(font);
}

// Each malformation leaves the font open, with its character map, and makes the subtable map no sequence.
TEST_CASE("a malformed variation sequences subtable maps no sequence") {
    Array<uint8_t> original = newCmMathBytes();
    uint32_t at = sequencesOffset(original);
    uint32_t length = peek32(original, at + 2);
    REQUIRE(peek32(original, at + 6) >= 2);
    // The first variant list of the subtable with more than one variant.
    uint32_t variants = 0;
    for (uint32_t i = 0; i < peek32(original, at + 6) && variants == 0; ++i) {
        uint32_t list = peek32(original, at + 10 + i * 11 + 7);
        if (list != 0 && peek32(original, at + list) >= 2)
            variants = list;
    }
    REQUIRE(variants != 0);
    struct Malformation {
        const char* name;
        void (*apply)(Array<uint8_t>& bytes, uint32_t at, uint32_t length, uint32_t variants);
    };
    const Malformation malformations[] = {
        {
            "a length past the table",
            [](Array<uint8_t>& b, uint32_t at, uint32_t length, uint32_t) {
                poke32(b, at + 2, length + 0x100000);
            },
        },
        {
            "a length short of the header",
            [](Array<uint8_t>& b, uint32_t at, uint32_t, uint32_t) {
                poke32(b, at + 2, 9);
            },
        },
        {
            "more records than the length holds",
            [](Array<uint8_t>& b, uint32_t at, uint32_t length, uint32_t) {
                poke32(b, at + 6, length / 11);
            },
        },
        {
            "records out of order",
            [](Array<uint8_t>& b, uint32_t at, uint32_t, uint32_t) {
                poke24(b, at + 10 + 11, peek24(b, at + 10));
            },
        },
        {
            "a selector past U+10FFFF",
            [](Array<uint8_t>& b, uint32_t at, uint32_t, uint32_t) {
                poke24(b, at + 10, 0x110000);
            },
        },
        {
            "a list past the length",
            [](Array<uint8_t>& b, uint32_t at, uint32_t length, uint32_t) {
                poke32(b, at + 10 + 7, length - 2);
            },
        },
        {
            "more variants than the length holds",
            [](Array<uint8_t>& b, uint32_t at, uint32_t length, uint32_t variants) {
                poke32(b, at + variants, length / 5);
            },
        },
        {
            "variants out of order",
            [](Array<uint8_t>& b, uint32_t at, uint32_t, uint32_t variants) {
                poke24(b, at + variants + 4 + 5, peek24(b, at + variants + 4));
            },
        },
    };
    for (const Malformation& m : malformations) {
        CAPTURE(m.name);
        Array<uint8_t> bytes = original;
        m.apply(bytes, at, length, variants);
        const PrFont* font = fromBytes(bytes);
        CHECK(prFontCodePointGlyph(font, 0x1D49C) != 0);
        CHECK(prFontVariationSequenceGlyph(font, 0x1D49C, 0xFE01).mapping == PrFontSequenceMapping_None);
        CHECK(prFontVariationSequenceGlyph(font, 0x2205, 0xFE00).mapping == PrFontSequenceMapping_None);
        prFontRelease(font);
    }
}

// A file of one cmap table, whose one subtable is a format 14 subtable of one record: U+FE00, with a default range that
// ends at U+10FFFF and one variant.
TEST_CASE("a variation sequences subtable at the limits of its records maps what it lists") {
    Array<uint8_t> sub;
    put16(sub, 14);
    put32(sub, 0);                      // the length, set below
    put32(sub, 1);
    sub.push(0x00);
    put16(sub, 0xFE00);
    put32(sub, 21);
    put32(sub, 29);
    put32(sub, 1);                      // the default list: U+10FFF0 to U+10FFFF
    sub.push(0x10);
    put16(sub, 0xFFF0);
    sub.push(15);
    put32(sub, 1);                      // the variant list: A as glyph 7
    sub.push(0x00);
    put16(sub, 'A');
    put16(sub, 7);
    poke32(sub, 2, sub.count());
    Array<uint8_t> file;
    put32(file, 0x00010000);
    put16(file, 1);
    put16(file, 16);
    put16(file, 0);
    put16(file, 0);
    put32(file, PR_FONT_TAG('c', 'm', 'a', 'p'));
    put32(file, 0);
    put32(file, 28);
    put32(file, 12 + sub.count());
    put16(file, 0);
    put16(file, 1);
    put16(file, 0);
    put16(file, 5);
    put32(file, 12);
    file.append(sub.span());
    constexpr uint32_t kSub = 28 + 12;
    Sfnt sfnt{};
    REQUIRE(sfntOpen(file.data(), file.count(), 0, &sfnt));
    SfntCmapSequences sequences{};
    REQUIRE(sfntCmapSequencesOpen(sfnt, &sequences));
    CHECK(sfntCmapSequenceGlyph(sequences, 0x10FFFF, 0xFE00).mapping == PrFontSequenceMapping_Default);
    CHECK(sfntCmapSequenceGlyph(sequences, 0x10FFF0, 0xFE00).mapping == PrFontSequenceMapping_Default);
    CHECK(sfntCmapSequenceGlyph(sequences, 0x10FFEF, 0xFE00).mapping == PrFontSequenceMapping_None);
    PrFontSequenceGlyph a = sfntCmapSequenceGlyph(sequences, 'A', 0xFE00);
    CHECK(a.mapping == PrFontSequenceMapping_Variant);
    CHECK(a.glyph == 7);
    CHECK(sfntCmapSequenceGlyph(sequences, 'A', 0xFE01).mapping == PrFontSequenceMapping_None);
    CHECK(sfntCmapSequenceGlyph(sequences, 'B', 0xFE00).mapping == PrFontSequenceMapping_None);
    // A default range past U+10FFFF is malformed, and so is a subtable one byte short of its last list.
    Array<uint8_t> past = file;
    past[kSub + 25 + 3] = 16;
    REQUIRE(sfntOpen(past.data(), past.count(), 0, &sfnt));
    CHECK_FALSE(sfntCmapSequencesOpen(sfnt, &sequences));
    Array<uint8_t> shorter = file;
    poke32(shorter, kSub + 2, sub.count() - 1);
    REQUIRE(sfntOpen(shorter.data(), shorter.count(), 0, &sfnt));
    CHECK_FALSE(sfntCmapSequencesOpen(sfnt, &sequences));
    CHECK(sequences.subtable.data == nullptr);
}

TEST_CASE("a font's design advances are FreeType's unhinted ones") {
    const char* names[] = {
        "Arimo-Regular.ttf",           "Cousine-Regular.ttf",     "noto_handwriting-cff2_colr_1.otf", "Literata.ttf",
        "NotoSans[wdth,wght].ttf",        "NotoSansJP-VF.otf",  "NotoSansArabic[wdth,wght].ttf",  "RobotoFlex.ttf",
        "Tinos-Regular.ttf",           "Twemoji.ttf",     "NotoColorEmoji.subset.multiple_size_tables.ttf",  "TwemojiSmileySbix.ttf",
    };
    FT_Library library = nullptr;
    REQUIRE(FT_Init_FreeType(&library) == 0);
    for (const char* name : names) {
        CAPTURE(name);
        Array<uint8_t> bytes = fontFile(name);
        FT_Face face = nullptr;
        REQUIRE(FT_New_Memory_Face(library, bytes.data(), FT_Long(bytes.count()), 0, &face) == 0);
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
    Array<uint8_t> bytes = fontFile("NotoSans[wdth,wght].ttf");
    CHECK(prFontCount(bytes) == 1);
    // A collection header that counts more faces than its bytes hold.
    const uint8_t collection[] = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
    CHECK(prFontCount(collection) == 0);
    CHECK(prFontCount(PrByteView{bytes.data(), 3}) == 0);
}

TEST_CASE("a font's PostScript name is its name table's, or names the axes of an instance away from its default") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    CHECK(text(prFontPostScriptName(font)) == "NotoSans-Regular");
    prFontRelease(font);
    const PrFont* flex = load("RobotoFlex.ttf");
    PrFontVariation bold[2] = {{PR_FONT_TAG('w', 'g', 'h', 't'), 700}, {PR_FONT_TAG('w', 'd', 't', 'h'), 87.5f}};
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = bold;
    derivation.variationCount = 2;
    const PrFont* instance = prFontCreateDerived(flex, &derivation);
    String name = text(prFontPostScriptName(instance));
    CHECK(prStringStartsWith(name, "RobotoFlex"));
    CHECK(prStringContains(name, "_700wght"));
    CHECK(prStringContains(name, "_87.5wdth"));
    prFontRelease(instance);
    prFontRelease(flex);
}

TEST_CASE("a font reads its names, metrics and glyphs") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    CHECK(text(prFontFamilyName(font)) == "Noto Sans");
    PrFontMetrics m = prFontMetrics(font);
    CHECK(m.unitsPerEm == 1000);
    CHECK(m.ascent == doctest::Approx(1.069));
    CHECK(m.descent == doctest::Approx(0.293));
    CHECK(m.capHeight == doctest::Approx(0.714));
    CHECK(m.xHeight == doctest::Approx(0.536));
    CHECK(m.averageAdvance == doctest::Approx(0.577));
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
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
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
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
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
    const PrFont* font = load("noto_handwriting-cff2_colr_1.otf");
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
            kinds[k] = kinds[k] || prStringContains(trace.calls, names[k]);
        }
    }
    CHECK(colored > 50);
    CHECK((kinds[0] && kinds[1] && kinds[2] && kinds[3]));
    painter.context = nullptr;
    CHECK_FALSE(prFontGlyphPaint(font, 0, nullptr, PrColor{0, 0, 0, 1}, &painter));
    prFontRelease(font);
}

TEST_CASE("one state of a font's face is one font, while it is open") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
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
    const PrFont* again = load("NotoSans[wdth,wght].ttf");
    CHECK(again != font);
    prFontRelease(again);
    prFontRelease(c);
    prFontRelease(b);
    prFontRelease(a);
    prFontRelease(font);
}

TEST_CASE("threads that derive one state at once share one font" * doctest::skip(!prThreadSupported())) {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    // What one thread derives from, and the font that it keeps.
    struct Deriving {
        const PrFont* font;
        const PrFont* PR_NULLABLE made;
    };
    PrThread threads[8];
    Deriving work[8];
    const PrFont* made[8] = {};
    for (uint32_t t = 0; t < 8; ++t) {
        work[t] = Deriving{font, nullptr};
        prThreadStart(&threads[t], [](void* PR_NULLABLE context) {
            Deriving& deriving = *static_cast<Deriving*>(context);
            PrFontVariation heavy{kWght, 600};
            PrFontDerivation d = prFontDerivationDefault();
            d.variations = &heavy;
            d.variationCount = 1;
            // Each thread makes and drops the state, then keeps it, so that a lookup meets fonts being freed.
            for (int i = 0; i < 50; ++i)
                prFontRelease(prFontCreateDerived(deriving.font, &d));
            deriving.made = prFontCreateDerived(deriving.font, &d);
        }, &work[t]);
    }
    for (uint32_t t = 0; t < 8; ++t) {
        prThreadJoin(&threads[t]);
        made[t] = work[t].made;
    }
    for (uint32_t t = 1; t < 8; ++t)
        CHECK(made[t] == made[0]);
    for (const PrFont* each : made)
        prFontRelease(each);
    prFontRelease(font);
}

TEST_CASE("a font is read from several threads at once" * doctest::skip(!prThreadSupported())) {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    // What one thread reads, at what size, and the sum of the advances that it read.
    struct Reading {
        const PrFont* font;
        float size;
        float width;
    };
    PrThread threads[4];
    Reading work[4];
    float widths[4] = {};
    for (uint32_t t = 0; t < 4; ++t) {
        work[t] = Reading{font, t == 0 ? 13.0f : 0, 0};
        prThreadStart(&threads[t], [](void* PR_NULLABLE context) {
            Reading& reading = *static_cast<Reading*>(context);
            PrPath* path = prPathCreate();
            float sum = 0;
            for (uint16_t g = 1; g < 400; ++g) {
                prPathClear(path);
                prFontGlyphOutline(reading.font, g, reading.size, prAffine2Identity(), path);
                sum += prFontGlyphAdvance(reading.font, g, 0);
            }
            reading.width = sum;
            prPathDestroy(path);
        }, &work[t]);
    }
    for (uint32_t t = 0; t < 4; ++t) {
        prThreadJoin(&threads[t]);
        widths[t] = work[t].width;
    }
    CHECK(widths[0] == widths[3]);
    prFontRelease(font);
}

TEST_CASE("baselines come from BASE, and the rest are related to them") {
    const PrFont* font = load("NotoSansJP-VF.otf");
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
    font = load("NotoSans[wdth,wght].ttf");
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
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
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
    const PrFont* font = load("NotoSansJP-VF.otf");
    uint16_t kan = prFontCodePointGlyph(font, 0x6f22);
    CHECK(prFontGlyphVerticalAdvance(font, kan) == doctest::Approx(1));
    PrVec2 origin = prFontGlyphVerticalOrigin(font, kan);
    CHECK(origin.x == doctest::Approx(0.5));
    CHECK(origin.y == doctest::Approx(-0.88).epsilon(0.01));
    prFontRelease(font);
}

TEST_CASE("a font is aliased where a derivation says so, and its derivations stay aliased") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
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
    String path = format("{}/Ahem.ttf", PR_FONTS_DIR);
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
            static_cast<Trace*>(c)->colors.push(paint->stops[i].color);
    };
    PrFontPaletteColor green{4, PrColor{0, 1, 0, 1}};
    PrFontPalette palette{0, &green, 1};
    painter.context = &stops;
    REQUIRE(prFontGlyphPaint(font, 12, &palette, PrColor{0, 0, 0, 1}, &painter));
    REQUIRE(stops.colors.count() == 4);
    CHECK(stops.colors[1].g == 1.0f);
    CHECK(stops.colors[1].b == 0.0f);
    CHECK(stops.colors[2].r == 1.0f);
    prFontRelease(font);
}

TEST_CASE("a face describes its weight, width and slope, as ranges where axes vary them") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
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
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    uint16_t x = prFontCodePointGlyph(font, 'x');
    const PrFontHinting hintings[] = {PrFontHinting_Light, PrFontHinting_Full};
    for (PrFontHinting hinting : hintings) {
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
    Array<uint8_t> bytes = fontFile("NotoSans[wdth,wght].ttf");
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
    CHECK(image.type == "image/png");
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
    Array<uint8_t> file = fontFile("TwemojiSmileySbix.ttf");
    const PrFont* original = nullptr;
    REQUIRE(prFontCreate(file, 0, nullptr, &original) == PrStatus_Ok);
    uint16_t glyphs = uint16_t(prFontGlyphCount(original));
    prFontRelease(original);
    // The start of image and the frame header of a JPEG, 64 wide and 48 tall.
    Array<uint8_t> jpeg({
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x04, 0x00, 0x00, 0xff, 0xc0, 0x00, 0x0b,
        0x08, 0x00, 0x30, 0x00, 0x40, 0x01, 0x01, 0x11, 0x00,
    });
    // The header of a little-endian TIFF and a directory of its width, 20, and length, 10.
    Array<uint8_t> tiff({
        'I', 'I', 42, 0, 8, 0, 0, 0, 2, 0,
        0, 1, 3, 0, 1, 0, 0, 0, 20, 0, 0, 0,
        1, 1, 4, 0, 1, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0,
    });
    struct Case {
        uint32_t type;
        const Array<uint8_t>& bytes;
        const char* mime;
        uint32_t width;
        uint32_t height;
    };
    const Case cases[] = {{0x6a706720, jpeg, "image/jpeg", 64, 48}, {0x74696666, tiff, "image/tiff", 20, 10}};
    for (const Case& c : cases) {
        Array<uint8_t> bytes = withSbixImage(file, 2, glyphs, c.type, c.bytes, 100);
        const PrFont* font = nullptr;
        REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
        PrFontGlyphImage image{};
        REQUIRE(prFontGlyphImage(font, 2, 100, &image));
        CHECK(image.type == c.mime);
        CHECK(image.bytes.size == c.bytes.count());
        CHECK(image.extent.x == c.width);
        CHECK(image.extent.y == c.height);
        CHECK(image.bounds.top == doctest::Approx(-float(c.height) / 100));
        CHECK_FALSE(prFontGlyphImage(font, 3, 100, &image));
        prFontRelease(font);
    }
}

TEST_CASE("a CBDT glyph's image comes from the strike best for the size") {
    const PrFont* font = load("NotoColorEmoji.subset.multiple_size_tables.ttf");
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
    Array<uint8_t> bytes = fontFile("NotoColorEmoji.subset.multiple_size_tables.ttf");
    // The PNG of each image follows its length. A length that wraps past 2^32 when added to its position fails.
    const uint8_t signature[] = {0x89, 'P', 'N', 'G'};
    uint32_t corrupted = 0;
    for (uint32_t i = 4; i + 4 <= bytes.count(); ++i) {
        if (prMemEquals(signature, bytes.data() + i, 4)) {
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
