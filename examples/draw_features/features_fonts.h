// Feature drawings for fonts: font sets matching styles, faces falling back, what lib/font reads from a font beyond its
// outlines, and web fonts.

#pragma once

#include "examples/draw_features/drawing.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

// Releases the font sets that the drawings created.
void releaseDrawingFonts();

// Returns the group's feature drawings, in the order that they are shown.
const Array<Drawing>& fontFeatures();

} // namespace drawings

PR_ASSUME_NONNULL_END
