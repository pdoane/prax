#include "pr/arena.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns an empty arena whose pages grow from 256 bytes to 1024, so that a test reaches the largest page quickly.
Arena smallArena() {
    Arena arena{};
    arena.firstPage = 256;
    arena.largestPage = 1024;
    return arena;
}

bool isAligned(const void* pointer, size_t alignment) {
    return uintptr_t(pointer) % alignment == 0;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an empty arena owns nothing") {
    Arena arena = smallArena();
    CHECK(arena.pages.count == 0);
    CHECK(arena.large.count == 0);
    CHECK(arena.capacity() == 0);
    CHECK(arena.used() == 0);
}

TEST_CASE("an arena's allocations are aligned as asked") {
    Arena arena = smallArena();
    for (size_t alignment = 1; alignment <= 256; alignment *= 2) {
        // An odd size between allocations moves the next one off every alignment.
        arena.allocBytes(1, 1);
        void* bytes = arena.allocBytes(3, alignment);
        CHECK(isAligned(bytes, alignment));
    }
    CHECK(isAligned(arena.alloc<double>(3), alignof(double)));
    CHECK(isAligned(arena.allocBytes(4096, 64), 64));
    arena.reset();
}

TEST_CASE("an arena zeroes what allocZeroed returns, including reused storage") {
    Arena arena = smallArena();
    uint8_t* first = static_cast<uint8_t*>(arena.allocBytes(200, 1));
    prMemFill(first, 0xab, 200);
    arena.clear();
    uint8_t* zeroed = static_cast<uint8_t*>(arena.allocBytesZeroed(200, 1));
    CHECK(zeroed == first);
    bool allZero = true;
    for (uint32_t i = 0; i < 200; ++i)
        allZero = allZero && zeroed[i] == 0;
    CHECK(allZero);

    uint32_t* words = arena.allocZeroed<uint32_t>(16);
    for (uint32_t i = 0; i < 16; ++i)
        CHECK(words[i] == 0);
    arena.reset();
}

TEST_CASE("an arena's pages double from the first size to the largest") {
    Arena arena = smallArena();
    for (uint32_t i = 0; i < 64; ++i)
        arena.allocBytes(100, 1);
    REQUIRE(arena.pages.count >= 4);
    CHECK(arena.pages.data[0].size == 256);
    CHECK(arena.pages.data[1].size == 512);
    CHECK(arena.pages.data[2].size == 1024);
    for (uint32_t i = 3; i < arena.pages.count; ++i)
        CHECK(arena.pages.data[i].size == 1024);
    CHECK(arena.large.count == 0);
    arena.reset();
}

TEST_CASE("an arena's allocations stay in place and do not overlap") {
    Arena arena = smallArena();
    uint32_t* values[200];
    for (uint32_t i = 0; i < 200; ++i) {
        values[i] = arena.alloc<uint32_t>(5);
        for (uint32_t j = 0; j < 5; ++j)
            values[i][j] = i * 5 + j;
    }
    for (uint32_t i = 0; i < 200; ++i) {
        for (uint32_t j = 0; j < 5; ++j)
            CHECK(values[i][j] == i * 5 + j);
    }
    arena.reset();
}

TEST_CASE("an allocation larger than a page gets a block of its own, and the page keeps filling") {
    Arena arena = smallArena();
    uint8_t* before = static_cast<uint8_t*>(arena.allocBytes(16, 1));
    uint8_t* huge = static_cast<uint8_t*>(arena.allocBytes(10000, 16));
    prMemFill(huge, 7, 10000);
    uint8_t* after = static_cast<uint8_t*>(arena.allocBytes(16, 1));
    CHECK(arena.large.count == 1);
    CHECK(arena.pages.count == 1);
    CHECK(after == before + 16);
    CHECK(huge[9999] == 7);
    CHECK(arena.used() >= 10000 + 32);
    arena.reset();
}

TEST_CASE("an allocation of zero bytes returns a pointer") {
    Arena arena = smallArena();
    CHECK(arena.allocBytes(0, 1) != nullptr);
    CHECK(arena.copy<uint32_t>(nullptr, 0) != nullptr);
    arena.reset();
}

TEST_CASE("copy returns the elements in the arena") {
    Arena arena = smallArena();
    const uint16_t items[] = {1, 2, 3};
    uint16_t* copied = arena.copy(items, 3);
    CHECK(copied != items);
    CHECK(copied[0] == 1);
    CHECK(copied[2] == 3);
    arena.reset();
}

TEST_CASE("clearing an arena keeps its pages, and the same allocations reuse them without allocating") {
    Arena arena = smallArena();
    auto fill = [&]() {
        for (uint32_t i = 0; i < 40; ++i)
            arena.allocBytes(100 + i, 8);
        arena.allocBytes(5000, 8);
    };
    fill();
    uint32_t pages = arena.pages.count;
    uint8_t* first = arena.pages.data[0].data;
    arena.clear();
    CHECK(arena.used() == 0);
    CHECK(arena.large.count == 0);
    CHECK(arena.pages.count == pages);

    // The large allocation is the only one that the repeat makes.
    size_t live = prMemoryLiveCount();
    size_t capacity = arena.capacity();
    for (uint32_t round = 0; round < 3; ++round) {
        arena.clear();
        fill();
        CHECK(arena.pages.count == pages);
        CHECK(arena.pages.data[0].data == first);
    }
    arena.clear();
    CHECK(prMemoryLiveCount() == live);
    CHECK(arena.capacity() == capacity);
    arena.reset();
}

TEST_CASE("resetting an arena frees its pages and blocks") {
    size_t live = prMemoryLiveCount();
    Arena arena = smallArena();
    for (uint32_t i = 0; i < 40; ++i)
        arena.allocBytes(100, 8);
    arena.allocBytes(5000, 8);
    CHECK(arena.capacity() != 0);
    arena.reset();
    CHECK(arena.pages.count == 0);
    CHECK(arena.pages.data == nullptr);
    CHECK(arena.large.data == nullptr);
    CHECK(arena.capacity() == 0);
    CHECK(prMemoryLiveCount() == live);

    // A reset arena grows again from its first page.
    arena.allocBytes(1, 1);
    CHECK(arena.pages.data[0].size == 256);
    arena.reset();
}

TEST_CASE("a zeroed arena is empty, and its first allocation of any size takes a page of the default size") {
    Arena arena{};
    CHECK(arena.capacity() == 0);
    void* none = arena.allocBytes(0, 1);
    CHECK(none != nullptr);
    CHECK(arena.pages.count == 1);
    CHECK(arena.pages.data[0].size == kArenaFirstPage);
    uint8_t* bytes = arena.alloc<uint8_t>(16);
    bytes[15] = 7;
    CHECK(arena.used() >= 16);
    arena.reset();
    CHECK(arena.capacity() == 0);
    CHECK(arena.alloc<uint64_t>() != nullptr);
    arena.reset();
}

TEST_CASE("an allocation that fills a page exactly stays in that page") {
    Arena arena = smallArena();
    uint8_t* first = arena.alloc<uint8_t>(1);
    uint8_t* rest = arena.alloc<uint8_t>(255);
    CHECK(rest == first + 1);
    CHECK(arena.pages.count == 1);
    arena.alloc<uint8_t>(1);
    CHECK(arena.pages.count == 2);
    arena.reset();
}
