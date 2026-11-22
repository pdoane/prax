// Backgrounds: the background layers that a tree interns for its decoration groups, and how they paint.
//
// css-backgrounds-3 - https://drafts.csswg.org/css-backgrounds-3/
// css-images-4 - https://drafts.csswg.org/css-images-4/

#pragma once

#include "pr/draw.h"
#include "pr/hash_index.h"
#include "pr/layout_style.h"
#include "pr/raw_array.h"
#include "pr/texture_cache.h"

#include "layout/src/layout_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An ask made by a recording of the tree, for a version that had not arrived.
struct LayoutTextureAsk {
    const PrTextureSource* image;       // the tree's textures hold a reference
    PrTextureCacheAsk ask;
};

// The textures that a tree's paints draw images from: its host's cache (prLayoutTreeSetTextures), and the asks that the
// tree's recordings wait for. A recording leaves out an image that has not arrived, or draws a smaller version of it.
// The tree keeps such a recording until the cache reports an arrival (layoutTexturesRefresh).
struct LayoutTextures {
    PrTextureCache* PR_NULLABLE cache;
    uint64_t generation;                // the generation of the cache that the tree's recordings drew from
    uint64_t arrivals;                  // the cache's count of arrivals before the first ask in `waiting` was made
    RawArray<LayoutTextureAsk> waiting; // the pending asks, each listed once
};

// One interned list of background layers. Its layers' gradients point into `stops`, and their filter lists into
// `filters`, which the entry owns.
struct LayoutBackground {
    PrLayoutBackgroundLayer* PR_NULLABLE layers;    // null while the entry is free, and for the empty list at index 0
    uint32_t count;
    PrLayoutColorStop* PR_NULLABLE stops;
    PrLayoutFilter* PR_NULLABLE filters;
    RawArray<uint8_t> key;                             // the bytes that equal lists share (backgroundKey)
    uint32_t hash;
    uint32_t references;                            // number of decoration groups that name it
    bool blends;                                    // a layer blends (layoutBackgroundBlends)
};

// The background layers of a tree's styles. Index 0 is the empty list, which is never freed. An index names its entry
// until the entry is freed.
struct LayoutBackgrounds {
    RawArray<LayoutBackground> entries;
    RawArray<uint32_t> free;           // freed indices, which interning reuses
    HashIndex index;
    RawArray<uint32_t> candidates;     // same as LayoutTable::candidates
};

// Prepares the table with the empty list at index 0.
void layoutBackgroundsInit(LayoutBackgrounds* backgrounds);

// Interns `count` layers as prLayoutBackgroundIntern defines. An entry that it adds holds references to the compounds
// that its lengths name, and to its images and lists.
PrLayoutBackgroundIndex layoutBackgroundsIntern(LayoutBackgrounds* backgrounds, LayoutCompounds* compounds,
    const PrLayoutBackgroundLayer* layers, uint32_t count);

// Returns the entry at `index`.
inline const LayoutBackground& layoutBackgroundAt(const LayoutBackgrounds* backgrounds, PrLayoutBackgroundIndex index) {
    PR_ASSERT(index < backgrounds->entries.count);
    return backgrounds->entries.data[index];
}

// Adds `delta` to the references of the entry at `index`.
void layoutBackgroundsCount(LayoutBackgrounds* backgrounds, PrLayoutBackgroundIndex index, uint32_t delta);

// Frees the entries that no group names, of those listed as candidates, and releases what they hold.
void layoutBackgroundsCollect(LayoutBackgrounds* backgrounds, LayoutCompounds* compounds);

// Returns true if every live entry that no group names is listed as a candidate.
bool layoutBackgroundsVerify(const LayoutBackgrounds* backgrounds);

// Frees every entry and the table's memory.
void layoutBackgroundsReset(LayoutBackgrounds* backgrounds);

// Appends the bytes that identify `count` layers to `key`: every field that painting reads, a gradient's stops and a
// filter list's items in place of the pointers to them, an image's or list's address, and a painter's function and
// context. Equal layers append equal bytes.
void layoutBackgroundKey(RawArray<uint8_t>& key, const PrLayoutBackgroundLayer* layers, uint32_t count);

// Adds `delta` to the references of what `count` layers name: the compounds of their lengths, their images and lists,
// the contexts of their painters that share them, and the sources of their filter lists. A list that a layer names is
// frozen when it gains a reference.
void layoutBackgroundCountLayers(LayoutCompounds* compounds, const PrLayoutBackgroundLayer* layers, uint32_t count,
    uint32_t delta);

// The boxes of a box whose background paints, in the space of the list it paints into. `area` is where a layer paints,
// if it is not empty, in place of the box that the layer's clip names: the canvas, for the root's background.
struct LayoutBackgroundBoxes {
    PrRect boxes[3];                // by PrLayoutBackgroundBox
    PrRect area;

