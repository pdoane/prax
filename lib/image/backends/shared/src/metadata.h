// Image metadata that the built-in codec reads for several formats.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The orientation and physical resolution of an image.
struct ImageMetadata {
    PrImageOrientation orientation;
    float pixelsPerMeterX;
    float pixelsPerMeterY;
    PrByteView thumbnail;       // JPEG thumbnail stored by the second directory, a view into the data, or empty
};

// Returns `perInch` converted to pixels per meter.
float imagePixelsPerMeterFromInch(float perInch);

// Returns `perCm` converted to pixels per meter.
float imagePixelsPerMeterFromCm(float perCm);

// Reads the orientation, a usable resolution and the embedded thumbnail from the TIFF structure of `size` bytes at
// `tiff`. If the data does not give a field, the matching member of `*out` is unchanged.
void imageReadExif(const uint8_t* tiff, size_t size, ImageMetadata* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
