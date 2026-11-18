// Sixel images, which a program sends as DCS P1 ; P2 ; P3 q data ST. The data draws columns of six pixels, a band at a
// time, in colors that it selects from registers and may define. The image shows as part of the cells that it covers
// (vtermGraphicsShow).

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct Vterm;
struct VtermAction;

// The number of color registers of an image. Each image starts with the VT340's colors in the first 16 registers and
// black in the rest, and changes only its own registers.
constexpr uint32_t kVtermSixelColors = 256;

// Shows the sixel image of a DCS string whose final byte is `q`.
//
// P2 of 1 leaves the pixels that no sixel sets transparent. Otherwise they are the terminal's background color, within
// the size that the raster attributes give or else the extent of the sixels. P1, the pixel aspect ratio, and P3, the
// grid size, are ignored: pixels are square, at the cell's pixels (prVtermCellPixelExtent), as kitty's protocol counts
// them. The raster attributes (") give the size, and their aspect ratio is ignored as P1 is. Colors are defined in RGB
// or HLS (#), each by percentages.
//
// The image's top left is the cursor, and the cursor then moves to the image's first column on the row below the image,
// as a line feed does, scrolling the screen. With DECSET 8452, the cursor instead moves past the image's last column on
// its last row. With sixel display mode (DECSET 80), the image's top left is the screen's, nothing scrolls, and the
// cursor stays.
void vtermSixelCommand(Vterm* vterm, const VtermAction* action);

// Answers a request for a graphics attribute (XTSMGRAPHICS, CSI ? Pi ; Pa ; Pv S). The number of color registers (Pi
// 1) is kVtermSixelColors, which a request to set or reset it leaves. The largest sixel image (Pi 2) is the screen's
// size in the cell's pixels, which a request to set it also leaves. Other attributes are errors.
void vtermSixelGraphicsAttributes(Vterm* vterm, const VtermAction* action);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
