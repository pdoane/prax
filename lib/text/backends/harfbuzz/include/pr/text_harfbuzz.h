// The HarfBuzz shaper, reading the font's bytes itself.

#pragma once

#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates the HarfBuzz shaper. It keeps a reference to each of the fonts that it shaped with most recently.
PrTextShaper* prTextHarfBuzzCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
