// Bits: finding the set bits of a word.

#pragma once

#include "pr/base.h"

// MSVC's bit scan.
#if defined(_MSC_VER) && !defined(__clang__)
extern "C" unsigned char _BitScanForward64(unsigned long* index, unsigned __int64 mask);
#pragma intrinsic(_BitScanForward64)
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the index of the lowest set bit of `word`, which is the number of zeros below it. `word` must not be zero.
inline uint32_t lowestBitU64(uint64_t word) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index = 0;
    _BitScanForward64(&index, word);
    return uint32_t(index);
#else
    return uint32_t(__builtin_ctzll(word));
#endif
}

} // namespace pr

PR_ASSUME_NONNULL_END
