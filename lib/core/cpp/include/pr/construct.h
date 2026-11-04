// Construction in place: making and destroying objects in storage that their owner allocated, which the managed
// containers do for their elements.

#pragma once

#include "pr/base.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

// The tag of core's own placement form of new, which cannot collide with <new>'s placement new in a program that
// includes it, and the form itself, which makes an object in storage that is already allocated.
struct PrPlacementTag {};
inline void* operator new(size_t, PrPlacementTag, void* at) noexcept {
    return at;
}
inline void operator delete(void*, PrPlacementTag, void*) noexcept {}

namespace pr {

// Makes a `T` from `arguments` in the storage at `at`, which is aligned and large enough for one, and returns it.
template <typename T, typename... Arguments>
T* PR_NONNULL construct(void* at, Arguments&&... arguments) {
    return new (PrPlacementTag{}, at) T(forward<Arguments>(arguments)...);
}

// Destroys the `T` at `value`, and leaves its storage to its owner.
template <typename T>
void destroy(T* PR_NONNULL value) {
    value->~T();
}

} // namespace pr

PR_ASSUME_NONNULL_END
