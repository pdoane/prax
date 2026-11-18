// iTerm2's inline images. A program sends an image file whole, as OSC 1337 ; File = arguments : base64 ST, or in parts:
// OSC 1337 ; MultipartFile = arguments ST, then OSC 1337 ; FilePart = base64 ST for each part, then OSC 1337 ; FileEnd
// ST. The image shows at the cursor as part of the cells that it covers (vtermGraphicsShow).

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct Vterm;

// Runs an OSC 1337 command, given its text after `1337;`. Shows the images of File and of a file in parts, and ignores
// the other commands.
//
// The arguments are `key=value` pairs separated by `;`. The terminal reads these:
// - inline: 1 shows the image. Any other value is a download, which the terminal ignores.
// - width and height: a number of cells, a number of pixels (`px`), a percentage of the screen (`%`), or `auto`, the
//   image's own size. Pixels are the cell's pixels (prVtermCellPixelExtent), as kitty's protocol counts them.
// - preserveAspectRatio: 0 stretches the image to the width and height. Otherwise, the default, the image keeps its
//   aspect ratio: a missing size follows from the other, and both fit the image within them.
// - doNotMoveCursor: 1 leaves the cursor where it is. Otherwise the cursor moves past the image as a kitty placement
//   moves it.
// - size: the file's size in bytes. A larger size than the budget refuses the file.
//
// An image wider than the room from the cursor to the screen's right edge shrinks to fit it.
void vtermItermCommand(Vterm* vterm, PrStringView text);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
