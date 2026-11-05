// Feature drawings for text: glyph runs shaped by lib/text from fonts that lib/font reads.

#pragma once

#include "examples/draw_features/drawing.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

// Returns the group's feature drawings, in the order that they are shown.
const Array<Drawing>& textFeatures();

} // namespace drawings

PR_ASSUME_NONNULL_END
