// Feature drawings for fonts: font sets matching styles, faces falling back, what lib/font reads from a font beyond its
// outlines, and web fonts.

#include "tools/viewer/features_fonts.h"

#include "pr/compress_builtin.h"
#include "pr/count_of.h"
#include "pr/font_set.h"
#include "pr/svg_glyph.h"
#include "pr/text_font.h"
#include "pr/text_layout.h"
#include "pr/vec.h"

#include "tools/viewer/drawing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

using namespace pr;

namespace viewer {

namespace {

constexpr uint32_t kWght = PR_FONT_TAG('w', 'g', 'h', 't');

PrFontSet* fileSet;
PrFontSet* systemSet;
const PrFont* adjusted;
std::vector<const PrFont*> matchedFonts;      // a reference to each font that a match returned
PrDrawGlyphDecoder* svgDecoder;               // the drawings' own decoder, created on first use

std::string fontPath(const char* file) {
    return std::string(PR_FONTS_DIR) + "/" + file;
}

void addFile(PrFontSet* set, const PrFontFaceDescriptor& descriptor, const char* file) {
    std::string path = fontPath(file);
    const PrFont* font = nullptr;
    if (prFontCreateFromFile(path, 0, nullptr, &font) != PrStatus_Ok)
        return;
    prFontSetAdd(set, &descriptor, font);
    prFontRelease(font);
}

// Returns a set with Noto Sans described as three static weights under a family of their own, and the other fonts
// described as they describe themselves.
PrFontSet* fontsOfFiles() {
    if (fileSet != nullptr)
        return fileSet;
    fileSet = prFontSetCreate(nullptr);
    for (float weight : {300.0f, 400.0f, 700.0f}) {
        PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
        d.automatic = PrFontFaceAutoFlag_Width | PrFontFaceAutoFlag_Slope;
        d.family = "Three Weights";
        d.weightMin = d.weightMax = weight;
        addFile(fileSet, d, "NotoSans.ttf");
    }
    PrFontFaceDescriptor self = prFontFaceDescriptorDefault();
    for (const char* file : {"NotoSans.ttf", "NotoSerif.ttf", "Literata.ttf", "RobotoFlex.ttf"})
        addFile(fileSet, self, file);
    return fileSet;
}

PrFontSet* fontsOfSystem() {
    if (systemSet == nullptr) {
        systemSet = prFontSetCreate(nullptr);
        prFontSystemInstall(PrFontSystemBackend_Default, systemSet);
    }
    return systemSet;
}

// What a drawing asks of a set: a family, and a style.
struct Asked {
    PrFontFamily family;
    PrFontStyle style;
};

// Returns the first face that `asked` matches, held until the drawings are released, or null for none.
const PrFont* PR_NULLABLE matchFont(PrFontSet* set, const Asked& asked) {
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, asked.family, &asked.style, match);
    const PrFont* PR_NULLABLE font = prFontMatchFaceCount(match) != 0 ? prFontMatchFace(match, 0).font : nullptr;
    if (font != nullptr && std::find(matchedFonts.begin(), matchedFonts.end(), font) == matchedFonts.end()) {
        prFontRetain(font);
        matchedFonts.push_back(font);
    }
    prFontMatchDestroy(match);
    return font;
}

Asked requestOf(const char* family, float size = 16) {
    Asked asked{prFontFamilyFromName(family), prFontStyleDefault()};
    asked.style.size = size;
    return asked;
}

TextStyle styleOf(const PrFont* font, float size, PrColor tint = kInk) {
    TextStyle style;
    style.font = font;
    style.size = size;
    style.paint.color = tint;
    return style;
}

TextStyle styleOf(const char* file, float size, PrColor tint = kInk) {
    return styleOf(fontNamed(file), size, tint);
}

void guide(PrDrawList* list, PrVec2 from, PrVec2 to, PrColor tint) {
    PrDrawStroke hairline = hairlineStroke();
    prDrawListLine(list, from, to, {.color = tint, .stroke = &hairline});
}

float valueOf(const PrFont* font, uint32_t tag) {
    for (uint32_t i = 0; i < prFontAxisCount(font); ++i) {
        if (prFontAxis(font, i).tag == tag)
            return prFontAxisValue(font, i);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Fallback

// A family of a fallback drawing's set, and the tint its glyphs are drawn in.
struct Tinted {
    const char* family;
    PrColor tint;
};

// Adds the font file `file` to `set` as the family that it names, or as `family` with `ranges` if they are given.
void addFace(PrFontSet* set, const char* file, const char* PR_NULLABLE family = nullptr,
             const PrFontCodePointRange* PR_NULLABLE ranges = nullptr, uint32_t rangeCount = 0) {
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    if (family != nullptr) {
        d.automatic &= PrFontFaceAutoFlags(~PrFontFaceAutoFlag_Family);
        d.family = family;
    }
    d.ranges = ranges;
    d.rangeCount = rangeCount;
    prFontSetAdd(set, &d, fontNamed(file));
}

// Returns the set that the fallback drawings set text in: Noto Sans as the default family, falling back to Arabic,
// Devanagari, Japanese and emoji families, and Noto Serif described as a family of the digits alone.
PrFontSet* fallbackSet() {
    static PrFontSet* set = nullptr;
    if (set != nullptr)
        return set;
    set = prFontSetCreate(nullptr);
    for (const char* file : {"NotoSans.ttf", "NotoSansArabic.ttf", "NotoSansDevanagari.ttf", "NotoSansJP.ttf",
                             "Twemoji.ttf"})
        addFace(set, file);
    static const PrFontCodePointRange digits{0x30, 0x39};
    addFace(set, "NotoSerif.ttf", "Digits", &digits, 1);
    prFontSetSetDefaultFamily(set, prFontFamilyFromName("Noto Sans"));
    const PrFontFallbackFamily fallback[] = {
        {"Noto Sans Arabic", 0, PrFontGeneric_None},
        {"Noto Sans Devanagari", 0, PrFontGeneric_None},
        {"Noto Sans JP", 0, PrFontGeneric_None},
        {"twemoji-glyf_colr_1", 0, PrFontGeneric_None},
    };
    prFontSetSetFallbackFamilies(set, fallback, uint32_t(countOf(fallback)));
    return set;
}

// Returns the tint of the family of the font `font` among `families`, or the ink color if the family has none.
PrColor tintOf(const std::vector<Tinted>& families, const PrFont* font) {
    PrString family = prFontFamilyName(font);
    PrStringView name = prStringView(&family);
    PrColor tint = kInk;
    for (const Tinted& t : families) {
        if (name.size == std::strlen(t.family) && std::memcmp(name.data, t.family, name.size) == 0)
            tint = t.tint;
    }
    prStringReset(&family);
    return tint;
}

// A family list, the first preferred.
using Families = std::vector<PrFontFamily>;

// Returns the text font of `families` at `size` in the fallback set, created once and held for the life of the
// drawings.
PrTextFont* textFontOf(const Families& families, float size) {
    struct Made {
        std::string key;
        PrTextFont* font;
    };
    static std::vector<Made> made;
    std::string key = std::to_string(size);
    for (const PrFontFamily& family : families)
        key += "/" + std::string(family.name.data, family.name.size);
    for (const Made& m : made) {
        if (m.key == key)
            return m.font;
    }
    PrFontStyle style = prFontStyleDefault();
    style.size = size;
    PrTextFontDesc desc{.set = fallbackSet(), .families = families.data(), .familyCount = uint32_t(families.size()),
                        .style = style};
    made.push_back(Made{key, prTextFontCreate(&desc)});
    return made.back().font;
}

// Returns the layout that the fallback drawings set their text in, held for the life of the drawings.
PrTextLayout* fallbackLayout() {
    static PrTextLayout* layout = prTextLayoutCreate();
    return layout;
}

// Sets `text` in `families` at `size` on one line in `fallbackLayout`, with its baseline's start at `origin`. Draws
// each run in the tint of its font's family if `tinted` gives one. Returns the line's advance.
float drawByFace(PrDrawList* list, const Families& families, const std::vector<Tinted>& tinted, const char* text,
                 float size, PrVec2 origin, PrTextDirection direction = PrTextDirection_Ltr) {
    PrTextLayout* layout = fallbackLayout();
    PrTextLayoutDesc desc{.text = text, .font = textFontOf(families, size), .shaper = drawingShaper(),
                          .direction = direction};
    prTextLayoutSet(layout, &desc);
    PrTextLine line = prTextLayoutLine(layout, 0);
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{origin.x, origin.y - line.baseline}));
    for (uint32_t i = 0; i < prTextLayoutRunCount(layout); ++i) {
        PrFontGlyphRun run = prTextLayoutRun(layout, i);
        prDrawListGlyphs(list, &run, PrVec2{}, {.color = tinted.empty() ? kInk : tintOf(tinted, run.font)});
    }
    prDrawListPopTransform(list);
    return line.width;
}

// Returns the families of the fallback set, each with its tint.
const std::vector<Tinted>& mainFamilies() {
    static const std::vector<Tinted> families = {
        {"Noto Sans", kInk},           {"Noto Sans Arabic", kTeal}, {"Noto Sans Devanagari", kPurple},
        {"Noto Sans JP", kRed},        {"twemoji-glyf_colr_1", kBlue},
    };
    return families;
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawings

void fontMatching(PrDrawList* list, float, float, double) {
    PrFontSet* set = fontsOfFiles();

    // The weights that a family of three static faces returns, with css-fonts-4's order deciding between them, and bold
    // synthesized if the face is lighter than requested.
    label("\"Three Weights\": Noto Sans described as three static faces, 300, 400 and 700",
          PrVec2{30, 30}, 12, kInk);
    float x = 30;
    for (float weight = 100; weight <= 900; weight += 100) {
        Asked r = requestOf("Three Weights");
        r.style.weight = weight;
        const PrFont* font = matchFont(set, r);
        if (font == nullptr)
            continue;
        TextStyle style = styleOf(font, 30);
        char text[8];
        std::snprintf(text, sizeof(text), "%g", double(weight));
        drawText(list, style, "Ag", PrVec2{x, 80});
        label(text, PrVec2{x, 102}, 11, kInk);
        std::snprintf(text, sizeof(text), "%g%s", double(valueOf(font, kWght)),
                      prFontEmbolden(font) > 0 ? "+b" : "");
        label(text, PrVec2{x, 116}, 11, prFontEmbolden(font) > 0 ? kRed : kGray);
        x += 110;
    }
    label("under each: the weight asked, and the face's weight given (+b: synthetic bold)",
          PrVec2{500, 30});

    // A variable family: every weight and width is an instance.
    label("\"Noto Sans\" variable: the request sets wght and wdth", PrVec2{30, 160}, 12, kInk);
    float y = 200;
    for (float stretch : {62.5f, 75.0f, 100.0f}) {
        x = 30;
        for (float weight : {100.0f, 300.0f, 500.0f, 700.0f, 900.0f}) {
            Asked r = requestOf("Noto Sans");
            r.style.weight = weight;
            r.style.width = stretch;
            const PrFont* font = matchFont(set, r);
            if (font != nullptr)
                x += drawText(list, styleOf(font, 26), "Hamburg", PrVec2{x, y}) + 16;
        }
        char text[24];
        std::snprintf(text, sizeof(text), "width %g", double(stretch));
        label(text, PrVec2{x + 10, y});
        y += 40;
    }

    // Slopes: an axis if the font has one, synthesized if it does not.
    label("Slopes: slnt where the font varies it, synthesized oblique where it does not",
          PrVec2{30, 340}, 12, kInk);
    struct Slope {
        const char* family;
        PrFontSlope slope;
        float angle;
        const char* name;
    };
    const Slope slopes[] = {
        {"Roboto Flex", PrFontSlope_Normal, 0, "Roboto Flex normal"},
        {"Roboto Flex", PrFontSlope_Italic, 14, "italic: slnt -10"},
        {"Roboto Flex", PrFontSlope_Oblique, 5, "oblique 5deg: slnt -5"},
        {"Literata", PrFontSlope_Normal, 0, "Literata normal"},
        {"Literata", PrFontSlope_Italic, 14, "italic: synthetic 14deg"},
        {"Noto Serif", PrFontSlope_Oblique, 25, "oblique 25deg: synthetic"},
    };
    for (uint32_t i = 0; i < 6; ++i) {
        Asked r = requestOf(slopes[i].family);
        r.style.slope = slopes[i].slope;
        r.style.obliqueAngle = slopes[i].angle;
        const PrFont* font = matchFont(set, r);
        if (font == nullptr)
            continue;
        TextStyle style = styleOf(font, 30);
        PrVec2 at{30 + float(i % 3) * 350, 390 + float(i / 3) * 60};
        drawText(list, style, "Slanted type", at);
        label(slopes[i].name, PrVec2{at.x, at.y + 20});
    }

    // Optical sizing: opsz follows the size, or stays at the default.
    label("Optical sizing: Literata with opsz following the size (above), fixed at its default "
          "12 (below)",
          PrVec2{30, 540}, 12, kInk);
    x = 30;
    for (float size : {9.0f, 14.0f, 24.0f, 40.0f, 72.0f}) {
        Asked r = requestOf("Literata", size);
        const PrFont* sized = matchFont(set, r);
        r.style.flags &= PrFontStyleFlags(~PrFontStyleFlag_OpticalSizing);
        const PrFont* fixed = matchFont(set, r);
        if (sized == nullptr || fixed == nullptr)
            continue;
        float advance = drawText(list, styleOf(sized, size), "Hag", PrVec2{x, 660});
        drawText(list, styleOf(fixed, size), "Hag", PrVec2{x, 760});
        char text[24];
        std::snprintf(text, sizeof(text), "%gpx", double(size));
        label(text, PrVec2{x, 600});
        x += std::max(advance, 40.0f) + 40;
    }
}

void fallback(PrDrawList* list, float width, float, double) {
    const std::vector<Tinted>& tinted = mainFamilies();
    // The legend: each family in its tint.
    float x = 30;
    for (const Tinted& f : tinted) {
        label(f.family, PrVec2{x, 30}, 12, f.tint);
        x += 170;
    }
    label("The default family, then the fallback families; each glyph drawn in the tint of the family that serves it",
          PrVec2{30, 50});

    // Latin, Han, Devanagari, emoji, a ZWJ sequence, a flag and accents.
    drawByFace(list, Families{}, tinted,
               "Hello, \xe4\xb8\x96\xe7\x95\x8c! \xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8"
               "\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87 \xf0\x9f\x98\x80 \xf0\x9f\x91\xa9"
               "\xe2\x80\x8d\xf0\x9f\x9a\x80 \xf0\x9f\x87\xaf\xf0\x9f\x87\xb5 \xc3\x80"
               "\xc3\x89\xc3\x8e",
               34, PrVec2{30, 110});
    // Arabic and an emoji.
    drawByFace(list, Families{}, tinted,
               "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9"
               "\xd8\xa7\xd9\x84\xd9\x85 \xf0\x9f\x98\x80",
               34, PrVec2{30, 170}, PrTextDirection_Rtl);
    label("right to left: the runs itemization gives, ordered by their levels, each joined across them",
          PrVec2{30, 195});

    // Presentation: text by default or if requested with U+FE0E, and emoji if U+FE0F requests it.
    label("Presentation: each pair as text (default or VS15), then emoji (default or VS16); a "
          "text request no text face serves falls through to emoji",
          PrVec2{30, 250}, 12, kInk);
    // Heart suit, copyright, arrow and number sign as text then emoji, and a grinning face requested as text, which no
    // text face has.
    const char* pairs[][2] = {{"\xe2\x99\xa5", "\xe2\x99\xa5\xef\xb8\x8f"},
                              {"\xc2\xa9", "\xc2\xa9\xef\xb8\x8f"},
                              {"\xe2\x86\x94", "\xe2\x86\x94\xef\xb8\x8f"},
                              {"#", "#\xef\xb8\x8f\xe2\x83\xa3"},
                              {"\xf0\x9f\x98\x80\xef\xb8\x8e", "\xf0\x9f\x98\x80"}};
    x = 30;
    for (const char* const* pair : pairs) {
        x += drawByFace(list, Families{}, tinted, pair[0], 40, PrVec2{x, 310}) + 8;
        x += drawByFace(list, Families{}, tinted, pair[1], 40, PrVec2{x, 310}) + 40;
    }

    // Unicode ranges: a family that serves only the digits, named first.
    const std::vector<Tinted> ranged = {{"Noto Serif", kRed}, {"Noto Sans", kInk}};
    label("unicode-range: \"Digits\", Noto Serif for U+0030-0039 only, named before the default family",
          PrVec2{30, 370},
          12, kInk);
    drawByFace(list, Families{prFontFamilyFromName("Digits")}, ranged, "Order 1234 shipped 56 items on 7/8/9", 34,
               PrVec2{30, 420});

    // The system's fonts, as the platform lists them: the families of macOS and of Windows, of which those the platform
    // lacks are left out.
    PrFontSet* system = fontsOfSystem();
    char text[96];
    std::snprintf(text, sizeof(text), "System fonts: %u families", prFontSetFamilyCount(system));
    label(text, PrVec2{30, 490}, 12, kInk);
    const char* families[] = {"Helvetica", "Times", "Menlo", "Avenir Next", "Futura", "Hiragino Sans", "Gill Sans",
                              "Georgia", "Segoe UI", "Times New Roman", "Consolas", "Arial", "Calibri", "Cambria"};
    float y = 530;
    float column = 30;
    for (const char* family : families) {
        Asked r = requestOf(family);
        const PrFont* regular = matchFont(system, r);
        r.style.weight = 700;
        const PrFont* heavy = matchFont(system, r);
        if (regular == nullptr || heavy == nullptr)
            continue;
        float advance = drawText(list, styleOf(regular, 22), family, PrVec2{column, y});
        TextStyle style = styleOf(heavy, 22);
        drawText(list, style, " Bold", PrVec2{column + advance, y});
        y += 40;
        if (y > 700) {
            y = 530;
            column += width / 2;
        }
    }
    if (prFontSetFaceCount(system) == 0)
        label("(the platform lists none)", PrVec2{30, 530});
}

// Shapes `text` down a column in `style`, and across for comparison, and draws it down from `origin` on the column's
// center line. Glyphs that the font gave a vertical form are drawn in `form`, and the rest in the style's tint. Returns
// the advance down.
float drawColumn(PrDrawList* list, const TextStyle& style, const char* text, PrVec2 origin, PrColor form) {
    PrTextRun run{};
    run.font = style.font;
    run.text = text;
    run.features = style.features;
    run.featureCount = style.featureCount;
    run.pixelsPerEm = style.size;
    PrTextGlyphBuffer* buffer = prTextGlyphBufferCreate();
    std::vector<std::pair<uint32_t, uint16_t>> across;
    if (prTextShaperShape(drawingShaper(), &run, buffer) == PrStatus_Ok) {
        for (uint32_t i = 0; i < prTextGlyphBufferGlyphCount(buffer); ++i)
            across.emplace_back(prTextGlyphBufferGlyphs(buffer)[i].cluster, prTextGlyphBufferGlyphs(buffer)[i].id);
    }
    run.axis = PrTextAxis_Vertical;
    ShapedText kinds[2];            // shaped across, and in a vertical form
    PrVec2 pen = origin;
    if (prTextShaperShape(drawingShaper(), &run, buffer) == PrStatus_Ok) {
        for (uint32_t i = 0; i < prTextGlyphBufferGlyphCount(buffer); ++i) {
            const PrTextGlyph& g = prTextGlyphBufferGlyphs(buffer)[i];
            bool isForm = std::find(across.begin(), across.end(), std::make_pair(g.cluster, g.id)) == across.end();
            kinds[isForm ? 1 : 0].glyphs.push_back(
                PrFontGlyph{g.id, pen + g.offset * style.size});
            pen.y += g.advance.y * style.size;
        }
    }
    prTextGlyphBufferDestroy(buffer);
    const PrColor tints[] = {style.paint.color, form};
    for (uint32_t k = 0; k < 2; ++k) {
        TextStyle tinted = style;
        tinted.paint.color = tints[k];
        if (!kinds[k].glyphs.empty())
            drawGlyphs(list, tinted, kinds[k]);
    }
    return pen.y - origin.y;
}

void verticalText(PrDrawList* list, float width, float, double) {
    // A page of manuscript paper: columns right to left, a square an em high for each character, and the vertical forms
    // the font substitutes for punctuation, brackets, the long vowel mark and small kana.
    const char* page[] = {
        "\xe3\x80\x8c\xe7\xb8\xa6\xe6\x9b\xb8\xe3\x81\x8d\xe3\x81\xae\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
        "\xe3\x80\x81",
        "\xe5\x8f\xa5\xe8\xaa\xad\xe7\x82\xb9\xe3\x82\x84\xe6\x8b\xac\xe5\xbc\xa7\xef\xbc\x88\xe3\x81\x8b"
        "\xe3\x81\xa3\xe3\x81\x93\xef\xbc\x89\xe3\x81\xaf",
        "\xe7\xb8\xa6\xe3\x81\xae\xe5\xad\x97\xe5\xbd\xa2\xe3\x81\xab\xe3\x81\xaa\xe3\x82\x8b\xe3\x80\x82"
        "\xe3\x83\xbc\xe3\x80\x9c",
        "\xe5\xb0\x8f\xe6\x9b\xb8\xe3\x81\x8d\xe3\x81\xae\xe3\x80\x8c\xe3\x81\x81\xe3\x80\x8d\xe3\x80\x8c"
        "\xe3\x81\xa3\xe3\x80\x8d\xe3\x82\x82",
        "\xe5\xad\x97\xe3\x81\xae\xe5\x8f\xb3\xe4\xb8\x8a\xe3\x81\xab\xe5\xaf\x84\xe3\x82\x8b\xe2\x80\xa6"
        "\xe2\x80\xa6\xe3\x80\x82",
    };
    const float em = 34;
    const float pitch = 58;
    const uint32_t squares = 13;
    TextStyle jp = styleOf("NotoSansJP.ttf", em);
    PrDrawStroke hairline = hairlineStroke();
    float right = width * 0.46f;
    float top = 64;
    // The paper: a square for each character, a gutter between columns, and a frame.
    PrRect paper{right - pitch * 5 - 12, top - 14, right + 12, top + em * squares + 14};
    prDrawListRect(list, paper, {.color = PrColor{0.99f, 0.97f, 0.93f, 1}});
    prDrawListRect(list, paper, {.color = withAlpha(kRed, 0.5f), .stroke = &hairline});
    for (uint32_t c = 0; c < 5; ++c) {
        float center = right - pitch * (float(c) + 0.5f);
        for (uint32_t k = 0; k < squares; ++k) {
            PrRect square{center - em / 2, top + em * float(k), center + em / 2, top + em * float(k + 1)};
            prDrawListRect(list, square, {.color = withAlpha(kRed, 0.25f), .stroke = &hairline});
        }
        drawColumn(list, jp, page[c], PrVec2{center, top}, kRed);
    }
    label("a page set top to bottom, columns right to left, an em square for each character",
          PrVec2{paper.left, paper.bottom + 24}, 12, kInk);
    label("red: the vertical forms the font substitutes (vert), for punctuation, brackets, the long vowel mark "
          "and small kana",
          PrVec2{paper.left, paper.bottom + 44});

    // The four vertical orientations of UAX #50, each a column of its characters.
    struct Orientation {
        const char* name;
        const char* what;
        const char* file;
        const char* text;
    };
    const Orientation orientations[] = {
        {"U", "upright", "NotoSansJP.ttf", "\xe6\xbc\xa2\xe5\xad\x97\xe3\x81\x8b\xe3\x81\xaa"},
        {"Tu", "upright, in a vertical form", "NotoSansJP.ttf", "\xe3\x80\x81\xe3\x80\x82\xe3\x83\xbc"},
        {"Tr", "a vertical form", "NotoSansJP.ttf", "\xef\xbc\x88\xe3\x80\x8c\xef\xbc\x89"},
    };
    float left = width * 0.52f;
    label("UAX #50 orientations, in a vertical run", PrVec2{left, top - 24}, 12, kInk);
    for (uint32_t i = 0; i < 3; ++i) {
        const Orientation& o = orientations[i];
        float center = left + 28 + float(i) * 80;
        TextStyle style = styleOf(o.file, 30);
        float down = drawColumn(list, style, o.text, PrVec2{center, top + 30}, kRed);
        guide(list, PrVec2{center, top + 22}, PrVec2{center, top + 38 + down}, withAlpha(kBlue, 0.35f));
        label(o.name, PrVec2{center - 8, top + 12}, 14, kInk);
        label(o.what, PrVec2{center - 24, top + 60 + down + float(i % 2) * 14}, 10);
    }
    // Vertical positioning, which only a shaper that shapes down applies: the same kana, set on the em grid and with
    // the font's proportional vertical metrics (vpal). A shaper that sets each character across leaves the kana on the
    // grid.
    const char* kana = "\xe3\x83\x81\xe3\x83\xa7\xe3\x82\xb3\xe3\x83\xac\xe3\x83\xbc\xe3\x83\x88"
                       "\xe3\x81\xa3\xe3\x81\xbd\xe3\x81\x84";
    const PrTextFeature vpal{PR_FONT_TAG('v', 'p', 'a', 'l'), 1, 0, ~0u};
    float spaced = width * 0.86f;
    label("vertical positioning", PrVec2{spaced - 40, top - 24}, 12, kInk);
    for (uint32_t i = 0; i < 2; ++i) {
        TextStyle style = styleOf("NotoSansJP.ttf", 30);
        if (i == 1) {
            style.features = &vpal;
            style.featureCount = 1;
        }
        float center = spaced + float(i) * 60;
        float down = drawColumn(list, style, kana, PrVec2{center, top + 30}, kRed);
        guide(list, PrVec2{center, top + 22}, PrVec2{center, top + 38 + down}, withAlpha(kBlue, 0.35f));
        label(i == 0 ? "em grid" : "vpal", PrVec2{center - 14, top + 12}, 12, kInk);
    }
    label("vpal moves and narrows each kana down the line: HarfBuzz and CoreText, which shape down, apply it, and",
          PrVec2{spaced - 120, top + 330});
    label("kb, which sets each character across, leaves the kana on the em grid", PrVec2{spaced - 120, top + 346});

    // One glyph's vertical metrics, large: its advance down, and its vertical origin on the line's center beside its
    // horizontal origin.
    const PrFont* font = fontNamed("NotoSansJP.ttf");
    uint16_t glyph = prFontCodePointGlyph(font, 0x6f22);
    const float big = 150;
    PrVec2 at{left + 150, top + 330};
    PrVec2 origin = prFontGlyphVerticalOrigin(font, glyph);
    float across = prFontGlyphAdvance(font, glyph, 0) * big;
    float down = prFontGlyphVerticalAdvance(font, glyph) * big;
    PrVec2 horizontal{at.x - origin.x * big, at.y - origin.y * big};
    prDrawListRect(list, PrRect{at.x - across / 2, at.y, at.x + across / 2, at.y + down},
                   {.color = withAlpha(kTeal, 0.12f)});
    prDrawListRect(list, PrRect{at.x - across / 2, at.y, at.x + across / 2, at.y + down},
                   {.color = withAlpha(kTeal, 0.8f), .stroke = &hairline});
    guide(list, PrVec2{at.x, at.y - 24}, PrVec2{at.x, at.y + down + 24}, withAlpha(kRed, 0.6f));
    TextStyle large = styleOf("NotoSansJP.ttf", big);
    drawText(list, large, "\xe6\xbc\xa2", horizontal);
    prDrawListEllipse(list, at, PrVec2{4, 4}, 0, {.color = kRed});
    prDrawListEllipse(list, horizontal, PrVec2{4, 4}, 0, {.color = kBlue});
    char text[96];
    std::snprintf(text, sizeof(text), "advance %.2f em down, %.2f across", double(down / big), double(across / big));
    label(text, PrVec2{at.x - 110, at.y + down + 44}, 11, kInk);
    label("red: the vertical origin on the line's center; blue: the horizontal origin",
          PrVec2{at.x - 110, at.y + down + 62});
    label("kb, which cannot shape down, sets each character across with vert, placed the same way",
          PrVec2{at.x - 110, at.y + down + 80});
}

void metricsAndBaselines(PrDrawList* list, float width, float, double) {
    struct Line {
        const char* name;
        float PrFontMetrics::*position;
        PrFontMetricFlags flag;     // zero if the font always gives it
        PrColor tint;
        bool up;                    // true for a length above the baseline rather than a position
    };
    const Line lines[] = {
        {"ascent", &PrFontMetrics::ascent, 0, kRed, true},
        {"cap height", &PrFontMetrics::capHeight, PrFontMetricFlag_CapHeight, kGold, true},
        {"x-height", &PrFontMetrics::xHeight, PrFontMetricFlag_XHeight, kGreen, true},
        {"hanging", &PrFontMetrics::hangingBaseline, PrFontMetricFlag_Hanging, kPurple, false},
        {"ideographic over", &PrFontMetrics::ideographicOver, PrFontMetricFlag_Ideographic, kTeal,
         false},
        {"ink over", &PrFontMetrics::ideographicInkOver, PrFontMetricFlag_IdeographicInk, kBlue,
         false},
        {"central", &PrFontMetrics::centralBaseline, PrFontMetricFlag_Ideographic, kGray, false},
        {"math", &PrFontMetrics::mathBaseline, PrFontMetricFlag_Math, kPurple, false},
        {"alphabetic", &PrFontMetrics::alphabeticBaseline, 0, kInk, false},
        {"ink under", &PrFontMetrics::ideographicInkUnder, PrFontMetricFlag_IdeographicInk,
         kBlue, false},
        {"ideographic under", &PrFontMetrics::ideographicUnder, PrFontMetricFlag_Ideographic,
         kTeal, false},
        {"descent", &PrFontMetrics::descent, 0, kRed, false},
    };
    struct Sample {
        const char* file;
        const char* text;
    };
    const Sample samples[] = {{"Literata.ttf", "Hxg"}, {"NotoSansJP.ttf", "\xe6\xbc\xa2Hg"}};
    float column = width / 2;
    for (uint32_t s = 0; s < 2; ++s) {
        const PrFont* font = fontNamed(samples[s].file);
        PrFontMetrics m = prFontMetrics(font);
        float size = 130;
        PrVec2 origin{30 + column * float(s), 320};
        float advance = drawText(list, styleOf(font, size), samples[s].text, origin);
        // Each line across the text, labeled beside it. A label too close to the one above moves down, joined to its
        // line by a leader.
        struct Placed {
            float at;
            const Line* line;
        };
        std::vector<Placed> placed;
        for (const Line& line : lines) {
            float value = m.*line.position;
            placed.push_back(Placed{origin.y + (line.up ? -value : value) * size, &line});
        }
        std::stable_sort(placed.begin(), placed.end(),
                         [](const Placed& a, const Placed& b) { return a.at < b.at; });
        float right = origin.x + advance + 10;
        float labelY = 0;
        for (const Placed& p : placed) {
            const Line& line = *p.line;
            guide(list, PrVec2{origin.x - 10, p.at}, PrVec2{right, p.at},
                  withAlpha(line.tint, 0.8f));
            labelY = std::max(p.at, labelY + 13);
            guide(list, PrVec2{right, p.at}, PrVec2{right + 14, labelY},
                  withAlpha(line.tint, 0.8f));
            float value = m.*line.position + 0.0f;
            bool synthesized = line.flag != 0 && (m.fromFont & line.flag) == 0;
            char text[64];
            std::snprintf(text, sizeof(text), "%s %.3f%s", line.name, double(value),
                          synthesized ? " (synthesized)" : "");
            label(text, PrVec2{right + 18, labelY + 4}, 10, line.tint);
        }
        PrString family = prFontFamilyName(font);
        PrStringView name = prStringView(&family);
        label(std::string(name.data != nullptr ? name.data : "", name.size).c_str(),
              PrVec2{origin.x, 60}, 13, kInk);
        prStringReset(&family);

        // Subscript and superscript where the font puts them, and its line's extent.
        TextStyle body = styleOf(font, 40);
        PrVec2 at{origin.x, 560};
        float x = at.x + drawText(list, body, "X", at);
        TextStyle sub = styleOf(font, 40 * m.subscriptSize);
        x += drawText(list, sub, "sub", PrVec2{x, at.y + m.subscriptPosition * 40});
        x += drawText(list, body, "X", PrVec2{x, at.y});
        TextStyle sup = styleOf(font, 40 * m.superscriptSize);
        drawText(list, sup, "sup", PrVec2{x, at.y + m.superscriptPosition * 40});
        label("subscript and superscript as the metrics place them", PrVec2{at.x, 600});
        char text[96];
        std::snprintf(text, sizeof(text), "line: ascent %.3f + descent %.3f + gap %.3f",
                      double(m.ascent), double(m.descent), double(m.lineGap));
        label(text, PrVec2{at.x, 640}, 11, kInk);
    }

    // Overrides and size-adjust, as the descriptors of @font-face give them.
    PrFontDerivation d = prFontDerivationDefault();
    d.sizeAdjust = 0.8f;
    d.ascent = 1.2f;
    d.overrides = PrFontOverrideFlag_Ascent;
    const PrFont* base = fontNamed("NotoSans.ttf");
    if (adjusted == nullptr)
        adjusted = prFontCreateDerived(base, &d);
    float x = 30;
    x += drawText(list, styleOf(base, 40), "Noto Sans ", PrVec2{x, 740});
    drawText(list, styleOf(adjusted, 40), "size-adjust 80%, ascent-override 120%",
             PrVec2{x, 740});
    PrFontMetrics m = prFontMetrics(adjusted);
    guide(list, PrVec2{x, 740 - m.ascent * 40}, PrVec2{width - 30, 740 - m.ascent * 40},
          withAlpha(kRed, 0.8f));
    label("its ascent", PrVec2{width - 100, 740 - m.ascent * 40 - 4}, 10, kRed);
}

// Text in runs with their own styles, each shaped with the rest of the text as context.
struct Piece {
    const char* text;
    PrTextPosition position;
};

float drawPieces(PrDrawList* list, TextStyle style, const Piece* pieces, uint32_t count,
                 PrVec2 origin) {
    float x = origin.x;
    for (uint32_t i = 0; i < count; ++i) {
        style.position = pieces[i].position;
        x += drawText(list, style, pieces[i].text, PrVec2{x, origin.y});
    }
    return x - origin.x;
}

void synthesis(PrDrawList* list, float width, float, double) {
    const char* fonts[] = {"NotoSans.ttf", "Literata.ttf", "RobotoFlex.ttf"};
    float column = (width - 200) / 2;
    label("synthesis off", PrVec2{200, 40}, 13, kInk);
    label("synthesis on", PrVec2{200 + column, 40}, 13, kInk);
    struct Row {
        const char* name;
        PrTextCaps caps;
        const char* text;
    };
    const Row rows[] = {{"small caps", PrTextCaps_Small, "Small Capitals Stra\xc3\x9f" "e"},
                        {"all small caps", PrTextCaps_AllSmall, "NASA and FBI"},
                        {"petite caps", PrTextCaps_Petite, "Petite Capitals"}};
    float y = 90;
    for (const char* file : fonts) {
        PrString family = prFontFamilyName(fontNamed(file));
        PrStringView name = prStringView(&family);
        label(std::string(name.data != nullptr ? name.data : "", name.size).c_str(),
              PrVec2{20, y}, 13, kInk);
        prStringReset(&family);
        for (const Row& row : rows) {
            label(row.name, PrVec2{40, y + 24});
            for (uint32_t on = 0; on < 2; ++on) {
                TextStyle style = styleOf(file, 26);
                style.caps = row.caps;
                style.synthesis = on != 0 ? PrTextSynthesisFlag_SmallCaps : 0;
                drawText(list, style, row.text, PrVec2{200 + column * float(on), y + 24});
            }
            y += 34;
        }
        label("sub and superscripts", PrVec2{40, y + 24});
        const Piece pieces[] = {{"H", PrTextPosition_Normal},      {"2", PrTextPosition_Subscript},
                                {"O and E=mc", PrTextPosition_Normal},
                                {"2", PrTextPosition_Superscript}, {" x", PrTextPosition_Normal},
                                {"i+1", PrTextPosition_Subscript}};
        for (uint32_t on = 0; on < 2; ++on) {
            TextStyle style = styleOf(file, 26);
            style.synthesis = on != 0 ? PrTextSynthesisFlag_Position : 0;
            drawPieces(list, style, pieces, 6, PrVec2{200 + column * float(on), y + 24});
        }
        y += 60;
    }
    label("where the font has the feature both columns match; where it lacks it, synthesis "
          "scales capitals to the x-height and places scripts by the metrics",
          PrVec2{20, y + 20});
}

void palettesAndHinting(PrDrawList* list, float width, float, double) {
    // Palettes for light and dark backgrounds, and colors overridden entry by entry.
    TextStyle glaze = styleOf("KalniaGlaze.ttf", 72);
    PrFontVariation bold{kWght, 700};
    glaze.font = fontVaried("KalniaGlaze.ttf", &bold, 1);
    PrRect light{30, 40, width / 2 - 10, 250};
    PrRect dark{width / 2 + 10, 40, width - 30, 250};
    prDrawListRect(list, light, {.color = kWhite});
    prDrawListRect(list, dark, {.color = PrColor{0.12f, 0.12f, 0.14f, 1}});
    PrFontPaletteColor green{1, PrColor{0.1f, 0.8f, 0.2f, 1}};
    struct Variant {
        PrRect panel;
        PrFontPaletteFlags flags;
        const PrFontPaletteColor* colors;
        float y;
        const char* name;
    };
    const Variant variants[] = {
        {light, PrFontPaletteFlag_Light, nullptr, 110, "the light palette"},
        {light, 0, &green, 210, "palette 0, entry 1 green"},
        {dark, PrFontPaletteFlag_Dark, nullptr, 110, "the dark palette"},
        {dark, 0, nullptr, 210, "palette 0"},
    };
    for (const Variant& v : variants) {
        ShapedText shaped;
        shapeText(glaze, "Glaze", PrVec2{v.panel.left + 20, v.y}, shaped);
        PrFontGlyphRun run{};
        run.font = glaze.font;
        run.size = glaze.size;
        run.glyphs = shaped.glyphs.data();
        run.glyphCount = uint32_t(shaped.glyphs.size());
        run.transforms = shaped.transforms.empty() ? nullptr : shaped.transforms.data();
        run.palette.index = prFontPaletteSelect(glaze.font, v.flags);
        run.palette.colors = v.colors;
        run.palette.colorCount = v.colors != nullptr ? 1 : 0;
        prDrawListGlyphs(list, &run, PrVec2{}, {.color = kBlue});
        char text[64];
        std::snprintf(text, sizeof(text), "%s (%u)", v.name, run.palette.index);
        label(text, PrVec2{v.panel.left + 20, v.y + 24});
    }

    // Hinting: outlines fitted to a small size's pixel grid by the drawing's hinter, shown enlarged over the grid.
    // Without a hinter, every mode draws the design outlines.
    auto hinted = [](uint32_t mode) {
        PrFontDerivation d = prFontDerivationDefault();
        d.hinting = PrFontHinting(mode);
        d.hinter = drawingHinter();
        d.overrides = PrFontOverrideFlag_Hinting;
        return fontDerived("NotoSans.ttf", d);
    };
    constexpr float kPixelsPerEm = 11;
    constexpr float kZoom = 14;
    const char* modes[] = {"none", "light", "full"};
    const char* letters = "aeg";
    for (uint32_t mode = 0; mode < 3; ++mode) {
        PrVec2 origin{40 + float(mode) * 350, 470};
        // The pixel grid.
        for (int i = 0; i <= 22; ++i) {
            float at = origin.x + float(i) * kZoom;
            guide(list, PrVec2{at, origin.y - 12 * kZoom}, PrVec2{at, origin.y + 4 * kZoom},
                  withAlpha(kInk, 0.12f));
        }
        for (int j = -12; j <= 4; ++j) {
            float at = origin.y + float(j) * kZoom;
            guide(list, PrVec2{origin.x, at}, PrVec2{origin.x + 22 * kZoom, at},
                  withAlpha(kInk, j == 0 ? 0.5f : 0.12f));
        }
        const PrFont* sans = hinted(mode);
        float pen = 0;
        for (const char* c = letters; *c != 0; ++c) {
            uint16_t glyph = prFontCodePointGlyph(sans, uint32_t(*c));
            PrPath* path = prPathCreate();
            PrAffine2 t = prAffine2Concat(
                prAffine2Scale(PrVec2{kPixelsPerEm * kZoom, kPixelsPerEm * kZoom}),
                prAffine2Translate(PrVec2{origin.x + std::round(pen) * kZoom, origin.y}));
            prFontGlyphOutline(sans, glyph, kPixelsPerEm, t, path);
            prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = withAlpha(kBlue, 0.6f)});
            prPathDestroy(path);
            pen += prFontGlyphAdvance(sans, glyph, kPixelsPerEm) * kPixelsPerEm;
        }
        char text[48];
        std::snprintf(text, sizeof(text), "%s, at %g pixels an em", modes[mode],
                      double(kPixelsPerEm));
        label(text, PrVec2{origin.x, origin.y + 5 * kZoom});
    }

