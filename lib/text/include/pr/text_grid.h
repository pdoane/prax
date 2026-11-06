// Text grids: text set in a grid of cells, as a terminal sets it. A grid serves a producer that draws glyph runs
// (prDrawListGlyphs) outside the inline formatting of layout.
//
// A grid's text is a row of cells. Each cell holds one grapheme cluster and spans one or more columns. The cells are
// shaped in the faces that their text fonts match, in cell order, with no bidirectional reordering, so each cell stays
// in its column whatever its script. The glyphs of each cluster are centered in the columns of its cell, so the text
// keeps to the grid whatever the advances of its faces. Glyphs wider than their columns scale down to fit, and are
// centered vertically on the line of the first available face of the cell's text font (prTextFontFirst). A ligature is
// centered in the columns of all the cells that it joins. One thread uses a grid at a time.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_font.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The size of a grid's cells, and the place of the lines that text draws in them, in px. Each value is a whole number
// of target pixels, so that adjacent cells and their lines meet without seams.
typedef struct PrTextGridMetrics {
    PrVec2 cell;
    float baseline;                 // from the top of a cell

    // The top of each line, from the top of a cell, and its thickness, which is at least one target pixel. The
    // underline stays inside the cell.
    float underlineTop;
    float underlineThickness;
    float strikeoutTop;
    float strikeoutThickness;
} PrTextGridMetrics;

// Returns the metrics of the cells that `font` sets text in at `pixelRatio` target pixels per px, which is above zero.
// A cell is as wide as the advance of the digit zero. It is as tall as the line of the first available face
// (prTextFontFirst) times `lineHeight`. A `lineHeight` of one gives the face's own line, and zero or less counts as
// one.
PrTextGridMetrics prTextGridMetrics(PrTextFont* font, float pixelRatio, float lineHeight);

// A row of text set in cells.
typedef struct PrTextGrid PrTextGrid;

// A cell of the grid's text.
typedef struct PrTextGridCell {
    uint32_t start;                 // the first byte of its cluster, which runs to the start of the next cell
    uint32_t column;
    uint32_t columns;               // the columns that it spans, at least one
    uint32_t font;                  // an index into PrTextGridDesc::fonts

    // A value that the caller chooses, such as a color. Every cell of a run has the same group (prTextGridRunGroup).
    uint32_t group;
} PrTextGridCell;

// The text and cells that prTextGridSet sets. The grid copies what it keeps, so none of this needs to outlive the call.
typedef struct PrTextGridDesc {
    PrStringView text;                          // UTF-8
    const PrTextGridCell* PR_NULLABLE cells;    // in the order of their columns and of their text, from byte zero
    uint32_t cellCount;
    PrTextFont* PR_NONNULL const* PR_NONNULL fonts;
    uint32_t fontCount;
    PrTextShaper* shaper;
    float pixelRatio;                           // target pixels per px, above zero
    float cellWidth;                            // in px
    float baseline;                             // the y of the glyphs' baseline, in px
} PrTextGridDesc;

// Creates an empty grid, or frees one.
PrTextGrid* prTextGridCreate(void);
void prTextGridDestroy(PrTextGrid* PR_NULLABLE grid);

// Sets the text of `desc` in its cells, replacing the contents of the grid and keeping its storage. Each cell's cluster
// is matched in the cell's text font (prTextFontMatch), which may add faces to that text font.
void prTextGridSet(PrTextGrid* grid, const PrTextGridDesc* desc);

// The glyph runs of the cells. Each run has one face and one group. Its glyphs are placed relative to the left of the
// row and the top of its cells. A run stays valid until the grid is set again, and its font belongs to the text font
// that matched it (prTextFontMatchedFont).
uint32_t prTextGridRunCount(const PrTextGrid* grid);
PrFontGlyphRun prTextGridRun(const PrTextGrid* grid, uint32_t index);

// The group of the cells of run `index`.
uint32_t prTextGridRunGroup(const PrTextGrid* grid, uint32_t index);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
