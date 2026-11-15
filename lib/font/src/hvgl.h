// Hierarchical variation glyphs: the outlines of Apple's hvgl table, with the part map of hvpm.
//
// The 'hvgl' table - https://developer.apple.com/fonts/TrueType-Reference-Manual/RM06/Chap6hvgl.html
//
// The 'hvpm' table - https://developer.apple.com/fonts/TrueType-Reference-Manual/RM06/Chap6hvpm.html

#pragma once

#include "pr/base.h"

#include "font/src/outline.h"
#include "font/src/sfnt.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The hvgl table of a face, and the map of its parts that its hvpm table gives, which faces sharing the hvgl table of a
// collection may each have their own of.
struct HvglFace {
    SfntTable table;
    SfntTable index;                // the part index: one offset for each part, and a sentinel
    uint32_t partCount;
    uint32_t glyphCount;            // the parts that glyphs name, which come first
    SfntTable ranges;               // the ranges of hvpm's map, or empty
    uint32_t rangeCount;
};

// Opens the hvgl table of the face, and its hvpm table if it has one. Returns false if the face has no hvgl table of a
// version that it reads, or its part index does not fit.
bool hvglOpen(const Sfnt& sfnt, HvglFace* out);

// Loads the outline of the glyph at the coordinates into `out`, which it replaces, in font units with y up. Each axis
// of the glyph's part takes the coordinate of the font axis at its index. Returns false for a glyph past the end; a
// part whose data does not parse loads no contours.
bool hvglLoad(const HvglFace& face, SfntCoords coords, uint32_t glyph, FontGlyphOutline* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
