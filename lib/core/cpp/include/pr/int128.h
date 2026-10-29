// 128-bit products of 64-bit integers.

#pragma once

#include "pr/base.h"

// The MSVC intrinsic for a 128-bit product.
#if defined(_MSC_VER)
extern "C" unsigned __int64 _umul128(unsigned __int64, unsigned __int64, unsigned __int64*);
#pragma intrinsic(_umul128)
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the low half of the 128-bit product of `a` and `b`, and writes the high half to `*high`.
inline uint64_t mul128(uint64_t a, uint64_t b, uint64_t* high) {
#if defined(_MSC_VER)
    return _umul128(a, b, high);
#elif defined(__SIZEOF_INT128__)
    unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    *high = uint64_t(product >> 64);
    return uint64_t(product);
#else
#error "mul128 needs the msvc intrinsic or a 128-bit integer type"
#endif
}

} // namespace pr

PR_ASSUME_NONNULL_END
