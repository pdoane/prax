// Construction: making objects in storage that their owner allocated and destructing them, which the managed containers
// do for their elements, and making objects on the heap and destroying them.

#pragma once

#include "pr/base.h"
#include "pr/memory.h"
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

// Runs the destructor of the `T` at `value`, and leaves its storage to its owner.
template <typename T>
void destruct(T* PR_NONNULL value) {
    value->~T();
}

// Allocates a `T` and makes it from `arguments`, and returns it, which destroy destructs and frees.
template <typename T, typename... Arguments>
T* PR_NONNULL create(Arguments&&... arguments) {
    return construct<T>(prMemoryAlloc(sizeof(T)), forward<Arguments>(arguments)...);
}

// Destructs the `T` at `value`, which create made, and frees its storage. Does nothing for null.
template <typename T>
void destroy(T* PR_NULLABLE value) {
    if (value == nullptr)
        return;
    value->~T();
    prMemoryFree(value);
}

} // namespace pr

PR_ASSUME_NONNULL_END
