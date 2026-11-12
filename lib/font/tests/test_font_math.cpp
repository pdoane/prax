// Math fonts: the MATH table of New Computer Modern Math and of WPT's math test fonts, whose values their generators
// set, the fallbacks of fonts without one, and MATH tables that are malformed.

#include "pr/compress_builtin.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/font_math.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

constexpr uint32_t kMath = PR_FONT_TAG('M', 'A', 'T', 'H');

std::vector<uint8_t> readPath(const std::string& path) {
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

std::vector<uint8_t> newCmBytes() {
    return readPath(std::string(PR_FONTS_DIR) + "/NewCMMath-Regular.otf");
}

const PrFont* fromBytes(const std::vector<uint8_t>& bytes) {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Loads one of WPT's math test fonts, which are WOFF files.
const PrFont* wptFont(const std::string& name) {
    std::vector<uint8_t> bytes = readPath(std::string(PR_FONTS_DIR) + "/math/" + name + ".woff");
    PrCompressCodec* codec = prCompressBuiltinCreate();
    const PrFont* font = nullptr;
    PrStatus status = prFontCreateWoff(bytes, 0, nullptr, codec, &font);
    prCompressCodecDestroy(codec);
    REQUIRE(status == PrStatus_Ok);
    return font;
}

uint32_t be32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

uint16_t be16(const uint8_t* bytes) {
    return uint16_t(bytes[0] << 8 | bytes[1]);
}

void put16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value);
}

void set16(std::vector<uint8_t>& out, size_t at, uint32_t value) {
    out[at] = uint8_t(value >> 8);
    out[at + 1] = uint8_t(value);
}

// Returns the bytes of table `tag` of a single font file, or empty if it has none.
std::vector<uint8_t> tableOf(const std::vector<uint8_t>& font, uint32_t tag) {
    uint16_t count = be16(font.data() + 4);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* record = font.data() + 12 + i * 16;
        if (be32(record) == tag)
            return std::vector<uint8_t>(font.data() + be32(record + 8), font.data() + be32(record + 8) + be32(record + 12));
    }
    return {};
}

