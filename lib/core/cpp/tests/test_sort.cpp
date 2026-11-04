#include "pr/array.h"
#include "pr/sort.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

#include <cstdint>
#include <vector>

using namespace pr_impl;

TEST_CASE("sorting moves values that own their storage") {
    const char* words[] = {"pear", "apple", "fig", "a longer word than fits inline", "banana", "cherry"};
    Array<String> items;
    for (uint32_t round = 0; round < 4; ++round) {
        for (const char* word : words)
            items.push(word);
    }
    sortItems(items.span(), [](const String& a, const String& b) { return prStringCompare(a, b) < 0; });
    for (uint32_t i = 1; i < items.count(); ++i)
        CHECK(prStringCompare(items[i - 1], items[i]) <= 0);
    CHECK(items[0] == "a longer word than fits inline");
    reverseItems(items.span().slice(2, 4));
    CHECK(items[2] == "apple");
    CHECK(items[5] == "a longer word than fits inline");
}

TEST_CASE("a span reads and slices the elements that it views") {
    int numbers[] = {1, 2, 3, 4, 5};
    Span<int> all(numbers);
    CHECK(all.count() == 5);
    CHECK(all.slice(1, 3) == Span(numbers + 1, 3));
    CHECK(all.from(3).first() == 4);
    CHECK(all.last() == 5);
    Span<const int> reading = all.asConst();
    int total = 0;
    for (int number : reading)
        total += number;
    CHECK(total == 15);
}

TEST_CASE("sorting orders few and many items, duplicates and all") {
    for (uint32_t count : {0u, 1u, 2u, 7u, 16u, 17u, 1000u}) {
        CAPTURE(count);
        std::vector<int> items;
        uint32_t state = 12345;
        for (uint32_t i = 0; i < count; ++i) {
            state = state * 1103515245u + 12345u;
            items.push_back(int(state >> 20) % 50);
        }
        sortItems(Span(items.data(), count), [](int a, int b) { return a < b; });
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
        reverseItems(Span(items.data(), count));
        for (uint32_t i = 0; i < count; ++i)
            CHECK(items[i] == int(count - 1 - i));
    }
}
