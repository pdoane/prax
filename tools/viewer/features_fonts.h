// Feature drawings for fonts: font sets matching styles, faces falling back, what lib/font reads from a font beyond its
// outlines, and web fonts.

#pragma once

#include "tools/viewer/drawing.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Releases the font sets that the drawings created.
void releaseDrawingFonts();

// Returns the group's feature drawings, in the order that the viewer turns through them.
const std::vector<Drawing>& fontFeatures();

} // namespace viewer

PR_ASSUME_NONNULL_END
