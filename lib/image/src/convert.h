// Converting pixels between formats.

#pragma once

#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the number of bytes that one pixel of `format` occupies, or zero for a format that is not valid.
uint32_t imagePixelBytes(const PrImageFormat& format);

// Converts `source` in format `from` into `destination` in format `to`. The two are the same size and do not overlap.
// `policy` holds the assumptions that the conversion may make about uncharacterized color.
PrStatus imageConvert(PrPixelView source, const PrImageFormat& from, PrPixels destination,
    const PrImageFormat& to, PrImageConversionFlags policy);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
