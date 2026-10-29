// Text transforms: text set in another case, width or form for styling, with the content itself unchanged.

#pragma once

#include "pr/base.h"
#include "pr/locale.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/unicode.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How text is transformed. A value names the case transform, or math-auto, and the flags add the width and size
// transforms, which apply after the case transform, the width before the size.
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

    // Every character in its full-width form if it has one. A space becomes U+3000 IDEOGRAPHIC SPACE.
    PrTextTransformFlag_FullWidth = 1u << 3,

    PrTextTransformFlag_FullSizeKana = 1u << 4,     // every small kana in its full-size form
};
typedef uint8_t PrTextTransform;

// The bits of a PrTextTransform that hold its value rather than its flags.
enum { kPrTextTransformValueMask = (1u << 3) - 1 };

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
