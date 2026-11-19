// WebP's lossy bitstream: the key frames of VP8 (rfc 6386).

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

#include "webp/src/vp8l.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The size of a key frame, from its frame tag and its start code (rfc 6386 9.1).
struct Vp8Header {
    uint32_t width;
    uint32_t height;
};

// Reads the header of the key frame that begins `bytes`. Returns false for a frame that is not a key frame of a known
// profile that shows, or whose first partition does not fit.
bool vp8ReadHeader(PrByteView bytes, Vp8Header* out);

// A decoded frame: its luma and its two chroma planes at half its size in each axis. Each plane covers whole
// macroblocks, of which the frame's own `width` by `height` pixels are the top-left.
struct Vp8Planes {
    uint8_t* PR_NULLABLE y;
    uint8_t* PR_NULLABLE u;
    uint8_t* PR_NULLABLE v;
    uint32_t yStride;
    uint32_t uvStride;
    uint32_t width;
    uint32_t height;
};

// Returns the bytes that vp8Decode holds for a frame of `width` by `height`, its planes included.
uint64_t vp8WorkingBytes(uint32_t width, uint32_t height);

// Decodes the key frame in `bytes` into planes that it allocates and `budget` counts. Returns InvalidData for a frame
// that breaks the format or ends early, and Limit if the frame does not fit the budget. vp8PlanesFree frees the planes,
// after a failure too.
PrStatus vp8Decode(PrByteView bytes, WebpBudget& budget, Vp8Planes* out);

// Frees the planes that vp8Decode allocated, and returns their bytes to `budget`.
void vp8PlanesFree(Vp8Planes& planes, WebpBudget& budget);

// Converts the frame to RGB8 if `channels` is 3, or to Rgba8 with opaque alpha if it is 4, `stride` bytes a row. The
// chroma is upsampled as the reference decoder upsamples it by default: each output pixel takes its chroma from the
// four nearest samples, weighted 9, 3, 3 and 1 by nearness, and the samples past an edge repeat the edge's.
void vp8ToRgb(const Vp8Planes& planes, uint8_t* out, size_t stride, uint32_t channels);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
