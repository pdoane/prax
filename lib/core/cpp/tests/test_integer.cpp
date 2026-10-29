#include "pr/integer.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

std::string format(uint64_t value, uint32_t radix = 10) {
    char text[kU64TextMaxBytes];
    return std::string(text, formatU64(value, text, radix));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an unsigned integer is its digits, most significant first") {
    CHECK(format(0) == "0");
    CHECK(format(7) == "7");
    CHECK(format(1234567890) == "1234567890");
    CHECK(format(0xffffffffffffffffull) == "18446744073709551615");
}

TEST_CASE("a digit above nine is a lowercase letter") {
    CHECK(format(255, 16) == "ff");
    CHECK(format(0x2603, 16) == "2603");
    CHECK(format(5, 2) == "101");
    CHECK(format(35, 36) == "z");
}

TEST_CASE("a prefix parse reads the digits a text starts with, and says how many") {
    uint64_t value = 99;
    CHECK(parseU64Prefix("12px", 4, &value) == 2);
    CHECK(value == 12);
    CHECK(parseU64Prefix("ffz", 3, &value, 16) == 2);
    CHECK(value == 255);
    CHECK(parseU64Prefix("x1", 2, &value) == 0);
    CHECK(value == 255);
    CHECK(parseU64Prefix("99999999999999999999999", 23, &value) == 23);
    CHECK(value == UINT64_MAX);
}
