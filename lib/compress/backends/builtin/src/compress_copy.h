// The copies of the LZ77 decoders: a match that repeats earlier output, a word at a time.

#pragma once

#include "pr/base.h"
#include "pr/mem.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The number of bytes past the end of a match that compressCopyMatch may write.
constexpr uint32_t kCompressMatchSlack = 8;

// Copies a match of `length` bytes from `distance` back, to `out`. The copy runs forward, so a match longer than its
// distance repeats a pattern. It copies eight bytes at a time and may write up to kCompressMatchSlack bytes past the
// match. A match nearer than eight bytes is a pattern with period `distance`. After its first eight bytes, it copies
// from the nearest multiple of the period that is at least eight bytes back.
inline void compressCopyMatch(uint8_t* out, uint32_t distance, uint32_t length) {
    uint32_t at = 0;
    const uint8_t* from = out - distance;
    if (distance >= 8) {
        for (; at < length; at += 8)
            prMemCopy(out + at, from + at, 8);
        return;
    }
    for (; at < 8; ++at)
        out[at] = from[at];
    // The eight bytes read may overlap the eight written, so they pass through a register.
    const uint32_t step = distance * (8 / distance);
    for (; at < length; at += step) {
        uint64_t word = 0;
        prMemCopy(&word, out + (at - step), 8);
        prMemCopy(out + at, &word, 8);
    }
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
