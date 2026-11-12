// The face and font states that a shaper keeps, which the backends share.
//
// A face is a face of a font file as the backend reads it, and every font made from those bytes shares it. A variable
// instance or a derived font differs from its base in its design point and features, not in its tables, and reading the
// tables is the costly part. The state of a font therefore holds only what the font adds to its face. The cache keeps
// the states of the fonts that the shaper shaped with most recently, and of the faces that they read. Each state starts
// zeroed and stays at its address while it is kept. The cache keeps alive the bytes that a face reads.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/text_backend.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct TextSharedFace;
struct TextSharedFont;

// A backend's states of a face and of a font: their sizes, the functions that create and destroy them, and the function
// that shapes a run with them. `createFace` returns false if the backend cannot read the face.
struct TextSharedFontStates {
    uint32_t faceSize;
    uint32_t fontSize;
    bool (*createFace)(PrTextShaper* shaper, const PrFont* font, void* face);
    void (*destroyFace)(PrTextShaper* shaper, void* face);
    void (*createFont)(PrTextShaper* shaper, void* face, const PrFont* font, void* state);
    void (*destroyFont)(PrTextShaper* shaper, void* state);
    PrStatus (*shape)(PrTextShaper* shaper, void* face, void* font, const PrTextRun* run, PrTextGlyphBuffer* out);
};

// The states that a shaper keeps, which its backend embeds.
struct TextSharedFontCache {
    const TextSharedFontStates* states;
    RawArray<TextSharedFace*> faces;
    RawArray<TextSharedFont*> fonts;
    uint64_t clock;                 // counts the finds, to order the states by their last use
};

// Shapes `run` with the states of its font and its face, creating them on the first use of the font. Returns
// NotSupported if the backend cannot read the face of the font.
PrStatus textSharedFontCacheShape(PrTextShaper* shaper, TextSharedFontCache* cache, const PrTextRun* run,
    PrTextGlyphBuffer* out);

// Frees every state that the cache keeps.
void textSharedFontCacheReset(PrTextShaper* shaper, TextSharedFontCache* cache);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
