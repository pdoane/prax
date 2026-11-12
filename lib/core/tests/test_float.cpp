#include "pr/float.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

double parse(PrStringView text) {
    return prF64Parse(text.data, text.size);
}

String formatted(double value) {
    char text[kPrF64TextMaxBytes];
    return String(PrStringView(text, prF64Format(value, text)));
}

String significant(double value, uint32_t digits) {
    char text[kPrF64TextMaxBytes];
    return String(PrStringView(text, prF64FormatSignificant(value, digits, text)));
}

String fixed(double value, uint32_t decimals) {
    char text[kPrF64FixedMaxBytes];
    return String(PrStringView(text, prF64FormatFixed(value, decimals, text)));
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
    CHECK(formatted(0.0) == "0");
    CHECK(formatted(-0.0) == "0");
    CHECK(formatted(1.0) == "1");
    CHECK(formatted(-1.5) == "-1.5");
    CHECK(formatted(0.1) == "0.1");
    CHECK(formatted(1.0 / 3.0) == "0.3333333333333333");
    CHECK(parse(formatted(0.1)) == 0.1);
    CHECK(parse(formatted(1e100)) == 1e100);
}

TEST_CASE("an exponent appears outside the places positional notation covers") {
    CHECK(formatted(1e-6) == "0.000001");
    CHECK(formatted(1e-7) == "1e-7");
    CHECK(formatted(1e20) == "100000000000000000000");
    CHECK(formatted(1e21) == "1e+21");
    CHECK(formatted(1.5e22) == "1.5e+22");
}

TEST_CASE("what has no decimal text is named instead") {
    double huge = 1e308 * 10.0;
    CHECK(formatted(huge) == "Infinity");
    CHECK(formatted(-huge) == "-Infinity");
    CHECK(formatted(huge - huge) == "NaN");
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

TEST_CASE("a value to a number of places is positional, rounds half away from zero and drops trailing zeros") {
    CHECK(fixed(0.12345, 4) == "0.1235");
    CHECK(fixed(-0.12345, 4) == "-0.1235");
    CHECK(fixed(2.5, 0) == "3");
    CHECK(fixed(1.5, 2) == "1.5");
    CHECK(fixed(0.00001, 4) == "0");
    CHECK(fixed(-0.00004, 4) == "0");
    CHECK(fixed(0.00005, 4) == "0.0001");
    CHECK(fixed(0.0105, 2) == "0.01");
    CHECK(fixed(9.99996, 4) == "10");
    CHECK(fixed(1e20, 4) == "100000000000000000000");
    CHECK(fixed(123456.789, 1) == "123456.8");
    CHECK(fixed(1e-30, 17) == "0");
    CHECK(fixed(0.0, 3) == "0");
    CHECK(fixed(-0.0, 3) == "0");
    CHECK(fixed(-999999999999999999999.0 / 10, 0) == "-100000000000000000000");
}

TEST_CASE("the next value toward another, and the error function") {
    CHECK(prF32NextAfter(0.5f, 0.0f) == 0x1.fffffep-2f);
    CHECK(prF32NextAfter(1.0f, 1.0f) == 1.0f);
    CHECK(prF64NextAfter(1.0, 2.0) == 0x1.0000000000001p0);
    CHECK(prF64Erf(0.0) == 0.0);
    CHECK(prF64Erf(1.0) == doctest::Approx(0.8427007929497149));
}
