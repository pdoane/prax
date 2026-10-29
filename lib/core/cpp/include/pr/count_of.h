// Element counts of fixed-size arrays.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The number of elements of a fixed-size array.
template <typename T, size_t N>
constexpr size_t countOf(const T (&)[N]) {
    return N;
}

} // namespace pr

PR_ASSUME_NONNULL_END
