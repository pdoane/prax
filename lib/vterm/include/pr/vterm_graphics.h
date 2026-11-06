// Images in a virtual terminal (pr/vterm.h). Programs send them with kitty's terminal graphics protocol, and the caller
// draws them from their placements.
//
// One thread uses a terminal at a time.

#pragma once

#include "pr/base.h"
#include "pr/vec.h"
#include "pr/vterm.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Graphics
//
// A program shows images with kitty's terminal graphics protocol, which uses APC sequences of the form ESC _ G keys ;
// payload ESC \. A terminal whose image budget is zero ignores them. The terminal supports these parts of the protocol:
//
// - The actions transmit (a=t), transmit and put (a=T), put (a=p), query (a=q, including the feature query of `e`),
//   and delete (a=d) with every selector of `d`. An upper-case selector also frees the images that no placement shows.
//   Animation and relative placements are refused with EINVAL.
// - Pixels in RGB (f=24), RGBA (f=32) or PNG (f=100), each optionally compressed with zlib (o=z). Pixels are sent in
//   the escape codes (t=d), in chunks (m=1). With PrVtermImageFlag_Files, they can also be read from a file (t=f), or
//   from a temporary file that the terminal then deletes (t=t). A temporary file must be under a temporary directory
//   and have tty-graphics-protocol in its path. Shared memory (t=s) is refused. Every failure to read a file gets the
//   same EBADF error, so that a program learns nothing about files that it cannot read itself.
// - Image ids (i), image numbers (I) and placement ids (p). The terminal answers an image number with the id that it
//   gave the image. A command that carries an id or a number is answered with OK or an error, as its `q` key allows.
// - Placements at the cursor. A placement takes a source rectangle (x, y, w, h), an offset within the cell (X, Y), and
//   a size in columns and rows (c, r). Without a size, the image shows at its own pixels, by the cell's pixel extent.
//   A placement has a z-index (z), and the cursor moves past it unless C=1.
// - Virtual placements (U=1), which show through placeholder characters in the text. A placeholder is U+10EEEE, with
//   the image id in its foreground color and the placement id in its underline color. Its diacritics give its row, its
//   column and the most significant byte of the image id.
//
// Each screen has its own images and placements. A placement is anchored to a line of text. It scrolls with that line
// into the scrollback, and is removed when the line is dropped. A scroll region moves only the placements that lie
// wholly within its rows and columns, and cuts the parts that scroll past its edges. Showing the alternate screen with
// a clear removes its images. Erasing the whole screen (ED 2) removes the placements that show on it, and ED 3 removes
// those in the scrollback. No other erase removes placements. A resize keeps each placement at its size and in its
// column, and moves it to the line that holds the first cell of its old line after the rewrap. A full reset removes
// every image. The pixels of all images count toward the terminal's budget. Past the budget, the terminal frees images
// least recently used first, starting with those that no placement shows.

// The size of a cell in pixels. An image placed without a size in cells shows at one image pixel per screen pixel by
// this extent. XTWINOPS reports it to programs, along with the screen's size in pixels. A terminal starts at 8 by 16
// until the caller sets the size of the cells that it draws. Both dimensions must be at least one.
PrUVec2 prVtermCellPixelExtent(const PrVterm* vterm);
void prVtermSetCellPixelExtent(PrVterm* vterm, PrUVec2 extent);

// The pixels of an image, in sRGB Rgba8 with straight alpha. Rows are `extent.x` pixels long, with no padding. `pixels`
// is valid until the terminal changes.
typedef struct PrVtermImage {
    PrUVec2 extent;
    const uint8_t* pixels;
} PrVtermImage;

// Writes the image with key `key` (PrVtermPlacement's `image`) to `*out`. Returns false if no image has that key.
bool prVtermImage(const PrVterm* vterm, uint64_t key, PrVtermImage* out);

// Where a part of an image shows. The geometry is in cells, relative to the top left of the anchor cell. The anchor is
// the cell at `column` of line `line`, where `line` is an index for prVtermLine and is negative for a line above the
// first. The placement draws the image's source rectangle into its destination rectangle. It is clipped to the
// `columns` columns that start at the anchor's column, and to the rows from `firstRow` to `endRow`, excluded, counted
// from the anchor's row.
typedef struct PrVtermPlacement {
    // The image's key. A key never names other pixels, so a caller can cache what it makes of the pixels by the key.
    uint64_t image;

    // The source rectangle, in the image's pixels.
    PrUVec2 sourceOrigin;
    PrUVec2 sourceExtent;

    int64_t line;
    uint32_t column;
    PrVec2 offset;              // top left of the destination
    PrVec2 extent;              // width and height of the destination
    uint32_t columns;
    uint32_t firstRow;
    uint32_t endRow;

    // The z-index. A placement at or above zero draws over the text. Below zero, it draws under the text but over the
    // cells' backgrounds. Below kPrVtermImageUnderBackgrounds, it draws under the backgrounds too, over the screen's
    // default background.
    int32_t z;
} PrVtermPlacement;

// A placement with a z-index below this draws under the cells' backgrounds.
enum { kPrVtermImageUnderBackgrounds = -1073741824 };

// Writes the placements that show on the `count` lines from line `first` to `out`, up to `capacity`, and returns the
// total. They are in drawing order: by z-index, then by image id, then in the order they were made. A virtual placement
// shows as one placement for each run of its placeholder cells in a line.
uint32_t prVtermPlacements(const PrVterm* vterm, uint32_t first, uint32_t count, PrVtermPlacement* PR_NULLABLE out,
    uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
