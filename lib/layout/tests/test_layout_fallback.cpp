// Font fallback: a cluster that no family named by its style serves is set in an installed family, chosen by its
// script, its language and its presentation, or in Last Resort if no family serves it.

#include "pr/locale_common.h"

#include "layout/tests/layout_fixture.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Adds the test font `file` to `f`'s set.
void install(Fixture& f, const char* file) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + file;
    const PrFont* PR_NULLABLE font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    prFontSetAdd(f.fonts, &descriptor, font);
    prFontRelease(font);
}

// The locales that the CJK families are made for, as the bundles give them: the writing systems they draw.
constexpr PrLocale kHans = prLocaleMake(PrLanguage_None, PrScript_Hans, PrRegion_None);
constexpr PrLocale kHant = prLocaleMake(PrLanguage_None, PrScript_Hant, PrRegion_None);
constexpr PrLocale kHantHongKong = prLocaleMake(PrLanguage_None, PrScript_Hant, PrRegion_HK);
constexpr PrLocale kJapanese = prLocaleMake(PrLanguage_None, PrScript_Jpan, PrRegion_None);
constexpr PrLocale kKorean = prLocaleMake(PrLanguage_None, PrScript_Kore, PrRegion_None);

// Returns the font file `bytes` with a name table that names its family `family` and nothing else.
std::vector<uint8_t> renamed(const std::vector<uint8_t>& bytes, const std::string& family) {
    auto read16 = [&](size_t at) { return uint32_t(bytes[at] << 8 | bytes[at + 1]); };
    auto read32 = [&](size_t at) { return read16(at) << 16 | read16(at + 2); };
    auto put16 = [](std::vector<uint8_t>& out, uint32_t value) {
        out.push_back(uint8_t(value >> 8));
        out.push_back(uint8_t(value));
    };
    auto put32 = [&](std::vector<uint8_t>& out, uint32_t value) {
        put16(out, value >> 16);
        put16(out, value & 0xffff);
    };
    struct Table {
        uint32_t tag;
        std::vector<uint8_t> data;
    };
    std::vector<Table> tables;
    constexpr uint32_t kName = PR_FONT_TAG('n', 'a', 'm', 'e');
    for (uint32_t i = 0; i < read16(4); ++i) {
        size_t record = 12 + 16 * size_t(i);
        const uint8_t* data = bytes.data() + read32(record + 8);
        if (read32(record) != kName)
            tables.push_back({read32(record), {data, data + read32(record + 12)}});
    }
    // The family's name, in UTF-16BE, as Windows' platform names it: its family (1) and its typographic family (16).
    std::vector<uint8_t> text;
    for (char c : family)
        put16(text, uint8_t(c));
    Table name{kName, {}};
    put16(name.data, 0);
    put16(name.data, 2);
    put16(name.data, 6 + 12 * 2);
    for (uint32_t id : {1u, 16u}) {
        for (uint32_t value : {3u, 1u, 0x409u, id, uint32_t(text.size()), 0u})
            put16(name.data, value);
    }
    name.data.insert(name.data.end(), text.begin(), text.end());
    tables.push_back(name);
    std::sort(tables.begin(), tables.end(), [](const Table& a, const Table& b) { return a.tag < b.tag; });
    std::vector<uint8_t> out;
    put32(out, read32(0));
    uint32_t count = uint32_t(tables.size());
    uint32_t power = 1;
    uint32_t log = 0;
    while (power * 2 <= count) {
        power *= 2;
        ++log;
    }
    for (uint32_t value : {count, power * 16, log, count * 16 - power * 16})
        put16(out, value);
    uint32_t offset = 12 + 16 * count;
    for (const Table& table : tables) {
        for (uint32_t value : {table.tag, 0u, offset, uint32_t(table.data.size())})
            put32(out, value);
        offset += (uint32_t(table.data.size()) + 3) & ~3u;
    }
    for (const Table& table : tables) {
        out.insert(out.end(), table.data.begin(), table.data.end());
        out.resize((out.size() + 3) & ~size_t(3));
    }
    return out;
}

