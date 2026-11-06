// The CoreText backend of lib/font: the system's fonts, as CoreText finds them by family name and by its fallback
// cascade.

#pragma once

#include "pr/base.h"
#include "pr/font_set.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the CoreText system. It has no state, so one system serves every set and thread. Destroying it does nothing.
PrFontSystem* prFontCoreTextCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
