// Bit sets: arrays of 64-bit words, read and written by bit.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the number of words that an array of `bits` bits needs.
constexpr size_t bitsetWords(size_t bits) {
    return (bits + 63) / 64;
}

// Sets, clears or tests bit `bit` of `words`, which holds the word that the bit is in.
constexpr void bitsetSet(uint64_t* words, size_t bit) {
    words[bit >> 6] |= UINT64_C(1) << (bit & 63);
}
constexpr void bitsetClear(uint64_t* words, size_t bit) {
    words[bit >> 6] &= ~(UINT64_C(1) << (bit & 63));
}
constexpr bool bitsetTest(const uint64_t* words, size_t bit) {
    return (words[bit >> 6] & (UINT64_C(1) << (bit & 63))) != 0;
}

} // namespace pr

PR_ASSUME_NONNULL_END
