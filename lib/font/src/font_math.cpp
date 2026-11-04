// Math fonts: the OpenType MATH table, read in place with every read bounds checked, and the stretching of glyphs that
// it describes. Every value is read in font units and scaled to ems on the way out.

#include "pr/font_math.h"
#include "font/src/font_math.h"

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/float.h"

#include "font/src/font.h"
#include "font/src/font_math_unicode.h"

namespace pr_impl {

namespace {

constexpr uint32_t kMath = PR_FONT_TAG('M', 'A', 'T', 'H');
constexpr uint32_t kPost = PR_FONT_TAG('p', 'o', 's', 't');

// The maximum number of repeats of each extender of an assembly. It bounds the glyphs of a stretch at a size far past
// any that a formula asks for.
constexpr uint32_t kMaxRepeats = 0xFFFF;

// Returns the number of entries of `entrySize` bytes from `start` that the table holds, at most `count`.
uint32_t fitting(SfntTable table, uint32_t start, uint32_t entrySize, uint32_t count) {
    uint32_t fit = table.size > start ? (table.size - start) / entrySize : 0;
    return count < fit ? count : fit;
}

// Returns the subtable at the 16-bit offset at `at`, or an empty table for a null offset.
SfntTable subtable(SfntTable table, uint32_t at) {
    uint16_t offset = table.uint16At(at);
    return offset != 0 ? table.from(offset) : SfntTable{};
}

// Reads the MathValueRecord of the glyph from a table that holds a coverage offset at byte 0, a record count at byte 2,
// and the records. Returns false if the coverage does not list the glyph.
bool recordValue(SfntTable table, SfntTable coverage, uint32_t glyph, int16_t* out) {
    uint32_t index = sfntCoverageIndex(coverage, glyph);
    if (index == kSfntCoverageMissing || index >= fitting(table, 4, 4, table.uint16At(2)))
        return false;
    *out = table.int16At(4 + index * 4);
    return true;
}

// The MATH constants that are MathValueRecords, in the order of MathConstants, from byte 8.
constexpr float PrFontMathConstants::* kValueRecords[] = {
    &PrFontMathConstants::mathLeading,
    &PrFontMathConstants::axisHeight,
    &PrFontMathConstants::accentBaseHeight,
    &PrFontMathConstants::flattenedAccentBaseHeight,
    &PrFontMathConstants::subscriptShiftDown,
    &PrFontMathConstants::subscriptTopMax,
    &PrFontMathConstants::subscriptBaselineDropMin,
    &PrFontMathConstants::superscriptShiftUp,
    &PrFontMathConstants::superscriptShiftUpCramped,
    &PrFontMathConstants::superscriptBottomMin,
    &PrFontMathConstants::superscriptBaselineDropMax,
    &PrFontMathConstants::subSuperscriptGapMin,
    &PrFontMathConstants::superscriptBottomMaxWithSubscript,
    &PrFontMathConstants::spaceAfterScript,
    &PrFontMathConstants::upperLimitGapMin,
    &PrFontMathConstants::upperLimitBaselineRiseMin,
    &PrFontMathConstants::lowerLimitGapMin,
    &PrFontMathConstants::lowerLimitBaselineDropMin,
    &PrFontMathConstants::stackTopShiftUp,
    &PrFontMathConstants::stackTopDisplayStyleShiftUp,
    &PrFontMathConstants::stackBottomShiftDown,
    &PrFontMathConstants::stackBottomDisplayStyleShiftDown,
    &PrFontMathConstants::stackGapMin,
    &PrFontMathConstants::stackDisplayStyleGapMin,
    &PrFontMathConstants::stretchStackTopShiftUp,
    &PrFontMathConstants::stretchStackBottomShiftDown,
    &PrFontMathConstants::stretchStackGapAboveMin,
    &PrFontMathConstants::stretchStackGapBelowMin,
    &PrFontMathConstants::fractionNumeratorShiftUp,
    &PrFontMathConstants::fractionNumeratorDisplayStyleShiftUp,
    &PrFontMathConstants::fractionDenominatorShiftDown,
    &PrFontMathConstants::fractionDenominatorDisplayStyleShiftDown,
    &PrFontMathConstants::fractionNumeratorGapMin,
    &PrFontMathConstants::fractionNumDisplayStyleGapMin,
    &PrFontMathConstants::fractionRuleThickness,
    &PrFontMathConstants::fractionDenominatorGapMin,
    &PrFontMathConstants::fractionDenomDisplayStyleGapMin,
    &PrFontMathConstants::skewedFractionHorizontalGap,
    &PrFontMathConstants::skewedFractionVerticalGap,
    &PrFontMathConstants::overbarVerticalGap,
    &PrFontMathConstants::overbarRuleThickness,
    &PrFontMathConstants::overbarExtraAscender,
    &PrFontMathConstants::underbarVerticalGap,
    &PrFontMathConstants::underbarRuleThickness,
    &PrFontMathConstants::underbarExtraDescender,
    &PrFontMathConstants::radicalVerticalGap,
    &PrFontMathConstants::radicalDisplayStyleVerticalGap,
    &PrFontMathConstants::radicalRuleThickness,
    &PrFontMathConstants::radicalExtraAscender,
    &PrFontMathConstants::radicalKernBeforeDegree,
    &PrFontMathConstants::radicalKernAfterDegree,
};
constexpr uint32_t kValueRecordCount = countOf(kValueRecords);

// Returns the MathML Core fallbacks of the constants, derived from the font's metrics in ems. `rule` is the default
// rule thickness.
PrFontMathConstants fallbackConstants(const PrFontMetrics& metrics, float rule) {
    float x = metrics.xHeight;
    return PrFontMathConstants{
        .scriptPercentScaleDown = 0.71f,
        .scriptScriptPercentScaleDown = 0.5041f,
        .axisHeight = x / 2,
        .accentBaseHeight = x,
        .flattenedAccentBaseHeight = metrics.capHeight,
        .subscriptShiftDown = metrics.subscriptPosition,
        .subscriptTopMax = x * 4 / 5,
        .superscriptShiftUp = -metrics.superscriptPosition,
        .superscriptBottomMin = x / 4,
        .subSuperscriptGapMin = 4 * rule,
        .superscriptBottomMaxWithSubscript = x * 4 / 5,
        .spaceAfterScript = 1.0f / 24,
        .stackGapMin = 3 * rule,
        .stackDisplayStyleGapMin = 7 * rule,
        .fractionNumeratorGapMin = rule,
        .fractionNumDisplayStyleGapMin = 3 * rule,
        .fractionRuleThickness = rule,
        .fractionDenominatorGapMin = rule,
        .fractionDenomDisplayStyleGapMin = 3 * rule,
        .overbarVerticalGap = 3 * rule,
        .overbarRuleThickness = rule,
        .overbarExtraAscender = rule,
        .underbarVerticalGap = 3 * rule,
        .underbarRuleThickness = rule,
        .underbarExtraDescender = rule,
        .radicalVerticalGap = rule * 5 / 4,
        .radicalDisplayStyleVerticalGap = rule + x / 4,
        .radicalRuleThickness = rule,
        .radicalExtraAscender = rule,
        .radicalKernBeforeDegree = 5.0f / 18,
        .radicalKernAfterDegree = -10.0f / 18,
        .radicalDegreeBottomRaisePercent = 0.6f,
    };
}

// Returns the MathGlyphConstruction of the glyph along the axis, or an empty table if there is none.
SfntTable constructionOf(const FontMathFace& face, uint32_t glyph, PrFontMathAxis axis) {
    bool vertical = axis == PrFontMathAxis_Vertical;
    uint32_t index = sfntCoverageIndex(vertical ? face.verticalCoverage : face.horizontalCoverage, glyph);
    if (index == kSfntCoverageMissing)
        return SfntTable{};
    uint16_t verticalCount = face.variants.uint16At(6);
    uint16_t count = vertical ? verticalCount : face.variants.uint16At(8);
    if (index >= count)
        return SfntTable{};
    uint32_t at = 10 + (vertical ? index : verticalCount + index) * 2;
    if (at + 2 > face.variants.size)
        return SfntTable{};
    return subtable(face.variants, at);
}

uint32_t variantCountOf(SfntTable construction) {
    return fitting(construction, 4, 4, construction.uint16At(2));
}

// Returns the GlyphAssembly of a construction, or an empty table if it has none.
SfntTable assemblyOf(SfntTable construction) {
    return subtable(construction, 0);
}

uint32_t partCountOf(SfntTable assembly) {
    return fitting(assembly, 6, 10, assembly.uint16At(4));
}

// A part of an assembly in font units. The algorithm of the assembly runs in double precision, so that an assembly of
// many repeats lands on its size.
struct Part {
    uint16_t glyph;
    bool extender;
    double start;
    double end;
    double full;
};

Part partAt(SfntTable assembly, uint32_t index) {
    uint32_t at = 6 + index * 10;
    return Part{
        .glyph = assembly.uint16At(at),
        .extender = (assembly.uint16At(at + 8) & 1u) != 0,
        .start = double(assembly.uint16At(at + 2)),
        .end = double(assembly.uint16At(at + 4)),
        .full = double(assembly.uint16At(at + 6)),
    };
}

// The sums over the parts of an assembly that MathML Core's algorithm reads, in font units.
struct Sums {
    uint32_t count;                 // the parts
    uint32_t extenders;             // NExt
    uint32_t others;                // NNonExt
    double extenderSize;            // SExt
    double otherSize;               // SNonExt
};

// Sums the parts of an assembly, and returns true if it is valid for the least overlap `minOverlap` (MathML Core). A
// valid assembly has an extender, grows as its extenders repeat, and has no connector shorter than `minOverlap` that
// joins another part. A part's start connector joins the part before it, and its end connector the part after it. So
// the start of the first part and the end of the last part join another part only if the part is an extender, which
// joins itself.
bool sumAssembly(SfntTable assembly, double minOverlap, Sums* out) {
    Sums sums{.count = partCountOf(assembly)};
    bool connected = true;
    for (uint32_t i = 0; i < sums.count; ++i) {
        Part part = partAt(assembly, i);
        if (part.extender) {
            ++sums.extenders;
            sums.extenderSize += part.full;
        } else {
            ++sums.others;
            sums.otherSize += part.full;
        }
        if ((i > 0 || part.extender) && part.start < minOverlap)
            connected = false;
        if ((i + 1 < sums.count || part.extender) && part.end < minOverlap)
            connected = false;
    }
    *out = sums;
    return connected && sums.extenders > 0 && sums.extenderSize - minOverlap * double(sums.extenders) > 0;
}

// The layout of an assembly for a target size, in font units, with the repeats of each extender and the overlap between
// adjacent glyphs.
struct AssemblyLayout {
    uint32_t repeats;
    uint32_t glyphCount;
    double overlap;
    double size;
};

// Lays out a valid assembly for `target` font units, as MathML Core shapes a glyph assembly. The layout takes the
// fewest repeats that cover the target at the least overlap, then the largest overlap that still covers it, within the
// length of the connectors that join.
AssemblyLayout layAssembly(SfntTable assembly, const Sums& sums, double minOverlap, double target) {
    AssemblyLayout layout{};
    double growth = sums.extenderSize - minOverlap * sums.extenders;
    double needed = (target - sums.otherSize + minOverlap * (double(sums.others) - 1)) / growth;
    // A target that the repeats miss only by the rounding of its conversion from ems takes no extra repeat. The
    // tolerance is a thousandth of a font unit, which no drawing shows.
    double repeats = needed > 0 ? prF64Ceil(needed - 1e-3 / growth) : 0;
    layout.repeats = repeats < double(kMaxRepeats) ? uint32_t(repeats) : kMaxRepeats;
    layout.glyphCount = sums.others + layout.repeats * sums.extenders;
    double natural = sums.otherSize + double(layout.repeats) * sums.extenderSize;
    if (layout.glyphCount > 1) {
        double overlap = (natural - target) / double(layout.glyphCount - 1);
        // A glyph's start connector joins if a glyph precedes it, and its end connector joins if a glyph follows it.
        uint32_t first = sums.count;
        uint32_t last = 0;
        for (uint32_t i = 0; i < sums.count; ++i) {
            if (layout.repeats != 0 || !partAt(assembly, i).extender) {
                first = first < i ? first : i;
                last = i;
            }
        }
        for (uint32_t i = first; i <= last && first < sums.count; ++i) {
            Part part = partAt(assembly, i);
            if (part.extender && layout.repeats == 0)
                continue;
            bool repeated = part.extender && layout.repeats > 1;
            if ((i > first || repeated) && part.start < overlap)
                overlap = part.start;
            if ((i < last || repeated) && part.end < overlap)
                overlap = part.end;
        }
        layout.overlap = overlap > minOverlap ? overlap : minOverlap;
    }
    layout.size = natural - layout.overlap * (layout.glyphCount > 0 ? layout.glyphCount - 1 : 0);
    return layout;
}

// Sets `info` to the box of one glyph, placed at the origin, and writes it to `out` if there is room. Returns 1.
uint32_t placeGlyph(const PrFont* font, uint16_t glyph, PrFontMathStretchInfo* info, PrFontGlyph* PR_NULLABLE out,
        uint32_t capacity) {
    PrRect bounds = prFontGlyphInkBounds(font, glyph);
    *info = PrFontMathStretchInfo{
        .kind = PrFontMathStretchKind_Glyph,
        .advance = prFontGlyphAdvance(font, glyph, 0),
        .ascent = -bounds.top,
        .descent = bounds.bottom,
        .italicsCorrection = prFontMathGlyphItalicsCorrection(font, glyph),
    };
    if (capacity > 0)
        out[0] = PrFontGlyph{glyph, PrVec2{0, 0}};
    return 1;
}

// Places the glyphs of a laid out assembly, writing up to `capacity` of them, and sets `info` to its box. Returns the
// number of glyphs.
uint32_t placeAssembly(const PrFont* font, const FontMathFace& face, SfntTable assembly, const Sums& sums,
        const AssemblyLayout& layout, PrFontMathAxis axis, PrFontMathStretchInfo* info, PrFontGlyph* PR_NULLABLE out,
        uint32_t capacity) {
    bool vertical = axis == PrFontMathAxis_Vertical;
    float scale = face.scale;
    PrFontMathStretchInfo box{
        .kind = PrFontMathStretchKind_Assembly,
        .italicsCorrection = float(assembly.int16At(0)) * scale,
    };
    // The pen advances along the axis in font units, right for a horizontal assembly and up for a vertical one. The
    // parts of a vertical assembly sit with the bottom of their ink on the pen.
    double pen = 0;
    uint32_t written = 0;
    for (uint32_t i = 0; i < sums.count; ++i) {
        Part part = partAt(assembly, i);
        PrRect bounds = prFontGlyphInkBounds(font, part.glyph);
        float advance = prFontGlyphAdvance(font, part.glyph, 0);
        if (vertical) {
            box.advance = prF32Max(box.advance, advance);
        } else if (i == 0) {
            // The ink of a horizontal assembly is the ink of its parts, which need not reach the baseline, as an
            // overline's does not.
            box.ascent = -bounds.top;
            box.descent = bounds.bottom;
        } else {
            box.ascent = prF32Max(box.ascent, -bounds.top);
            box.descent = prF32Max(box.descent, bounds.bottom);
        }
        uint32_t repeats = part.extender ? layout.repeats : 1;
        for (uint32_t k = 0; k < repeats; ++k) {
            if (written < capacity) {
                float at = float(pen) * scale;
                out[written].id = part.glyph;
                out[written].position = vertical ? PrVec2{0, -at - bounds.bottom} : PrVec2{at, 0};
            }
            ++written;
            pen += part.full - layout.overlap;
        }
    }
    float size = float(layout.size) * scale;
    if (vertical)
        box.ascent = size;
    else
        box.advance = size;
    *info = box;
    return written;
}

// Returns the size of a glyph along an axis: its advance, or the height of its ink.
float glyphSize(const PrFont* font, uint16_t glyph, PrFontMathAxis axis) {
    if (axis == PrFontMathAxis_Horizontal)
        return prFontGlyphAdvance(font, glyph, 0);
    PrRect bounds = prFontGlyphInkBounds(font, glyph);
    return bounds.bottom - bounds.top;
}

// The maximum size in bytes of a GlyphAssembly built from Unicode's characters, with an italics correction, a part
// count and five parts.
constexpr uint32_t kUnicodeAssemblyBytes = 6 + 5 * 10;

// Writes to `bytes` the GlyphAssembly that Unicode's characters build for `codePoint` along `axis`, in the format of
// the font's own table, and sets `out` to it. Each part is a glyph of the font with connectors of no length, and its
// full advance is its size along the axis. Returns false if Unicode builds none along the axis, or if the font lacks
// the glyph of a part.
bool unicodeAssembly(const PrFont* font, uint32_t codePoint, PrFontMathAxis axis,
        uint8_t (&bytes)[kUnicodeAssemblyBytes], SfntTable* out) {
    const FontMathUnicodeAssembly* parts = fontMathUnicodeAssembly(codePoint);
    if (parts == nullptr || parts->horizontal != (axis == PrFontMathAxis_Horizontal))
        return false;
    uint32_t codePoints[5] = {
        parts->start, parts->extender, parts->middle, parts->middle != 0 ? parts->extender : 0,
        parts->end,
    };
    bool extenders[5] = {false, true, false, true, false};
    uint32_t count = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        if (codePoints[i] == 0)
            continue;
        uint16_t glyph = prFontCodePointGlyph(font, codePoints[i]);
        if (glyph == 0)
            return false;
        double full = double(glyphSize(font, glyph, axis)) / double(font->math.scale);
        uint8_t* part = bytes + 6 + count * 10;
        uint16_t fields[5] = {
            glyph, 0, 0, uint16_t(prF64Min(prF64Max(full + 0.5, 0), 65535)),
            uint16_t(extenders[i] ? 1 : 0),
        };
        for (uint32_t k = 0; k < 5; ++k) {
            part[k * 2] = uint8_t(fields[k] >> 8);
            part[k * 2 + 1] = uint8_t(fields[k]);
        }
        ++count;
    }
    // The italics correction (a MathValueRecord) is zero, and the part count follows it.
    for (uint32_t i = 0; i < 6; ++i)
        bytes[i] = 0;
    bytes[5] = uint8_t(count);
    *out = SfntTable{bytes, 6 + count * 10};
    return true;
}

} // namespace

