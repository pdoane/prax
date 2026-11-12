#include "pr/integer.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

String formatted(uint64_t value, uint32_t radix = 10) {
    char text[kPrU64TextMaxBytes];
    return String(PrStringView(text, prU64Format(value, text, radix)));
}

String repeated(char c, uint32_t count) {
    String out("");
    out.append(count, c);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an unsigned integer is its digits, most significant first") {
    CHECK(formatted(0) == "0");
    CHECK(formatted(7) == "7");
    CHECK(formatted(1234567890) == "1234567890");
    CHECK(formatted(0xffffffffffffffffull) == "18446744073709551615");
}

TEST_CASE("a digit above nine is a lowercase letter") {
    CHECK(formatted(255, 16) == "ff");
    CHECK(formatted(0x2603, 16) == "2603");
    CHECK(formatted(5, 2) == "101");
    CHECK(formatted(35, 36) == "z");
}

TEST_CASE("a prefix parse reads the digits a text starts with, and says how many") {
    uint64_t value = 99;
    CHECK(prU64ParsePrefix("12px", 4, &value, 10) == 2);
    CHECK(value == 12);
    CHECK(prU64ParsePrefix("ffz", 3, &value, 16) == 2);
    CHECK(value == 255);
    CHECK(prU64ParsePrefix("x1", 2, &value, 10) == 0);
    CHECK(value == 255);
    CHECK(prU64ParsePrefix("99999999999999999999999", 23, &value, 10) == 23);
    CHECK(value == UINT64_MAX);
}

TEST_CASE("every count of decimal digits writes whole, at each power of ten and below it") {
    uint64_t power = 1;
    for (uint32_t digits = 1; digits <= 20; ++digits) {
        String expected = repeated('1', 1);
        expected.append(digits - 1, '0');
        CHECK(formatted(power) == expected);
        CHECK(prU64DecimalDigits(power) == digits);
        if (digits > 1) {
            CHECK(formatted(power - 1) == repeated('9', digits - 1));
            CHECK(prU64DecimalDigits(power - 1) == digits - 1);
        }
        if (digits < 20)
            power *= 10;
    }
    CHECK(prU64DecimalDigits(0) == 1);
    CHECK(prU64DecimalDigits(UINT64_MAX) == 20);
    CHECK(prU64Pow10(0) == 1);
    CHECK(prU64Pow10(kPrU64MaxPow10) == 10000000000000000000ull);
}

TEST_CASE("a power of two radix writes its digits from the bits") {
    CHECK(formatted(0, 16) == "0");
    CHECK(formatted(UINT64_MAX, 16) == "ffffffffffffffff");
    CHECK(formatted(UINT64_MAX, 2) == repeated('1', 64));
    CHECK(formatted(UINT64_MAX, 8) == "1777777777777777777777");
    CHECK(formatted(8, 8) == "10");
    CHECK(formatted(UINT64_MAX, 36) == "3w5e11264sgsf");
    CHECK(prU64HighestBit(1) == 0);
    CHECK(prU64HighestBit(UINT64_MAX) == 63);
}

TEST_CASE("a parse saturates past the largest value, and reads letters of either case") {
    uint64_t value = 0;
    CHECK(prU64Parse("18446744073709551615", 20, &value, 10));
    CHECK(value == UINT64_MAX);
    CHECK(prU64Parse("18446744073709551614", 20, &value, 10));
    CHECK(value == UINT64_MAX - 1);
    CHECK(prU64Parse("18446744073709551616", 20, &value, 10));
    CHECK(value == UINT64_MAX);
    CHECK(prU64Parse("000000000000000000000000000042", 30, &value, 10));
    CHECK(value == 42);
    CHECK(prU64Parse("FfFf", 4, &value, 16));
    CHECK(value == 0xffff);
    CHECK(prU64Parse("Zz", 2, &value, 36));
    CHECK(value == 35 * 36 + 35);
    CHECK_FALSE(prU64Parse("12a", 3, &value, 10));
    CHECK_FALSE(prU64Parse("", 0, &value, 10));
}
