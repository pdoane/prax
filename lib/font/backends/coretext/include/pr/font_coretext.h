// The CoreText backend of lib/font: the system's fonts, as the files that CoreText's font manager makes available.

#pragma once

#include "pr/base.h"
#include "pr/font_set.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Adds the families of the fonts that CoreText makes available to `set`, each by the file that holds it, without
// reading the files (prFontSetAddFileFamily), and returns how many it added.
uint32_t prFontCoreTextInstall(PrFontSet* set);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
