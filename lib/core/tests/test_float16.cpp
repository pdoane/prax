#include "pr/bit_cast.h"
#include "pr/float.h"
#include "pr/float16.h"

#include "doctest/doctest.h"

#include <cmath>

namespace pr_impl {

namespace {

uint16_t bitsOf(PrFloat16 value) {
    return bitCast<uint16_t>(value);
}

PrFloat16 fromBits(uint32_t bits) {
    return bitCast<PrFloat16>(uint16_t(bits));
}

bool isNan(float value) {
    return value != value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// If the compiler has a 16-bit float type, these tests check that it converts by the rules of IEEE binary16. Otherwise,
// they check core's conversions by integer arithmetic.
TEST_CASE("every 16-bit float widens exactly and narrows back to itself") {
    uint32_t wrong = 0;
    for (uint32_t bits = 0; bits <= 0xffff; ++bits) {
        float wide = prFloat16ToFloat(fromBits(bits));
        bool nan = (bits & 0x7c00) == 0x7c00 && (bits & 0x3ff) != 0;
        if (nan)
            wrong += isNan(wide) ? 0 : 1;
        else
            wrong += bitsOf(prFloat16FromFloat(wide)) == bits ? 0 : 1;
    }
    CHECK(wrong == 0);
}

TEST_CASE("a float narrows to the nearest 16-bit float, a tie to the even one") {
    uint32_t wrong = 0;
    for (uint32_t bits = 0; bits < 0x7bff; ++bits) {
        float low = prFloat16ToFloat(fromBits(bits));
        float high = prFloat16ToFloat(fromBits(bits + 1));
        // The midpoint, which a float holds exactly, narrows to the even neighbor. A value on either side of it narrows
        // to the nearer neighbor.
        float middle = (low + high) / 2;
        uint32_t even = (bits & 1) == 0 ? bits : bits + 1;
        wrong += bitsOf(prFloat16FromFloat(middle)) == even ? 0 : 1;
        wrong += bitsOf(prFloat16FromFloat(std::nextafter(middle, 0.0f))) == bits ? 0 : 1;
        wrong += bitsOf(prFloat16FromFloat(std::nextafter(middle, high))) == bits + 1 ? 0 : 1;
    }
    CHECK(wrong == 0);
}

TEST_CASE("16-bit floats at the edges of their range") {
    CHECK(bitsOf(prFloat16FromFloat(1)) == 0x3c00);
    CHECK(bitsOf(prFloat16FromFloat(-2)) == 0xc000);
    CHECK(bitsOf(prFloat16FromFloat(-0.0f)) == 0x8000);
    CHECK(bitsOf(prFloat16FromFloat(65504)) == 0x7bff);
    CHECK(bitsOf(prFloat16FromFloat(65519)) == 0x7bff);
    CHECK(bitsOf(prFloat16FromFloat(65520)) == 0x7c00);
    CHECK(bitsOf(prFloat16FromFloat(1e10f)) == 0x7c00);
    CHECK(bitsOf(prFloat16FromFloat(-kPrF32Infinity)) == 0xfc00);
    CHECK(bitsOf(prFloat16FromFloat(6.1035156e-5f)) == 0x0400);
    CHECK(bitsOf(prFloat16FromFloat(5.9604645e-8f)) == 0x0001);
    // Half the smallest subnormal ties to 0, and anything above it reaches the subnormal.
    CHECK(bitsOf(prFloat16FromFloat(2.9802322e-8f)) == 0x0000);
    CHECK(bitsOf(prFloat16FromFloat(2.9802326e-8f)) == 0x0001);
    CHECK(bitsOf(prFloat16FromFloat(1e-10f)) == 0x0000);
    CHECK(isNan(prFloat16ToFloat(prFloat16FromFloat(kPrF32Nan))));
}
