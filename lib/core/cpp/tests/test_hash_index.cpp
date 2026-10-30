#include "pr/hash_index.h"

#include "doctest/doctest.h"

using namespace pr_impl;

TEST_CASE("hash collisions survive growth and backward-shift deletion") {
    HashIndex index;
    for (uint32_t i = 0; i < 300; ++i)
        index.insert(15, i);
    for (uint32_t i = 0; i < 300; i += 3)
        index.erase(15, i);
    CHECK(index.count == 200);
    for (uint32_t i = 0; i < 300; ++i) {
        CHECK(index.find(15, [i](uint32_t at) { return at == i; }) == (i % 3 == 0 ? kHashMissing : i));
    }
    index.erase(15, 900);
    CHECK(index.count == 200);
    index.clear();
    CHECK(index.count == 0);
    CHECK(index.find(15, [](uint32_t) { return true; }) == kHashMissing);
    index.insert(31, 700);
    CHECK(index.find(31, [](uint32_t at) { return at == 700; }) == 700);
    index.reset();
}

TEST_CASE("hash deletion preserves interleaved probe chains across the table boundary") {
    HashIndex index;
    const uint32_t hashes[] = {15, 31, 0, 16, 1, 17, 14, 30, 2, 18};
    bool present[10] = {};
    for (uint32_t round = 0; round < 100; ++round) {
        for (uint32_t i = 0; i < 10; ++i) {
            if (!present[i]) {
                index.insert(hashes[i], i);
                present[i] = true;
            }
        }
        for (uint32_t i = round % 3; i < 10; i += 3) {
            index.erase(hashes[i], i);
            present[i] = false;
            for (uint32_t j = 0; j < 10; ++j) {
                CHECK(index.find(hashes[j], [j](uint32_t at) { return at == j; }) == (present[j] ? j : kHashMissing));
            }
        }
    }
    index.reset();
}

TEST_CASE("a hash index reserved for a count takes that many without growing, and keeps what it holds") {
    HashIndex index;
    index.insert(3, 0);
    index.reserve(1000);
    HashSlot* slots = index.slots;
    for (uint32_t i = 1; i < 1000; ++i)
        index.insert(i * 2654435761u, i);
    CHECK(index.slots == slots);
    CHECK(index.count == 1000);
    CHECK(index.find(3, [](uint32_t at) { return at == 0; }) == 0);
    CHECK(index.find(500 * 2654435761u, [](uint32_t at) { return at == 500; }) == 500);
    // Reserving no more than it has room for keeps its slots.
    index.reserve(10);
    CHECK(index.slots == slots);
    index.reset();
}

TEST_CASE("a relocated position is found at its new place among positions of the same hash") {
    HashIndex index;
    for (uint32_t i = 0; i < 20; ++i)
        index.insert(7, i);
    // The client moves its last entry into the place of one it removed.
    index.erase(7, 4);
    index.relocate(7, 19, 4);
    CHECK(index.count == 19);
    CHECK(index.find(7, [](uint32_t at) { return at == 4; }) == 4);
    CHECK(index.find(7, [](uint32_t at) { return at == 19; }) == kHashMissing);
    for (uint32_t i = 0; i < 19; ++i)
        CHECK(index.find(7, [i](uint32_t at) { return at == i; }) == i);
    index.reset();
}
