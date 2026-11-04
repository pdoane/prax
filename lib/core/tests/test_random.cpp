#include "pr/random.h"

#include "tools/harness/test.h"

TEST_CASE("a generator gives the same wyrand sequence for a seed on every platform") {
    PrRandom zero = {};
    CHECK(prRandomU64(&zero) == 0x111cb3a78f59a58eull);
    CHECK(prRandomU64(&zero) == 0xceabd938ff4e856dull);
    CHECK(prRandomU64(&zero) == 0x61fb51318f47d2a4ull);
    PrRandom seeded = {.state = 42};
    CHECK(prRandomU64(&seeded) == 0xae4a7cbfdda9b434ull);
    PrRandom again = {.state = 42};
    CHECK(prRandomU64(&again) == 0xae4a7cbfdda9b434ull);
}

TEST_CASE("a bounded draw stays below its bound and reaches every value about equally") {
    PrRandom random = {.state = 7};
    uint32_t counts[6] = {};
    for (uint32_t i = 0; i < 60000; ++i) {
        uint32_t value = prRandomU32Below(&random, 6);
        REQUIRE(value < 6);
        ++counts[value];
    }
    for (uint32_t count : counts)
        CHECK((count > 9500 && count < 10500));
    for (uint32_t i = 0; i < 1000; ++i) {
        CHECK(prRandomU32Below(&random, 1) == 0);
        CHECK(prRandomU32Below(&random, 0x80000001u) < 0x80000001u);
        CHECK(prRandomU64Below(&random, 3) < 3);
        CHECK(prRandomU64Below(&random, 0x8000000000000001ull) < 0x8000000000000001ull);
    }
}

TEST_CASE("a draw between two integers includes both, across the whole range") {
    PrRandom random = {.state = 11};
    bool sawLow = false;
    bool sawHigh = false;
    for (uint32_t i = 0; i < 1000; ++i) {
        int32_t value = prRandomI32Between(&random, -2, 2);
        REQUIRE((value >= -2 && value <= 2));
        sawLow = sawLow || value == -2;
        sawHigh = sawHigh || value == 2;
    }
    CHECK(sawLow);
    CHECK(sawHigh);
    CHECK(prRandomI32Between(&random, 5, 5) == 5);
    bool negative = false;
    for (uint32_t i = 0; i < 100; ++i)
        negative = negative || prRandomI32Between(&random, INT32_MIN, INT32_MAX) < 0;
    CHECK(negative);
}

TEST_CASE("a float draw lies below one, and a gaussian has mean 0 and deviation 1 within three") {
    PrRandom random = {.state = 3};
    double sum = 0;
    double squares = 0;
    uint32_t trues = 0;
    constexpr uint32_t kDraws = 100000;
    for (uint32_t i = 0; i < kDraws; ++i) {
        float unit = prRandomF32(&random);
        REQUIRE((unit >= 0 && unit < 1));
        double wide = prRandomF64(&random);
        REQUIRE((wide >= 0 && wide < 1));
        float between = prRandomF32Between(&random, -4, 4);
        REQUIRE((between >= -4 && between < 4));
        float gaussian = prRandomGaussianF32(&random);
        REQUIRE((gaussian >= -3 && gaussian < 3));
        sum += double(gaussian);
        squares += double(gaussian) * double(gaussian);
        trues += prRandomBool(&random) ? 1 : 0;
    }
    CHECK(sum / kDraws == doctest::Approx(0).epsilon(0.01));
    CHECK(squares / kDraws == doctest::Approx(1).epsilon(0.02));
    CHECK((trues > 49000 && trues < 51000));
}

TEST_CASE("a seed from the platform differs from run to run") {
    CHECK(prRandomSeed() != prRandomSeed());
}