    // Small text in each mode, on target pixels.
    const char* sample = "Hinting fits outlines to the pixel grid: 0123456789";
    for (uint32_t mode = 0; mode < 3; ++mode) {
        float y = 620 + float(mode) * 60;
        label(modes[mode], PrVec2{30, y});
        float x = 100;
        for (float size : {9.0f, 11.0f, 13.0f}) {
            TextStyle style = styleOf(hinted(mode), size);
            drawText(list, style, sample, PrVec2{x, y});
            x += size * 26;
        }
    }
    label("full hinting fits advances to whole pixels as well, at the page's size: on a display "
          "that scales the page, the fit is for the wrong size",
          PrVec2{30, 800});
}

void contextAndGraphemes(PrDrawList* list, float, float, double) {
    // An Arabic word in two colors, split into two runs. Shaped alone, each half takes its isolated forms. Shaped with
    // the other half as context, the halves join.
    const char* word = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7";
    size_t split = 6;  // after the dual-joining hah, which joins the beh after it
    const PrFont* arabic = fontNamed("NotoSansArabic.ttf");
    label("An Arabic word split between two runs of different colors", PrVec2{30, 40}, 12,
          kInk);
    float y = 110;
    for (uint32_t withContext = 0; withContext < 2; ++withContext) {
        PrTextGlyphBuffer* glyphs = prTextGlyphBufferCreate();
        PrVec2 origin{30 + float(withContext) * 300, y};
        // Right to left: the second half first, from the left.
        float x = origin.x;
        for (uint32_t half = 2; half-- > 0;) {
            PrTextRun run{};
            run.font = arabic;
            run.direction = PrTextDirection_Rtl;
            run.text = half == 0 ? PrStringView{word, split}
                                 : PrStringView{word + split, std::strlen(word) - split};
            if (withContext != 0) {
                if (half == 0)
                    run.after = PrStringView{word + split, std::strlen(word) - split};
                else
                    run.before = PrStringView{word, split};
            }
            if (prTextShaperShape(drawingShaper(), &run, glyphs) != PrStatus_Ok)
                continue;
            std::vector<PrFontGlyph> placed;
            const PrTextGlyph* data = prTextGlyphBufferGlyphs(glyphs);
            for (uint32_t i = 0; i < prTextGlyphBufferGlyphCount(glyphs); ++i) {
                placed.push_back(PrFontGlyph{
                    data[i].id, PrVec2{x + data[i].offset.x * 48, y + data[i].offset.y * 48}});
                x += data[i].advance.x * 48;
            }
            PrFontGlyphRun r{};
            r.font = arabic;
            r.size = 48;
            r.glyphs = placed.data();
            r.glyphCount = uint32_t(placed.size());
            prDrawListGlyphs(list, &r, PrVec2{}, {.color = half == 0 ? kRed : kBlue});
        }
        prTextGlyphBufferDestroy(glyphs);
        label(withContext != 0 ? "with context" : "alone", PrVec2{origin.x, y + 30});
    }

    // Grapheme clusters: a caret stops before each glyph that starts one.
    label("Grapheme clusters: a caret mark before each glyph that starts one", PrVec2{30, 240},
          12, kInk);
    const char* samples[] = {
        ("e\xcc\x81 n\xcc\x83 a\xcc\x8a\xcc\x81 \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x9a\x80 "
         "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd \xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x87\xab"
         "\xf0\x9f\x87\xb7"),
        ("\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87 \xe0\xa4\x95"
         "\xe0\xa5\x8d\xe0\xa4\xb7\xe0\xa4\xa4\xe0\xa5\x8d\xe0\xa4\xb0\xe0\xa4\xbf\xe0\xa4\xaf"),
    };
    y = 320;
    for (const char* sample : samples) {
        float size = 48;
        drawByFace(list, Families{}, {}, sample, size, PrVec2{30, y});
        // drawByFace left the sample in the fallback layout. Draws a guide at each caret stop.
        PrTextLayout* layout = fallbackLayout();
        uint32_t end = uint32_t(std::strlen(sample));
        for (uint32_t at = 0; at < end; at = prTextLayoutNextCaret(layout, at)) {
            float x = 30 + prTextLayoutCaret(layout, at).x;
            guide(list, PrVec2{x, y - size}, PrVec2{x, y + 12}, withAlpha(kRed, 0.7f));
        }
        y += 100;
    }
}

