// The FreeType backend of font hinters.

#pragma once

#include "pr/base.h"
#include "pr/font_hinter.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a hinter that fits outlines with FreeType. Light hinting runs FreeType's autohinter vertically. Full hinting
// runs a font's own instructions if it has them, and the autohinter otherwise. The hinter keeps a FreeType face for
// each font that it hints, at the font's axis values, until the font is freed.
PrFontHinter* prFontFreeTypeCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