// Adds Noto Sans JP's outlines to `f`'s set as the family `family`, made once for each family and kept for the run.
void installAs(Fixture& f, const char* family) {
    static std::map<std::string, std::vector<uint8_t>> made;
    std::vector<uint8_t>& bytes = made[family];
    if (bytes.empty()) {
        std::ifstream file(std::string(PR_FONTS_DIR) + "/NotoSansJP-VF.otf", std::ios::binary);
        REQUIRE(file.good());
        bytes = renamed({std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}, family);
    }
    const PrFont* PR_NULLABLE font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    prFontSetAdd(f.fonts, &descriptor, font);
    prFontRelease(font);
}

// Installs the sans CJK families, then Noto Serif SC, and Last Resort to fall back to, in the bundles' order, each made
// for its writing system and drawn in its generic family. The CJK families are one face of Noto Sans JP's outlines
// under each family's name: what falls back to them is chosen by their names and their locales, which their glyphs do
// not change.
void installCjk(Fixture& f) {
    for (const char* family : {"Noto Sans SC", "Noto Sans TC", "Noto Sans HK", "Noto Sans JP", "Noto Sans KR",
        "Noto Serif SC"})
        installAs(f, family);
    install(f, "LastResort-Regular.ttf");
    const PrFontFallbackFamily families[] = {
        {"Noto Sans SC", kHans, PrFontGeneric_SansSerif},
        {"Noto Sans TC", kHant, PrFontGeneric_SansSerif},
        {"Noto Sans HK", kHantHongKong, PrFontGeneric_SansSerif},
        {"Noto Sans JP", kJapanese, PrFontGeneric_SansSerif},
        {"Noto Sans KR", kKorean, PrFontGeneric_SansSerif},
        {"Noto Serif SC", kHans, PrFontGeneric_Serif},
        {"Last Resort", 0, PrFontGeneric_None},
    };
    prFontSetSetFallbackFamilies(f.fonts, families, 7);
}

// Lays out a block of 20px text in `locale`, holding `text`, in the family `family` (a generic family if it is a
// generic's keyword), or in the default family if `family` is null.
void layOut(Fixture& f, const char* text, const char* PR_NULLABLE locale = nullptr, const char* PR_NULLABLE family = nullptr) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    std::vector<PrFontFamily> families;
    if (family != nullptr) {
        PrFontGeneric generic = prFontGenericFromName(family);
        families.push_back(generic != PrFontGeneric_None ? prFontFamilyGeneric(generic) : prFontFamilyFromName(family));
    }
    font.font = f.textFont(20, families, locale != nullptr ? prLocaleFromTag(locale) : 0);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    f.text(root, text);
    f.layout(px(800, 600));
    CHECK(verified(f.tree));
}

// Appends the fonts of the glyph runs that the tree paints, in order, including through the lists it embeds.
void appendFonts(const PrDrawList* list, std::vector<const PrFont*>& out) {
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        if (commands[i].kind == PrDrawCommandKind_Embed)
            appendFonts(commands[i].embed.list, out);
        if (commands[i].kind == PrDrawCommandKind_Glyphs)
            out.push_back(commands[i].glyphs->font);
    }
}
std::vector<const PrFont*> paintedFonts(Fixture& f) {
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(f.tree, list);
    std::vector<const PrFont*> fonts;
    appendFonts(list, fonts);
    prDrawListRelease(list);
    return fonts;
}

// Returns the family of a font, such as "Noto Sans SC".
std::string familyOf(const PrFont* font) {
    PrRawString family = prFontFamilyName(font);
    PrStringView view = prRawStringView(&family);
    std::string out(view.data, view.size);
    prRawStringReset(&family);
    return out;
}

