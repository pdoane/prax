// Integers: formatting and parsing their digits, and the bit scans and wide products of 64-bit words.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// MSVC's bit scan, and its 128-bit product, which x64 alone has.
#if defined(_MSC_VER) && !defined(__clang__)
unsigned char _BitScanForward64(unsigned long* index, unsigned __int64 mask);
#pragma intrinsic(_BitScanForward64)
#endif
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
unsigned __int64 _umul128(unsigned __int64 a, unsigned __int64 b, unsigned __int64* high);
#pragma intrinsic(_umul128)
#endif

// The bounds of the digits of a uint64_t.
enum {
    kPrU64TextMaxBytes = 64,   // the most digits that a uint64_t needs in any radix, without a terminator
    kPrU64MaxRadix = 36,       // the largest radix, whose digits are 0 through 9 and a through z
};

// Writes the digits of `value` in `radix` to `out`, and returns the number of digits. `radix` must be 2 through
// kPrU64MaxRadix, and digits above nine are lowercase letters. `out` must hold at least kPrU64TextMaxBytes bytes. The
// output is not terminated.
size_t prU64Format(uint64_t value, char* out, uint32_t radix);

// Parses the `size` bytes at `data` as digits in `radix`, and writes the value to `*out`. `radix` must be 2 through
// kPrU64MaxRadix, and digits above nine are letters of either case. Returns false if there are no digits or a byte is
// not a digit. A value above UINT64_MAX saturates to UINT64_MAX.
bool prU64Parse(const char* data, size_t size, uint64_t* out, uint32_t radix);

// Parses the digits at the start of the `size` bytes at `data` by the rules of prU64Parse, and returns the number of
// digits. Returns zero, and leaves `*out` unchanged, if `data` does not start with a digit.
size_t prU64ParsePrefix(const char* data, size_t size, uint64_t* out, uint32_t radix);

// Returns the index of the lowest set bit of `word`, which is the number of zeros below it. `word` must not be zero.
static inline uint32_t prU64LowestBit(uint64_t word) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index = 0;
    _BitScanForward64(&index, word);
    return PR_VALUE_CAST(uint32_t, index);
#else
    return PR_VALUE_CAST(uint32_t, __builtin_ctzll(word));
#endif
}

// Returns the low half of the 128-bit product of `a` and `b`, and writes the high half to `*high`.
static inline uint64_t prU64Mul128(uint64_t a, uint64_t b, uint64_t* high) {
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
    return _umul128(a, b, high);
#elif defined(__SIZEOF_INT128__)
    unsigned __int128 product = PR_VALUE_CAST(unsigned __int128, a) * b;
    *high = PR_VALUE_CAST(uint64_t, product >> 64);
    return PR_VALUE_CAST(uint64_t, product);
#else
    // The products of the 32-bit halves, summed by columns. The middle column's carries reach the high half.
    uint64_t aLow = a & 0xffffffffu;
    uint64_t aHigh = a >> 32;
    uint64_t bLow = b & 0xffffffffu;
    uint64_t bHigh = b >> 32;
    uint64_t low = aLow * bLow;
    uint64_t middle = aHigh * bLow + (low >> 32);
    uint64_t middle2 = aLow * bHigh + (middle & 0xffffffffu);
    *high = aHigh * bHigh + (middle >> 32) + (middle2 >> 32);
    return (middle2 << 32) | (low & 0xffffffffu);
#endif
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
