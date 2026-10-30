// Image comparison for render tests, using the difference measures of the SVG corpora and of WPT's reftests.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// The tolerances of compareImages.
struct ImageComparison {
    float blurRadius = 0.6f;    // the Gaussian's standard deviation, in pixels
    uint32_t channelDelta = 24; // the largest change that is not a difference
};

// The difference between two images, as measured by compareImages.
struct ImageDifference {
    double changedRatio;        // the fraction of all pixels that differ
    double contentChangedRatio; // the fraction of the pixels that either image draws on and that differ
    double mae;                 // the blurred images' mean absolute difference, over RGB
};

// Compares two images with the measure that the SVG corpora use (scripts/update.py in the svg-compliance corpus). Both
// images are composited on white and blurred with the steps of Pillow's GaussianBlur. A pixel differs if a channel
// changes by more than `comparison.channelDelta`. Every step uses Pillow's arithmetic, and the figures match the
// corpora's own figures to the last bit.
//
// The images are straight RGBA, as stored in a PNG, `width` by `height` with rows `stride` bytes apart. If `diff` is
// not null, it receives the difference in the same layout, opaque: the reference faded toward white, and red where the
// images differ.
ImageDifference compareImages(const uint8_t* reference, const uint8_t* rendered, uint32_t width,
    uint32_t height, size_t stride, const ImageComparison& comparison, uint8_t* PR_NULLABLE diff);

// The difference between two images, as wptrunner measures it for WPT's reftests (web-platform-tests docs, "Fuzzy
// Matching"). A pixel differs if the luma of its channels' differences is nonzero, with luma as Pillow converts RGB to
// L, in 16-bit fixed point and rounded. So a pixel that differs by one level in red or blue alone does not differ.
struct PixelDifference {
    uint32_t maxDifference;     // the largest difference in any channel of the pixels that differ
    uint64_t differingPixels;   // number of pixels that differ
};

// Measures how two images differ, each composited on white as a screenshot shows it. The images use the same layout as
// for compareImages.
PixelDifference countDifferences(const uint8_t* reference, const uint8_t* rendered, uint32_t width, uint32_t height,
    size_t stride);

// Returns true if any pixel of the image differs from white once composited on it, by more than the rounding of a
// render on white. The image is laid out as for compareImages. A reftest whose reference draws nothing tests nothing.
bool imageDrawsAnything(const uint8_t* rgba, uint32_t width, uint32_t height, size_t stride);

// Converts premultiplied BGRA to straight RGBA in place, with the rounding that the engine uses when it writes a PNG.
void straightenBgra(uint8_t* pixels, uint32_t width, uint32_t height, size_t stride);

} // namespace harness

PR_ASSUME_NONNULL_END