void bitmapGlyphs(PrDrawList* list, float, float, double) {
    // One sbix strike of 109 pixels per em, at sizes around it: scaled down, and scaled up past it.
    label("sbix (Twemoji smiley): one strike of 109 pixels an em, drawn at each size",
          PrVec2{30, 30}, 12, kInk);
    const PrFont* smiley = fontNamed("TwemojiSmileySbix.ttf");
    float x = 30;
    for (float size : {16.0f, 32.0f, 64.0f, 109.0f, 180.0f}) {
        PrFontGlyph glyph{2, PrVec2{x, 60 + size}};
        PrFontGlyphRun run{};
        run.font = smiley;
        run.size = size;
        run.glyphs = &glyph;
        run.glyphCount = 1;
        prDrawListGlyphs(list, &run, PrVec2{}, {.color = kInk});
        char text[16];
        std::snprintf(text, sizeof(text), "%gpx", double(size));
        label(text, PrVec2{x, 270});
        x += size * 1.1f + 30;
    }

    // Regional indicator pairs that the font's GSUB joins into flags.
    label("sbix (Noto flags): regional indicators shaped into flags", PrVec2{30, 320}, 12, kInk);
    TextStyle flags = styleOf("NotoFlagsSbix.ttf", 56);
    drawText(list, flags,
        "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x87\xab\xf0\x9f\x87\xb7\xf0\x9f"
        "\x87\xa7\xf0\x9f\x87\xb7\xf0\x9f\x87\xb0\xf0\x9f\x87\xaa\xf0\x9f\x87\xa8"
        "\xf0\x9f\x87\xa6\xf0\x9f\x87\xba\xf0\x9f\x87\xa6", PrVec2{30, 395});

    // CBDT: one strike of 109 pixels per em, shaped, with keycaps, flags and ZWJ sequences.
    label("CBDT (Noto Color Emoji): one strike of 109 pixels an em, drawn at each size",
          PrVec2{30, 450}, 12, kInk);
    float y = 490;
    for (float size : {16.0f, 28.0f, 52.0f}) {
        TextStyle emoji = styleOf("NotoColorEmoji.ttf", size);
        y += size * 1.3f;
        drawText(list, emoji,
                 "\xf0\x9f\x98\x80\xf0\x9f\x9a\x80\xf0\x9f\x90\x99"
                 "8\xef\xb8\x8f\xe2\x83\xa3#\xef\xb8\x8f\xe2\x83\xa3"
                 "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x87\xb0\xf0\x9f\x87\xaa"
                 "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x9a\x80",
                 PrVec2{90, y});
        char text[24];
        std::snprintf(text, sizeof(text), "%gpx", double(size));
        label(text, PrVec2{30, y});
    }

    // The system's emoji, if it has them.
    const PrFont* apple = matchFont(fontsOfSystem(), requestOf("Apple Color Emoji"));
    label("Apple Color Emoji, from the system's fonts", PrVec2{30, 700}, 12, kInk);
    if (apple != nullptr) {
        TextStyle emoji = styleOf(apple, 48);
        drawText(list, emoji,
            "\xf0\x9f\x98\x80\xf0\x9f\x9a\x80\xf0\x9f\x8c\x88\xf0\x9f\x90\x99\xf0\x9f"
            "\x8e\x89\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x9a\x80\xf0\x9f\x87\xaf\xf0"
            "\x9f\x87\xb5", PrVec2{30, 770});
    } else {
        label("(not installed here)", PrVec2{30, 760});
    }
}