void fontMathOpen(const Sfnt& sfnt, FontMathFace* out) {
    FontMathFace face{};
    face.table = sfntTable(sfnt, kMath);
    if (face.table.size >= 10) {
        face.constants = subtable(face.table, 4);
        SfntTable info = subtable(face.table, 6);
        face.italics = subtable(info, 0);
        face.italicsCoverage = subtable(face.italics, 0);
        face.topAccents = subtable(info, 2);
        face.topAccentCoverage = subtable(face.topAccents, 0);
        face.extendedShapes = subtable(info, 4);
        face.kerns = subtable(info, 6);
        face.kernCoverage = subtable(face.kerns, 0);
        face.variants = subtable(face.table, 8);
        face.verticalCoverage = subtable(face.variants, 2);
        face.horizontalCoverage = subtable(face.variants, 4);
    } else {
        face.table = SfntTable{};
    }
    *out = face;
}

void fontMathMeasure(FontMathFace* face, const Sfnt& sfnt, float scale, const PrFontMetrics& metrics) {
    face->scale = scale;
    // MathML Core's default rule thickness is post's underline thickness, or zero without one.
    SfntTable post = sfntTable(sfnt, kPost);
    float rule = post.size >= 12 && post.int16At(10) > 0 ? metrics.underlineThickness : 0;
    PrFontMathConstants c = fallbackConstants(metrics, rule);
    SfntTable t = face->constants;
    auto has = [&](uint32_t at) { return t.size >= at + 2; };
    // A percentage of zero takes the fallback, as MathML Core specifies for the two scale-downs.
    if (has(0) && t.int16At(0) > 0)
        c.scriptPercentScaleDown = float(t.int16At(0)) / 100;
    if (has(2) && t.int16At(2) > 0)
        c.scriptScriptPercentScaleDown = float(t.int16At(2)) / 100;
    if (has(4))
        c.delimitedSubFormulaMinHeight = float(t.uint16At(4)) * scale;
    if (has(6))
        c.displayOperatorMinHeight = float(t.uint16At(6)) * scale;
    for (uint32_t i = 0; i < kValueRecordCount; ++i) {
        uint32_t at = 8 + i * 4;
        if (has(at))
            c.*kValueRecords[i] = float(t.int16At(at)) * scale;
    }
    uint32_t raise = 8 + kValueRecordCount * 4;
    if (has(raise))
        c.radicalDegreeBottomRaisePercent = float(t.int16At(raise)) / 100;
    face->values = c;
}

} // namespace pr_impl