// Returns true if a font declares the OpenType script `tag`.
bool declaresScript(const PrFont* font, const char* tag) {
    uint32_t tags[64];
    uint32_t count = prFontScriptTags(font, tags, 64);
    uint32_t wanted = PR_FONT_TAG(uint8_t(tag[0]), uint8_t(tag[1]), uint8_t(tag[2]), uint8_t(tag[3]));
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        if (tags[i] == wanted)
            return true;
    }
    return false;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("Han falls back to the family made for its writing system, the closest first, and of none to the first") {
    struct Case {
        const char* PR_NULLABLE locale;
        const char* family;
    };
    for (Case c : {Case{nullptr, "Noto Sans SC"}, Case{"zh-Hans", "Noto Sans SC"}, Case{"zh", "Noto Sans SC"},
        Case{"zh-TW", "Noto Sans TC"}, Case{"zh-HK", "Noto Sans HK"}, Case{"yue", "Noto Sans HK"},
                Case{"yue-Hans", "Noto Sans SC"}, Case{"ja", "Noto Sans JP"}, Case{"ko", "Noto Sans KR"}}) {
        CAPTURE(c.locale != nullptr ? c.locale : "none");
        Fixture f;
        installCjk(f);
        layOut(f, "ab \xe6\x97\xa5\xe6\x9c\xac", c.locale);
        std::vector<const PrFont*> fonts = paintedFonts(f);
        REQUIRE(fonts.size() == 2);
        CHECK(familyOf(fonts[1]) == c.family);
    }
}

TEST_CASE("Han falls back to a family drawn in its style's generic family, after the locale it is made for") {
    struct Case {
        const char* locale;
        const char* generic;
        const char* family;
    };
    // Noto Serif SC is the only serif family. Serif Simplified Chinese uses it, and serif Traditional Chinese uses a
    // sans family made for Traditional Chinese over a serif family that is not.
    for (Case c : {Case{"zh-Hans", "serif", "Noto Serif SC"}, Case{"zh-Hans", "sans-serif", "Noto Sans SC"},
        Case{"zh-Hans", "monospace", "Noto Sans SC"}, Case{"zh-TW", "serif", "Noto Sans TC"}}) {
        CAPTURE(c.locale);
        CAPTURE(c.generic);
        Fixture f;
        installCjk(f);
        layOut(f, "\xe6\x97\xa5\xe6\x9c\xac", c.locale, c.generic);
        std::vector<const PrFont*> fonts = paintedFonts(f);
        REQUIRE(fonts.size() == 1);
        CHECK(familyOf(fonts[0]) == c.family);
    }
}

TEST_CASE("a family is preferred for the locale the host makes it for, and only for that") {
    // Noto Nastaliq Urdu, after Noto Sans Arabic in the host's order, declares the language systems of Arabic and
    // Uyghur among others. Arabic text uses Noto Sans Arabic, the first family that serves it, except in Urdu, which
    // the host makes Noto Nastaliq Urdu for.
    struct Case {
        const char* locale;
        const char* family;
    };
    for (Case c : {Case{"ar", "Noto Sans Arabic"}, Case{"ug", "Noto Sans Arabic"}, Case{"ur", "Noto Nastaliq Urdu"}}) {
        CAPTURE(c.locale);
        Fixture f;
        install(f, "NotoSansArabic[wdth,wght].ttf");
        install(f, "NotoNastaliqUrdu[wght].ttf");
        const PrFontFallbackFamily families[] = {
            {"Noto Sans Arabic", 0, PrFontGeneric_SansSerif},
            {"Noto Nastaliq Urdu", prLocaleMake(PrLanguage_Ur, PrScript_Zzzz, PrRegion_None), PrFontGeneric_None},
        };
        prFontSetSetFallbackFamilies(f.fonts, families, 2);
        layOut(f, "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85", c.locale);
        std::vector<const PrFont*> fonts = paintedFonts(f);
        REQUIRE(fonts.size() == 1);
        CHECK(familyOf(fonts[0]) == c.family);
    }
}

TEST_CASE("a cluster no face serves is set in Last Resort, alone") {
    Fixture f;
    installCjk(f);
    // U+0F00, Tibetan, which none of these families except Last Resort serves.
    layOut(f, "a\xe0\xbc\x80" "b");
    std::vector<const PrFont*> fonts = paintedFonts(f);
    REQUIRE(fonts.size() == 3);
    CHECK(fonts[0] == fonts[2]);
    CHECK(fonts[1] != fonts[0]);
    CHECK_FALSE(declaresScript(fonts[1], "hani"));
}

