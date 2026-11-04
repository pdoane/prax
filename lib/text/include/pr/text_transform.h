// Text transforms: text set in another case, width or form for styling, with the content itself unchanged.

#pragma once

#include "pr/base.h"
#include "pr/locale.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/unicode.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How text is transformed. A value names a case transform, math-auto or a mathematical variant. The flags add the width
// and size transforms, which apply after the value's transform, width before size.
enum {
    PrTextTransform_None,

    // The first character of each word in titlecase, if it is a lowercase letter. A word is a run of letters, numbers
    // and connector punctuation, unbroken by marks, format characters, or case-ignorable punctuation between two
    // letters, such as an apostrophe or a middle dot.
    PrTextTransform_Capitalize,
    PrTextTransform_Uppercase,
    PrTextTransform_Lowercase,

    // Text of a single character, besides white space, in its Mathematical Italic form if it has one.
    PrTextTransform_MathAuto,

    // Every character in its form in one style of the Mathematical Alphanumeric Symbols, if it has one
    // (prUnicodeMathVariant), for MathML's mathvariant. The members follow the order of PrUnicodeMathVariant.
    PrTextTransform_MathBold,
    PrTextTransform_MathItalic,
    PrTextTransform_MathBoldItalic,
    PrTextTransform_MathScript,
    PrTextTransform_MathBoldScript,
    PrTextTransform_MathFraktur,
    PrTextTransform_MathDoubleStruck,
    PrTextTransform_MathBoldFraktur,
    PrTextTransform_MathSansSerif,
    PrTextTransform_MathBoldSansSerif,
    PrTextTransform_MathSansSerifItalic,
    PrTextTransform_MathSansSerifBoldItalic,
    PrTextTransform_MathMonospace,
    PrTextTransform_MathInitial,
    PrTextTransform_MathTailed,
    PrTextTransform_MathLooped,
    PrTextTransform_MathStretched,

    // Every character in its full-width form if it has one. A space becomes U+3000 IDEOGRAPHIC SPACE.
    PrTextTransformFlag_FullWidth = 1u << 5,

    PrTextTransformFlag_FullSizeKana = 1u << 6,     // every small kana in its full-size form
};
typedef uint8_t PrTextTransform;

// The bits of a PrTextTransform that hold its value rather than its flags.
enum { kPrTextTransformValueMask = (1u << 5) - 1 };

// Returns the transform that sets text in the mathematical alphanumeric style `variant`.
PR_CONSTEXPR PrTextTransform prTextTransformFromMathVariant(PrUnicodeMathVariant variant) {
    return PR_VALUE_CAST(PrTextTransform, PrTextTransform_MathBold + variant);
}

// The most code points that transforming one code point gives.
enum { kPrTextTransformMaxCodePoints = kPrUnicodeCaseMaxCodePoints };

// Text to transform: the bytes [start, end) of `text`, UTF-8, in one transform and one language. The rest of `text` is
// context, which the words of capitalize and the conditions of case mapping read, so text that continues across a
// change of style reads as one.
typedef struct PrTextTransformRun {
    PrStringView text;
    size_t start;
    size_t end;
    PrTextTransform transform;
    PrLocale locale;            // the language of the text, which tailors case mapping; zero for none
} PrTextTransformRun;

// Writes the transform of the code point at byte `at` of the run, which must start a code point in [start, end), to
// `out`. Returns how many code points it wrote, which is zero if the transform removes the code point.
uint32_t prTextTransformAt(const PrTextTransformRun* run, size_t at,
    uint32_t out[PR_NONNULL kPrTextTransformMaxCodePoints]);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