// Draws Nabla, whose color glyphs are both COLR layers and SVG documents, each way in two of its palettes.
void svgGlyphs(PrDrawList* list, float, float, double) {
    if (svgDecoder == nullptr)
        svgDecoder = prSvgGlyphDecoderCreate(nullptr, nullptr);
    const PrFont* nabla = fontNamed("Nabla.ttf");
    label("Nabla: its COLR layers, then the same glyphs drawn from its SVG documents, in two palettes",
          PrVec2{30, 30}, 12, kInk);
    float y = 140;
    for (uint32_t palette : {0u, 1u}) {
        TextStyle style = styleOf(nabla, 72);
        style.palette = palette;
        ShapedText shaped;
        shapeText(style, "Nabla", PrVec2{30, y}, shaped);
        drawGlyphs(list, style, shaped);
        PrFontGlyphRun run{};
        run.font = nabla;
        run.size = style.size;
        run.glyphs = shaped.glyphs.data();
        run.glyphCount = uint32_t(shaped.glyphs.size());
        run.palette.index = palette;
        for (uint32_t i = 0; i < run.glyphCount; ++i) {
            PrAffine2 placed = prAffine2Concat(prDrawGlyphTransform(&run, i), prAffine2Translate(PrVec2{320, 0}));
            prDrawListPushTransformAffine2(list, placed);
            prDrawGlyphDecoderDecode(svgDecoder, nabla, run.glyphs[i].id, style.size, &run.palette, kInk, list);
            prDrawListPopTransform(list);
        }
        char text[32];
        std::snprintf(text, sizeof(text), "palette %u: COLR, SVG", palette);
        label(text, PrVec2{30, y + 30});
        y += 150;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Web fonts

constexpr uint32_t kOpsz = PR_FONT_TAG('o', 'p', 's', 'z');

const PrFont* collectionFaces[2];             // the faces of a WOFF2 collection, loaded on first use

std::vector<uint8_t> fileBytes(const char* file) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(fontPath(file).c_str(), "rb")) {
        uint8_t buffer[65536];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), f)) != 0)
            bytes.insert(bytes.end(), buffer, buffer + read);
        std::fclose(f);
    }
    return bytes;
}

