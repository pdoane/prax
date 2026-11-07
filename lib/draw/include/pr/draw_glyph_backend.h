// The interface that glyph decoders implement. Callers use the functions of pr/draw_glyph.h, which call these. Only
// decoders include this header.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a glyph decoder's backend, at the start of its state. Each implements the function of
// pr/draw_glyph.h that it is named for. A null `destroyFn` means that there is nothing to free.
struct PrDrawGlyphDecoder {
    void (*PR_NULLABLE destroyFn)(PrDrawGlyphDecoder* decoder);
    bool (*decodeFn)(PrDrawGlyphDecoder* decoder, const PrFont* font, uint16_t glyph, float pixelsPerEm,
        const PrFontPalette* PR_NULLABLE palette, PrColor foreground, PrDrawList* list);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
