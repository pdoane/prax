#include "pr/hash.h"

#include "doctest/doctest.h"

#include <initializer_list>

using namespace pr_impl;

TEST_CASE("a hash reads every byte, at any length and any alignment") {
    constexpr uint64_t kSeed = 0x1234;
    char text[160] = {};
    for (size_t i = 0; i < sizeof(text); ++i)
        text[i] = char('a' + i % 26);

    // Changing any one byte of a text changes its hash, at every length the steps handle differently.
    for (size_t size :
            {
                size_t(0), size_t(1), size_t(3), size_t(4), size_t(8), size_t(15), size_t(16), size_t(17),
                size_t(32), size_t(48), size_t(49), size_t(97), size_t(150),
            }) {
        uint64_t hash = prHashBytes(text, size, kSeed);
        CHECK(hash != prHashBytes(text, size + 1, kSeed));
        for (size_t i = 0; i < size; ++i) {
            text[i] ^= 1;
            CHECK(prHashBytes(text, size, kSeed) != hash);
            text[i] ^= 1;
        }
    }

    // The same bytes hash the same at any address, and differently under another seed.
    char copy[64] = {};
    for (size_t i = 0; i < 60; ++i)
        copy[3 + i] = text[i];
    CHECK(prHashBytes(copy + 3, 60, kSeed) == prHashBytes(text, 60, kSeed));
    CHECK(prHashBytes(text, 60, kSeed) != prHashBytes(text, 60, kSeed + 1));
}
