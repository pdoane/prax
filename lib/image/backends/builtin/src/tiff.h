// TIFF decoding for the built-in codec: the first image of a file, uncompressed, with 8-bit samples stored chunky.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if `bytes` begin with a TIFF header in either byte order.
bool tiffHasSignature(PrByteView bytes);

// The first image of a file, as its tags describe it. Its strips refer to `bytes`.
struct TiffImage {
    PrByteView bytes;
    uint32_t width;
    uint32_t height;
    uint32_t samples;               // per pixel: the color channels, then alpha if there is one
    uint32_t rowsPerStrip;
    uint32_t stripCount;
    uint32_t stripOffsets;          // the file offset of the strip offsets, or the one offset itself
    uint32_t stripCounts;           // the file offset of the strip byte counts, or the one count itself
    uint16_t offsetType;            // the field type of `stripOffsets`, SHORT or LONG
    uint16_t countType;             // the field type of `stripCounts`, SHORT or LONG
    PrImageAlpha alpha;
    bool gray;
    bool whiteIsZero;
    bool bigEndian;
};

// Reads the tags of the first image in `bytes`. Returns NotSupported for a compression, sample size or layout that the
// codec does not read, and Syntax for a file whose tags are malformed.
PrStatus tiffImage(PrByteView bytes, TiffImage* out);

// Decodes `image` into `pixels` as tightly packed rows of its samples. Returns false if a strip lies outside the file
// or `pixelsSize` is not the size of the image.
bool tiffDecode(const TiffImage& image, uint8_t* pixels, size_t pixelsSize);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
