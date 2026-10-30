#include "pr/font_freetype.h"
#include "pr/locale_common.h"
#include "pr/text.h"
#include "pr/text_font.h"
#include "pr/text_harfbuzz.h"
#include "pr/text_itemize.h"
#include "pr/text_kb.h"
#include "pr/text_layout.h"
#include "pr/vec.h"
#if defined(__APPLE__)
#include "pr/text_coretext.h"
#endif

#include "doctest/doctest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" int prTestTextCApi(void);

namespace pr_impl {

namespace {

const PrFont* load(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    uint8_t buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

struct Case {
    const char* font;
    const char* text;
    PrTextDirection direction;
    PrScript script;
    const char* language;           // BCP 47, or null for none
};

std::vector<PrTextGlyph> shape(PrTextShaper* shaper, const PrTextRun& run) {
    PrTextGlyphBuffer* glyphs = prTextGlyphBufferCreate();
    REQUIRE(prTextShaperShape(shaper, &run, glyphs) == PrStatus_Ok);
    std::vector<PrTextGlyph> out(prTextGlyphBufferGlyphs(glyphs),
        prTextGlyphBufferGlyphs(glyphs) + prTextGlyphBufferGlyphCount(glyphs));
    prTextGlyphBufferDestroy(glyphs);
    return out;
}

// Returns each glyph as its id, its cluster and its position.
std::string describe(const std::vector<PrTextGlyph>& glyphs) {
    std::string out;
    char text[96];
    PrVec2 pen{0, 0};
    for (const PrTextGlyph& g : glyphs) {
        std::snprintf(text, sizeof(text), "%u@%u(%.3f,%.3f) ", g.id, g.cluster, double(pen.x + g.offset.x),
            double(pen.y + g.offset.y));
        out += text;
        pen.x += g.advance.x;
        pen.y += g.advance.y;
    }
    return out;
}

// Returns true if two shapers agree: the same glyphs from the same clusters, each within `tolerance` of the same
// position, and the same total advance.
bool agree(const std::vector<PrTextGlyph>& a, const std::vector<PrTextGlyph>& b, float tolerance) {
    if (a.size() != b.size())
        return false;
    PrVec2 penA{0, 0}, penB{0, 0};
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].cluster != b[i].cluster)
            return false;
        if (std::fabs(penA.x + a[i].offset.x - penB.x - b[i].offset.x) > tolerance ||
                std::fabs(penA.y + a[i].offset.y - penB.y - b[i].offset.y) > tolerance)
            return false;
        penA.x += a[i].advance.x;
        penA.y += a[i].advance.y;
        penB.x += b[i].advance.x;
        penB.y += b[i].advance.y;
    }
    return std::fabs(penA.x - penB.x) <= tolerance && std::fabs(penA.y - penB.y) <= tolerance;
}

constexpr Case kCases[] = {
    {"NotoSans.ttf", "office affine flow", PrTextDirection_Ltr, 0, nullptr},
    {"NotoSans.ttf", "AVATAR Toyota WAVE", PrTextDirection_Ltr, PrScript_Latn, "en"},
    {"NotoSans.ttf", "e\xcc\x81 n\xcc\x83 q\xcc\x87\xcc\xa3", PrTextDirection_Ltr, 0, nullptr},
    {
        "NotoSans.ttf", "\xce\xb3\xcf\x85\xce\xb1\xce\xbb\xce\xb9\xce\xac \xcf\x87\xcf\x89\xcf\x81" "\xce\xaf\xcf\x82",
        PrTextDirection_Ltr, 0, nullptr,
    },
    {
        "NotoSansArabic.ttf", "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84"
            "\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85",
        PrTextDirection_Rtl, PrScript_Arab, "ar",
    },
    {
        "NotoSansArabic.ttf", "\xd8\xa8\xd9\x90\xd8\xb3\xd9\x92\xd9\x85\xd9\x90 \xd8\xa7\xd9\x84"
            "\xd9\x84\xd9\x91\xd9\x8e\xd9\x87\xd9\x90",
        PrTextDirection_Rtl, 0, nullptr,
    },
    {
        "NotoSansDevanagari.ttf",
        "\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87 "
            "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7\xe0\xa4\xa4\xe0\xa5\x8d\xe0\xa4\xb0\xe0\xa4\xbf\xe0\xa4\xaf",
        PrTextDirection_Ltr, PrScript_Deva, "hi",
    },
    {
        "Twemoji.ttf", "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x9a\x80",
        PrTextDirection_Ltr, 0, nullptr,
    },
    {
        "Twemoji.ttf", "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x87\xab\xf0\x9f\x87\xb7",
        PrTextDirection_Ltr, 0, nullptr,
    },
};