// The transforms a WOFF2 file's table directory names (woff2 4.1): glyf and loca rebuilt point by point, and hmtx's
// bearings taken from the glyphs' boxes.
struct Woff2Transforms {
    uint32_t tables = 0;
    bool glyf = false;
    bool hmtx = false;
};

Woff2Transforms transformsOf(const std::vector<uint8_t>& bytes) {
    Woff2Transforms out;
    if (bytes.size() < 48)
        return out;
    out.tables = uint32_t(bytes[12]) << 8 | bytes[13];
    size_t at = 48;
    auto base128 = [&]() {
        while (at < bytes.size() && (bytes[at++] & 0x80) != 0) {
        }
    };
    for (uint32_t i = 0; i < out.tables && at < bytes.size(); ++i) {
        const uint8_t flags = bytes[at++];
        std::string tag;
        if ((flags & 63) == 63) {
            tag.assign(reinterpret_cast<const char*>(bytes.data() + at), 4);
            at += 4;
        }
        const uint32_t known = flags & 63;
        const uint32_t version = flags >> 6;
        const bool glyfOrLoca = known == 10 || known == 11 || tag == "glyf" || tag == "loca";
        const bool hmtx = known == 3 || tag == "hmtx";
        const bool transformed = (glyfOrLoca && version == 0) || (hmtx && version == 1);
        base128();
        if (transformed)
            base128();
        out.glyf = out.glyf || (glyfOrLoca && transformed);
        out.hmtx = out.hmtx || (hmtx && transformed);
    }
    return out;
}