TEST_CASE("a private-use code point falls back to no installed family") {
    Fixture f;
    installCjk(f);
    layOut(f, "a\xee\x80\x80" "b");
    std::vector<const PrFont*> fonts = paintedFonts(f);
    REQUIRE(fonts.size() == 1);
}

TEST_CASE("a control character falls back to no installed family") {
    // U+007F DELETE, whose visible glyph is the primary face's missing glyph.
    Fixture f;
    installCjk(f);
    layOut(f, "a\x7f" "b");
    std::vector<const PrFont*> fonts = paintedFonts(f);
    REQUIRE(fonts.size() == 1);
}

TEST_CASE("a cluster's face does not depend on what text came before it") {
    // The same Japanese after Korean and alone uses one face: the choice is a function of the cluster and the style.
    auto faceOf = [](const char* text) {
        Fixture f;
        installCjk(f);
        layOut(f, text, "ja");
        std::vector<const PrFont*> fonts = paintedFonts(f);
        REQUIRE(!fonts.empty());
        return familyOf(fonts.back());
    };
    CHECK(faceOf("한국 日本") == faceOf("日本"));
    CHECK(faceOf("日本") == "Noto Sans JP");
}

TEST_CASE("a page of text its named family serves falls back to nothing") {
    Fixture f;
    installCjk(f);
    layOut(f, "The quick brown fox");
    std::vector<const PrFont*> fonts = paintedFonts(f);
    REQUIRE(fonts.size() == 1);
    CHECK(familyOf(fonts[0]) == "Arimo");
}

TEST_CASE("a cluster of emoji presentation falls back to a color font first, and of text presentation to text") {
    Fixture f;
    install(f, "NotoSansSymbols2-Regular.ttf");
    install(f, "Twemoji.ttf");
    const PrFontFallbackFamily families[] = {
        {"Noto Sans Symbols 2", 0, PrFontGeneric_None},
        {"twemoji-glyf_colr_1", 0, PrFontGeneric_None},
    };
    prFontSetSetFallbackFamilies(f.fonts, families, 2);
    // U+2601 CLOUD, of text presentation by default, which both families serve and Arimo does not: with VS16 and with
    // VS15.
    layOut(f, "a\xe2\x98\x81\xef\xb8\x8f" "b\xe2\x98\x81\xef\xb8\x8e");
    std::vector<const PrFont*> fonts = paintedFonts(f);
    REQUIRE(fonts.size() == 4);
    CHECK(fonts[1] != fonts[3]);
    CHECK(fonts[1] != fonts[0]);
    CHECK(fonts[3] != fonts[0]);
}

TEST_CASE("a shaping holds its font groups through the collect a layout starts with") {
    // The container's text falls back, so its shaping indexes a face that its font group added. The container then
    // takes another font, which leaves no record naming the first, and is laid out, which collects as it starts. Then
    // it takes the first font again. The group it names must set the faces that the first group set, whether it is the
    // group kept or a group interned again.
    Fixture f;
    installCjk(f);
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = f.textFont(20, {}, prLocaleFromTag("ja"));
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = f.element(root, block(), nullptr, nullptr, &font);
    f.text(container, "日本");
    f.layout(px(800, 600));
    PrLayoutFontStyle other = font;
    other.font = f.textFont(21, {}, prLocaleFromTag("ja"));
    prLayoutNodeSetStyle(f.tree, container, f.style(block(), nullptr, nullptr, &other));
    f.layout(px(800, 600));
    f.layout(px(800, 600));
    prLayoutNodeSetStyle(f.tree, container, f.style(block(), nullptr, nullptr, &font));
    f.layout(px(800, 600));
    CHECK(verified(f.tree));
    std::vector<const PrFont*> fonts = paintedFonts(f);
    REQUIRE(!fonts.empty());
    CHECK(familyOf(fonts.back()) == "Noto Sans JP");
}