using namespace pr_impl;

bool prFontHasMath(const PrFont* font) {
    return font->math.table.size != 0;
}

const PrFontMathConstants* prFontMathConstants(const PrFont* font) {
    return &font->math.values;
}

float prFontMathGlyphItalicsCorrection(const PrFont* font, uint16_t glyph) {
    const FontMathFace& face = font->math;
    int16_t value = 0;
    return recordValue(face.italics, face.italicsCoverage, glyph, &value) ? float(value) * face.scale : 0;
}

float prFontMathGlyphTopAccentAttachment(const PrFont* font, uint16_t glyph) {
    const FontMathFace& face = font->math;
    int16_t value = 0;
    if (recordValue(face.topAccents, face.topAccentCoverage, glyph, &value))
        return float(value) * face.scale;
    return prFontGlyphAdvance(font, glyph, 0) / 2;
}

bool prFontMathGlyphIsExtendedShape(const PrFont* font, uint16_t glyph) {
    return sfntCoverageIndex(font->math.extendedShapes, glyph) != kSfntCoverageMissing;
}

float prFontMathGlyphKern(const PrFont* font, uint16_t glyph, PrFontMathCorner corner, float height) {
    PR_ASSERT(corner <= PrFontMathCorner_BottomLeft);
    const FontMathFace& face = font->math;
    uint32_t index = sfntCoverageIndex(face.kernCoverage, glyph);
    if (index == kSfntCoverageMissing || index >= fitting(face.kerns, 4, 8, face.kerns.uint16At(2)))
        return 0;
    SfntTable kern = subtable(face.kerns, 4 + index * 8 + uint32_t(corner) * 2);
    // A MathKern holds heightCount correction heights in increasing order, then one more kern than heights. Kern i
    // applies below height i, and the last kern above the last height. A table cut short searches the heights that it
    // holds, and a kern past its end reads as zero.
    uint32_t declared = kern.uint16At(0);
    uint32_t count = fitting(kern, 2, 4, declared);
    float units = height / face.scale;
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (float(kern.int16At(2 + middle * 4)) <= units)
            low = middle + 1;
        else
            high = middle;
    }
    return float(kern.int16At(2 + declared * 4 + low * 4)) * face.scale;
}

