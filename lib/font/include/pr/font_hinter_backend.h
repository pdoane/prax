// The interface that hinter backends implement. Callers use the functions of pr/font_hinter.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/font_hinter.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a hinter's backend, at the start of its state. Each implements the function of pr/font_hinter.h that
// it is named for. A null `destroyFn` means that there is nothing to free, and a null `forgetFn` that the hinter keeps
// nothing for a font.
//
// A hinter is called from any thread, so it locks whatever it shares between calls. It reads the fonts that it hints
// through their public functions: their bytes, index and axis values.
struct PrFontHinter {
    void (*PR_NULLABLE destroyFn)(PrFontHinter* hinter);
    bool (*outlineFn)(PrFontHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontHinting hinting,
        float embolden, PrPath* out);
    bool (*advanceFn)(PrFontHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontHinting hinting,
        float* out);
    void (*PR_NULLABLE forgetFn)(PrFontHinter* hinter, uint64_t fontId);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
