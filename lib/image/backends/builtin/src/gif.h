// GIF decoding for the built-in codec.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if `bytes` begin with the GIF signature and a version that the codec reads.
bool gifHasSignature(PrByteView bytes);

// A GIF being decoded. It keeps the composed image from one frame to the next.
struct GifAnimation;

// Opens the GIF in `bytes`, which must stay valid until gifAnimationClose. Returns null for invalid data or for
// dimensions past the limit of the codec.
GifAnimation* PR_NULLABLE gifAnimationOpen(PrByteView bytes);

// Frees the animation and everything it holds.
void gifAnimationClose(GifAnimation* PR_NULLABLE animation);

// Returns the delay of `frame` in milliseconds, or zero for a frame that the GIF does not have.
uint32_t gifAnimationDelay(const GifAnimation& animation, uint32_t frame);

// Decodes `frame`, composed over the frames before it, into `pixels` as top-down, tightly packed, straight-alpha sRGB
// Rgba8. Decoding the frames in order continues the composition, and any other order restarts it. Returns false for
// invalid data, a frame the GIF does not have, or a `pixelsSize` that is not the size of the image.
bool gifAnimationFrame(GifAnimation& animation, uint32_t frame, uint8_t* pixels, size_t pixelsSize);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
