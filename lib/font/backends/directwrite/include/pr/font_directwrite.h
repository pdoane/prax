// The DirectWrite backend of lib/font: the system's fonts, as DirectWrite finds them by family name and by its
// fallback.

#pragma once

#include "pr/base.h"
#include "pr/font_set.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates the DirectWrite system, or returns null if DirectWrite is not available.
PrFontSystem* PR_NULLABLE prFontDirectWriteCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