void dot(PrDrawList* list, PrVec2 at, float radius, PrColor tint, bool hollow) {
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, at, PrVec2{radius, radius}, 0);
    PrDrawStroke stroke = strokeOf(1.25f);
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = tint, .stroke = hollow ? &stroke : nullptr});
    prPathDestroy(path);
}

void webFonts(PrDrawList* list, float, float, double) {
    const char* woff2File = "Literata-Latin.woff2";
    const char* woffFile = "Literata-Latin.woff";
    const PrFont* fromWoff2 = fontNamed(woff2File);
    const PrFont* fromWoff = fontNamed(woffFile);
    const std::vector<uint8_t> woff2Bytes = fileBytes(woff2File);
    const Woff2Transforms transforms = transformsOf(woff2Bytes);

    // Literata's Latin subset as WOFF2 and as WOFF, each beside the font file it decodes to. Fontsource builds the two
    // files separately, so their font files differ slightly.
    label("Literata's Latin subset as WOFF2 and as WOFF, and the font files they hold", PrVec2{30, 40}, 13, kInk);
    struct Row {
        const char* name;
        size_t size;
        size_t decoded;
        PrColor tint;
        const PrFont* font;
    };
    const Row rows[] = {
        {"WOFF2: one Brotli stream", woff2Bytes.size(), prFontData(fromWoff2).size, kBlue, fromWoff2},
        {"WOFF: zlib, table by table", fileBytes(woffFile).size(), prFontData(fromWoff).size, kTeal, fromWoff},
    };
    const float largest = float(std::max(rows[0].decoded, rows[1].decoded));
    for (uint32_t i = 0; i < 2; ++i) {
        const Row& row = rows[i];
        const float y = 84 + float(i) * 110;
        label(row.name, PrVec2{30, y + 4}, 12, kInk);
        // The font file's bar, with the bar of the compressed file over it.
        const float whole = 280 * float(row.decoded) / largest;
        const float part = 280 * float(row.size) / largest;
        prDrawListRect(list, PrRect{200, y - 10, 200 + whole, y + 8}, {.color = withAlpha(kGray, 0.35f)});
        prDrawListRect(list, PrRect{200, y - 10, 200 + part, y + 8}, {.color = row.tint});
        char text[96];
        std::snprintf(text, sizeof(text), "%zu bytes, %.0f%% of its font file's %zu", row.size,
                      100.0 * double(row.size) / double(row.decoded), row.decoded);
        label(text, PrVec2{200, y + 24});
        drawText(list, styleOf(row.font, 40, row.tint), "Hamburgefonstiv", PrVec2{30, y + 74});
    }

    // What WOFF2 rebuilds: the transformed glyf table's points, each glyph's box computed from them, and the bearings
    // that the transformed hmtx table leaves to the boxes.
    label("What WOFF2 rebuilds from its transformed tables", PrVec2{600, 40}, 13, kInk);
    char listed[160];
    std::snprintf(listed, sizeof(listed), "%u tables; glyf and loca %s; hmtx %s", transforms.tables,
                  transforms.glyf ? "transformed, rebuilt point by point" : "stored",
                  transforms.hmtx ? "transformed, bearings from the boxes" : "stored");
    label(listed, PrVec2{600, 62});
    const float size = 260;
    const PrVec2 origin{680, 330};
    const uint16_t glyph = prFontCodePointGlyph(fromWoff2, 'g');
    const PrAffine2 toPage = prAffine2Concat(prAffine2Scale(PrVec2{size, size}), prAffine2Translate(origin));
    PrPath* outline = prPathCreate();
    prFontGlyphOutline(fromWoff2, glyph, 0, toPage, outline);
    prDrawListPath(list, outline, PrPathFillRule_NonZero, {.color = withAlpha(kBlue, 0.12f)});
    PrDrawStroke hairline = hairlineStroke();
    prDrawListPath(list, outline, PrPathFillRule_NonZero, {.color = withAlpha(kBlue, 0.7f), .stroke = &hairline});
    const PrPathVerb* verbs = prPathVerbs(outline);
    const PrVec2* points = prPathPoints(outline);
    uint32_t at = 0;
    PrVec2 last{};
    for (uint32_t v = 0; v < prPathVerbCount(outline); ++v) {
        if (verbs[v] == PrPathVerb_Move || verbs[v] == PrPathVerb_Line) {
            last = points[at++];
            dot(list, last, 3, kInk, false);
        } else if (verbs[v] == PrPathVerb_Quad) {
            guide(list, last, points[at], withAlpha(kRed, 0.4f));
            guide(list, points[at], points[at + 1], withAlpha(kRed, 0.4f));
            dot(list, points[at], 3, kRed, true);
            last = points[at + 1];
            dot(list, last, 3, kInk, false);
            at += 2;
        } else if (verbs[v] == PrPathVerb_Cubic) {
            last = points[at + 2];
            at += 3;
        }
    }
    prPathDestroy(outline);
    const PrRect bounds = prFontGlyphBounds(fromWoff2, glyph);
    const PrRect box{origin.x + bounds.left * size, origin.y + bounds.top * size, origin.x + bounds.right * size,
                     origin.y + bounds.bottom * size};
    PrDrawStroke boxStroke = strokeOf(1.5f);
    prDrawListRect(list, box, {.color = kGold, .stroke = &boxStroke});
    const float advance = prFontGlyphAdvance(fromWoff2, glyph, 0) * size;
    guide(list, PrVec2{origin.x - 40, origin.y}, PrVec2{origin.x + advance + 40, origin.y}, withAlpha(kInk, 0.5f));
    guide(list, PrVec2{origin.x, box.top - 20}, PrVec2{origin.x, box.bottom + 40}, withAlpha(kInk, 0.5f));
    guide(list, PrVec2{origin.x + advance, box.top - 20}, PrVec2{origin.x + advance, box.bottom + 40},
          withAlpha(kInk, 0.5f));
    const float bearingY = box.bottom + 24;
    guide(list, PrVec2{origin.x, bearingY}, PrVec2{box.left, bearingY}, kPurple);
    label("on-curve points", PrVec2{900, 120}, 12, kInk);
    dot(list, PrVec2{890, 116}, 3, kInk, false);
    label("control points", PrVec2{900, 142}, 12, kRed);
    dot(list, PrVec2{890, 138}, 3, kRed, true);
    label("box: computed from the points", PrVec2{900, 164}, 12, kGold);
    label(transforms.hmtx ? "left side bearing: the box's left" : "left side bearing: from hmtx",
          PrVec2{900, 186}, 12, kPurple);
    label("origin and advance", PrVec2{900, 208}, 12, kInk);

    // A variable font in WOFF2: its axes and their variation tables come through, so each instance draws.
    label("A variable font in WOFF2: Literata across its weight and optical size axes", PrVec2{30, 330}, 13, kInk);
    const char* variableFile = "Literata-Latin-Variable.woff2";
    const PrFont* variable = fontNamed(variableFile);
    std::string axes;
    for (uint32_t i = 0; i < prFontAxisCount(variable); ++i) {
        const PrFontAxis axis = prFontAxis(variable, i);
        char text[64];
        const char tag[5] = {char(axis.tag >> 24), char(axis.tag >> 16), char(axis.tag >> 8), char(axis.tag), 0};
        std::snprintf(text, sizeof(text), "%s%s %g to %g", axes.empty() ? "" : ", ", tag, double(axis.min),
                      double(axis.max));
        axes += text;
    }
    char described[160];
    std::snprintf(described, sizeof(described), "%zu bytes of WOFF2; axes %s", fileBytes(variableFile).size(),
                  axes.c_str());
    label(described, PrVec2{30, 352});
    const float sizes[] = {8, 18, 48};
    const float weights[] = {200, 400, 600, 900};
    for (uint32_t column = 0; column < 3; ++column) {
        char text[32];
        std::snprintf(text, sizeof(text), "opsz %g", double(sizes[column]));
        label(text, PrVec2{90 + float(column) * 160, 384});
    }
    for (uint32_t row = 0; row < 4; ++row) {
        const float y = 430 + float(row) * 56;
        char text[32];
        std::snprintf(text, sizeof(text), "wght %g", double(weights[row]));
        label(text, PrVec2{30, y});
        for (uint32_t column = 0; column < 3; ++column) {
            const PrFontVariation values[] = {{kWght, weights[row]}, {kOpsz, sizes[column]}};
            const PrFont* instance = fontVaried(variableFile, values, 2);
            drawText(list, styleOf(instance, 36), "Rag", PrVec2{90 + float(column) * 160, y});
        }
    }

    // A collection in one WOFF2 file: its fonts share the file, and the font file it decodes to is a collection.
    label("A collection in one WOFF2 file: WPT's two test faces", PrVec2{600, 470}, 13, kInk);
    if (collectionFaces[0] == nullptr) {
        const std::vector<uint8_t> bytes = fileBytes("woff2/available-002.woff2");
        for (uint32_t index = 0; index < 2; ++index)
            prFontCreateWoff(bytes, index, nullptr, prCompressBuiltinCreate(),
                             &collectionFaces[index]);
    }
    for (uint32_t index = 0; index < 2; ++index) {
        const PrFont* face = collectionFaces[index];
        const float y = 560 + float(index) * 120;
        if (face == nullptr) {
            label("(./dev.sh fetch fonts)", PrVec2{600, y});
            continue;
        }
        PrString family = prFontFamilyName(face);
        PrStringView name = prStringView(&family);
        char text[128];
        std::snprintf(text, sizeof(text), "font %u of %u: %.*s, %u glyphs", index, prFontCount(prFontData(face)),
                      int(name.size), name.data, prFontGlyphCount(face));
        prStringReset(&family);
        label(text, PrVec2{600, y - 50}, 12, kInk);
        drawText(list, styleOf(face, 72, index == 0 ? kBlue : kTeal), "P", PrVec2{600, y + 30});
    }
}

} // namespace

