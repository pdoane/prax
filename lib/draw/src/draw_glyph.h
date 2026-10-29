// The lists that color and bitmap glyphs paint themselves with.

#pragma once

#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Creates a list of the color glyph's layers in its ems, in the palette's colors, with `foreground` wherever the font
// asks for the text color. Returns null for a glyph with no layers.
PrDrawList* PR_NULLABLE drawGlyphList(const PrFont* font, uint16_t glyph, const PrFontPalette* PR_NULLABLE palette,
                                      PrColor foreground);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
