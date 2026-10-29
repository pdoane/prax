// Integers: rounding them, and formatting and parsing their digits.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns `value` rounded up to a multiple of `multiple`, which is not zero and converts to the type of `value`.
template <typename T, typename M>
constexpr T alignUp(T value, M multiple) {
    T step = T(multiple);
    return (value + step - 1) / step * step;
}

// The maximum number of digits that a uint64_t needs in any supported radix, without a terminator.
constexpr size_t kU64TextMaxBytes = 64;

// The largest radix, whose digits are 0 through 9 and a through z.
constexpr uint32_t kIntegerMaxRadix = 36;

// Writes the digits of `value` in `radix` to `out`, and returns the number of digits. `radix` must be 2 through
// kIntegerMaxRadix, and digits above nine are lowercase letters. `out` must hold at least kU64TextMaxBytes bytes. The
// output is not terminated.
size_t formatU64(uint64_t value, char* out, uint32_t radix = 10);

// Parses the `size` bytes at `data` as digits in `radix`, and writes the value to `*out`. `radix` must be 2 through
// kIntegerMaxRadix, and digits above nine are letters of either case. Returns false if there are no digits or a byte is
// not a digit. A value above UINT64_MAX saturates to UINT64_MAX.
bool parseU64(const char* data, size_t size, uint64_t* out, uint32_t radix = 10);

// Parses the digits at the start of the `size` bytes at `data` by the rules of parseU64, and returns the number of
// digits. Returns zero, and leaves `*out` unchanged, if `data` does not start with a digit.
size_t parseU64Prefix(const char* data, size_t size, uint64_t* out, uint32_t radix = 10);

} // namespace pr

PR_ASSUME_NONNULL_END
