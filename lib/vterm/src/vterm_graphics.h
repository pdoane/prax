// kitty's terminal graphics protocol. This module runs the commands that transmit, place, query and delete images. It
// keeps the images and placements of each screen, and moves the placements with the text as the screen changes.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/vterm.h"
#include "pr/vterm_graphics.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct Vterm;

// An image that a program transmitted.
struct VtermStoredImage {
    uint32_t id;                // the program's id, the id that the terminal gave, or zero for none
    uint32_t number;            // the program's image number (I), or zero
    uint64_t key;               // unique among the terminal's images (PrVtermPlacement's `image`)
    uint64_t used;              // the graphics clock when the image was last transmitted or put
    PrUVec2 extent;
    RawArray<uint8_t> pixels;   // Rgba8 with straight alpha, by row
};

// A placement of an image. A placement on the screen is anchored at `line`, a line number of its screen. On the primary
// screen, line numbers include the lines that the scrollback dropped (prVtermScrollbackDropped). On the alternate
// screen, the line number is the row. A virtual placement has no anchor and shows through placeholders.
struct VtermStoredPlacement {
    uint64_t image;             // the image's key
    uint32_t id;                // the placement's id, or zero
    uint64_t order;             // the graphics clock when the placement was made
    bool isVirtual;
    int64_t line;
    uint32_t column;
    PrUVec2 sourceOrigin;
    PrUVec2 sourceExtent;

    // The destination, in cells from the top left of the anchor, and the number of cells that the placement spans. Only
    // the rows from `firstRow` to `endRow`, excluded, still show.
    PrVec2 offset;
    PrVec2 extent;
    uint32_t columns;
    uint32_t rows;
    uint32_t firstRow;
    uint32_t endRow;

    int32_t z;
};

// The images and placements of one screen.
struct VtermGraphicsStore {
    RawArray<VtermStoredImage> images;
    RawArray<VtermStoredPlacement> placements;
};

// A transmission that arrives in chunks. It keeps the text of the first chunk's keys and the bytes decoded so far. The
// base64 characters past a chunk's last whole group of four wait in `carry` for the next chunk. A failure is answered
// when the last chunk arrives.
struct VtermGraphicsLoad {
    bool active;
    bool failed;                // a chunk was not base64 or passed the limit
    bool tooLarge;              // the chunks passed the limit
    RawArray<char> control;
    RawArray<uint8_t> data;
    char carry[4];
    uint8_t carryCount;
};

// The graphics of a terminal.
struct VtermGraphics {
    // What the caller allows: the budget in bytes for the images' pixels, the media, and the codecs. With a budget of
    // zero, the terminal ignores the protocol.
    uint64_t budget;
    PrVtermImageFlags flags;
    PrCompressCodec* PR_NULLABLE compressCodec;
    PrImageCodec* PR_NULLABLE imageCodec;

    VtermGraphicsStore stores[2];   // by VtermScreenKind
    VtermGraphicsLoad load;
    PrUVec2 cellPixels;
    uint64_t clock;                 // the last value given as a key, a use or an order
    uint64_t bytes;                 // the size of the pixels of both stores' images
    uint32_t nextId;                // the next id that the terminal gives an image
};

// Initializes the graphics of a terminal that `desc` describes.
void vtermGraphicsInit(VtermGraphics* graphics, const PrVtermDesc* desc);

// Frees the graphics' storage.
void vtermGraphicsReset(VtermGraphics* graphics);

// Removes every image and placement, and drops a transmission in progress, as a full reset does.
void vtermGraphicsClear(Vterm* vterm);

// Runs a graphics command, given the text of its APC string after the `G`.
void vtermGraphicsCommand(Vterm* vterm, PrStringView text);

// Moves the active screen's placements as the scroll region scrolls by `delta` rows. A positive delta scrolls down. The
// caller calls this before the region scrolls. The region is the rows from `top` to `bottom`, excluded, and the columns
// between the margins. Only the placements that lie wholly within the region move. If the region reaches an edge of the
// screen, it also holds the placements that extend past that edge. A moved placement keeps only the rows that stay in
// the region, and is removed if none do. `scrollback` is true if the region's top is the screen's top and the rows that
// leave it enter the scrollback. The placements then follow their lines into the scrollback.
void vtermGraphicsScroll(Vterm* vterm, uint32_t top, uint32_t bottom, int32_t delta, bool scrollback);

// vtermGraphicsEraseScreen removes the placements that show on the screen's rows (ED 2). vtermGraphicsEraseScrollback
// clips the primary screen's placements to the screen's rows and removes those that show only in the scrollback (ED 3).
// The scrollback's clear then drops the lines.
void vtermGraphicsEraseScreen(Vterm* vterm);
void vtermGraphicsEraseScrollback(Vterm* vterm);

// Removes every image and placement of the alternate screen, as clearing that screen does.
void vtermGraphicsClearAlternate(Vterm* vterm);

// Moves the primary screen's placements to their lines after a resize rewraps them. `firstLine` is the line number of
// the first old line, and `newFirstLine` is that of the first new line. `map` holds, for each old line by index, the
// index of the new line that holds its first cell. A placement on a line outside `map` keeps its distance from the
// nearest line in `map`.
void vtermGraphicsRewrap(Vterm* vterm, uint64_t firstLine, const RawArray<uint32_t>& map, uint64_t newFirstLine);

// Moves the alternate screen's placements up by `rows`, the number of rows that a resize cut from the screen's top.
void vtermGraphicsShiftAlternate(Vterm* vterm, uint32_t rows);

// Clips each screen's placements to the screen's current lines, and removes the placements with no rows left. The
// caller calls this after the scrollback drops its oldest lines or a resize cuts lines.
void vtermGraphicsPrune(Vterm* vterm);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
