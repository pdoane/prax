// The lesser, the greater, the clamp and the magnitude of values of one type.
//
// <windows.h> defines min and max as macros unless NOMINMAX is defined first, so a source that includes it defines
// NOMINMAX. A source that includes <windows.h> without it, before this header, is stopped here.

#pragma once

#include "pr/base.h"

#if defined(min) || defined(max)
#error "min or max is a macro: define NOMINMAX before including <windows.h>"
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Return the lesser or the greater of `a` and `b`. If they compare equal, or either is NaN, the result is `a`.
template <typename T>
constexpr T min(T a, T b) {
    return b < a ? b : a;
}
template <typename T>
constexpr T max(T a, T b) {
    return b > a ? b : a;
}

// Returns `value` clamped to [`low`, `high`]. NaN stays NaN.
template <typename T>
constexpr T clamp(T value, T low, T high) {
    return value < low ? low : value > high ? high : value;
}

// Returns the magnitude of `value`, and how far apart `a` and `b` are, which an unsigned type gives without wrapping.
template <typename T>
constexpr T abs(T value) {
    return value < T(0) ? T(-value) : value;
}
template <typename T>
constexpr T absDifference(T a, T b) {
    return a < b ? T(b - a) : T(a - b);
}

} // namespace pr

PR_ASSUME_NONNULL_END
