// Math fonts: the reader of the OpenType MATH table of a face, which implements pr/font_math.h.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/font_math.h"

#include "font/src/sfnt.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The subtables of the MATH table of a face, each empty if the face lacks it or its offset is null, and the constants
// of a font of the face. Reading it changes nothing, so any number of threads may read it at once.
struct FontMathFace {
    SfntTable table;                // empty if the face has no MATH table
    SfntTable constants;            // MathConstants
    SfntTable italics;              // MathItalicsCorrectionInfo
    SfntTable italicsCoverage;
    SfntTable topAccents;           // MathTopAccentAttachment
    SfntTable topAccentCoverage;
    SfntTable extendedShapes;       // ExtendedShapeCoverage
    SfntTable kerns;                // MathKernInfo
    SfntTable kernCoverage;
    SfntTable variants;             // MathVariants
    SfntTable verticalCoverage;
    SfntTable horizontalCoverage;

    float scale;                    // ems per font unit, with the size-adjust applied
    PrFontMathConstants values;     // in ems, with fallbacks
};

// Opens the MATH table of the face.
void fontMathOpen(const Sfnt& sfnt, FontMathFace* out);

// Sets the scale for a font of the face and reads its constants. A constant that the table lacks falls back to a value
// derived from `metrics`, the metrics of the font.
void fontMathMeasure(FontMathFace* face, const Sfnt& sfnt, float scale, const PrFontMetrics& metrics);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
