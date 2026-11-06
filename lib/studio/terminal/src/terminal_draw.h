// Draws a terminal's screen row by row, in the colors that the terminal resolves. Box drawing, block elements and
// similar glyphs draw as shapes, so that they join seamlessly across cells in any font.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/raw_array.h"
#include "pr/text.h"
#include "pr/text_font.h"
#include "pr/text_grid.h"
#include "pr/vterm.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The number of fonts of a screen: regular, bold, italic and bold italic. Bit 0 of the index is bold and bit 1 italic.
constexpr uint32_t kTerminalFontCount = 4;

// The colors that the view adds to the program's colors.
struct TerminalDrawColors {
    PrColor selection;  // the background of selected cells
};

// Draws the rows of a screen. It reuses its scratch arrays from row to row.
struct TerminalDrawer {
    PrTextFont* PR_NONNULL fonts[kTerminalFontCount];
    PrTextShaper* shaper;
    PrTextGrid* grid;
    PrTextGridMetrics metrics;
    float pixelRatio;
    TerminalDrawColors colors;

    RawArray<char> text;
    RawArray<PrTextGridCell> cells;
    RawArray<PrColor> groups;           // glyph colors, by group of the grid's cells
};

// A copy of a screen's colors (pr/vterm.h), which its cells resolve against: the palette, the color roles, and whether
// reverse video swaps the default foreground and background.
struct TerminalColors {
    PrColor8 palette[kPrVtermPaletteCount];
    PrColor8 roles[PrVtermColorRole_Cursor + 1];  // by PrVtermColorRole
    bool reverse;
};

// Returns a copy of the current colors of `vterm`.
TerminalColors terminalColors(const PrVterm* vterm);

// The resolved colors of a cell as drawn.
struct TerminalCellColors {
    PrColor foreground;
    PrColor background;
    PrColor underline;
};

// Returns the colors of `cell`, resolved against `colors`, with the cell's attributes and the screen's reverse video
// applied. If `selected` is true, the background is the selection color.
TerminalCellColors terminalCellColors(const TerminalDrawer* drawer, const TerminalColors& colors, const PrVtermCell& cell,
    bool selected);

// Returns the screen's default background color, with reverse video applied.
PrColor terminalBackground(const TerminalColors& colors);

// Records `line` relative to the top left of its row. The cell backgrounds that differ from the screen's background go
// into `backgrounds`, and the glyphs and decorations go into `list`. The two may be the same list. They are separate so
// that images can draw between them.
//
// The columns from `selectionStart` to `selectionEnd`, exclusive, are selected. If `blinkHidden` is true, the glyphs
// and decorations of blinking text are left out. An image placeholder draws nothing, since the image draws in its cell.
// Returns true if the line has blinking text, which is a cell with PrVtermCellFlag_Blink that draws a glyph or a
// decoration.
bool terminalDrawRow(TerminalDrawer* drawer, PrDrawList* backgrounds, PrDrawList* list, const TerminalColors& colors,
    const PrVtermLine& line, uint32_t selectionStart, uint32_t selectionEnd, bool blinkHidden);

// Records the glyph of the cell at `column` of `line` at `origin`, in `colors`. The block cursor uses it to draw the
// cell that it covers.
void terminalDrawCell(TerminalDrawer* drawer, PrDrawList* list, const PrVtermLine& line, uint32_t column, PrVec2 origin,
    const TerminalCellColors& colors);

// Records the glyph of `codePoint` as shapes filling `rect`, so that it lines up with neighboring cells in any font.
// Handles the box drawing characters, block elements, braille patterns and Powerline separators that the terminal draws
// itself. Returns false for any other code point, which the font draws.
bool terminalDrawShape(const TerminalDrawer* drawer, PrDrawList* list, uint32_t codePoint, PrRect rect, PrColor color);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
