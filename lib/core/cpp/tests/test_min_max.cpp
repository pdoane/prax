#include "pr/float.h"
#include "pr/min_max.h"

#include "tools/harness/test.h"

using namespace pr_impl;

static_assert(min(1, 2) == 1);
static_assert(max(1, 2) == 2);
static_assert(clamp(5, 0, 3) == 3);
static_assert(clamp(-1, 0, 3) == 0);
static_assert(clamp(2, 0, 3) == 2);

TEST_CASE("min and max give the first value when the values tie or either is NaN") {
    CHECK(prF64SignBit(double(min(-0.0f, 0.0f))));
    CHECK(!prF64SignBit(double(max(0.0f, -0.0f))));
    CHECK(prF32IsNan(min(kPrF32Nan, 1.0f)));
    CHECK(min(1.0f, kPrF32Nan) == 1.0f);
    CHECK(prF32IsNan(max(kPrF32Nan, 1.0f)));
    CHECK(max(1.0f, kPrF32Nan) == 1.0f);
}

TEST_CASE("clamp keeps NaN and bounds everything else") {
    CHECK(prF32IsNan(clamp(kPrF32Nan, 0.0f, 1.0f)));
    CHECK(clamp(2.0f, 0.0f, 1.0f) == 1.0f);
    CHECK(clamp(-kPrF32Infinity, 0.0f, 1.0f) == 0.0f);
}

TEST_CASE("a magnitude, and a difference that an unsigned type gives without wrapping") {
    CHECK(abs(-3) == 3);
    CHECK(abs(3) == 3);
    CHECK(abs(-1.5) == 1.5);
    CHECK(absDifference(uint8_t(10), uint8_t(250)) == 240);
    CHECK(absDifference(uint8_t(250), uint8_t(10)) == 240);
    CHECK(absDifference(-2, 5) == 7);
}
