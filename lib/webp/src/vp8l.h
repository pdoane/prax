// WebP's lossless bitstream, VP8L (rfc 9649 3).

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The bytes that one WebP decode may hold at once beyond its input and its output, and how many it holds. A decode that
// would hold more fails with Limit before it allocates them.
struct WebpBudget {
    uint64_t limit;
    uint64_t used;
    uint64_t peak;  // the most that `used` has been
};

// Counts `bytes` against `budget`. Returns false, and counts nothing, if they do not fit.
bool webpBudgetTake(WebpBudget& budget, uint64_t bytes);

// Returns `bytes` that webpBudgetTake counted.
void webpBudgetGive(WebpBudget& budget, uint64_t bytes);

// The header of a lossless bitstream (rfc 9649 3.4).
struct Vp8lHeader {
    uint32_t width;
    uint32_t height;
    bool alpha;     // the encoder's hint that some alpha is not opaque
};

// The size of the header, which ends on a byte.
constexpr size_t kVp8lHeaderBytes = 5;

// Reads the header that begins `bytes`. Returns false if they do not begin with a header of version 0.
bool vp8lReadHeader(PrByteView bytes, Vp8lHeader* out);

// Decodes the image stream in `stream`, of `width` by `height` pixels: a bitstream after its header, or the data of a
// lossless alpha chunk, which has none. Writes the pixels to `argb` as 0xAARRGGBB, top-down and packed. With a null
// `argb`, reads only what precedes the coded pixels, the transforms and the prefix codes, which checks them and counts
// what they hold. Returns InvalidData for a stream that breaks the format or ends early, and Limit if the decode would
// hold more than `budget` allows.
PrStatus vp8lDecode(PrByteView stream, uint32_t width, uint32_t height, WebpBudget& budget, uint32_t* PR_NULLABLE argb);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
