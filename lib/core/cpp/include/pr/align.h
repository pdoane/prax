// Alignment: rounding sizes and offsets up to a multiple.

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

} // namespace pr

PR_ASSUME_NONNULL_END
