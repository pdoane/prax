// PNG files that the harnesses write, with the built-in codecs.

#pragma once

#include "pr/pixels.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Writes `rgba`, straight Rgba8, to `path` as a PNG. Returns false if the image cannot be encoded or the file written.
bool harnessWritePng(const char* path, PrPixelView rgba);

// Converts `bgra`, premultiplied Bgra8 as a platform's capture gives it, to straight Rgba8 in `out`, which holds as
// many pixels, packed.
void harnessStraightRgba(PrPixelView bgra, uint8_t* out);

} // namespace harness

PR_ASSUME_NONNULL_END
