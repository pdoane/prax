// The kb shaper: kb_text_shape, reading the font's bytes itself.

#pragma once

#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates the kb shaper. It keeps a reference to each of the fonts that it shaped with most recently. It reads no
// context. It shapes a vertical run one grapheme cluster at a time. Each cluster is shaped across, with the vertical
// forms of the font and with no context, and is placed down the line by the vertical metrics of its base glyph. Nothing
// joins across clusters, and the vertical positioning of the font is left out.
PrTextShaper* prTextKbCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
