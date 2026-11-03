// Estimates of how text sets, from the advance of each code point alone, for text too long to shape.
//
// An estimator reads no glyph sequence, so it knows nothing of ligatures, kerning or reordering. Its cost is a table
// lookup for each code point, so a host can estimate a whole document in the time that it takes to shape a few pages.
// One thread uses an estimator at a time.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_font.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The advances of a text font's code points along one axis.
typedef struct PrTextEstimator PrTextEstimator;

// Creates an estimator of `font` along `axis`, which holds a reference to the font, or frees one.
PrTextEstimator* prTextEstimatorCreate(PrTextFont* font, PrTextAxis axis);
void prTextEstimatorDestroy(PrTextEstimator* PR_NULLABLE estimator);

// Returns the advance of `codePoint` in px, or of the font's U+FFFD or zero if no face has a glyph for it.
float prTextEstimatorAdvance(PrTextEstimator* estimator, uint32_t codePoint);

// Returns the advance of an average byte of Latin text in px, for a length of text that has not been read.
float prTextEstimatorByteAdvance(PrTextEstimator* estimator);

// Returns the number of lines that `text` sets in, with greedy breaks at white space, after a hyphen and between wide
// characters, at `width` px. A word that fits on no line breaks anywhere. A tab advances to the next stop of eight
// spaces. Text with no bytes takes one line. The cost is proportional to the size of the text.
uint32_t prTextEstimatorLineCount(PrTextEstimator* estimator, PrStringView text, float width);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
