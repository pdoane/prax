// Bit casts: the bits of a value read as another type.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the bits of `value` read as a `To`, with no aliasing or alignment requirement. The call is a constant
// expression if neither type contains a pointer.
template <typename To, typename From>
constexpr To bitCast(const From& value) {
    static_assert(sizeof(To) == sizeof(From));
    static_assert(__is_trivially_copyable(To) && __is_trivially_copyable(From));
    return __builtin_bit_cast(To, value);
}

// Reads a `T` from the `sizeof(T)` bytes at `data`, which need not be aligned. The bytes may be text or binary data.
// The load is valid in constant evaluation, and is an unaligned load at runtime.
template <typename T, typename Byte>
constexpr T loadUnaligned(const Byte* PR_NONNULL data) {
    static_assert(sizeof(Byte) == 1);
    Byte bytes[sizeof(T)] = {};
    for (size_t i = 0; i < sizeof(T); ++i)
        bytes[i] = data[i];
    return bitCast<T>(bytes);
}

} // namespace pr

PR_ASSUME_NONNULL_END