void releaseDrawingFonts() {
    prDrawGlyphDecoderDestroy(svgDecoder);
    svgDecoder = nullptr;
    for (const PrFont* font : matchedFonts)
        prFontRelease(font);
    matchedFonts.clear();
    prFontRelease(adjusted);
    adjusted = nullptr;
    prFontSetDestroy(fileSet);
    fileSet = nullptr;
    prFontSetDestroy(systemSet);
    systemSet = nullptr;
    for (const PrFont*& face : collectionFaces) {
        prFontRelease(face);
        face = nullptr;
    }
}

const std::vector<Drawing>& fontFeatures() {
    static const std::vector<Drawing> features = {
        {"Font matching", "Font sets: weights, widths, slopes and optical sizes matched to faces.",
         fontMatching},
        {"Fallback", "Faces falling back per grapheme cluster, emoji presentation, and system "
                     "fonts.",
         fallback},
        {"Vertical text", "Top-to-bottom runs with vertical forms and metrics.", verticalText},
        {"Metrics and baselines", "Every metric and baseline a font gives or synthesizes.",
         metricsAndBaselines},
        {"Synthesized variants", "Small capitals and sub- and superscripts, from the font or "
                                 "synthesized.",
         synthesis},
        {"Bitmap glyphs", "Color glyphs as images in strikes of set sizes: sbix and CBDT.",
         bitmapGlyphs},
        {"SVG glyphs", "Color glyphs as SVG documents, beside the same glyphs' color layers.", svgGlyphs},
        {"Palettes and hinting", "Light and dark palettes, color overrides, and outlines fitted to "
                                 "the pixel grid.",
         palettesAndHinting},
        {"Context and graphemes", "Runs shaped with their context, and grapheme cluster starts.",
         contextAndGraphemes},
        {"Web fonts", "WOFF and WOFF2: a font in each, what WOFF2 rebuilds, a variable font and a collection.",
         webFonts},
    };
    return features;
}

} // namespace viewer
