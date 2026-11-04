// Math fonts: the layout constants, glyph information and glyph constructions of a font's OpenType MATH table, and the
// stretching of glyphs that it describes.
//
// Every length is in ems with the size-adjust applied, as in pr/font.h. MATH gives heights, shifts and gaps as
// distances, with y up. The values here keep the sign that MATH gives them, and each comment says which way a value
// points. The glyphs of an assembly are positioned with y down, as every glyph is. A font without a MATH table returns
// the fallbacks that MathML Core specifies.

#pragma once

#include "pr/base.h"
#include "pr/font.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns true if the font has a MATH table. The other functions answer for any font, from fallbacks if it has none.
bool prFontHasMath(const PrFont* font);

// ---------------------------------------------------------------------------------------------------------------------
// Constants

// The layout constants of a font, in the order of MathConstants. The percentages are fractions, such as 0.71 for 71%.
// The other constants are lengths in ems. A shift is a distance from the baseline in the direction that its name says.
// A height is above the baseline. A gap, a drop or a rise is a distance, normally positive. The kerns of the degree of
// a radical are added to the horizontal space before and after the degree.
//
// A constant that the font lacks takes its MathML Core fallback. The fallback derives from the font's x-height, its
// subscript and superscript offsets or its rule thickness (post's underline thickness), or is a fixed length in ems, or
// zero. A constant that MathML Core does not cover falls back to zero. The exceptions are the overbar and underbar rule
// thicknesses, which fall back to the rule thickness, and the flattened accent base height, which falls back to the cap
// height.
typedef struct PrFontMathConstants {
    float scriptPercentScaleDown;
    float scriptScriptPercentScaleDown;
    float delimitedSubFormulaMinHeight;
    float displayOperatorMinHeight;
    float mathLeading;
    float axisHeight;
    float accentBaseHeight;
    float flattenedAccentBaseHeight;
    float subscriptShiftDown;
    float subscriptTopMax;
    float subscriptBaselineDropMin;
    float superscriptShiftUp;
    float superscriptShiftUpCramped;
    float superscriptBottomMin;
    float superscriptBaselineDropMax;
    float subSuperscriptGapMin;
    float superscriptBottomMaxWithSubscript;
    float spaceAfterScript;
    float upperLimitGapMin;
    float upperLimitBaselineRiseMin;
    float lowerLimitGapMin;
    float lowerLimitBaselineDropMin;
    float stackTopShiftUp;
    float stackTopDisplayStyleShiftUp;
    float stackBottomShiftDown;
    float stackBottomDisplayStyleShiftDown;
    float stackGapMin;
    float stackDisplayStyleGapMin;
    float stretchStackTopShiftUp;
    float stretchStackBottomShiftDown;
    float stretchStackGapAboveMin;
    float stretchStackGapBelowMin;
    float fractionNumeratorShiftUp;
    float fractionNumeratorDisplayStyleShiftUp;
    float fractionDenominatorShiftDown;
    float fractionDenominatorDisplayStyleShiftDown;
    float fractionNumeratorGapMin;
    float fractionNumDisplayStyleGapMin;
    float fractionRuleThickness;
    float fractionDenominatorGapMin;
    float fractionDenomDisplayStyleGapMin;
    float skewedFractionHorizontalGap;
    float skewedFractionVerticalGap;
    float overbarVerticalGap;
    float overbarRuleThickness;
    float overbarExtraAscender;
    float underbarVerticalGap;
    float underbarRuleThickness;
    float underbarExtraDescender;
    float radicalVerticalGap;
    float radicalDisplayStyleVerticalGap;
    float radicalRuleThickness;
    float radicalExtraAscender;
    float radicalKernBeforeDegree;
    float radicalKernAfterDegree;
    float radicalDegreeBottomRaisePercent;
} PrFontMathConstants;

// The layout constants of the font, valid while the font lives. Reading them costs nothing.
const PrFontMathConstants* prFontMathConstants(const PrFont* font);

// ---------------------------------------------------------------------------------------------------------------------
// Glyphs

// Returns the italics correction of the glyph, the distance that a following glyph or a superscript moves right.
// Returns zero if the font gives none.
float prFontMathGlyphItalicsCorrection(const PrFont* font, uint16_t glyph);

// Returns the horizontal position, right of the origin of the glyph, at which an accent above it is centered. Returns
// half the advance of the glyph if the font gives none.
float prFontMathGlyphTopAccentAttachment(const PrFont* font, uint16_t glyph);

// Returns true if the font marks the glyph as an extended shape, such as a large operator or a variant of a stretchy
// glyph, whose scripts attach to its ink rather than to its accent attachment or its height.
bool prFontMathGlyphIsExtendedShape(const PrFont* font, uint16_t glyph);

// The corners of a glyph at which a script attaches.
enum {
    PrFontMathCorner_TopRight,
    PrFontMathCorner_TopLeft,
    PrFontMathCorner_BottomRight,
    PrFontMathCorner_BottomLeft,
};
typedef uint8_t PrFontMathCorner;

// Returns the cut-in kern of the glyph at `corner` for a script whose nearest edge is `height` above the baseline. The
// kern is added to the horizontal space between the glyph and the script, so a negative kern moves the script into the
// corner. Returns zero if the font gives none. The cost is a coverage lookup and a search of the corner's heights.
float prFontMathGlyphKern(const PrFont* font, uint16_t glyph, PrFontMathCorner corner, float height);

// ---------------------------------------------------------------------------------------------------------------------
// Stretchy glyphs