// Returns the single font file `font` with table `tag` replaced by `table`, or added if it has none.
std::vector<uint8_t> withTable(const std::vector<uint8_t>& font, uint32_t tag, const std::vector<uint8_t>& table) {
    struct Table {
        uint32_t tag;
        std::vector<uint8_t> bytes;
    };
    std::vector<Table> tables;
    uint16_t count = be16(font.data() + 4);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* record = font.data() + 12 + i * 16;
        if (be32(record) != tag)
            tables.push_back(Table{be32(record), tableOf(font, be32(record))});
    }
    tables.push_back(Table{tag, table});
    std::sort(tables.begin(), tables.end(), [](const Table& a, const Table& b) { return a.tag < b.tag; });
    std::vector<uint8_t> out(font.begin(), font.begin() + 4);
    put16(out, uint32_t(tables.size()));
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    uint32_t offset = uint32_t(12 + tables.size() * 16);
    for (const Table& t : tables) {
        put32(out, t.tag);
        put32(out, 0);
        put32(out, offset);
        put32(out, uint32_t(t.bytes.size()));
        offset += (uint32_t(t.bytes.size()) + 3) & ~3u;
    }
    for (const Table& t : tables) {
        out.insert(out.end(), t.bytes.begin(), t.bytes.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    return out;
}

// Stretches and returns the glyphs, checking that a call with too little room writes only what fits.
std::vector<PrFontGlyph> stretch(const PrFont* font, uint16_t glyph, PrFontMathAxis axis, float size,
        PrFontMathStretchInfo* info) {
    uint32_t count = prFontMathGlyphStretch(font, glyph, axis, size, info, nullptr, 0);
    std::vector<PrFontGlyph> glyphs(count + 1, PrFontGlyph{0xFFFF, PrVec2{0, 0}});
    CHECK(prFontMathGlyphStretch(font, glyph, axis, size, info, glyphs.data(), count) == count);
    CHECK(glyphs[count].id == 0xFFFF);
    glyphs.pop_back();
    return glyphs;
}

std::vector<uint16_t> ids(const std::vector<PrFontGlyph>& glyphs) {
    std::vector<uint16_t> out;
    for (const PrFontGlyph& g : glyphs)
        out.push_back(g.id);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("New Computer Modern Math's constants are those of its MATH table, in ems") {
    const PrFont* font = fromBytes(newCmBytes());
    CHECK(prFontHasMath(font));
    const PrFontMathConstants& c = *prFontMathConstants(font);
    CHECK(c.scriptPercentScaleDown == doctest::Approx(0.70));
    CHECK(c.scriptScriptPercentScaleDown == doctest::Approx(0.50));
    CHECK(c.delimitedSubFormulaMinHeight == doctest::Approx(1.3));
    CHECK(c.displayOperatorMinHeight == doctest::Approx(1.3));
    CHECK(c.mathLeading == doctest::Approx(0.154));
    CHECK(c.axisHeight == doctest::Approx(0.25));
    CHECK(c.accentBaseHeight == doctest::Approx(0.45));
    CHECK(c.flattenedAccentBaseHeight == doctest::Approx(0.664));
    CHECK(c.subscriptShiftDown == doctest::Approx(0.247));
    CHECK(c.superscriptShiftUp == doctest::Approx(0.363));
    CHECK(c.superscriptShiftUpCramped == doctest::Approx(0.289));
    CHECK(c.spaceAfterScript == doctest::Approx(0.056));
    CHECK(c.lowerLimitBaselineDropMin == doctest::Approx(0.6));
    CHECK(c.fractionDenominatorDisplayStyleShiftDown == doctest::Approx(0.686));
    CHECK(c.skewedFractionHorizontalGap == doctest::Approx(0.35));
    CHECK(c.underbarExtraDescender == doctest::Approx(0.04));
    CHECK(c.radicalDisplayStyleVerticalGap == doctest::Approx(0.148));
    CHECK(c.radicalKernBeforeDegree == doctest::Approx(0.278));
    CHECK(c.radicalKernAfterDegree == doctest::Approx(-0.556));
    CHECK(c.radicalDegreeBottomRaisePercent == doctest::Approx(0.60));
    prFontRelease(font);
}

TEST_CASE("New Computer Modern Math's glyph info: italics correction, accent attachment, shapes and kerns") {
    const PrFont* font = fromBytes(newCmBytes());
    uint16_t f = prFontCodePointGlyph(font, 'f');
    uint16_t integral = prFontCodePointGlyph(font, 0x222B);
    uint16_t italicP = prFontCodePointGlyph(font, 0x1D443);
    REQUIRE(f == 71);
    REQUIRE(integral == 1006);
    REQUIRE(italicP == 2981);
    CHECK(prFontMathGlyphItalicsCorrection(font, f) == doctest::Approx(0.079));
    CHECK(prFontMathGlyphItalicsCorrection(font, integral) == doctest::Approx(0.232));
    CHECK(prFontMathGlyphItalicsCorrection(font, prFontCodePointGlyph(font, '(')) == 0);
    CHECK(prFontMathGlyphTopAccentAttachment(font, f) == doctest::Approx(0.264));
    CHECK(prFontMathGlyphTopAccentAttachment(font, prFontCodePointGlyph(font, 0x1D453)) == doctest::Approx(0.464));
    CHECK(prFontMathGlyphIsExtendedShape(font, integral));
    CHECK_FALSE(prFontMathGlyphIsExtendedShape(font, f));
    // 𝑃 has one kern at its bottom right, at every height, and none at its other corners.
    for (float height : {-1.0f, 0.0f, 0.3f, 2.0f})
        CHECK(prFontMathGlyphKern(font, italicP, PrFontMathCorner_BottomRight, height) == doctest::Approx(-0.1));
    CHECK(prFontMathGlyphKern(font, italicP, PrFontMathCorner_TopRight, 0.5f) == 0);
    CHECK(prFontMathGlyphKern(font, f, PrFontMathCorner_TopRight, 0.5f) == 0);
    prFontRelease(font);
}

TEST_CASE("New Computer Modern Math's constructions read as the table gives them") {
    const PrFont* font = fromBytes(newCmBytes());
    uint16_t paren = prFontCodePointGlyph(font, '(');
    REQUIRE(paren == 9);
    CHECK(prFontMathMinConnectorOverlap(font) == doctest::Approx(0.02));
    REQUIRE(prFontMathGlyphVariantCount(font, paren, PrFontMathAxis_Vertical) == 8);
    CHECK(prFontMathGlyphVariantCount(font, paren, PrFontMathAxis_Horizontal) == 0);
    PrFontMathVariant first = prFontMathGlyphVariant(font, paren, PrFontMathAxis_Vertical, 0);
    PrFontMathVariant last = prFontMathGlyphVariant(font, paren, PrFontMathAxis_Vertical, 7);
    CHECK(first.glyph == 9);
    CHECK(first.advance == doctest::Approx(0.997));
    CHECK(last.glyph == 6047);
    CHECK(last.advance == doctest::Approx(2.991));
    CHECK(prFontMathGlyphVariant(font, paren, PrFontMathAxis_Vertical, 8).glyph == 0);
    REQUIRE(prFontMathGlyphPartCount(font, paren, PrFontMathAxis_Vertical) == 3);
    PrFontMathPart bottom = prFontMathGlyphPart(font, paren, PrFontMathAxis_Vertical, 0);
    PrFontMathPart extender = prFontMathGlyphPart(font, paren, PrFontMathAxis_Vertical, 1);
    CHECK(bottom.glyph == 1376);
    CHECK(bottom.flags == 0);
    CHECK(bottom.startConnector == 0);
    CHECK(bottom.endConnector == doctest::Approx(0.249));
    CHECK(bottom.fullAdvance == doctest::Approx(1.495));
    CHECK(extender.glyph == 1375);
    CHECK(extender.flags == PrFontMathPartFlag_Extender);
    CHECK(extender.fullAdvance == doctest::Approx(0.498));
    uint16_t integral = prFontCodePointGlyph(font, 0x222B);
    CHECK(prFontMathGlyphAssemblyItalicsCorrection(font, integral, PrFontMathAxis_Vertical) == doctest::Approx(0.08));
    uint16_t circumflex = prFontCodePointGlyph(font, 0x302);
    CHECK(prFontMathGlyphVariantCount(font, circumflex, PrFontMathAxis_Horizontal) == 8);
    CHECK(prFontMathGlyphPartCount(font, circumflex, PrFontMathAxis_Horizontal) == 0);
    // The largest advance among the parenthesis' variants and parts is that of its largest variant and its parts.
    CHECK(prFontMathGlyphPreferredAdvance(font, paren) == doctest::Approx(0.875));
    CHECK(prFontMathGlyphPreferredAdvance(font, integral) == doctest::Approx(1.185));
    CHECK(prFontMathGlyphPreferredAdvance(font, prFontCodePointGlyph(font, 'a')) ==
        prFontGlyphAdvance(font, prFontCodePointGlyph(font, 'a'), 0));
    prFontRelease(font);
}

TEST_CASE("New Computer Modern Math's parenthesis stretches through its variants to an exact assembly") {
    const PrFont* font = fromBytes(newCmBytes());
    uint16_t paren = prFontCodePointGlyph(font, '(');
    PrFontMathStretchInfo info{};
    // The glyph's own ink height covers a small size.
    std::vector<PrFontGlyph> glyphs = stretch(font, paren, PrFontMathAxis_Vertical, 0.5f, &info);
    CHECK(info.kind == PrFontMathStretchKind_Glyph);
    CHECK(ids(glyphs) == std::vector<uint16_t>{9});
    CHECK(info.ascent == doctest::Approx(0.748));
    CHECK(info.descent == doctest::Approx(0.248));
    // The first variant whose measurement covers the size.
    glyphs = stretch(font, paren, PrFontMathAxis_Vertical, 1.4f, &info);
    CHECK(ids(glyphs) == std::vector<uint16_t>{5959});
    CHECK(info.advance == doctest::Approx(0.523));
    // Past the largest variant, the assembly: bottom, five extenders and top, which repeat at the fewest repeats that
    // reach 5em, 2.99 + 5 × 0.478, and overlap by 0.08em each to land on 5em exactly.
    glyphs = stretch(font, paren, PrFontMathAxis_Vertical, 5, &info);
    CHECK(info.kind == PrFontMathStretchKind_Assembly);
    CHECK(ids(glyphs) == std::vector<uint16_t>{1376, 1375, 1375, 1375, 1375, 1375, 1374});
    CHECK(info.ascent == doctest::Approx(5));
    CHECK(info.descent == 0);
    CHECK(info.advance == doctest::Approx(0.875));
    CHECK(info.italicsCorrection == 0);
    // Each part sits on the end of the one before less the overlap. The parts' ink starts at their origins.
    const float bottoms[] = {0, 1.415f, 1.833f, 2.251f, 2.669f, 3.087f, 3.505f};
    for (size_t i = 0; i < glyphs.size(); ++i) {
        CHECK(glyphs[i].position.x == 0);
        CHECK(glyphs[i].position.y == doctest::Approx(-bottoms[i]));
    }
    // The top part ends at the size.
    CHECK(-glyphs.back().position.y + 1.495f == doctest::Approx(5));
    // The integral's middle extender has ink below its origin, which its position makes up for.
    uint16_t integral = prFontCodePointGlyph(font, 0x222B);
    glyphs = stretch(font, integral, PrFontMathAxis_Vertical, 4, &info);
    CHECK(info.kind == PrFontMathStretchKind_Assembly);
    CHECK(info.italicsCorrection == doctest::Approx(0.08));
    REQUIRE(glyphs.size() == 4);
    CHECK(glyphs[0].position.y == doctest::Approx(-0.022));
    // Two extenders after the 1.344em bottom reach 4.446em, which an overlap of 0.446 / 3em brings to 4em.
    CHECK(glyphs[1].position.y + prFontGlyphInkBounds(font, glyphs[1].id).bottom == doctest::Approx(-(1.344 - 0.446 / 3)));
    CHECK(glyphs[3].position.y + prFontGlyphInkBounds(font, glyphs[3].id).top == doctest::Approx(-4));
    prFontRelease(font);
}

TEST_CASE("a horizontal assembly's ink is its parts' ink, which an overline's stays above the baseline") {
    const PrFont* font = fromBytes(newCmBytes());
    uint16_t overline = prFontCodePointGlyph(font, 0x0305);
    PrRect bar = prFontGlyphInkBounds(font, overline);
    REQUIRE(bar.bottom < 0);
    PrFontMathStretchInfo info{};
    std::vector<PrFontGlyph> glyphs = stretch(font, overline, PrFontMathAxis_Horizontal, 6, &info);
    CHECK(info.kind == PrFontMathStretchKind_Assembly);
    CHECK(info.advance == doctest::Approx(6));
    CHECK(info.descent == doctest::Approx(bar.bottom));
    CHECK(info.ascent == doctest::Approx(-bar.top));
    prFontRelease(font);
}

TEST_CASE("Noto Sans Math, a TrueType font, reads and stretches from its MATH table") {
    const PrFont* font = fromBytes(readPath(std::string(PR_FONTS_DIR) + "/NotoSansMath-Regular.ttf"));
    CHECK(prFontHasMath(font));
    CHECK(prFontMathConstants(font)->axisHeight == doctest::Approx(0.278));
    CHECK(prFontMathConstants(font)->scriptPercentScaleDown == doctest::Approx(0.6));
    uint16_t integral = prFontCodePointGlyph(font, 0x222B);
    REQUIRE(integral == 2847);
    CHECK(prFontMathGlyphItalicsCorrection(font, integral) == doctest::Approx(0.274));
    PrFontMathStretchInfo info{};
    // 6em takes five extenders after the 2.138em of the ends, and overlaps of 1.483 / 6em.
    std::vector<PrFontGlyph> glyphs = stretch(font, prFontCodePointGlyph(font, '('), PrFontMathAxis_Vertical, 6, &info);
    CHECK(ids(glyphs) == std::vector<uint16_t>{1959, 1961, 1961, 1961, 1961, 1961, 1960});
    CHECK(info.ascent == doctest::Approx(6));
    glyphs = stretch(font, integral, PrFontMathAxis_Vertical, 5, &info);
    CHECK(info.kind == PrFontMathStretchKind_Assembly);
    CHECK(info.italicsCorrection == doctest::Approx(0.09));
    CHECK(info.ascent == doctest::Approx(5));
    prFontRelease(font);
}

TEST_CASE("a font without constructions stretches by the assembly of Unicode's parts of stretched operators") {
    std::vector<uint8_t> bytes = readPath(std::string(PR_FONTS_DIR) + "/NotoSansMath-Regular.ttf");
    const PrFont* font = fromBytes(withTable(bytes, kMath, {}));
    REQUIRE_FALSE(prFontHasMath(font));
    uint16_t paren = prFontCodePointGlyph(font, '(');
    uint16_t bottom = prFontCodePointGlyph(font, 0x239D);
    uint16_t extender = prFontCodePointGlyph(font, 0x239C);
    uint16_t top = prFontCodePointGlyph(font, 0x239B);
    PrFontMathStretchInfo info{};
    PrFontGlyph glyphs[64];
    CHECK(prFontMathGlyphStretch(font, paren, PrFontMathAxis_Vertical, 6, &info, glyphs, 64) == 0);
    uint32_t count = prFontMathCodePointStretch(font, '(', paren, PrFontMathAxis_Vertical, 6, &info, glyphs, 64);
    REQUIRE(count >= 3);
    CHECK(info.kind == PrFontMathStretchKind_Assembly);
    CHECK(info.ascent >= 6);
    CHECK(glyphs[0].id == bottom);
    CHECK(glyphs[count - 1].id == top);
    for (uint32_t i = 1; i + 1 < count; ++i)
        CHECK(glyphs[i].id == extender);
    // The parts do not overlap: each sits on the ink top of the one below it.
    PrRect bottomInk = prFontGlyphInkBounds(font, bottom);
    CHECK(glyphs[1].position.y + prFontGlyphInkBounds(font, extender).bottom ==
        doctest::Approx(glyphs[0].position.y + bottomInk.top).epsilon(0.002));
    // A size that the glyph covers takes the glyph, and a character for which Unicode builds no assembly does not
    // stretch.
    CHECK(prFontMathCodePointStretch(font, '(', paren, PrFontMathAxis_Vertical, 0.1f, &info, glyphs, 64) == 1);
    CHECK(info.kind == PrFontMathStretchKind_Glyph);
    uint16_t x = prFontCodePointGlyph(font, 'x');
    CHECK(prFontMathCodePointStretch(font, 'x', x, PrFontMathAxis_Vertical, 6, &info, glyphs, 64) == 0);
    CHECK(prFontMathCodePointStretch(font, '(', paren, PrFontMathAxis_Horizontal, 6, &info, glyphs, 64) == 0);
    prFontRelease(font);
}

TEST_CASE("a feature's single substitutions give a glyph's math forms: dotless and right-to-left mirrored") {
    std::vector<uint8_t> bytes = readPath(std::string(PR_FONTS_DIR) + "/NotoSansMath-Regular.ttf");
    const PrFont* font = fromBytes(bytes);
    uint16_t i = prFontCodePointGlyph(font, 'i');
    uint16_t partial = prFontCodePointGlyph(font, 0x2202);
    uint16_t dotless = prFontCodePointGlyph(font, 0x0131);
    CHECK(prFontFeatureGlyph(font, PR_FONT_TAG('d', 't', 'l', 's'), i) == dotless);
    uint16_t mirrored = prFontFeatureGlyph(font, PR_FONT_TAG('r', 't', 'l', 'm'), partial);
    CHECK(mirrored != partial);
    CHECK(mirrored != 0);
    // A glyph that the feature does not cover, and a feature that the font lacks, give the glyph itself.
    CHECK(prFontFeatureGlyph(font, PR_FONT_TAG('r', 't', 'l', 'm'), i) == i);
    CHECK(prFontFeatureGlyph(font, PR_FONT_TAG('f', 'l', 'a', 'c'), partial) == partial);
    // A GSUB table cut short anywhere substitutes nothing out of bounds.
    constexpr uint32_t kGsub = PR_FONT_TAG('G', 'S', 'U', 'B');
    std::vector<uint8_t> gsub = tableOf(bytes, kGsub);
    REQUIRE(!gsub.empty());
    for (size_t size = 0; size < gsub.size(); size += 97) {
        std::vector<uint8_t> cut(gsub.begin(), gsub.begin() + ptrdiff_t(size));
        const PrFont* truncated = fromBytes(withTable(bytes, kGsub, cut));
        CHECK(prFontFeatureGlyph(truncated, PR_FONT_TAG('r', 't', 'l', 'm'), partial) < prFontGlyphCount(truncated));
        prFontRelease(truncated);
    }
    prFontRelease(font);
}

TEST_CASE("the size-adjust scales New Computer Modern Math's lengths and stretches, but not its percentages") {
    const PrFont* base = fromBytes(newCmBytes());
    PrFontDerivation d = prFontDerivationDefault();
    d.sizeAdjust = 2;
    const PrFont* font = prFontCreateDerived(base, &d);
    const PrFontMathConstants& c = *prFontMathConstants(font);
    CHECK(c.axisHeight == doctest::Approx(0.5));
    CHECK(c.scriptPercentScaleDown == doctest::Approx(0.7));
    CHECK(c.radicalDegreeBottomRaisePercent == doctest::Approx(0.6));
    uint16_t f = prFontCodePointGlyph(font, 'f');
    CHECK(prFontMathGlyphItalicsCorrection(font, f) == doctest::Approx(0.158));
    CHECK(prFontMathGlyphTopAccentAttachment(font, f) == doctest::Approx(0.528));
    CHECK(prFontMathGlyphKern(font, 2981, PrFontMathCorner_BottomRight, 0) == doctest::Approx(-0.2));
    CHECK(prFontMathMinConnectorOverlap(font) == doctest::Approx(0.04));
    PrFontMathStretchInfo info{};
    std::vector<PrFontGlyph> glyphs = stretch(font, prFontCodePointGlyph(font, '('), PrFontMathAxis_Vertical, 10, &info);
    CHECK(glyphs.size() == 7);
    CHECK(info.ascent == doctest::Approx(10));
    CHECK(glyphs[1].position.y == doctest::Approx(-2.83));
    prFontRelease(font);
    prFontRelease(base);
}

TEST_CASE("WPT's math fonts give the constants that their generators set") {
    struct Expected {
        const char* name;
        float PrFontMathConstants::* constant;
        float value;
    };
    const Expected expected[] = {
        {"fraction-axisheight7000-rulethickness1000", &PrFontMathConstants::axisHeight, 7},
        {"fraction-axisheight7000-rulethickness1000", &PrFontMathConstants::fractionRuleThickness, 1},
        {"scripts-subscriptshiftdown6000", &PrFontMathConstants::subscriptShiftDown, 6},
        {"stack-gapmin8000", &PrFontMathConstants::stackGapMin, 8},
        {"underover-accentbaseheight4000-overbarextraascender3000", &PrFontMathConstants::accentBaseHeight, 4},
        {"underover-accentbaseheight4000-overbarextraascender3000", &PrFontMathConstants::overbarExtraAscender, 3},
        {"radical-kernafterdegreeminus5000-rulethickness1000", &PrFontMathConstants::radicalKernAfterDegree, -5},
        {"radical-kernafterdegreeminus5000-rulethickness1000", &PrFontMathConstants::radicalRuleThickness, 1},
        {
            "radical-degreebottomraisepercent25-rulethickness1000",
            &PrFontMathConstants::radicalDegreeBottomRaisePercent, 0.25f,
        },
        {
            "largeop-displayoperatorminheight2000-2AFF-italiccorrection3000",
            &PrFontMathConstants::displayOperatorMinHeight, 2,
        },
        // A scale-down of zero takes MathML Core's fallback.
        {"scriptpercentscaledown0-scriptscriptpercentscaledown40", &PrFontMathConstants::scriptPercentScaleDown, 0.71f},
        {
            "scriptpercentscaledown0-scriptscriptpercentscaledown40",
            &PrFontMathConstants::scriptScriptPercentScaleDown, 0.4f,
        },
        {"scriptpercentscaledown80-scriptscriptpercentscaledown0", &PrFontMathConstants::scriptPercentScaleDown, 0.8f},
        {
            "scriptpercentscaledown80-scriptscriptpercentscaledown0",
            &PrFontMathConstants::scriptScriptPercentScaleDown, 0.5041f,
        },
    };
    for (const Expected& e : expected) {
        CAPTURE(e.name);
        const PrFont* font = wptFont(e.name);
        CHECK(prFontHasMath(font));
        CHECK(prFontMathConstants(font)->*e.constant == doctest::Approx(e.value));
        prFontRelease(font);
    }
}

TEST_CASE("WPT's large operator font stretches U+2AFF to its display variant, with that variant's italics correction") {
    const PrFont* font = wptFont("largeop-displayoperatorminheight2000-2AFF-italiccorrection3000");
    uint16_t glyph = prFontCodePointGlyph(font, 0x2AFF);
    REQUIRE(glyph == 2);
    PrFontMathStretchInfo info{};
    std::vector<PrFontGlyph> glyphs = stretch(font, glyph, PrFontMathAxis_Vertical, 2, &info);
    CHECK(ids(glyphs) == std::vector<uint16_t>{3});
    CHECK(info.kind == PrFontMathStretchKind_Glyph);
    CHECK(info.italicsCorrection == doctest::Approx(3));
    CHECK(info.ascent == 2);
    CHECK(info.descent == 0);
    CHECK(info.advance == 4);
    CHECK(prFontMathGlyphItalicsCorrection(font, glyph) == 0);
    prFontRelease(font);
}

TEST_CASE("WPT's stretchy font stretches by its variants and its assembly to exact sizes") {
    const PrFont* font = wptFont("stretchy");
    uint16_t horizontal = prFontCodePointGlyph(font, 0x295A);
    uint16_t vertical = prFontCodePointGlyph(font, 0x295C);
    uint16_t base = prFontCodePointGlyph(font, 0x21A8);
    REQUIRE(horizontal == 3);
    REQUIRE(vertical == 4);
    CHECK(prFontMathMinConnectorOverlap(font) == 0.5f);
    CHECK(prFontMathGlyphTopAccentAttachment(font, vertical) == 0.5f);
    PrFontMathStretchInfo info{};

    // The glyph's own height covers half an em, and the variants each up to their measurement (n em plus one unit).
    CHECK(ids(stretch(font, vertical, PrFontMathAxis_Vertical, 0.5f, &info)) == std::vector<uint16_t>{4});
    CHECK(ids(stretch(font, vertical, PrFontMathAxis_Vertical, 1.5f, &info)) == std::vector<uint16_t>{7});
    CHECK(ids(stretch(font, vertical, PrFontMathAxis_Vertical, 4, &info)) == std::vector<uint16_t>{11});
    CHECK(info.ascent == 4);

    // 4.5em: one extender after the 3em part, overlapping by half an em.
    std::vector<PrFontGlyph> glyphs = stretch(font, vertical, PrFontMathAxis_Vertical, 4.5f, &info);
    CHECK(info.kind == PrFontMathStretchKind_Assembly);
    CHECK(ids(glyphs) == std::vector<uint16_t>{9, 7});
    CHECK(glyphs[1].position.y == doctest::Approx(-2.5));
    CHECK(info.ascent == doctest::Approx(4.5));
    CHECK(info.advance == 1);

    // 5em: two extenders, overlapping by the full em of their connectors.
    glyphs = stretch(font, vertical, PrFontMathAxis_Vertical, 5, &info);
    CHECK(ids(glyphs) == std::vector<uint16_t>{9, 7, 7});
    CHECK(glyphs[1].position.y == -2);
    CHECK(glyphs[2].position.y == doctest::Approx(-3));
    CHECK(info.ascent == 5);

    // 6em: two extenders, overlapping by half an em.
    glyphs = stretch(font, vertical, PrFontMathAxis_Vertical, 6, &info);
    CHECK(ids(glyphs) == std::vector<uint16_t>{9, 7, 7});
    CHECK(glyphs[1].position.y == doctest::Approx(-2.5));
    CHECK(glyphs[2].position.y == doctest::Approx(-4));
    CHECK(info.ascent == 6);
    CHECK(info.descent == 0);

    // 10em horizontally: five extenders, overlapping by 0.6em.
    glyphs = stretch(font, horizontal, PrFontMathAxis_Horizontal, 10, &info);
    CHECK(ids(glyphs) == std::vector<uint16_t>{10, 8, 8, 8, 8, 8});
    const float lefts[] = {0, 2.4f, 3.8f, 5.2f, 6.6f, 8};
    for (size_t i = 0; i < glyphs.size(); ++i) {
        CHECK(glyphs[i].position.x == doctest::Approx(lefts[i]));
        CHECK(glyphs[i].position.y == 0);
    }
    CHECK(info.advance == doctest::Approx(10));
    CHECK(info.ascent == 1);
    CHECK(info.descent == 0);

    // A glyph with variants and no assembly stretches no further than its largest variant.
    CHECK(ids(stretch(font, base, PrFontMathAxis_Vertical, 6, &info)) == std::vector<uint16_t>{11});
    CHECK(info.kind == PrFontMathStretchKind_Glyph);
    CHECK(info.ascent == 4);

    // A glyph with no construction along the axis does not stretch.
    CHECK(prFontMathGlyphStretch(font, base, PrFontMathAxis_Horizontal, 6, &info, nullptr, 0) == 0);
    CHECK(info.kind == PrFontMathStretchKind_None);

    // Sizes that are not finite stretch to nothing, or to the most repeats.
    CHECK(ids(stretch(font, vertical, PrFontMathAxis_Vertical, -1, &info)) == std::vector<uint16_t>{4});
    CHECK(ids(stretch(font, vertical, PrFontMathAxis_Vertical, kPrF32Nan, &info)) == std::vector<uint16_t>{4});
    uint32_t most = prFontMathGlyphStretch(font, vertical, PrFontMathAxis_Vertical, kPrF32Infinity, &info, nullptr, 0);
    CHECK(most == 1 + 0xFFFF);
    CHECK(info.kind == PrFontMathStretchKind_Assembly);
    prFontRelease(font);
}

TEST_CASE("an assembly of unequal connectors, from WPT's asymmetric stretchy font") {
    const PrFont* font = wptFont("stretchy-text-direction-asymetrical");
    uint16_t paren = prFontCodePointGlyph(font, ')');
    PrFontMathStretchInfo info{};
    // Three extenders of 2em after a 3em part make 9em, and 7em needs an overlap of 2/3em, which the connectors allow.
    std::vector<PrFontGlyph> glyphs = stretch(font, paren, PrFontMathAxis_Vertical, 7, &info);
    CHECK(glyphs.size() == 4);
    CHECK(info.ascent == doctest::Approx(7));
    CHECK(glyphs[3].position.y == doctest::Approx(-(3 + 2 * 2 - 3 * 2.0 / 3)));
    CHECK(info.advance == 2);
    CHECK(prFontMathGlyphPreferredAdvance(font, paren) == 2);
    CHECK(prFontMathGlyphPreferredAdvance(font, prFontCodePointGlyph(font, '{')) == 1);
    prFontRelease(font);
}

TEST_CASE("a font without MATH takes MathML Core's fallbacks, derived from its metrics") {
    const PrFont* font = fromBytes(readPath(std::string(PR_FONTS_DIR) + "/NotoSans[wdth,wght].ttf"));
    CHECK_FALSE(prFontHasMath(font));
    PrFontMetrics m = prFontMetrics(font);
    float rule = m.underlineThickness;
    const PrFontMathConstants& c = *prFontMathConstants(font);
    CHECK(c.scriptPercentScaleDown == doctest::Approx(0.71));
    CHECK(c.scriptScriptPercentScaleDown == doctest::Approx(0.5041));
    CHECK(c.displayOperatorMinHeight == 0);
    CHECK(c.axisHeight == doctest::Approx(m.xHeight / 2));
    CHECK(c.accentBaseHeight == doctest::Approx(m.xHeight));
    CHECK(c.subscriptShiftDown == doctest::Approx(m.subscriptPosition));
    CHECK(c.superscriptShiftUp == doctest::Approx(-m.superscriptPosition));
    CHECK(c.subscriptTopMax == doctest::Approx(m.xHeight * 0.8f));
    CHECK(c.superscriptBottomMin == doctest::Approx(m.xHeight / 4));
    CHECK(c.subSuperscriptGapMin == doctest::Approx(4 * rule));
    CHECK(c.spaceAfterScript == doctest::Approx(1.0 / 24));
    CHECK(c.stackDisplayStyleGapMin == doctest::Approx(7 * rule));
    CHECK(c.fractionRuleThickness == doctest::Approx(rule));
    CHECK(c.radicalVerticalGap == doctest::Approx(1.25f * rule));
    CHECK(c.radicalDisplayStyleVerticalGap == doctest::Approx(rule + m.xHeight / 4));
    CHECK(c.radicalKernBeforeDegree == doctest::Approx(5.0 / 18));
    CHECK(c.radicalKernAfterDegree == doctest::Approx(-10.0 / 18));
    CHECK(c.radicalDegreeBottomRaisePercent == doctest::Approx(0.6));
    CHECK(c.upperLimitGapMin == 0);
    uint16_t a = prFontCodePointGlyph(font, 'a');
    CHECK(prFontMathGlyphItalicsCorrection(font, a) == 0);
    CHECK(prFontMathGlyphTopAccentAttachment(font, a) == doctest::Approx(prFontGlyphAdvance(font, a, 0) / 2));
    CHECK(prFontMathGlyphKern(font, a, PrFontMathCorner_TopRight, 0.5f) == 0);
    CHECK_FALSE(prFontMathGlyphIsExtendedShape(font, a));
    CHECK(prFontMathMinConnectorOverlap(font) == 0);
    CHECK(prFontMathGlyphVariantCount(font, a, PrFontMathAxis_Vertical) == 0);
    PrFontMathStretchInfo info{};
    CHECK(prFontMathGlyphStretch(font, a, PrFontMathAxis_Vertical, 3, &info, nullptr, 0) == 0);
    CHECK(prFontMathGlyphPreferredAdvance(font, a) == prFontGlyphAdvance(font, a, 0));

    // The fallbacks follow the metrics of a derived font: its instance and its size-adjust, except for lengths that
    // MathML Core gives in ems of the font size.
    const PrFont* flex = fromBytes(readPath(std::string(PR_FONTS_DIR) + "/RobotoFlex.ttf"));
    PrFontVariation wide{PR_FONT_TAG('w', 'g', 'h', 't'), 900};
    PrFontDerivation d = prFontDerivationDefault();
    d.variations = &wide;
    d.variationCount = 1;
    d.sizeAdjust = 1.5f;
    const PrFont* derived = prFontCreateDerived(flex, &d);
    CHECK(prFontMathConstants(derived)->axisHeight == doctest::Approx(prFontMetrics(derived).xHeight / 2));
    CHECK(prFontMathConstants(derived)->spaceAfterScript == doctest::Approx(1.0 / 24));
    prFontRelease(derived);
    prFontRelease(flex);
    prFontRelease(font);
}

TEST_CASE("a cut-in kern applies between the correction heights that bound the height asked") {
    // A MATH table with only a MathKernInfo: glyph 1 kerns at its top right by -10 units below 100, -20 up to 300, and
    // -30 above.
    std::vector<uint8_t> math;
    put32(math, 0x00010000);
    put16(math, 0);                 // MathConstants
    put16(math, 10);                // MathGlyphInfo
    put16(math, 0);                 // MathVariants
    put16(math, 0);                 // MathGlyphInfo at 10: no italics, accents or shapes
    put16(math, 0);
    put16(math, 0);
    put16(math, 8);                 // MathKernInfo at 18
    put16(math, 12);                // its coverage, at 30
    put16(math, 1);
    put16(math, 18);                // top right MathKern, at 36
    put16(math, 0);
    put16(math, 0);
    put16(math, 0);
    put16(math, 1);                 // coverage: format 1, glyph 1
    put16(math, 1);
    put16(math, 1);
    put16(math, 2);                 // MathKern: two heights, three kerns
    const int16_t values[] = {100, 300, -10, -20, -30};
    for (int16_t value : values) {
        put16(math, uint16_t(value));
        put16(math, 0);
    }
    std::vector<uint8_t> ahem = readPath(std::string(PR_FONTS_DIR) + "/Ahem.ttf");
    const PrFont* font = fromBytes(withTable(ahem, kMath, math));
    REQUIRE(prFontHasMath(font));
    float em = float(prFontMetrics(font).unitsPerEm);
    CHECK(prFontMathGlyphKern(font, 1, PrFontMathCorner_TopRight, -1) == doctest::Approx(-10 / em));
    CHECK(prFontMathGlyphKern(font, 1, PrFontMathCorner_TopRight, 99 / em) == doctest::Approx(-10 / em));
    CHECK(prFontMathGlyphKern(font, 1, PrFontMathCorner_TopRight, 100 / em) == doctest::Approx(-20 / em));
    CHECK(prFontMathGlyphKern(font, 1, PrFontMathCorner_TopRight, 299 / em) == doctest::Approx(-20 / em));
    CHECK(prFontMathGlyphKern(font, 1, PrFontMathCorner_TopRight, 301 / em) == doctest::Approx(-30 / em));
    CHECK(prFontMathGlyphKern(font, 1, PrFontMathCorner_TopLeft, 0) == 0);
    CHECK(prFontMathGlyphKern(font, 2, PrFontMathCorner_TopRight, 0) == 0);
    // A table without MathConstants takes the fallbacks.
    CHECK(prFontMathConstants(font)->scriptPercentScaleDown == doctest::Approx(0.71));

    // Cut short inside the MathKern, the kern reads the heights that fit, or none.
    for (size_t size = math.size(); size > 30; --size) {
        std::vector<uint8_t> cut(math.begin(), math.begin() + ptrdiff_t(size));
        const PrFont* truncated = fromBytes(withTable(ahem, kMath, cut));
        float kern = prFontMathGlyphKern(truncated, 1, PrFontMathCorner_TopRight, 1);
        CHECK(kern <= 0);
        CHECK(kern >= -30.5f / em);
        prFontRelease(truncated);
    }
    prFontRelease(font);
}

TEST_CASE("a truncated MATH table reads what is left and falls back for the rest") {
    std::vector<uint8_t> bytes = newCmBytes();
    std::vector<uint8_t> math = tableOf(bytes, kMath);
    REQUIRE(math.size() > 1000);
    uint16_t constants = be16(math.data() + 4);
    // MathConstants cut after axisHeight: the constants before it read, and those after it fall back.
    {
        std::vector<uint8_t> cut(math.begin(), math.begin() + constants + 16);
        const PrFont* font = fromBytes(withTable(bytes, kMath, cut));
        const PrFontMathConstants& c = *prFontMathConstants(font);
        CHECK(c.scriptPercentScaleDown == doctest::Approx(0.70));
        CHECK(c.axisHeight == doctest::Approx(0.25));
        CHECK(c.accentBaseHeight == doctest::Approx(prFontMetrics(font).xHeight));
        CHECK(c.radicalDegreeBottomRaisePercent == doctest::Approx(0.6));
        CHECK(prFontMathGlyphVariantCount(font, 9, PrFontMathAxis_Vertical) == 0);
        prFontRelease(font);
    }
    // Offsets past the end, and every length of the table, read without fault. The glyphs are those of the tests above,
    // and one past the font.
    std::vector<size_t> sizes = {0, 1, 4, 9, 10, 11, 100, 213, 214, 215};
    for (size_t size = 300; size < math.size(); size += 997)
        sizes.push_back(size);
    sizes.push_back(math.size() - 1);
    std::vector<uint8_t> outOfRange = math;
    for (size_t at = 4; at < 10; at += 2)
        set16(outOfRange, at, 0xFFF0);
    for (size_t size : sizes) {
        CAPTURE(size);
        std::vector<uint8_t> cut(math.begin(), math.begin() + ptrdiff_t(std::min(size, math.size())));
        for (const std::vector<uint8_t>* table : {&cut, &outOfRange}) {
            const PrFont* font = fromBytes(withTable(bytes, kMath, *table));
            CHECK(prFontMathConstants(font)->scriptScriptPercentScaleDown > 0);
            for (uint16_t glyph : {uint16_t(9), uint16_t(71), uint16_t(1006), uint16_t(2981), uint16_t(0xFFFF)}) {
                prFontMathGlyphItalicsCorrection(font, glyph);
                prFontMathGlyphTopAccentAttachment(font, glyph);
                prFontMathGlyphIsExtendedShape(font, glyph);
                prFontMathGlyphKern(font, glyph, PrFontMathCorner_BottomRight, 0.5f);
                prFontMathGlyphPreferredAdvance(font, glyph);
                for (PrFontMathAxis axis : {PrFontMathAxis_Horizontal, PrFontMathAxis_Vertical}) {
                    uint32_t parts = prFontMathGlyphPartCount(font, glyph, axis);
                    for (uint32_t i = 0; i < parts; ++i)
                        prFontMathGlyphPart(font, glyph, axis, i);
                    PrFontMathStretchInfo info{};
                    PrFontGlyph out[64];
                    uint32_t count = prFontMathGlyphStretch(font, glyph, axis, 20, &info, out, 64);
                    CHECK(count < 0x10000);
                }
            }
            prFontRelease(font);
        }
    }
}
