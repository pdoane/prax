// Arenas: storage whose allocations never move and are freed together.

#include "pr/arena.h"

namespace pr {

namespace {

// Frees the memory of each of `blocks`, and leaves the array to its caller.
void freeBlocks(const RawArray<ArenaPage>& blocks) {
    for (uint32_t i = 0; i < blocks.count; ++i)
        prMemoryFree(blocks.data[i].data);
}

// Returns `address` rounded up to a multiple of `alignment`, a power of two.
uintptr_t alignAddress(uintptr_t address, size_t alignment) {
    uintptr_t mask = uintptr_t(alignment) - 1;
    return (address + mask) & ~mask;
}

// Makes the page at `index` the one that the arena fills, from its start.
void fill(Arena& arena, uint32_t index) {
    arena.page = index;
    const ArenaPage& filled = arena.pages.data[index];
    arena.cursor = uintptr_t(filled.data);
    arena.bound = uintptr_t(filled.data) + filled.size + 1;
}

} // namespace

size_t Arena::used() const {
    size_t total = page < pages.count ? size_t(cursor - uintptr_t(pages.data[page].data)) : 0;
    for (uint32_t i = 0; i < page && i < pages.count; ++i)
        total += pages.data[i].size;
    for (uint32_t i = 0; i < large.count; ++i)
        total += large.data[i].size;
    return total;
}

size_t Arena::capacity() const {
    size_t total = 0;
    for (uint32_t i = 0; i < pages.count; ++i)
        total += pages.data[i].size;
    for (uint32_t i = 0; i < large.count; ++i)
        total += large.data[i].size;
    return total;
}

void Arena::clear() {
    freeBlocks(large);
    large.clear();
    if (pages.count != 0) {
        fill(*this, 0);
    } else {
        page = 0;
        cursor = 0;
        bound = 0;
    }
}

void Arena::reset() {
    freeBlocks(pages);
    freeBlocks(large);
    pages.reset();
    large.reset();
    page = 0;
    cursor = 0;
    bound = 0;
}

void* PR_NONNULL Arena::allocSlow(size_t size, size_t alignment) {
    // The bytes that hold the allocation in an empty page at any alignment of the page's start.
    size_t needed = size + (alignment - 1);
    PR_ASSERT(needed >= size);
    const size_t first = firstPage != 0 ? firstPage : kArenaFirstPage;
    const size_t largest = largestPage != 0 ? largestPage : kArenaLargestPage;
    PR_ASSERT(first <= largest);
    size_t grown = first;
    if (pages.count != 0) {
        size_t last = pages.last().size;
        grown = last >= largest / 2 ? largest : last * 2;
    }
    if (needed > grown) {
        uint8_t* data = static_cast<uint8_t*>(prMemoryAlloc(needed));
        large.push(ArenaPage{.data = data, .size = needed});
        return reinterpret_cast<void*>(alignAddress(uintptr_t(data), alignment));
    }

    // A kept page that is too small for the allocation stays empty until the arena is cleared. An arena that fills no
    // page yet starts at its first.
    uint32_t next = page < pages.count && cursor != 0 ? page + 1 : page;
    while (next < pages.count && pages.data[next].size < needed)
        ++next;
    if (next == pages.count)
        pages.push(ArenaPage{.data = static_cast<uint8_t*>(prMemoryAlloc(grown)), .size = grown});
    fill(*this, next);
    uintptr_t at = alignAddress(cursor, alignment);
    cursor = at + size;
    return reinterpret_cast<void*>(at);
}

} // namespace pr