float prFontMathMinConnectorOverlap(const PrFont* font) {
    const FontMathFace& face = font->math;
    return float(face.variants.uint16At(0)) * face.scale;
}

uint32_t prFontMathGlyphVariantCount(const PrFont* font, uint16_t glyph, PrFontMathAxis axis) {
    return variantCountOf(constructionOf(font->math, glyph, axis));
}

PrFontMathVariant prFontMathGlyphVariant(const PrFont* font, uint16_t glyph, PrFontMathAxis axis, uint32_t index) {
    const FontMathFace& face = font->math;
    SfntTable construction = constructionOf(face, glyph, axis);
    if (index >= variantCountOf(construction))
        return PrFontMathVariant{0, 0};
    uint32_t at = 4 + index * 4;
    return PrFontMathVariant{construction.uint16At(at), float(construction.uint16At(at + 2)) * face.scale};
}

uint32_t prFontMathGlyphPartCount(const PrFont* font, uint16_t glyph, PrFontMathAxis axis) {
    return partCountOf(assemblyOf(constructionOf(font->math, glyph, axis)));
}

PrFontMathPart prFontMathGlyphPart(const PrFont* font, uint16_t glyph, PrFontMathAxis axis, uint32_t index) {
    const FontMathFace& face = font->math;
    SfntTable assembly = assemblyOf(constructionOf(face, glyph, axis));
    if (index >= partCountOf(assembly))
        return PrFontMathPart{};
    Part part = partAt(assembly, index);
    return PrFontMathPart{
        .glyph = part.glyph,
        .flags = part.extender ? PrFontMathPartFlags(PrFontMathPartFlag_Extender) : PrFontMathPartFlags(0),
        .startConnector = float(part.start) * face.scale,
        .endConnector = float(part.end) * face.scale,
        .fullAdvance = float(part.full) * face.scale,
    };
}

