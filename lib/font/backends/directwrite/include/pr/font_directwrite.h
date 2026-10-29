// The DirectWrite backend of lib/font: the system's fonts, as the files that DirectWrite's set of the system's fonts
// holds.

#pragma once

#include "pr/base.h"
#include "pr/font_set.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Adds the families of the fonts in DirectWrite's set of the system's fonts to `set`, each by the file that holds it,
// without reading the files (prFontSetAddFileFamily), and returns how many it added.
uint32_t prFontDirectWriteInstall(PrFontSet* set);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
