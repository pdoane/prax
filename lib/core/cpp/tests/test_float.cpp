#include "pr/float.h"
#include "pr/string_view.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

double parse(const char* text) {
    return parseF64(text, PrStringView(text).size);
}

std::string format(double value) {
    char text[kF64TextMaxBytes];
    return std::string(text, formatF64(value, text));
}

std::string significant(double value, uint32_t digits) {
    char text[kF64TextMaxBytes];
    return std::string(text, formatF64Significant(value, digits, text));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a sign, a fraction and an exponent are each optional") {
    CHECK(parse("0") == 0.0);
    CHECK(parse("1") == 1.0);
    CHECK(parse("1.5") == 1.5);
    CHECK(parse("-3") == -3.0);
    CHECK(parse("+2") == 2.0);
    CHECK(parse(".5") == 0.5);
    CHECK(parse("1e2") == 100.0);
    CHECK(parse("1E+2") == 100.0);
    CHECK(parse("1e-2") == 0.01);
    CHECK(parse("00012.50") == 12.5);
}

TEST_CASE("correctly rounded, not merely close") {
    // Each of these differs by an ulp or more from the product of a significand and a power of ten, so they pass only
    // with a correctly rounding converter.
    CHECK(parse("0.1") == 0.1);
    CHECK(parse("2.2250738585072011e-308") == 2.2250738585072011e-308);
    CHECK(parse("1.7976931348623157e308") == 1.7976931348623157e308);
    CHECK(parse("4.9406564584124654e-324") == 4.9406564584124654e-324);
    CHECK(parse("123456789012345678901234567890") == 123456789012345678901234567890.0);
    CHECK(parse("0.000000000000000000000000001") == 1e-27);
}

TEST_CASE("ties go to even") {
    CHECK(parse("9007199254740993") == 9007199254740992.0);   // 2^53 + 1
    CHECK(parse("1.0000000000000002") == 1.0000000000000002);
}

TEST_CASE("the extremes saturate") {
    CHECK(parse("1e400") > 1e308);          // infinity
    CHECK(parse("1e-400") == 0.0);
    CHECK(parse("-1e-400") == 0.0);
}

TEST_CASE("a formatted value is the shortest text that parses back to it") {
    CHECK(format(0.0) == "0");
    CHECK(format(-0.0) == "0");
    CHECK(format(1.0) == "1");
    CHECK(format(-1.5) == "-1.5");
    CHECK(format(0.1) == "0.1");
    CHECK(format(1.0 / 3.0) == "0.3333333333333333");
    CHECK(parse(format(0.1).c_str()) == 0.1);
    CHECK(parse(format(1e100).c_str()) == 1e100);
}

TEST_CASE("an exponent appears outside the places positional notation covers") {
    CHECK(format(1e-6) == "0.000001");
    CHECK(format(1e-7) == "1e-7");
    CHECK(format(1e20) == "100000000000000000000");
    CHECK(format(1e21) == "1e+21");
    CHECK(format(1.5e22) == "1.5e+22");
}

TEST_CASE("what has no decimal text is named instead") {
    double huge = 1e308 * 10.0;
    CHECK(format(huge) == "Infinity");
    CHECK(format(-huge) == "-Infinity");
    CHECK(format(huge - huge) == "NaN");
}

TEST_CASE("a value to a number of significant digits rounds half away from zero") {
    CHECK(significant(73.33859777674978, 6) == "73.3386");
    CHECK(significant(-0.08000000000000002, 6) == "-0.08");
    CHECK(significant(0.1234565, 6) == "0.123457");
    CHECK(significant(-0.1234565, 6) == "-0.123457");
    CHECK(significant(999999.5, 6) == "1000000");
    CHECK(significant(1234567, 6) == "1234570");
    CHECK(significant(1.0 / 3.0, 6) == "0.333333");
    CHECK(significant(0.5, 6) == "0.5");
    CHECK(significant(-0.0, 6) == "0");
    CHECK(significant(1e-7 / 3.0, 6) == "3.33333e-8");
}