float prFontMathGlyphAssemblyItalicsCorrection(const PrFont* font, uint16_t glyph, PrFontMathAxis axis) {
    const FontMathFace& face = font->math;
    return float(assemblyOf(constructionOf(face, glyph, axis)).int16At(0)) * face.scale;
}

uint32_t prFontMathGlyphStretch(const PrFont* font, uint16_t glyph, PrFontMathAxis axis, float size,
        PrFontMathStretchInfo* info, PrFontGlyph* PR_NULLABLE out, uint32_t capacity) {
    const FontMathFace& face = font->math;
    *info = PrFontMathStretchInfo{};
    SfntTable construction = constructionOf(face, glyph, axis);
    if (construction.size == 0)
        return 0;
    float target = size >= 0 ? size : 0;
    if (glyphSize(font, glyph, axis) >= target)
        return placeGlyph(font, glyph, info, out, capacity);
    uint16_t chosen = glyph;
    uint32_t variants = variantCountOf(construction);
    for (uint32_t i = 0; i < variants; ++i) {
        chosen = construction.uint16At(4 + i * 4);
        if (float(construction.uint16At(4 + i * 4 + 2)) * face.scale >= target)
            return placeGlyph(font, chosen, info, out, capacity);
    }
    SfntTable assembly = assemblyOf(construction);
    double minOverlap = double(face.variants.uint16At(0));
    Sums sums{};
    if (assembly.size != 0 && sumAssembly(assembly, minOverlap, &sums)) {
        double units = double(prF32Min(target, 1e30f)) / double(face.scale);
        AssemblyLayout layout = layAssembly(assembly, sums, minOverlap, units);
        return placeAssembly(font, face, assembly, sums, layout, axis, info, out, capacity);
    }
    return placeGlyph(font, chosen, info, out, capacity);
}

