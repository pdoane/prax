// PostScript outlines: the Type 2 charstrings of CFF and CFF2, with the variations that CFF2 blends into them.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"

#include "font/src/outline.h"
#include "font/src/sfnt.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An INDEX of CFF: a count of items and the offsets of their data.
struct CffIndex {
    SfntTable table;                // the bytes from the INDEX's start
    uint32_t count;
    uint32_t offsetSize;
    uint32_t offsets;               // where the offsets start
    uint32_t data;                  // where the data starts, less one, as offsets count from 1
    uint32_t end;                   // the byte after the INDEX
};

// The private data of a font dictionary: its local subroutines, and the variation data that its glyphs blend by.
struct CffPrivate {
    CffIndex subrs;
    uint32_t vsindex;
};

// The outline tables of a face: CFF, or CFF2 with variations.
struct CffFace {
    SfntTable table;
    bool cff2;
    CffIndex charStrings;
    CffIndex globalSubrs;
    RawArray<CffPrivate> privates;  // one for each font dictionary
    SfntTable fdSelect;             // empty if every glyph takes the first font dictionary
    uint32_t fdSelectCount;         // the glyphs that fdSelect maps
    SfntTable variations;           // the ItemVariationStore of CFF2, or empty
    SfntTable charset;              // CFF's charset, which seac reads, or empty for the ISOAdobe charset
    bool cid;                       // CFF's glyphs are named by CID, so seac does not apply
};

// Opens the CFF or CFF2 table of the face. Returns false if the face has neither, or its table does not parse.
bool cffOpen(const Sfnt& sfnt, CffFace* out);

// Frees what the face holds.
void cffReset(CffFace& face);

// Returns the number of glyphs of the face.
uint32_t cffGlyphCount(const CffFace& face);

// Loads the outline of the glyph at the coordinates into `out`, which it replaces, in font units with y up. Returns
// false for a glyph past the end; a charstring that does not run to its end loads what it drew.
bool cffLoad(const CffFace& face, SfntCoords coords, uint32_t glyph, FontGlyphOutline* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
