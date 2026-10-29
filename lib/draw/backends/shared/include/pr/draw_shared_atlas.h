// Atlases: caches of the coverage of glyphs and small paths, on pages of 8-bit alpha, for backends that draw them as
// textured rectangles.

#pragma once

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/draw_list.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/pixels.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A cache of the coverage of glyphs and of filled or stroked paths, on pages. A glyph or path is drawn from the atlas
// if it is drawn without rotation or skew at a modest size. The atlas rasterizes an entry's coverage the first time its
// glyph or path is drawn at a scale and at one of a few positions within a pixel. The coverage lands on the pixel grid.
// The caller keeps a texture for each page and updates it with the page's changes.
typedef struct PrDrawAtlas PrDrawAtlas;

// How an atlas lays out its pages and rasterizes its entries.
typedef struct PrDrawAtlasOptions {
    uint32_t size;              // a page is `size` by `size` texels

    // The maximum number of pages. If every page holds an entry found since prDrawAtlasNext, a new entry is not drawn
    // from the atlas.
    uint32_t maxPages;

    // The side length of a block of full coverage at the top left of the first page. Entries are never placed in it.
    uint32_t solidSize;

    float tolerance;            // how far a flattened outline may lie from its curves, in target pixels
} PrDrawAtlasOptions;

// Returns options for pages of 1024 texels on a side, at most four pages, no solid block, and a tolerance of a
// twentieth of a pixel.
PrDrawAtlasOptions prDrawAtlasOptionsDefault(void);

// An entry drawn from the atlas: its texels on a page, which are placed at `origin` in target pixels.
typedef struct PrDrawAtlasEntry {
    uint32_t page;
    PrUVec2 texel;              // the texels' top left on the page
    PrUVec2 extent;             // zero for an entry with no texels
    PrIVec2 origin;
} PrDrawAtlasEntry;

// What an atlas holds and has done.
typedef struct PrDrawAtlasInfo {
    uint32_t entryCount;
    uint64_t rasterizations;    // number of entries rasterized since the atlas was created
    uint64_t clears;            // number of pages emptied to make room since the atlas was created
} PrDrawAtlasInfo;

// Creates an empty atlas, or frees one. A null `options` means the defaults.
PrDrawAtlas* prDrawAtlasCreate(const PrDrawAtlasOptions* PR_NULLABLE options);
void prDrawAtlasDestroy(PrDrawAtlas* PR_NULLABLE atlas);

// Forgets every page and entry.
void prDrawAtlasClear(PrDrawAtlas* atlas);

// Releases the entries found since the last call. An entry that prDrawAtlasGlyph or prDrawAtlasPath finds keeps its
// texels in place until the next call, so that the draws that read them see them unchanged. When the atlas is full, it
// empties only a page that holds none of those entries.
void prDrawAtlasNext(PrDrawAtlas* atlas);

// Keeps the page's texels in place until the next prDrawAtlasNext, as if an entry on it had been found, for draws that
// read entries found in an earlier frame.
void prDrawAtlasKeepPage(PrDrawAtlas* atlas, uint32_t page);

// Finds glyph `index` of the run, with `transform` mapping the run's space to target pixels, and rasterizes the glyph
// on first use. Returns false if the glyph is not drawn from the atlas because it is rotated, skewed, mirrored or too
// large, or fits on no page.
bool prDrawAtlasGlyph(PrDrawAtlas* atlas, const PrFontGlyphRun* run, uint32_t index, PrAffine2 transform,
                      PrDrawAtlasEntry* out);

// Finds the coverage of `path`, with `transform` mapping it to target pixels. The path is filled by `rule`, or stroked
// by `stroke` (prDrawLowerStroke) and filled by the nonzero rule. Rasterizes the coverage on first use. The entry is
// keyed on the path's id, the rule, the stroke's shape, measure and alignment, the transform's scale, and the
// transform's translation within a pixel to a quarter pixel. Returns false if the path is not drawn from the atlas.
// That is the case if it is rotated, skewed, mirrored or too large, if it fits on no page, or if an aligned stroke has
// both open and closed contours, which composite as two fills.
bool prDrawAtlasPath(PrDrawAtlas* atlas, const PrPath* path, PrPathFillRule rule,
                     const PrDrawStroke* PR_NULLABLE stroke, PrAffine2 transform, PrDrawAtlasEntry* out);

// Returns what the atlas holds and has done.
PrDrawAtlasInfo prDrawAtlasInfo(const PrDrawAtlas* atlas);

// Returns the number of pages.
uint32_t prDrawAtlasPageCount(const PrDrawAtlas* atlas);

// Returns the page's texels, in R8.
PrPixelView prDrawAtlasPageTexels(const PrDrawAtlas* atlas, uint32_t page);

// Writes the region of the page that changed since the previous call for the page to `out`. The first call covers the
// whole page. Returns false if nothing changed.
bool prDrawAtlasTakeChanges(PrDrawAtlas* atlas, uint32_t page, PrIRect* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