// Returns true if two shapers draw the same: the same glyphs in the same places, in any order, and the same total
// advance.
bool drawAlike(const std::vector<PrTextGlyph>& a, const std::vector<PrTextGlyph>& b, float tolerance) {
    struct Placed {
        uint32_t id;
        PrVec2 at;
    };
    auto place = [](const std::vector<PrTextGlyph>& glyphs, PrVec2* end) {
        std::vector<Placed> out;
        PrVec2 pen{0, 0};
        for (const PrTextGlyph& g : glyphs) {
            out.push_back(Placed{g.id, pen + g.offset});
            pen.x += g.advance.x;
            pen.y += g.advance.y;
        }
        *end = pen;
        return out;
    };
    PrVec2 endA{}, endB{};
    std::vector<Placed> pa = place(a, &endA);
    std::vector<Placed> pb = place(b, &endB);
    if (pa.size() != pb.size() || std::fabs(endA.x - endB.x) > tolerance)
        return false;
    for (const Placed& p : pa) {
        bool found = false;
        for (Placed& q : pb) {
            if (q.id == p.id && std::fabs(q.at.x - p.at.x) <= tolerance && std::fabs(q.at.y - p.at.y) <= tolerance) {
                q.id = 0xffffffffu;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

// Returns the shapers that shape features, one of each.
std::vector<PrTextShaper*> featureShapers() {
    std::vector<PrTextShaper*> shapers{prTextKbCreate(), prTextHarfBuzzCreate()};
#if defined(__APPLE__)
    shapers.push_back(prTextCoreTextCreate());
#endif
    return shapers;
}

PrTextRun runOf(const PrFont* font, const char* text) {
    PrTextRun run{};
    run.font = font;
    run.text = text;
    return run;
}

std::vector<uint32_t> idsOf(const std::vector<PrTextGlyph>& glyphs) {
    std::vector<uint32_t> ids;
    for (const PrTextGlyph& g : glyphs)
        ids.push_back(g.id);
    return ids;
}

// A face of the font set of a test: its file, and the family that it is described as, or null for the family of the
// file, with ranges.
struct SetFace {
    const char* file;
    const char* PR_NULLABLE family = nullptr;
    const PrFontCodePointRange* PR_NULLABLE ranges = nullptr;
};

PrFontSet* setOf(std::initializer_list<SetFace> faces) {
    PrFontSet* set = prFontSetCreate(nullptr);
    for (const SetFace& f : faces) {
        const PrFont* font = load(f.file);
        PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
        if (f.family != nullptr) {
            d.automatic &= PrFontFaceAutoFlags(~PrFontFaceAutoFlag_Family);
            d.family = f.family;
        }
        d.ranges = f.ranges;
        d.rangeCount = f.ranges != nullptr ? 1 : 0;
        prFontSetAdd(set, &d, font);
        prFontRelease(font);
    }
    return set;
}

// Returns the text font of the named families, in `set`.
PrTextFont* fontOf(PrFontSet* set, std::initializer_list<const char*> names) {
    std::vector<PrFontFamily> families;
    for (const char* name : names)
        families.push_back(prFontFamilyFromName(name));
    PrTextFontDesc desc{
        .set = set,
        .families = families.data(),
        .familyCount = uint32_t(families.size()),
        .style = prFontStyleDefault(),
    };
    return prTextFontCreate(&desc);
}

// Returns the runs of `text` as one paragraph in the text font `group`, left to right unless `rtl`.
std::vector<PrTextItemRun> itemize(PrTextFont* group, const std::string& text, bool rtl = false) {
    PrTextPiece piece{};
    piece.kind = PrTextPieceKind_Text;
    piece.end = uint32_t(text.size());
    piece.font = group;
    PrTextItemizeDesc desc{};
    desc.text = text;
    desc.pieces = &piece;
    desc.pieceCount = 1;
    desc.direction = rtl ? PrTextDirection_Rtl : PrTextDirection_Ltr;
    PrTextItems* items = prTextItemsCreate();
    prTextItemsItemize(items, &desc);
    std::vector<PrTextItemRun> runs(prTextItemsRuns(items), prTextItemsRuns(items) + prTextItemsRunCount(items));
    prTextItemsDestroy(items);
    return runs;
}

// Returns the families of the faces that set the runs.
std::vector<std::string> familiesOf(PrTextFont* group, const std::vector<PrTextItemRun>& runs) {
    std::vector<std::string> families;
    for (const PrTextItemRun& run : runs) {
        PrString family = prFontFamilyName(prTextFontMatchedFont(group, run.face));
        PrStringView name = prStringView(&family);
        families.emplace_back(name.data, name.size);
        prStringReset(&family);
    }
    return families;
}

// A set over a host set of Noto Sans and Noto Serif, with serif as its default family, in the way that the set of a
// document is layered over the set of its host.
struct Layered {
    PrFontSet* host = setOf({{"NotoSans.ttf"}, {"NotoSerif.ttf"}});
    PrFontSet* document = prFontSetCreate(host);

    Layered() {
        prFontSetSetGeneric(host, PrFontGeneric_Serif, "Noto Serif");
        prFontSetSetDefaultFamily(host, prFontFamilyGeneric(PrFontGeneric_Serif));
    }

    ~Layered() {
        prFontSetDestroy(document);
        prFontSetDestroy(host);
    }

    // Adds a pending face of `family` with `key`, loaded with `file` unless it is null, with the ranges `range` if
    // given.
    void add(const char* family, uint64_t key, const char* PR_NULLABLE file,
            const PrFontCodePointRange* PR_NULLABLE range = nullptr) {
        PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
        d.family = family;
        d.ranges = range;
        d.rangeCount = range != nullptr ? 1 : 0;
        prFontSetAddPending(document, &d, key);
        if (file != nullptr) {
            const PrFont* font = load(file);
            prFontSetLoaded(document, key, font);
            prFontRelease(font);
        }
    }
};

// Returns the family of the face that sets `cluster` in `font`, or an empty string if no face does.
std::string familyFor(PrTextFont* font, const char* cluster, PrScript script = PrScript_Latn) {
    uint32_t face = prTextFontMatch(font, cluster, script);
    if (face == kPrTextMatchNone)
        return "";
    PrString family = prFontFamilyName(prTextFontMatchedFont(font, face));
    std::string name(prStringView(&family).data, prStringView(&family).size);
    prStringReset(&family);
    return name;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestTextCApi() == 0);
}

TEST_CASE("kb shapes as HarfBuzz does") {
    PrTextShaper* kb = prTextKbCreate();
    PrTextShaper* hb = prTextHarfBuzzCreate();
    for (const Case& c : kCases) {
        const PrFont* font = load(c.font);
        PrTextRun run{};
        run.font = font;
        run.text = c.text;
        run.direction = c.direction;
        run.script = c.script;
        run.locale = c.language != nullptr ? prLocaleFromTag(c.language) : 0;
        std::vector<PrTextGlyph> expected = shape(hb, run);
        std::vector<PrTextGlyph> actual = shape(kb, run);
        INFO(c.text);
        INFO("harfbuzz: ", describe(expected));
        INFO("kb:       ", describe(actual));
        CHECK(agree(expected, actual, 1e-3f));
        prFontRelease(font);
    }
    prTextShaperDestroy(kb);
    prTextShaperDestroy(hb);
}

// The advances and the kerning follow the design point, at a point away from the default on every axis.
TEST_CASE("kb shapes a variable font's instance as HarfBuzz does") {
    PrTextShaper* kb = prTextKbCreate();
    PrTextShaper* hb = prTextHarfBuzzCreate();
    const PrFont* font = load("NotoSans.ttf");
    const PrFontVariation point[] = {{PR_FONT_TAG('w', 'g', 'h', 't'), 830}, {PR_FONT_TAG('w', 'd', 't', 'h'), 70}};
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = point;
    derivation.variationCount = 2;
    const PrFont* instance = prFontCreateDerived(font, &derivation);
    PrTextRun run{};
    run.text = "AVATAR Toyota WAVE office";
    run.font = font;
    std::vector<PrTextGlyph> regular = shape(kb, run);
    run.font = instance;
    std::vector<PrTextGlyph> expected = shape(hb, run);
    std::vector<PrTextGlyph> actual = shape(kb, run);
    INFO("harfbuzz: ", describe(expected));
    INFO("kb:       ", describe(actual));
    CHECK_FALSE(agree(regular, actual, 1e-3f));
    CHECK(agree(expected, actual, 1e-3f));
    prFontRelease(instance);
    prFontRelease(font);
    prTextShaperDestroy(kb);
    prTextShaperDestroy(hb);
}

// A base with two marks that compose, in sequence, to a character the font has: a, U+030A and U+0301 to U+01FB.
TEST_CASE("kb composes a sequence of marks as HarfBuzz does") {
    PrTextShaper* kb = prTextKbCreate();
    PrTextShaper* hb = prTextHarfBuzzCreate();
    const PrFont* font = load("NotoSans.ttf");
    PrTextRun run{};
    run.font = font;
    run.text = "a\xcc\x8a\xcc\x81";
    std::vector<PrTextGlyph> expected = shape(hb, run);
    std::vector<PrTextGlyph> actual = shape(kb, run);
    INFO("harfbuzz: ", describe(expected));
    INFO("kb:       ", describe(actual));
    CHECK(agree(expected, actual, 1e-3f));
    prFontRelease(font);
    prTextShaperDestroy(kb);
    prTextShaperDestroy(hb);
}

#if defined(__APPLE__)
TEST_CASE("CoreText draws as HarfBuzz does") {
    PrTextShaper* ct = prTextCoreTextCreate();
    PrTextShaper* hb = prTextHarfBuzzCreate();
    for (const Case& c : kCases) {
        const PrFont* font = load(c.font);
        PrTextRun run{};
        run.font = font;
        run.text = c.text;
        run.direction = c.direction;
        run.script = c.script;
        run.locale = c.language != nullptr ? prLocaleFromTag(c.language) : 0;
        std::vector<PrTextGlyph> expected = shape(hb, run);
        std::vector<PrTextGlyph> actual = shape(ct, run);
        INFO(c.text);
        INFO("harfbuzz: ", describe(expected));
        INFO("coretext: ", describe(actual));
        CHECK(drawAlike(expected, actual, 5e-3f));
        prFontRelease(font);
    }
    prTextShaperDestroy(ct);
    prTextShaperDestroy(hb);
}

// A design point of Roboto Flex between its named instances, for which the variation of a font descriptor gives
// CoreText no usable font. CoreText places the glyphs of Roboto Flex at its own advances, which are narrower than those
// of the font, so only the glyphs are compared.
TEST_CASE("CoreText shapes a variable font between its named instances in the font's own glyphs") {
    PrTextShaper* ct = prTextCoreTextCreate();
    PrTextShaper* hb = prTextHarfBuzzCreate();
    const PrFont* font = load("RobotoFlex.ttf");
    const PrFontVariation slant{PR_FONT_TAG('s', 'l', 'n', 't'), -5};
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = &slant;
    derivation.variationCount = 1;
    const PrFont* instance = prFontCreateDerived(font, &derivation);
    PrTextRun run{};
    run.font = instance;
    run.text = "Slanted type";
    std::vector<PrTextGlyph> expected = shape(hb, run);
    std::vector<PrTextGlyph> actual = shape(ct, run);
    INFO("harfbuzz: ", describe(expected));
    INFO("coretext: ", describe(actual));
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i)
        CHECK(actual[i].id == expected[i].id);
    prFontRelease(instance);
    prFontRelease(font);
    prTextShaperDestroy(ct);
    prTextShaperDestroy(hb);
}
#endif

TEST_CASE("features turn on and off over ranges") {
    const PrFont* font = load("NotoSans.ttf");
    const char* text = "fi fi";
    for (PrTextShaper* shaper : featureShapers()) {
        PrTextRun run{};
        run.font = font;
        run.text = PrStringView{text, 5};
        std::vector<PrTextGlyph> ligated = shape(shaper, run);
        CHECK(ligated.size() == 3);
        // Ligatures off for the first word only.
        PrTextFeature off{PR_FONT_TAG('l', 'i', 'g', 'a'), 0, 0, 2};
        run.features = &off;
        run.featureCount = 1;
        std::vector<PrTextGlyph> split = shape(shaper, run);
        CHECK(split.size() == 4);
        prTextShaperDestroy(shaper);
    }
    prFontRelease(font);
}

TEST_CASE("a variation shapes with its own advances") {
    const PrFont* font = load("NotoSans.ttf");
    PrFontVariation heavy{PR_FONT_TAG('w', 'g', 'h', 't'), 900};
    const PrFont* black = nullptr;
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = &heavy;
    derivation.variationCount = 1;
    black = prFontCreateDerived(font, &derivation);
    PrTextShaper* shapers[] = {prTextKbCreate(), prTextHarfBuzzCreate()};
    for (PrTextShaper* shaper : shapers) {
        PrTextRun run{};
        run.font = font;
        run.text = "Hamburgefonstiv";
        PrTextGlyphBuffer* glyphs = prTextGlyphBufferCreate();
        REQUIRE(prTextShaperShape(shaper, &run, glyphs) == PrStatus_Ok);
        float regular = prTextGlyphBufferAdvance(glyphs).x;
        run.font = black;
        REQUIRE(prTextShaperShape(shaper, &run, glyphs) == PrStatus_Ok);
        float bold = prTextGlyphBufferAdvance(glyphs).x;
        CHECK(bold > regular * 1.05f);
        // The advances are the font's own.
        float sum = 0;
        for (uint32_t i = 0; i < prTextGlyphBufferGlyphCount(glyphs); ++i)
            sum += prFontGlyphAdvance(black, prTextGlyphBufferGlyphs(glyphs)[i].id, 0);
        CHECK(bold == doctest::Approx(sum).epsilon(0.001));
        prTextGlyphBufferDestroy(glyphs);
        prTextShaperDestroy(shaper);
    }
    prFontRelease(black);
    prFontRelease(font);
}

TEST_CASE("a run shaped with its context joins as the whole text does") {
    // An Arabic word split in two. Each half, shaped with the other as context, gets the forms the whole word gives it.
    const PrFont* font = load("NotoSansArabic.ttf");
    PrTextShaper* hb = prTextHarfBuzzCreate();
    const char* word = "\xd8\xa8\xd8\xb3\xd9\x85\xd9\x84\xd8\xa9";
    size_t split = 4;
    PrTextRun whole = runOf(font, word);
    whole.direction = PrTextDirection_Rtl;
    std::vector<PrTextGlyph> all = shape(hb, whole);
    PrTextRun first = whole;
    first.text = PrStringView{word, split};
    first.after = PrStringView{word + split, std::strlen(word) - split};
    PrTextRun second = whole;
    second.text = PrStringView{word + split, std::strlen(word) - split};
    second.before = PrStringView{word, split};
    std::vector<uint32_t> a = idsOf(shape(hb, first));
    std::vector<uint32_t> b = idsOf(shape(hb, second));
    // Right to left: the glyphs of the second half come first.
    std::vector<uint32_t> joined = b;
    joined.insert(joined.end(), a.begin(), a.end());
    CHECK(joined == idsOf(all));
    // Without context, the halves get their isolated and final forms.
    first.after = PrStringView{};
    std::vector<uint32_t> ids = idsOf(all);
    CHECK(idsOf(shape(hb, first)) != std::vector<uint32_t>(ids.end() - long(a.size()), ids.end()));
    prTextShaperDestroy(hb);
    prFontRelease(font);
}

TEST_CASE("kb joins a run with its context as HarfBuzz does" * doctest::should_fail()) {
    // The Arabic word above, split in two and each half shaped by kb with the other as context.
    const PrFont* font = load("NotoSansArabic.ttf");
    PrTextShaper* kb = prTextKbCreate();
    const char* word = "\xd8\xa8\xd8\xb3\xd9\x85\xd9\x84\xd8\xa9";
    size_t split = 4;
    PrTextRun whole = runOf(font, word);
    whole.direction = PrTextDirection_Rtl;
    std::vector<uint32_t> all = idsOf(shape(kb, whole));
    PrTextRun first = whole;
    first.text = PrStringView{word, split};
    first.after = PrStringView{word + split, std::strlen(word) - split};
    PrTextRun second = whole;
    second.text = PrStringView{word + split, std::strlen(word) - split};
    second.before = PrStringView{word, split};
    // Right to left: the glyphs of the second half come first.
    std::vector<uint32_t> joined = idsOf(shape(kb, second));
    std::vector<uint32_t> a = idsOf(shape(kb, first));
    joined.insert(joined.end(), a.begin(), a.end());
    CHECK(joined == all);
    prTextShaperDestroy(kb);
    prFontRelease(font);
}

TEST_CASE("the first glyph of each grapheme cluster is marked") {
    const PrFont* font = load("NotoSans.ttf");
    PrTextShaper* hb = prTextHarfBuzzCreate();
    // e and a combining acute, which NotoSans composes, then q with two marks it keeps separate.
    std::vector<PrTextGlyph> glyphs = shape(hb, runOf(font, "e\xcc\x81q\xcc\x87\xcc\xa3x"));
    uint32_t starts = 0;
    for (const PrTextGlyph& g : glyphs)
        starts += (g.flags & PrTextGlyphFlag_ClusterStart) != 0 ? 1 : 0;
    CHECK(starts == 3);
    CHECK((glyphs.front().flags & PrTextGlyphFlag_ClusterStart) != 0);
    CHECK((glyphs.back().flags & PrTextGlyphFlag_ClusterStart) != 0);
    prTextShaperDestroy(hb);
    prFontRelease(font);
}

TEST_CASE("features over the same text all apply") {
    const PrFont* font = load("NotoSans.ttf");
    for (PrTextShaper* shaper : featureShapers()) {
        PrTextRun run{};
        run.font = font;
        run.text = "fi 1";
        std::vector<PrTextGlyph> plain = shape(shaper, run);
        PrTextFeature features[] = {
            {PR_FONT_TAG('l', 'i', 'g', 'a'), 0, 0, 0xffffffffu},
            {PR_FONT_TAG('o', 'n', 'u', 'm'), 1, 0, 0xffffffffu},
        };
        run.features = features;
        run.featureCount = 2;
        std::vector<PrTextGlyph> both = shape(shaper, run);
        INFO(describe(plain), " / ", describe(both));
        REQUIRE(both.size() == 4);
        CHECK(both[3].id != plain.back().id);
        prTextShaperDestroy(shaper);
    }
    prFontRelease(font);
}

TEST_CASE("kerning is the font's, or on, or off") {
    const PrFont* font = load("NotoSans.ttf");
    PrTextShaper* shapers[] = {prTextKbCreate(), prTextHarfBuzzCreate()};
    for (PrTextShaper* shaper : shapers) {
        PrTextRun run = runOf(font, "AVAWAY");
        PrTextGlyphBuffer* glyphs = prTextGlyphBufferCreate();
        REQUIRE(prTextShaperShape(shaper, &run, glyphs) == PrStatus_Ok);
        float kerned = prTextGlyphBufferAdvance(glyphs).x;
        run.kerning = PrTextKerning_None;
        REQUIRE(prTextShaperShape(shaper, &run, glyphs) == PrStatus_Ok);
        CHECK(prTextGlyphBufferAdvance(glyphs).x > kerned + 0.05f);
        run.kerning = PrTextKerning_Normal;
        REQUIRE(prTextShaperShape(shaper, &run, glyphs) == PrStatus_Ok);
        CHECK(prTextGlyphBufferAdvance(glyphs).x == doctest::Approx(kerned));
        prTextGlyphBufferDestroy(glyphs);
        prTextShaperDestroy(shaper);
    }
    prFontRelease(font);
}

TEST_CASE("small capitals come from the font, or are synthesized from its capitals") {
    PrTextShaper* hb = prTextHarfBuzzCreate();
    // Noto Sans has smcp: nothing is synthesized.
    const PrFont* noto = load("NotoSans.ttf");
    PrTextRun run = runOf(noto, "Caps");
    run.caps = PrTextCaps_Small;
    run.synthesis = PrTextSynthesisFlag_SmallCaps;
    std::vector<PrTextGlyph> real = shape(hb, run);
    for (const PrTextGlyph& g : real)
        CHECK(g.scale == 1.0f);
    CHECK(real[1].id != prFontCodePointGlyph(noto, 'a'));
    CHECK(real[1].id != prFontCodePointGlyph(noto, 'A'));
    // Roboto Flex has no smcp. Lowercase becomes its capitals scaled, and sharp s becomes two of them.
    const PrFont* flex = load("RobotoFlex.ttf");
    run = runOf(flex, "Stra\xc3\x9f" "e");
    run.caps = PrTextCaps_Small;
    run.synthesis = PrTextSynthesisFlag_SmallCaps;
    std::vector<PrTextGlyph> glyphs = shape(hb, run);
    REQUIRE(glyphs.size() == 7);
    CHECK(glyphs[0].id == prFontCodePointGlyph(flex, 'S'));
    CHECK(glyphs[0].scale == 1.0f);
    CHECK(glyphs[1].id == prFontCodePointGlyph(flex, 'T'));
    CHECK(glyphs[1].scale < 0.9f);
    CHECK(glyphs[1].cluster == 1);
    CHECK(glyphs[4].id == prFontCodePointGlyph(flex, 'S'));
    CHECK(glyphs[4].cluster == 4);
    CHECK(glyphs[5].cluster == 4);
    CHECK(glyphs[6].cluster == 6);
    // Without synthesis, nothing changes.
    run.synthesis = 0;
    CHECK(shape(hb, run)[1].id == prFontCodePointGlyph(flex, 't'));
    prFontRelease(flex);
    prFontRelease(noto);
    prTextShaperDestroy(hb);
}

TEST_CASE("a superscript the font lacks is scaled and raised as its metrics say") {
    const PrFont* flex = load("RobotoFlex.ttf");
    PrTextShaper* hb = prTextHarfBuzzCreate();
    PrTextRun run = runOf(flex, "x2");
    std::vector<PrTextGlyph> plain = shape(hb, run);
    run.position = PrTextPosition_Superscript;
    run.synthesis = PrTextSynthesisFlag_Position;
    std::vector<PrTextGlyph> raised = shape(hb, run);
    PrFontMetrics m = prFontMetrics(flex);
    REQUIRE(raised.size() == 2);
    CHECK(raised[0].id == plain[0].id);
    CHECK(raised[0].scale == doctest::Approx(m.superscriptSize));
    CHECK(raised[0].offset.y == doctest::Approx(m.superscriptPosition));
    CHECK(raised[0].advance.x == doctest::Approx(plain[0].advance.x * m.superscriptSize));
    prTextShaperDestroy(hb);
    prFontRelease(flex);
}

TEST_CASE("a vertical run goes down, with vertical forms") {
    const PrFont* font = load("NotoSansJP.ttf");
    PrTextShaper* hb = prTextHarfBuzzCreate();
    PrTextRun run = runOf(font, "\xe6\x97\xa5\xe6\x9c\xac\xef\xbc\x88");
    std::vector<PrTextGlyph> across = shape(hb, run);
    run.axis = PrTextAxis_Vertical;
    std::vector<PrTextGlyph> down = shape(hb, run);
    REQUIRE(down.size() == 3);
    for (const PrTextGlyph& g : down) {
        CHECK(g.advance.x == doctest::Approx(0));
        CHECK(g.advance.y == doctest::Approx(1));
    }
    // The full-width parenthesis is rotated for vertical text.
    CHECK(down[2].id != across[2].id);
    CHECK(down[0].id == across[0].id);
    // Placed by its horizontal origin: half an em left of the center of the line.
    CHECK(down[0].offset.x == doctest::Approx(-0.5));
    // CoreText and HarfBuzz both shape down in their vertical layouts. kb cannot, so it sets each unit across with the
    // vertical forms of the font, and places it in the same position as HarfBuzz.
#if defined(__APPLE__)
    PrTextShaper* others[] = {prTextCoreTextCreate(), prTextKbCreate()};
#else
    PrTextShaper* others[] = {prTextKbCreate()};
#endif
    for (PrTextShaper* other : others) {
        std::vector<PrTextGlyph> units = shape(other, run);
        REQUIRE(units.size() == 3);
        for (uint32_t i = 0; i < 3; ++i) {
            CAPTURE(i);
            CHECK(units[i].id == down[i].id);
            CHECK(units[i].advance.x == doctest::Approx(0));
            CHECK(units[i].advance.y == doctest::Approx(down[i].advance.y));
            CHECK(units[i].offset.x == doctest::Approx(down[i].offset.x).epsilon(0.01));
            CHECK(units[i].offset.y == doctest::Approx(down[i].offset.y).epsilon(0.01));
        }
        prTextShaperDestroy(other);
    }
    prTextShaperDestroy(hb);
    prFontRelease(font);
}

TEST_CASE("vertical positioning applies in a vertical shaping, and not where a backend sets each unit across") {
    // The proportional vertical metrics (vpal) of Noto Sans JP move kana up the line. HarfBuzz and CoreText shape down,
    // and place each glyph alike, though they divide the placement between advances and offsets differently. kb sets
    // each kana across, and leaves them on the em grid.
    const PrFont* font = load("NotoSansJP.ttf");
    PrTextRun run = runOf(font, "\xe3\x83\x81\xe3\x83\xa7\xe3\x82\xb3");
    run.axis = PrTextAxis_Vertical;
    PrTextFeature vpal{PR_FONT_TAG('v', 'p', 'a', 'l'), 1, 0, ~0u};
    // The position of the origin of each glyph down the line.
    auto origins = [&](PrTextShaper* shaper, bool on) {
        PrTextRun with = run;
        with.features = on ? &vpal : nullptr;
        with.featureCount = on ? 1 : 0;
        std::vector<float> at;
        float pen = 0;
        for (const PrTextGlyph& g : shape(shaper, with)) {
            at.push_back(pen + g.offset.y);
            pen += g.advance.y;
        }
        return at;
    };
    PrTextShaper* hb = prTextHarfBuzzCreate();
    PrTextShaper* kb = prTextKbCreate();
    std::vector<float> grid = origins(hb, false);
    std::vector<float> proportional = origins(hb, true);
    std::vector<float> units = origins(kb, true);
    REQUIRE(proportional.size() == 3);
    REQUIRE(units.size() == 3);
    CHECK(proportional[2] < grid[2] - 0.1f);
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(units[i] == doctest::Approx(grid[i]).epsilon(0.01));
#if defined(__APPLE__)
    PrTextShaper* ct = prTextCoreTextCreate();
    std::vector<float> coreText = origins(ct, true);
    REQUIRE(coreText.size() == 3);
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(coreText[i] == doctest::Approx(proportional[i]).epsilon(0.01));
    prTextShaperDestroy(ct);
#endif
    prTextShaperDestroy(kb);
    prTextShaperDestroy(hb);
    prFontRelease(font);
}

TEST_CASE("an upright run that a backend sets a unit at a time joins nothing across units, and keeps marks") {
    // Arabic letters that join across take their isolated forms one at a time, and a base keeps its mark.
    const PrFont* font = load("NotoSansArabic.ttf");
    PrTextShaper* kb = prTextKbCreate();
    PrTextRun alone = runOf(font, "\xd8\xb9");
    std::vector<PrTextGlyph> isolated = shape(kb, alone);
    PrTextRun run = runOf(font, "\xd8\xb9\xd8\xb9\xd8\xb9\xd9\x8e");
    run.axis = PrTextAxis_Vertical;
    std::vector<PrTextGlyph> down = shape(kb, run);
    REQUIRE(isolated.size() == 1);
    REQUIRE(down.size() == 4);
    CHECK(down[0].id == isolated[0].id);
    CHECK(down[1].id == isolated[0].id);
    // The mark is at the place of its base on the line, which the unit leaves after the mark.
    CHECK(down[2].advance.y == doctest::Approx(0));
    CHECK(down[3].advance.y > 0.0f);
    CHECK(down[3].offset.y < down[2].offset.y);
    prTextShaperDestroy(kb);
    prFontRelease(font);
}

TEST_CASE("a font's size-adjust scales what it shapes") {
    const PrFont* font = load("NotoSans.ttf");
    PrFontDerivation d = prFontDerivationDefault();
    d.sizeAdjust = 0.5f;
    const PrFont* half = prFontCreateDerived(font, &d);
    PrTextShaper* kb = prTextKbCreate();
    PrTextGlyphBuffer* glyphs = prTextGlyphBufferCreate();
    PrTextRun run = runOf(font, "Sized");
    REQUIRE(prTextShaperShape(kb, &run, glyphs) == PrStatus_Ok);
    float full = prTextGlyphBufferAdvance(glyphs).x;
    run.font = half;
    REQUIRE(prTextShaperShape(kb, &run, glyphs) == PrStatus_Ok);
    CHECK(prTextGlyphBufferAdvance(glyphs).x == doctest::Approx(full / 2));
    prTextGlyphBufferDestroy(glyphs);
    prTextShaperDestroy(kb);
    prFontRelease(half);
    prFontRelease(font);
}

TEST_CASE("text divides into runs of the faces that set its grapheme clusters") {
    PrFontSet* set = setOf({{"NotoSans.ttf"}, {"NotoSansJP.ttf"}, {"Twemoji.ttf"}});
    PrTextFont* group = fontOf(set, {"Noto Sans", "Noto Sans JP", "twemoji-glyf_colr_1"});
    PrTextShaper* hb = prTextHarfBuzzCreate();
    // The exclamation mark goes back to the first family, which has it. The emoji, which Noto Sans JP lacks, falls
    // through to Twemoji. The script of the Han splits it from the Latin.
    std::string text = "Hi \xe6\xbc\xa2\xe5\xad\x97!\xf0\x9f\x98\x80";
    std::vector<PrTextItemRun> runs = itemize(group, text);
    CHECK(familiesOf(group, runs) ==
        std::vector<std::string>{"Noto Sans", "Noto Sans JP", "Noto Sans", "twemoji-glyf_colr_1"});
    REQUIRE(runs.size() == 4);
    CHECK(runs[1].start == 3);
    CHECK(runs[1].script == PrScript_Hani);
    CHECK(runs[2].start == 9);
    CHECK(runs[3].start == 10);
    CHECK(runs[3].end == text.size());
    // Each run shapes in the font of its face, with glyphs for all of the run.
    for (const PrTextItemRun& range : runs) {
        PrTextRun run = runOf(prTextFontMatchedFont(group, range.face), "");
        run.text = PrStringView{text.data() + range.start, range.end - range.start};
        for (const PrTextGlyph& glyph : shape(hb, run))
            CHECK(glyph.id != 0);
    }
    prTextShaperDestroy(hb);
    prTextFontRelease(group);
    prFontSetDestroy(set);
}

TEST_CASE("a family of more faces than a match keeps sets text, and holds the references it was given") {
    // Twelve faces of one family, of one style, each serving its own digit.
    PrFontSet* set = prFontSetCreate(nullptr);
    const PrFont* sans = load("NotoSans.ttf");
    PrFontCodePointRange digits[12];
    for (uint32_t i = 0; i < 12; ++i) {
        digits[i] = PrFontCodePointRange{0x30 + i % 10, 0x30 + i % 10};
        PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
        d.automatic &= PrFontFaceAutoFlags(~PrFontFaceAutoFlag_Family);
        d.family = "Digits";
        d.ranges = &digits[i];
        d.rangeCount = 1;
        prFontSetAdd(set, &d, sans);
    }
    prFontRelease(sans);
    PrTextFont* group = fontOf(set, {"Digits"});
    CHECK(itemize(group, "0123456789").size() >= 1);
    prTextFontRelease(group);
    prFontSetDestroy(set);
}

TEST_CASE("a unicode-range excludes a face from code points it has glyphs for") {
    PrFontCodePointRange kanji{0x4e00, 0x9fff};
    PrFontSet* set = setOf({{"NotoSansJP.ttf", "Kanji", &kanji}, {"NotoSans.ttf"}});
    PrTextFont* group = fontOf(set, {"Kanji", "Noto Sans"});
    CHECK(familiesOf(group, itemize(group, "A\xe6\xbc\xa2")) == std::vector<std::string>{"Noto Sans", "Noto Sans JP"});
    CHECK(itemize(group, "").empty());
    prTextFontRelease(group);
    prFontSetDestroy(set);
}

TEST_CASE("a cluster of emoji presentation tries the emoji faces first") {
    PrFontSet* set = setOf({{"NotoSansJP.ttf"}, {"Twemoji.ttf"}});
    PrTextFont* group = fontOf(set, {"Noto Sans JP", "twemoji-glyf_colr_1"});
    const PrFont* jp = prTextFontMatchedFont(group, prTextFontFirst(group));
    REQUIRE(prFontCodePointGlyph(jp, 0x2665) != 0);
    // A heart suit is text by default, and emoji when a variation selector requests it.
    CHECK(familiesOf(group, itemize(group, "\xe2\x99\xa5")) == std::vector<std::string>{"Noto Sans JP"});
    CHECK(familiesOf(group, itemize(group, "\xe2\x99\xa5\xef\xb8\x8f")) == std::vector<std::string>{"twemoji-glyf_colr_1"});
    // A grinning face is emoji by default. Requested as text, it moves past a text face that lacks it.
    CHECK(familiesOf(group, itemize(group, "\xf0\x9f\x98\x80\xef\xb8\x8e")) ==
        std::vector<std::string>{"twemoji-glyf_colr_1"});
    prTextFontRelease(group);
    prFontSetDestroy(set);
}

TEST_CASE("a run shaped with the text around it as context keeps its joining forms") {
    // The Arabic font also has Latin letters, which its range excludes. The space and the Latin letters fall to the
    // second family.
    PrFontCodePointRange range{0x600, 0x6ff};
    PrFontSet* set = setOf({{"NotoSansArabic.ttf", "Arabic", &range}, {"NotoSans.ttf"}});
    PrTextFont* group = fontOf(set, {"Arabic", "Noto Sans"});
    PrTextShaper* hb = prTextHarfBuzzCreate();
    const char* word = "\xd8\xa8\xd8\xb3\xd9\x85\xd9\x84\xd8\xa9";
    std::string text = std::string(word) + " abc";
    std::vector<PrTextItemRun> runs = itemize(group, text);
    REQUIRE(familiesOf(group, runs).front() == "Noto Sans Arabic");
    CHECK(runs[0].level == 1);
    const PrFont* arabic = prTextFontMatchedFont(group, runs[0].face);
    PrTextRun run = runOf(arabic, "");
    run.direction = PrTextDirection_Rtl;
    run.text = PrStringView{text.data(), runs[0].end};
    run.after = PrStringView{text.data() + runs[0].end, runs[0].contextEnd - runs[0].end};
    PrTextRun alone = run;
    alone.text = word;
    alone.after = PrStringView{};
    CHECK(idsOf(shape(hb, run)) == idsOf(shape(hb, alone)));
    prTextShaperDestroy(hb);
    prTextFontRelease(group);
    prFontSetDestroy(set);
}

TEST_CASE("a run of a font with full hinting takes the hinted advances") {
    const PrFont* font = load("NotoSans.ttf");
    PrTextShaper* hb = prTextHarfBuzzCreate();
    PrFontHinter* hinter = prFontFreeTypeCreate();
    for (PrFontHinting hinting : {PrFontHinting_Full, PrFontHinting_Light}) {
        PrFontDerivation d = prFontDerivationDefault();
        d.hinting = hinting;
        d.hinter = hinter;
        d.overrides = PrFontOverrideFlag_Hinting;
        const PrFont* hinted = prFontCreateDerived(font, &d);
        PrTextRun run = runOf(hinted, "minimum");
        run.kerning = PrTextKerning_None;
        run.pixelsPerEm = 11;
        for (const PrTextGlyph& g : shape(hb, run)) {
            float expected;
            if (hinting == PrFontHinting_Full)
                expected = prFontGlyphAdvance(hinted, g.id, 11);
            else
                expected = prFontGlyphAdvance(font, g.id, 0);
            CHECK(g.advance.x == doctest::Approx(expected));
            if (hinting == PrFontHinting_Full)
                CHECK(g.advance.x * 11 == doctest::Approx(std::round(g.advance.x * 11)));
        }
        prFontRelease(hinted);
    }
    // The shaper's cache holds the hinted fonts, and a hinter must outlive the fonts that name it.
    prTextShaperDestroy(hb);
    prFontHinterDestroy(hinter);
    prFontRelease(font);
}

TEST_CASE("a text layout breaks lines where text may break, the spaces a line ends in hanging past its width") {
    PrFontSet* set = setOf({{"NotoSans.ttf"}});
    PrTextFont* font = fontOf(set, {"Noto Sans"});
    PrTextShaper* hb = prTextHarfBuzzCreate();
    PrTextLayout* layout = prTextLayoutCreate();
    PrTextLayoutDesc desc{.text = "one two three", .font = font, .shaper = hb};
    prTextLayoutSet(layout, &desc);
    REQUIRE(prTextLayoutLineCount(layout) == 1);
    float whole = prTextLayoutLine(layout, 0).width;
    CHECK(prTextLayoutExtent(layout).x == whole);

    // A width that holds "one two" and not "three": the line breaks after the space, which hangs.
    desc.width = whole - 1;
    prTextLayoutSet(layout, &desc);
    REQUIRE(prTextLayoutLineCount(layout) == 2);
    PrTextLine first = prTextLayoutLine(layout, 0);
    PrTextLine second = prTextLayoutLine(layout, 1);
    CHECK(first.start == 0);
    CHECK(first.end == 8);
    CHECK(second.start == 8);
    CHECK(second.end == 13);
    CHECK(first.width < desc.width);
    CHECK(second.top == first.top + first.height);
    CHECK(prTextLayoutExtent(layout).y == first.height + second.height);
    REQUIRE(second.runCount == 1);
    PrFontGlyphRun run = prTextLayoutRun(layout, second.runStart);
    CHECK(run.glyphCount == 5);
    CHECK(run.size == 16.0f);
    CHECK(run.glyphs[0].position.x == 0.0f);
    CHECK(run.glyphs[0].position.y == second.baseline);

    // A line feed ends a line, and one at the end leaves an empty line after it.
    desc.text = "a\nb\n";
    desc.width = 0;
    prTextLayoutSet(layout, &desc);
    REQUIRE(prTextLayoutLineCount(layout) == 3);
    CHECK(prTextLayoutLine(layout, 0).end == 2);
    CHECK(prTextLayoutLine(layout, 1).start == 2);
    CHECK(prTextLayoutLine(layout, 2).start == 4);
    CHECK(prTextLayoutLine(layout, 2).end == 4);

    // Caret stops are the starts of grapheme clusters: an accent combines with the letter before it.
    desc.text = "e\xcc\x81x";
    prTextLayoutSet(layout, &desc);
    CHECK(prTextLayoutNextCaret(layout, 0) == 3);
    CHECK(prTextLayoutNextCaret(layout, 3) == 4);
    CHECK(prTextLayoutNextCaret(layout, 4) == 4);
    prTextLayoutDestroy(layout);
    prTextShaperDestroy(hb);
    prTextFontRelease(font);
    prFontSetDestroy(set);
}

TEST_CASE("a text layout aligns each line in the width the lines take, its start and end by its direction") {
    PrFontSet* set = setOf({{"NotoSans.ttf"}});
    PrTextFont* font = fontOf(set, {"Noto Sans"});
    PrTextShaper* hb = prTextHarfBuzzCreate();
    PrTextLayout* layout = prTextLayoutCreate();
    PrTextLayoutDesc desc{.text = "word", .font = font, .shaper = hb, .width = 200};
    auto leftOf = [&](PrTextDirection direction, PrTextAlign align) {
        desc.direction = direction;
        desc.align = align;
        prTextLayoutSet(layout, &desc);
        return prTextLayoutLine(layout, 0).left;
    };
    prTextLayoutSet(layout, &desc);
    float room = 200 - prTextLayoutLine(layout, 0).width;
    CHECK(leftOf(PrTextDirection_Ltr, PrTextAlign_Start) == 0.0f);
    CHECK(leftOf(PrTextDirection_Ltr, PrTextAlign_End) == room);
    CHECK(leftOf(PrTextDirection_Rtl, PrTextAlign_Start) == room);
    CHECK(leftOf(PrTextDirection_Rtl, PrTextAlign_Left) == 0.0f);
    CHECK(leftOf(PrTextDirection_Ltr, PrTextAlign_Right) == room);
    CHECK(leftOf(PrTextDirection_Ltr, PrTextAlign_Center) == room / 2);
    prTextLayoutDestroy(layout);
    prTextShaperDestroy(hb);
    prTextFontRelease(font);
    prFontSetDestroy(set);
}

TEST_CASE("a text layout orders a line's runs for display, and puts carets at their clusters' leading edges") {
    PrFontSet* set = setOf({{"NotoSans.ttf"}, {"NotoSansArabic.ttf"}});
    PrTextFont* font = fontOf(set, {"Noto Sans", "Noto Sans Arabic"});
    PrTextShaper* hb = prTextHarfBuzzCreate();
    PrTextLayout* layout = prTextLayoutCreate();
    // Latin, a space, and Arabic, in a right-to-left paragraph: the Arabic goes left of the Latin.
    std::string text = "abc \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85";
    PrTextLayoutDesc desc{.text = text, .font = font, .shaper = hb, .direction = PrTextDirection_Rtl};
    prTextLayoutSet(layout, &desc);
    REQUIRE(prTextLayoutLineCount(layout) == 1);
    PrTextLine line = prTextLayoutLine(layout, 0);
    REQUIRE(line.runCount >= 2);
    CHECK(prTextLayoutRunStart(layout, line.runStart) >= 4);
    CHECK(prTextLayoutRunStart(layout, line.runStart + line.runCount - 1) == 0);
    // The caret before the Latin is at its left edge, and before the Arabic at its right edge, left of the Latin.
    PrVec2 latin = prTextLayoutCaret(layout, 0);
    PrVec2 arabic = prTextLayoutCaret(layout, 4);
    CHECK(latin.y == line.baseline);
    CHECK(arabic.x < latin.x);
    // The end of the content of a right-to-left line is at its left.
    CHECK(prTextLayoutCaret(layout, uint32_t(text.size())).x == line.left);
    // A point left of everything is past the end of the Arabic, in its direction. A point right of everything is at the
    // end of the Latin.
    CHECK(prTextLayoutHitTest(layout, PrVec2{-10, line.baseline}) == uint32_t(text.size()));
    CHECK(prTextLayoutHitTest(layout, PrVec2{1000, line.baseline}) == 3);
    CHECK(prTextLayoutHitTest(layout, PrVec2{latin.x + 0.1f, line.baseline}) == 0);
    prTextLayoutDestroy(layout);
    prTextShaperDestroy(hb);
    prTextFontRelease(font);
    prFontSetDestroy(set);
}

TEST_CASE("a text font keeps its answers where a change of its set changes none of them") {
    Layered sets;
    const PrFont* sans = load("NotoSans.ttf");
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.family = "Web";
    prFontSetAddPending(sets.document, &d, 1);
    prFontSetAddPending(sets.document, &d, 2);
    prFontSetLoaded(sets.document, 1, sans);
    prFontSetLoaded(sets.document, 2, sans);
    PrTextFont* font = fontOf(sets.document, {"Web"});
    uint32_t a = prTextFontMatch(font, "a", PrScript_Latn);
    uint32_t first = prTextFontFirst(font);
    uint64_t revision = prTextFontRevision(font);

    // With the serving rule removed and a rule of the same font and style left, the same font serves, at the same
    // index.
    prFontSetRemove(sets.document, 2);
    CHECK_FALSE(prTextFontUpdate(font));
    CHECK(prTextFontRevision(font) == revision);
    CHECK(prTextFontMatch(font, "a", PrScript_Latn) == a);
    CHECK(prTextFontFirst(font) == first);
    // A change to a family that the text font does not name does not affect it.
    d.family = "Other";
    prFontSetAddPending(sets.document, &d, 3);
    CHECK_FALSE(prTextFontUpdate(font));

    // With the only serving face removed, the answers change.
    prFontSetRemove(sets.document, 1);
    CHECK(prTextFontUpdate(font));
    CHECK(prTextFontRevision(font) > revision);
    CHECK(familyFor(font, "a") == "Noto Serif");
    prTextFontRelease(font);
    prFontRelease(sans);
}

TEST_CASE("a load that changes an answer raises the revision") {
    Layered sets;
    sets.add("Web", 1, nullptr);
    PrTextFont* font = fontOf(sets.document, {"Web"});
    CHECK(familyFor(font, "a") == "Noto Serif");
    uint64_t revision = prTextFontRevision(font);
    const PrFont* sans = load("NotoSans.ttf");
    prFontSetLoaded(sets.document, 1, sans);
    prFontRelease(sans);
    CHECK(prTextFontUpdate(font));
    CHECK(prTextFontRevision(font) > revision);
    CHECK(familyFor(font, "a") == "Noto Sans");
    prTextFontRelease(font);
}

TEST_CASE("after a change that keeps its answers, a text font answers new questions from its set as it is") {
    Layered sets;
    sets.add("Web", 1, "NotoSans.ttf");
    PrTextFont* font = fontOf(sets.document, {"Web"});
    CHECK(familyFor(font, "a") == "Noto Sans");
    uint64_t revision = prTextFontRevision(font);

    // A face for code points that no answer reached: the answers stay, and Cyrillic text takes the new face.
    PrFontCodePointRange cyrillic{0x400, 0x4ff};
    sets.add("Web", 2, "NotoSerif.ttf", &cyrillic);
    CHECK_FALSE(prTextFontUpdate(font));
    CHECK(prTextFontRevision(font) == revision);
    CHECK(familyFor(font, "\xd0\x96", PrScript_Cyrl) == "Noto Serif");

    // A pending face that only another text font requests loads, and a later question of this text font finds it
    // loaded.
    PrFontCodePointRange greek{0x370, 0x3ff};
    sets.add("Web", 3, nullptr, &greek);
    CHECK_FALSE(prTextFontUpdate(font));
    PrTextFont* other = fontOf(sets.document, {"Web"});
    CHECK(familyFor(other, "\xce\xb1", PrScript_Grek) == "Noto Sans");
    const PrFont* serif = load("NotoSerif.ttf");
    prFontSetLoaded(sets.document, 3, serif);
    prFontRelease(serif);
    CHECK_FALSE(prTextFontUpdate(font));
    CHECK(familyFor(font, "\xce\xb1", PrScript_Grek) == "Noto Serif");
    prTextFontRelease(other);
    prTextFontRelease(font);
}

TEST_CASE("clusters of more than one code point are answered again with their scripts") {
    Layered sets;
    sets.add("Web", 1, "NotoSans.ttf");
    PrTextFont* font = fontOf(sets.document, {"Web"});
    // An e with a combining acute is kept as one question, keyed by its bytes and script.
    uint32_t accented = prTextFontMatch(font, "e\xcc\x81", PrScript_Latn);
    REQUIRE(accented != kPrTextMatchNone);
    CHECK(prTextFontMatch(font, "e\xcc\x81", PrScript_Latn) == accented);
    sets.add("Web", 2, "NotoSerif.ttf");
    CHECK(prTextFontUpdate(font));
    CHECK(familyFor(font, "e\xcc\x81") == "Noto Serif");
    prTextFontRelease(font);
}

TEST_CASE("a text font whose record passed its bound gives up its answers at the next change") {
    Layered sets;
    sets.add("Web", 1, "NotoSans.ttf");
    PrTextFont* font = fontOf(sets.document, {"Web"});
    // One cluster of a base and more combining marks than the record can keep the bytes of.
    std::string cluster = "e";
    for (int i = 0; i < 40000; ++i)
        cluster += "\xcc\x81";
    uint32_t face = prTextFontMatch(font, cluster, PrScript_Latn);
    CHECK(face != kPrTextMatchNone);
    uint64_t revision = prTextFontRevision(font);
    // The answers hold while nothing changes. A change of a face that serves none of them raises the revision, because
    // the unrecorded question cannot be asked again.
    CHECK_FALSE(prTextFontUpdate(font));
    PrFontCodePointRange cyrillic{0x400, 0x4ff};
    sets.add("Web", 2, "NotoSerif.ttf", &cyrillic);
    CHECK(prTextFontUpdate(font));
    CHECK(prTextFontRevision(font) > revision);
    prTextFontRelease(font);
}

TEST_CASE("a text font takes the set's new default family") {
    Layered sets;
    PrTextFont* font = fontOf(sets.document, {});
    CHECK(familyFor(font, "a") == "Noto Serif");
    prFontSetSetDefaultFamily(sets.document, prFontFamilyFromName("Noto Sans"));
    CHECK(prTextFontUpdate(font));
    CHECK(familyFor(font, "a") == "Noto Sans");
    prTextFontRelease(font);
}

TEST_CASE("a feature set gives a run its forms, kerning, synthesis and features, and null gives the defaults") {
    PrTextFeature features[] = {{PR_FONT_TAG('l', 'i', 'g', 'a'), 0, 0, UINT32_MAX}};
    PrTextFeatureSetDesc desc{PrTextCaps_Small, PrTextPosition_Superscript, PrTextKerning_None, 0, features, 1};
    PrTextFeatureSet* set = prTextFeatureSetCreate(&desc);
    PrTextRun run{};
    prTextFeatureSetApply(set, &run);
    CHECK(run.caps == PrTextCaps_Small);
    CHECK(run.position == PrTextPosition_Superscript);
    CHECK(run.kerning == PrTextKerning_None);
    CHECK(run.synthesis == 0);
    REQUIRE(run.featureCount == 1);
    CHECK(run.features[0].tag == PR_FONT_TAG('l', 'i', 'g', 'a'));
    // The set keeps its own copy of the features.
    features[0].value = 1;
    CHECK(run.features[0].value == 0);
    prTextFeatureSetApply(nullptr, &run);
    CHECK(run.caps == PrTextCaps_Normal);
    CHECK(run.kerning == PrTextKerning_Auto);
    CHECK(run.synthesis == (PrTextSynthesisFlag_SmallCaps | PrTextSynthesisFlag_Position));
    CHECK(run.featureCount == 0);

    PrTextFeatureSet* other = prTextFeatureSetCreate(&desc);
    CHECK_FALSE(prTextFeatureSetEquals(set, other));
    features[0].value = 0;
    PrTextFeatureSet* equal = prTextFeatureSetCreate(&desc);
    CHECK(prTextFeatureSetEquals(set, equal));
    CHECK_FALSE(prTextFeatureSetEquals(set, nullptr));
    CHECK(prTextFeatureSetEquals(nullptr, nullptr));
    prTextFeatureSetRetain(set);
    prTextFeatureSetRelease(set);
    prTextFeatureSetRelease(set);
    prTextFeatureSetRelease(other);
    prTextFeatureSetRelease(equal);
}

TEST_CASE("a text font measures the advance of a code point in the face that sets it, along or down the line") {
    PrFontSet* set = setOf({{"Ahem.ttf", "Ahem"}, {"NotoSans.ttf", "Sans"}});
    PrTextFont* ahem = fontOf(set, {"Ahem"});
    float advance = 0;
    REQUIRE(prTextFontAdvance(ahem, '0', PrScript_Zyyy, PrTextAxis_Horizontal, &advance));
    CHECK(advance == 1.0f);
    REQUIRE(prTextFontAdvance(ahem, '0', PrScript_Zyyy, PrTextAxis_Vertical, &advance));
    CHECK(advance == 1.0f);
    PrTextFont* sans = fontOf(set, {"Sans"});
    REQUIRE(prTextFontAdvance(sans, '0', PrScript_Zyyy, PrTextAxis_Horizontal, &advance));
    CHECK(advance > 0.4f);
    CHECK(advance < 0.7f);
    // No face of the set has a glyph for the water ideograph.
    CHECK_FALSE(prTextFontAdvance(sans, 0x6c34, PrScript_Hani, PrTextAxis_Horizontal, &advance));
    prTextFontRelease(sans);
    prTextFontRelease(ahem);
    prFontSetDestroy(set);
}
