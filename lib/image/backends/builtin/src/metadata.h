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
};

// Returns `perInch` converted to pixels per meter.
float imagePixelsPerMeterFromInch(float perInch);

// Returns `perCm` converted to pixels per meter.
float imagePixelsPerMeterFromCm(float perCm);

// Reads the orientation and a usable resolution from the TIFF structure of `size` bytes at `tiff`. A field that the
// data does not give leaves its member of `*out` unchanged.
void imageReadExif(const uint8_t* tiff, size_t size, ImageMetadata* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
