// The simple shaper, for text that takes one glyph per character and pair kerning.

#pragma once

#include "pr/base.h"
#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a shaper that gives each character the glyph that the font's character map assigns it, and kerns pairs of
// glyphs by the font's GPOS pair adjustments, or its kern table if GPOS has none. It forms no ligatures, joins nothing
// and positions no marks, so it suits scripts such as Latin, Greek and Cyrillic, without a shaping library. A run down
// a vertical line places each glyph upright by its vertical origin.
PrTextShaper* prTextSimpleCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