    // If `rounded`, the boxes with their corners, by PrLayoutBackgroundBox, which the background is clipped to the
    // curves of (css-backgrounds-3/#corner-clipping). Otherwise every corner is square.
    PrCornerRect shapes[3];
    bool rounded;
};

// Records `g` over `rect`, in a gradient box of `box`. `compounds` resolves the compound lengths that `g` names, if
// any.
void layoutBackgroundDrawGradient(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutGradient& g, PrRect box,
    PrRect rect, PrDrawEdges edges, PrDrawList* list);

// Gives `boxes` the corners of `border`, the border box's shape: the padding box's and the content box's corners are
// the border box's, reduced by the widths between them (css-backgrounds-3/#corner-shaping). Only the boxes in `used`, a
// mask of PrLayoutBackgroundBox bits, are given their shapes, since insetting a shape costs more than its use.
void layoutBackgroundRound(LayoutBackgroundBoxes& boxes, const PrCornerRect& border, uint32_t used);

// Records the background layer `layer`. It is sized and placed in the box that its origin names, and painted within the
// box that its clip names, with its blend mode and operator. An image is drawn from `textures`, and a layer whose image
// has not arrived draws nothing. Returns true if anything was recorded.
bool layoutBackgroundPaintLayer(LayoutTextures* PR_NULLABLE textures, const LayoutCompounds* compounds,
    const PrLayoutBackgroundLayer& layer, const LayoutBackgroundBoxes& boxes, PrDrawList* list);

// Asks the cache for a texture of `image` as `ask` describes. Returns true and writes the region of the texture to
// `out` if a texture has arrived. That texture is the version that the ask needs, or a smaller version while the needed
// one is on its way. If the needed version has not arrived, the ask joins the asks that the tree waits for. Without a
// cache, no image arrives. The caller holds the image on the list that draws the region (prDrawListHoldSource).
bool layoutTextureOf(LayoutTextures* PR_NULLABLE textures, const PrTextureSource* image, PrTextureCacheAsk ask,
    PrTextureRegion* out);

// Returns true if the tree's recordings are stale: the cache moved to another device, or a texture arrived while a
// recording waited for one. Otherwise makes each waiting ask again, because a trim of the cache cancels a queued
// realization that no ask in the frame requested. An ask that has failed is removed from the waiting asks.
bool layoutTexturesRefresh(LayoutTextures* textures);

// Stops waiting for every ask, and records the cache's current generation as the generation that the next recordings
// draw from.
void layoutTexturesForget(LayoutTextures* textures);

// Stops waiting for every ask and frees the memory that the asks used. Does not read the cache.
void layoutTexturesReset(LayoutTextures* textures);

// Copies the filter items of `image` into `filters` from `next`, points the image at the copy, and advances `next`.
void layoutImageTakeFilters(PrLayoutBackgroundLayer& image, PrLayoutFilter* PR_NULLABLE filters, uint32_t* next);

// Returns the source of the image of `layer` drawn at its concrete object size `size`, in pixels, filtered by its
// filter list. The source covers the rectangle from the origin to `size`, or the image's own extent if it is unfiltered
// and is scaled to `size`. A list that the source reads may be written to `drawn`, which the caller releases.
//
// An image is drawn from `textures`. If its texture has not arrived, the source is not valid (prDrawSourceIsValid), and
// the caller records nothing with it. `into` is the list that the caller records the source into. It must not be
// frozen. If the source is a region of a texture, `into` holds a reference to the image.
PrDrawSource layoutImageSource(LayoutTextures* PR_NULLABLE textures, const LayoutCompounds* PR_NULLABLE compounds,
    const PrLayoutBackgroundLayer& layer, PrVec2 size, PrDrawList* into, PrDrawList* PR_NULLABLE* PR_NONNULL drawn);

// Returns true if one of `count` layers composites onto the layers below it otherwise than normally and source-over, so
// that the background is an isolated group (compositing-1/#background-blend-mode).
bool layoutBackgroundBlends(const PrLayoutBackgroundLayer* layers, uint32_t count);

// Appends the bytes that identify a source to `key`: its functions, its context and its version.
void layoutSourceKey(RawArray<uint8_t>& key, const PrLayoutSource& source);

// Adds `delta` to the references of a source's context, if the source shares it.
void layoutSourceCount(const PrLayoutSource& source, uint32_t delta);

// Appends the bytes that identify `count` filter items to `key`: every field that drawing reads, and their sources.
void layoutFilterKey(RawArray<uint8_t>& key, const PrLayoutFilter* PR_NULLABLE filters, uint32_t count);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
