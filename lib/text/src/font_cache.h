// The face and font states that a shaper keeps (PrTextShaper).

#pragma once

#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Writes the states of the font and of its face to `face` and `font`, creating them with the functions of the shaper on
// the first use of the font. They are kept at least until the next call. Returns false and writes nothing if the
// backend cannot read the face of the font.
bool textFontCacheFind(PrTextShaper* shaper, const PrFont* font, void* PR_NULLABLE* PR_NONNULL face,
    void* PR_NULLABLE* PR_NONNULL state);

// Frees every state that the shaper keeps.
void textFontCacheDestroy(PrTextShaper* shaper);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
