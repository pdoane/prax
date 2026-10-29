#include "pr/bitset.h"

#include "doctest/doctest.h"

using namespace pr_impl;

TEST_CASE("bitset operations address word boundaries independently") {
    uint64_t words[4] = {};
    for (size_t bit = 0; bit < 256; ++bit) {
        CHECK_FALSE(bitsetTest(words, bit));
        bitsetSet(words, bit);
        CHECK(bitsetTest(words, bit));
        if (bit != 0) CHECK(bitsetTest(words, bit - 1));
    }
    for (size_t bit = 0; bit < 256; ++bit) {
        bitsetClear(words, bit);
        CHECK_FALSE(bitsetTest(words, bit));
        if (bit != 255) CHECK(bitsetTest(words, bit + 1));
    }
}
