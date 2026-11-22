// Glyphs as a backend draws them: the position at which a run places each glyph, the glyph's outline, and the lists of
// glyphs that paint themselves.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_commands.h"
#include "pr/draw_transform.h"
#include "pr/font.h"
#include "pr/geom.h"
#include "pr/image.h"
#include "pr/path.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the position at which the run places glyph `index`. This is the transform from the glyph's ems to the run's
// space, including the font's oblique.
PrAffine2 prDrawGlyphTransform(const PrFontGlyphRun* run, uint32_t index);

// The positions within a target pixel, along each axis, at which an upright glyph is drawn, and the largest em, in
// target pixels, of a glyph drawn at them.
enum { kPrDrawGlyphPhases = 4 };
PR_CONSTANT float kPrDrawGlyphMaxPhasedEm = 160;

// Where a glyph is drawn at a phase of a target pixel, as an atlas holds it.
typedef struct PrDrawGlyphPlacement {
    PrAffine2 glyph;            // the glyph's space to target pixels (prDrawGlyphTransform, then the run's)

    // Where the glyph's origin is drawn, in target pixels: moved to the nearest of kPrDrawGlyphPhases positions within
    // a target pixel along each axis if the glyph is phased, and where its run puts it otherwise.
    PrVec2 origin;

    // True if the glyph is drawn upright, unskewed and unmirrored, at an em of at most kPrDrawGlyphMaxPhasedEm, from a
    // finite origin.
    bool phased;
} PrDrawGlyphPlacement;

// Returns where glyph `index` is drawn at a phase under `toTarget`, which maps the run's space to target pixels. A
// backend that draws glyphs at phases draws a glyph there, whether from an atlas or from its outline, so that a glyph
// lands the same however the glyphs around it are split into runs.
PrDrawGlyphPlacement prDrawGlyphPlace(const PrFontGlyphRun* run, uint32_t index, PrAffine2 toTarget);

// Appends the outline of glyph `index` to `out`, in the run's space, as the font renders it. The outline fills with the
// nonzero rule. Returns false for a glyph with no outline. `toTarget` maps the run's space to target pixels. If the
// font hints and `toTarget` places the glyph without rotation or skew, the outline fits the em's size in the target. A
// null `toTarget` gives the unhinted outline.
bool prDrawGlyphPath(const PrFontGlyphRun* run, uint32_t index, const PrAffine2* PR_NULLABLE toTarget, PrPath* out);

// Returns the number of target pixels that an em of glyph `index` covers. `toTarget` maps the run's space to target
// pixels.
float prDrawGlyphPixelsPerEm(const PrFontGlyphRun* run, uint32_t index, PrAffine2 toTarget);

// ---------------------------------------------------------------------------------------------------------------------
// Glyph decoders

// A glyph decoder draws the glyphs that a font holds as pictures, such as SVG documents (prFontGlyphSvg) and images in
// bitmap strikes (prFontGlyphImage). It is an interface so that a module that builds on lib/draw can draw them: lib/svg
// draws both (prSvgGlyphDecoderCreate). A decoder that draws images draws them as textures on the device of the
// renderer that it serves. It realizes those textures through its host's texture cache. One thread uses a decoder at a
// time.
typedef struct PrDrawGlyphDecoder PrDrawGlyphDecoder;

// What a decoder drew of a glyph.
enum {
    PrDrawGlyphDecoded_None,        // nothing: the glyph has no picture that the decoder draws
    PrDrawGlyphDecoded_Kept,        // its picture, which stays valid while the glyph cache keeps it

    // The glyph's picture, valid for this use alone. The picture draws textures that the decoder asks its cache for in
    // each use, or textures that the decoder is waiting for. The glyph cache asks the decoder again in the next use.
    PrDrawGlyphDecoded_Use,
};
typedef uint8_t PrDrawGlyphDecoded;

// Frees the decoder.
void prDrawGlyphDecoderDestroy(PrDrawGlyphDecoder* PR_NULLABLE decoder);

// Appends the glyph's picture to `list`, in ems with y down from the glyph's origin, for drawing at a size at which an
// em covers `pixelsPerEm` target pixels. The picture is the glyph's SVG document if the font has one, and otherwise its
// image from the strike for that size. An SVG document takes its colors from the palette's entries, and its text color
// from `foreground`. Returns what it drew, and appends nothing for None.
PrDrawGlyphDecoded prDrawGlyphDecoderDecode(PrDrawGlyphDecoder* decoder, const PrFont* font, uint16_t glyph,
    float pixelsPerEm, const PrFontPalette* PR_NULLABLE palette, PrColor foreground, PrDrawList* list);

// ---------------------------------------------------------------------------------------------------------------------
// Glyph cache

// A cache of the lists of glyphs that paint themselves. A backend plays a glyph's list at the glyph's position
// (prDrawGlyphTransform). The cache keeps each list for as long as each use requests it. One thread uses the cache at a
// time.
typedef struct PrDrawGlyphCache PrDrawGlyphCache;

// Creates an empty cache, or frees one. `decoder` draws the glyphs that are pictures. Without it, those glyphs paint
// nothing themselves.
PrDrawGlyphCache* prDrawGlyphCacheCreate(PrDrawGlyphDecoder* PR_NULLABLE decoder);
void prDrawGlyphCacheDestroy(PrDrawGlyphCache* PR_NULLABLE cache);

// Brackets a use, such as a renderer's prepare. The end drops the lists that the use did not request.
void prDrawGlyphCacheBegin(PrDrawGlyphCache* cache);
void prDrawGlyphCacheEnd(PrDrawGlyphCache* cache);

// Returns the list that the glyph paints itself with at a size at which an em covers `pixelsPerEm` target pixels. If
// the glyph has color layers, the list holds them in the palette's colors, with `foreground` wherever the font asks for
// the text color. Otherwise the list holds the glyph's picture from the decoder (prDrawGlyphDecoderDecode). Returns
// null for a glyph that has neither. The list stays valid until the end of the use that requested it.
PrDrawList* PR_NULLABLE prDrawGlyphCacheList(PrDrawGlyphCache* cache, const PrFont* font, uint16_t glyph,
    float pixelsPerEm, const PrFontPalette* PR_NULLABLE palette, PrColor foreground);

// The maximum number of commands that play one glyph's list.
enum { kPrDrawGlyphMaxCommands = 5 };

// Writes to `out` the commands that play `glyphList` at the position where the Glyphs command `glyphs` places glyph
// `index`, and returns the number of commands. `glyphList` is the glyph's list (prDrawGlyphCacheList). The commands are
// a transform push, a group if needed, an embed of the list, and their pops. The group applies the color's alpha and
// the command's operator and blend mode. It is needed unless the alpha is one, the operator is source-over and the
// blend mode is normal. The pushed group is written to `group`.
uint32_t prDrawGlyphListCommands(const PrDrawCommand* glyphs, uint32_t index, PrDrawList* glyphList,
    PrDrawGroup* group, PrDrawCommand out[PR_NONNULL kPrDrawGlyphMaxCommands]);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
