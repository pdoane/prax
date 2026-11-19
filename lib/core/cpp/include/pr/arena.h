// Arenas: storage whose allocations never move and are freed together.

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The size in bytes of an arena's first page, and of the largest page that its growth makes, unless its owner sets its
// own.
constexpr size_t kArenaFirstPage = 4096;
constexpr size_t kArenaLargestPage = 64 * 1024;

// A block of memory that an arena allocates from.
struct ArenaPage {
    uint8_t* data;
    size_t size;
};

// Storage that hands out allocations from pages, in order. An allocation stays in place until the arena is cleared or
// reset, and nothing frees it on its own. The arena runs no constructor or destructor, and a zeroed arena is empty.
//
// Each page that growth makes is twice the size of the one before it, from `firstPage` up to `largestPage`. An
// allocation that does not fit in the page that growth would make next gets a block of its own. Clearing keeps the
// pages for the allocations that follow, and frees the blocks of the large allocations, which pages of a repeated
// workload would not reuse well. The owner frees everything with `reset`.
//
// An allocation from a page costs an alignment, an addition and a comparison, of the arena's own fields. A new page or
// a large allocation costs a call to the platform's allocator.
struct Arena {
    // The free bytes of the page being filled, from `cursor` up to `bound` less one. An allocation fits if it ends
    // before `bound`, so that a zeroed arena, whose bound is zero, takes the slow path for its first allocation of any
    // size.
    uintptr_t cursor;
    uintptr_t bound;

    // The sizes of the first page that growth makes and of the largest, which the owner may set before the first
    // allocation. Zero takes kArenaFirstPage or kArenaLargestPage.
    size_t firstPage;
    size_t largestPage;

    // The pages in the order that growth made them, which is also the order of their sizes. The pages before `page` are
    // full until the arena is cleared.
    RawArray<ArenaPage> pages;

    RawArray<ArenaPage> large;      // the blocks of the allocations larger than a page
    uint32_t page;                  // the page being filled, or `pages.count` if there is none

    // Return `size` bytes, uninitialized or zeroed, at a multiple of `alignment`, which is a power of two. The bytes
    // stay in place until the arena is cleared or reset. A size of zero returns a pointer that must not be read.
    PR_FORCE_INLINE void* PR_NONNULL allocBytes(size_t size, size_t alignment) {
        PR_ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0);
        uintptr_t at = (cursor + (alignment - 1)) & ~uintptr_t(alignment - 1);
        if (at + size < bound) {
            cursor = at + size;
            return reinterpret_cast<void*>(at);
        }
        return allocSlow(size, alignment);
    }
    void* PR_NONNULL allocBytesZeroed(size_t size, size_t alignment) {
        void* bytes = allocBytes(size, alignment);
        prMemZero(bytes, size);
        return bytes;
    }

    // Return storage for `count` elements of type T, uninitialized or zeroed. No constructor runs.
    template <typename T>
    T* PR_NONNULL alloc(uint32_t count = 1) {
        return static_cast<T*>(allocBytes(sizeof(T) * size_t(count), alignof(T)));
    }
    template <typename T>
    T* PR_NONNULL allocZeroed(uint32_t count = 1) {
        return static_cast<T*>(allocBytesZeroed(sizeof(T) * size_t(count), alignof(T)));
    }

    // Returns a copy of the `count` elements at `items`, which may be null if `count` is zero. The elements are copied
    // as bytes.
    template <typename T>
    T* PR_NONNULL copy(const T* PR_NULLABLE items, uint32_t count) {
        static_assert(isTriviallyCopyable<T>, "an arena copies its elements as bytes");
        T* copied = alloc<T>(count);
        if (count != 0)
            prMemCopy(copied, items, sizeof(T) * size_t(count));
        return copied;
    }

    // The bytes that the allocations since the last clear occupy. The count includes alignment, and the unused ends of
    // the pages that the arena filled. Its cost grows with the number of pages.
    size_t used() const;

    // The bytes of every page and large block that the arena holds. Its cost grows with the number of pages.
    size_t capacity() const;

    // Empties the arena. It keeps the pages for the allocations that follow, and frees the blocks of large allocations.
    void clear();

    // Frees the storage and leaves an empty arena behind.
    void reset();

    // Returns the start of `size` bytes, aligned to `alignment`, in the next page that holds them or in a block of
    // their own, and moves the cursor past them if they are in a page.
    PR_NOINLINE void* PR_NONNULL allocSlow(size_t size, size_t alignment);
};

} // namespace pr

PR_ASSUME_NONNULL_END
