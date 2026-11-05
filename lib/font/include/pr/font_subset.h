// Font subsets: a font's file cut down to the glyphs that a document draws, for the document to embed.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/font.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Receives the next bytes of a file, in order. The bytes are valid only during the call.
typedef void (*PrFontWriteFn)(void* PR_NULLABLE context, PrByteView bytes);

// The forms that a subset is written in.
enum {
    PrFontSubsetFormat_None,        // no subset: the font has no outlines, only bitmap strikes
    PrFontSubsetFormat_TrueType,    // an sfnt of TrueType outlines (glyf), as PDF's FontFile2 holds one
    PrFontSubsetFormat_Cff,         // a bare CFF table, as PDF's FontFile3 of subtype CIDFontType0C holds one
};
typedef uint8_t PrFontSubsetFormat;

// Returns the form of the font's subsets. A font whose file holds the outlines that it draws is subset in the form of
// its outlines: TrueType for glyf, and CFF for CFF. Any other font with outlines, such as an instance of a variable
// font away from its default, a font with synthetic bold, or a font of CFF2 outlines, is subset as CFF, with the
// outlines that it draws. A font of bitmap strikes only has none. Synthetic oblique and size-adjust are left to the
// code that places the glyphs, as they are for outlines (prFontGlyphOutline).
PrFontSubsetFormat prFontSubsetFormat(const PrFont* font);

// Writes a subset of the font's file to `write`, in the form that prFontSubsetFormat returns, and returns that form.
// The glyphs of `glyphs`, the glyphs that they are built from, and glyph zero keep their outlines and metrics under the
// ids that they have in the font. Every other glyph below the largest id kept is empty, and glyphs past it are left
// out. Glyph ids past the font's glyphs are ignored. A subset of the outlines that the font draws holds them unhinted,
// with no widths. Writes nothing and returns None for a font that has no subsets, or whose tables do not parse.
PrFontSubsetFormat prFontSubset(const PrFont* font, const uint16_t* PR_NULLABLE glyphs, uint32_t glyphCount,
    PrFontWriteFn write, void* PR_NULLABLE context);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
