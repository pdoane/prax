#include "pr/integer.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

String formatted(uint64_t value, uint32_t radix = 10) {
    char text[kPrU64TextMaxBytes];
    return String(PrStringView(text, prU64Format(value, text, radix)));
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
