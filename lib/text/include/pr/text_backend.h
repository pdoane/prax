// The interface that text shaper backends implement. Callers use the functions of pr/text.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a shaper's backend, at the start of its state. Each implements the function of pr/text.h that it is
// named for.
//
// prTextShaperShape passes `shapeFn` a run whose kerning, caps and position are already expressed as features, with the
// font's own features before the run's. Its script is the one that the text starts in, which is Zzzz only for text with
// no script other than Common and Inherited. prTextShaperShape then scales and marks the glyphs that `shapeFn` appends.
// One thread uses a shaper at a time. Threads that shape at the same time each use a shaper of their own.
struct PrTextShaper {
    void (*destroyFn)(PrTextShaper* shaper);
    PrStatus (*shapeFn)(PrTextShaper* shaper, const PrTextRun* run, PrTextGlyphBuffer* out);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
