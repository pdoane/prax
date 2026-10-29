// The CoreText shaper, over a CTFont made from the font's bytes.

#pragma once

#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates the CoreText shaper. It keeps a reference to each of the fonts that it shaped with most recently. If CoreText
// falls back to another font for a character, the glyph is zero, which is the missing glyph of the font. The shaper
// reads no context. It shapes a vertical run in the vertical layout of CoreText, with the vertical forms and vertical
// positioning of the font.
PrTextShaper* prTextCoreTextCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
