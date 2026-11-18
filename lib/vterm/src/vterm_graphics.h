// The images of a terminal. This module runs the commands of kitty's terminal graphics protocol, which transmit, place,
// query and delete images, and shows the images of the protocols whose images are part of the cells that they cover,
// iTerm2's and sixel (vterm_iterm.h, vterm_sixel.h). It keeps the images and placements of each screen, and moves the
// placements with the text as the screen changes.

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

    // The placement is part of the cells that it covers, as iTerm2's and sixel images are (vtermGraphicsShow).
    bool inCells;
};

// The images and placements of one screen. `mayHaveCellImages` is false only if no placement is `inCells`, so that
// writing a cell checks the placements only when it may need to.
struct VtermGraphicsStore {
    RawArray<VtermStoredImage> images;
    RawArray<VtermStoredPlacement> placements;
    bool mayHaveCellImages;
};

// A transmission that arrives in chunks. It keeps the text of the first chunk's keys or arguments and the bytes decoded
// so far. The base64 characters past a chunk's last whole group of four wait in `carry` for the next chunk. A failure
// is answered or dropped when the last chunk arrives.
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
    VtermGraphicsLoad load;         // kitty's chunks
    VtermGraphicsLoad itermLoad;    // iTerm2's file parts
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

// Appends the bytes of a chunk's base64 `payload` to the load. Characters past the last whole group of four wait for
// the next chunk. Returns false if the payload is not base64 or the load passes `limit` bytes.
bool vtermGraphicsLoadAppend(VtermGraphicsLoad* load, PrStringView payload, uint64_t limit);

// Ends the load by decoding the characters that still wait. Returns false if they are not base64.
bool vtermGraphicsLoadFinish(VtermGraphicsLoad* load);

// Ends the load and drops what it holds.
void vtermGraphicsLoadDrop(VtermGraphicsLoad* load);

// Decodes the image file in `bytes`, in any format that the caller's image codec reads, into `*pixels`, Rgba8 with
// straight alpha, and its size into `*extent`. With `oriented`, the pixels are as the image's orientation displays
// them. Returns false, and leaves `*pixels` empty, if there is no codec, or the image does not decode, or it is larger
// than the terminal's limits or its budget.
bool vtermGraphicsDecode(Vterm* vterm, PrByteView bytes, bool oriented, PrUVec2* extent, RawArray<uint8_t>* pixels);

// Runs a graphics command, given the text of its APC string after the `G`.
void vtermGraphicsCommand(Vterm* vterm, PrStringView text);

// Where the cursor goes after vtermGraphicsShow places an image.
enum {
    VtermShowCursor_Stay,
    VtermShowCursor_After,  // past the image's last column on its last row, as a kitty placement moves it
    VtermShowCursor_Below,  // to the image's first column on the row below the image, scrolling as a line feed does
};
typedef uint8_t VtermShowCursor;

// An image to show in cells.
struct VtermCellImage {
    PrUVec2 extent;
    RawArray<uint8_t> pixels;   // Rgba8 with straight alpha, by row
    uint32_t column;            // the screen's cell at the image's top left
    uint32_t row;
    PrVec2 size;                // in cells; the image spans the whole cells that hold it
};

// Shows `image`, whose pixels it takes, as part of the cells that it covers, on the active screen. Nothing can name the
// image. Writing or erasing any cell that it covers removes it whole (vtermGraphicsCellsWritten), and so does a later
// image that covers all of its cells. Then moves the cursor as `cursor` says. Returns false, and shows nothing, if the
// image is larger than the budget.
bool vtermGraphicsShow(Vterm* vterm, VtermCellImage* image, VtermShowCursor cursor);

// Removes the active screen's images that are part of the cells from `start` to `end`, excluded, of `row`, as writing
// or erasing those cells does.
void vtermGraphicsCellsWritten(Vterm* vterm, uint32_t row, uint32_t start, uint32_t end);

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
