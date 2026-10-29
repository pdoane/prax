#include "pr/sort.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <vector>

using namespace pr_impl;

TEST_CASE("sorting orders few and many items, duplicates and all") {
    for (uint32_t count : {0u, 1u, 2u, 7u, 16u, 17u, 1000u}) {
        CAPTURE(count);
        std::vector<int> items;
        uint32_t state = 12345;
        for (uint32_t i = 0; i < count; ++i) {
            state = state * 1103515245u + 12345u;
            items.push_back(int(state >> 20) % 50);
        }
        sortItems(items.data(), count, [](int a, int b) { return a < b; });
        for (uint32_t i = 1; i < count; ++i)
            CHECK(items[i - 1] <= items[i]);
    }
}

TEST_CASE("reversing puts the items in the opposite order, an odd count's middle in place") {
    for (uint32_t count : {0u, 1u, 2u, 5u, 6u}) {
        CAPTURE(count);
        std::vector<int> items;
        for (uint32_t i = 0; i < count; ++i)
            items.push_back(int(i));
        reverseItems(items.data(), count);
        for (uint32_t i = 0; i < count; ++i)
            CHECK(items[i] == int(count - 1 - i));
    }
}
