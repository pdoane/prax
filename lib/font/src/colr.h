// Color glyphs: the layers of COLR version 0 and the paint graphs of version 1, with their variations, walked into
// steps that a painter replays, and the palettes of CPAL that they color with.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/font.h"
#include "pr/geom.h"
#include "pr/raw_array.h"

#include "font/src/sfnt.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The COLR and CPAL tables of a face.
struct ColrFace {
    SfntTable colr;
    SfntTable cpal;
    SfntTable baseGlyphs;           // version 1's BaseGlyphList, or empty
    SfntTable layers;               // version 1's LayerList, or empty
    SfntTable variationMap;         // version 1's DeltaSetIndexMap, or empty
    SfntTable variations;           // version 1's ItemVariationStore, or empty
};

// What a step of a walk asks the painter to do.
enum {
    ColrOp_PushTransform,
    ColrOp_PopTransform,
    ColrOp_PushClipGlyph,
    ColrOp_PopClip,
    ColrOp_PushLayer,
    ColrOp_PopLayer,
    ColrOp_Fill,
};
typedef uint8_t ColrOp;

// A painter call, recorded by a walk and replayed after it.
struct ColrStep {
    ColrOp op;
    PrFontComposite composite;      // PushLayer
    uint16_t glyph;                 // PushClipGlyph
    uint32_t stopStart;             // Fill: the start of its stops among the stops of the walk
    PrAffine2 transform;            // PushTransform
    PrFontPaint paint;              // Fill, whose stops are set as it replays
};

// A walk of one glyph: what it colors with, and the steps and gradient stops that it records.
struct ColrWalk {
    uint32_t palette;               // the CPAL palette that the glyph's colors index
    const PrFontPaletteColor* PR_NULLABLE overrides;
    uint32_t overrideCount;
    PrColor foreground;             // the text color, which the palette index 0xFFFF asks for
    float unitsPerEm;               // the font units in an em of the result
    RawArray<ColrStep> steps;
    RawArray<PrFontColorStop> stops;
};

// Opens the color tables of the face. A face without COLR opens with no color glyphs.
void colrOpen(const Sfnt& sfnt, ColrFace* out);

// Returns true if the glyph has a paint graph or layers.
bool colrHasGlyph(const ColrFace& face, uint32_t glyph);

// Walks the glyph's paint graph, or its layers if it has none, at the coordinates, into the steps of `walk`, in ems
// with y down. Returns false if the glyph has neither.
bool colrWalk(const ColrFace& face, SfntCoords coords, uint32_t glyph, ColrWalk* walk);

// Returns the number of CPAL palettes, or of the entries of each.
uint32_t colrPaletteCount(const ColrFace& face);
uint32_t colrPaletteEntryCount(const ColrFace& face);

// Returns entry `index` of the palette, or transparent black if there is no such entry. A palette past the end reads
// the first.
PrColor colrPaletteEntry(const ColrFace& face, uint32_t palette, uint32_t index);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