uint32_t prFontMathCodePointStretch(const PrFont* font, uint32_t codePoint, uint16_t glyph, PrFontMathAxis axis,
        float size, PrFontMathStretchInfo* info, PrFontGlyph* PR_NULLABLE out, uint32_t capacity) {
    uint32_t count = prFontMathGlyphStretch(font, glyph, axis, size, info, out, capacity);
    const FontMathFace& face = font->math;
    if (info->kind != PrFontMathStretchKind_None || face.variants.size != 0)
        return count;
    uint8_t bytes[kUnicodeAssemblyBytes];
    SfntTable assembly{};
    if (!unicodeAssembly(font, codePoint, axis, bytes, &assembly))
        return 0;
    float target = size >= 0 ? size : 0;
    if (glyphSize(font, glyph, axis) >= target)
        return placeGlyph(font, glyph, info, out, capacity);
    Sums sums{};
    if (!sumAssembly(assembly, 0, &sums))
        return 0;
    double units = double(prF32Min(target, 1e30f)) / double(face.scale);
    AssemblyLayout layout = layAssembly(assembly, sums, 0, units);
    return placeAssembly(font, face, assembly, sums, layout, axis, info, out, capacity);
}

uint32_t prFontMathUnicodeParts(uint32_t codePoint, PrFontMathAxis axis, uint32_t* PR_NULLABLE out, uint32_t capacity) {
    const FontMathUnicodeAssembly* parts = fontMathUnicodeAssembly(codePoint);
    if (parts == nullptr || parts->horizontal != (axis == PrFontMathAxis_Horizontal))
        return 0;
    const uint32_t codePoints[] = {parts->start, parts->extender, parts->middle, parts->end};
    uint32_t count = 0;
    for (uint32_t part : codePoints) {
        if (part == 0)
            continue;
        if (out != nullptr && count < capacity)
            out[count] = part;
        ++count;
    }
    return count;
}

float prFontMathGlyphPreferredAdvance(const PrFont* font, uint16_t glyph) {
    const FontMathFace& face = font->math;
    float advance = prFontGlyphAdvance(font, glyph, 0);
    SfntTable construction = constructionOf(face, glyph, PrFontMathAxis_Vertical);
    uint32_t variants = variantCountOf(construction);
    for (uint32_t i = 0; i < variants; ++i)
        advance = prF32Max(advance, prFontGlyphAdvance(font, construction.uint16At(4 + i * 4), 0));
    SfntTable assembly = assemblyOf(construction);
    Sums sums{};
    if (assembly.size != 0 && sumAssembly(assembly, double(face.variants.uint16At(0)), &sums)) {
        for (uint32_t i = 0; i < sums.count; ++i)
            advance = prF32Max(advance, prFontGlyphAdvance(font, partAt(assembly, i).glyph, 0));
    }
    return advance;
}