// The axis along which a glyph stretches.
enum {
    PrFontMathAxis_Horizontal,
    PrFontMathAxis_Vertical,
};
typedef uint8_t PrFontMathAxis;

// Returns the least overlap between the connectors of adjacent parts of an assembly.
float prFontMathMinConnectorOverlap(const PrFont* font);

// A size variant of a glyph along an axis.
typedef struct PrFontMathVariant {
    uint16_t glyph;
    float advance;              // the size of the variant along the axis
} PrFontMathVariant;

// Returns the size variants of the glyph along `axis`, from the smallest, which may be the glyph itself. There are none
// for a glyph that the font does not stretch along the axis. An `index` past the variants gives zeros.
uint32_t prFontMathGlyphVariantCount(const PrFont* font, uint16_t glyph, PrFontMathAxis axis);
PrFontMathVariant prFontMathGlyphVariant(const PrFont* font, uint16_t glyph, PrFontMathAxis axis, uint32_t index);

// The flags of a part of an assembly.
enum {
    PrFontMathPartFlag_Extender = 1u << 0,      // the part repeats any number of times, zero included
};
typedef uint8_t PrFontMathPartFlags;

// A part of the assembly of a glyph. The connectors are the lengths at its start and end along the axis that may
// overlap the connectors of its neighbors.
typedef struct PrFontMathPart {
    uint16_t glyph;
    PrFontMathPartFlags flags;
    float startConnector;
    float endConnector;
    float fullAdvance;          // the size of the part along the axis
} PrFontMathPart;

// Returns the parts of the assembly of the glyph along `axis`, from the left or the bottom, or none if the font has no
// assembly for it. An `index` past the parts gives zeros. The assembly may be one that prFontMathGlyphStretch rejects.
uint32_t prFontMathGlyphPartCount(const PrFont* font, uint16_t glyph, PrFontMathAxis axis);
PrFontMathPart prFontMathGlyphPart(const PrFont* font, uint16_t glyph, PrFontMathAxis axis, uint32_t index);

// Returns the italics correction of the assembly of the glyph along `axis`, or zero if it has none.
float prFontMathGlyphAssemblyItalicsCorrection(const PrFont* font, uint16_t glyph, PrFontMathAxis axis);

// What a stretch of a glyph made.
enum {
    PrFontMathStretchKind_None,         // the font does not stretch the glyph along the axis
    PrFontMathStretchKind_Glyph,        // one glyph: the glyph itself or one of its size variants
    PrFontMathStretchKind_Assembly,     // parts of an assembly
};
typedef uint8_t PrFontMathStretchKind;

// The box of a stretched glyph, relative to its origin on the baseline. A vertical assembly sits on the baseline, and
// its ascent is the size that it reached. A horizontal assembly starts at the origin, and its advance is the size that
// it reached.
typedef struct PrFontMathStretchInfo {
    PrFontMathStretchKind kind;
    float advance;
    float ascent;               // the ink above the baseline, positive up
    float descent;              // the ink below the baseline, positive down
    float italicsCorrection;    // the italics correction of the chosen glyph or of the assembly
} PrFontMathStretchInfo;

// Stretches the glyph along `axis` to cover `size`, as MathML Core shapes a stretchy glyph. The result is the glyph
// itself if its advance (horizontal) or ink height (vertical) covers `size`. Otherwise it is the first size variant
// that covers `size`, or else the assembly with the fewest extender repeats that covers it. An assembly spreads its
// connector overlap evenly to come as close to `size` as the connectors allow. If nothing covers `size`, the result is
// the largest variant. A non-finite or negative `size` is taken as zero.
//
// Writes `info`, and up to `capacity` glyphs to `out`, each with the position of its origin in ems with y down. Returns
// the number of glyphs, which is zero for kind None. The call has no side effects, so a caller whose buffer was too
// small calls again with a larger one.
uint32_t prFontMathGlyphStretch(const PrFont* font, uint16_t glyph, PrFontMathAxis axis, float size,
    PrFontMathStretchInfo* info, PrFontGlyph* PR_NULLABLE out, uint32_t capacity);

// Stretches `glyph`, the glyph of `codePoint` or one of its forms, as prFontMathGlyphStretch does. A font without a
// MathVariants table stretches it instead with the Unicode characters for the parts of stretched operators that MathML
// Core lists, if the font has their glyphs. The assembly is the bottom or left part, an extender, the middle part and a
// second extender if there is a middle part, and the top or right part. Its connectors have no length and do not
// overlap.
uint32_t prFontMathCodePointStretch(const PrFont* font, uint32_t codePoint, uint16_t glyph, PrFontMathAxis axis,
    float size, PrFontMathStretchInfo* info, PrFontGlyph* PR_NULLABLE out, uint32_t capacity);

// Writes up to `capacity` of the distinct Unicode characters for the parts of `codePoint` stretched along `axis` to
// `out`. These are the parts that prFontMathCodePointStretch assembles in a font without a MathVariants table. Returns
// the number of parts, which is zero for a character that the Unicode parts do not build along the axis.
uint32_t prFontMathUnicodeParts(uint32_t codePoint, PrFontMathAxis axis, uint32_t* PR_NULLABLE out, uint32_t capacity);

// Returns the preferred inline size of the glyph stretched along the block axis, as MathML Core defines it. It is the
// largest advance of the glyph, its vertical size variants and its vertical assembly.
float prFontMathGlyphPreferredAdvance(const PrFont* font, uint16_t glyph);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
