// Typed allocation over pr/memory.h.

#pragma once

#include "pr/base.h"
#include "pr/memory.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Allocates uninitialized storage for one `T`. No constructor runs.
template <typename T>
T* PR_NONNULL alloc() {
    return static_cast<T*>(prMemoryAlloc(sizeof(T)));
}

// Allocates zeroed storage for one `T`.
template <typename T>
T* PR_NONNULL allocZeroed() {
    return static_cast<T*>(prMemoryAllocZeroed(sizeof(T)));
}

} // namespace pr

PR_ASSUME_NONNULL_END
