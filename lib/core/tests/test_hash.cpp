#include "pr/hash.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("a hash reads every byte, at any length and any alignment") {
    constexpr uint64_t kSeed = 0x1234;
    char text[160] = {};
    for (size_t i = 0; i < sizeof(text); ++i)
        text[i] = char('a' + i % 26);

    // Changing any one byte of a text changes its hash, at every length the steps handle differently.
    const size_t sizes[] = {0, 1, 3, 4, 8, 15, 16, 17, 32, 48, 49, 97, 150};
    for (size_t size : sizes) {
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
