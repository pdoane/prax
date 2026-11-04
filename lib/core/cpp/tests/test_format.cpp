#include "pr/float.h"
#include "pr/format.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {
namespace {

// An enumeration, which formats as its value.
enum {
    Fruit_Apple,
    Fruit_Pear,
};
typedef uint8_t Fruit;

// A type that joins formatting with an overload of formatValue.
struct Point {
    int x;
    int y;
};

void formatValue(Writer& writer, const Point& point, const FormatSpec&) {
    formatTo(writer, "({}, {})", point.x, point.y);
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a format replaces each field with its argument, in order or by index") {
    CHECK(format("{} of {}", 3, 10) == "3 of 10");
    CHECK(format("{1} before {0}", "b", "a") == "a before b");
    CHECK(format("{{literal}} {}", true) == "{literal} true");
    CHECK(format("no fields") == "no fields");
    CHECK(format("{}{}", 'x', Fruit_Pear) == "x1");
    String name("ada");
    CHECK(format("[{}]", name) == "[ada]");
    PrStringView view = "view";
    CHECK(format("{}", view) == "view");
    CHECK(format("{}", Point{1, -2}) == "(1, -2)");
}

TEST_CASE("an integer's spec sets its base, sign, prefix and width") {
    CHECK(format("{:x} {:X} {:o} {:b} {:B}", 255, 255, 8, 5, 5) == "ff FF 10 101 101");
    CHECK(format("{:#x} {:#X} {:#o} {:#b} {:#B}", 255, 255, 8, 5, 5) == "0xff 0XFF 010 0b101 0B101");
    CHECK(format("{:+} {: } {}", 7, 7, -7) == "+7  7 -7");
    CHECK(format("{:05} {:05}", 42, -42) == "00042 -0042");
    CHECK(format("{:#06x}", 255) == "0x00ff");
    CHECK(format("{:0>5}", -4) == "000-4");
    CHECK(format("{:>4}|{:<4}|{:^5}|{:*^5}", 1, 2, 3, 4) == "   1|2   |  3  |**4**");
    CHECK(format("{}", uint64_t(18446744073709551615ull)) == "18446744073709551615");
    CHECK(format("{}", INT64_MIN) == "-9223372036854775808");
    CHECK(format("{:c}", 65) == "A");
    CHECK(format("{:d} {:x}", 'A', true) == "65 1");
}

TEST_CASE("a width or a precision may come from an argument") {
    CHECK(format("{:<{}}|", "hi", 5) == "hi   |");
    CHECK(format("{:.{}f}", 3.14159, 2) == "3.14");
    CHECK(format("{:{}.{}f}", 3.14159, 8, 3) == "   3.142");
    CHECK(format("{0:>{1}}", 7, 3) == "  7");
}

TEST_CASE("a float is its shortest decimal at its own width") {
    CHECK(format("{}", 0.1) == "0.1");
    CHECK(format("{}", 0.1f) == "0.1");
    CHECK(format("{}", 1.5f) == "1.5");
    CHECK(format("{}", 0.0) == "0");
    CHECK(format("{}", -0.0) == "-0");
    CHECK(format("{}", 1e16) == "10000000000000000");
    CHECK(format("{}", 1e17) == "1e17");
    CHECK(format("{}", 1.5e-7) == "1.5e-7");
    CHECK(format("{}", 0.00012) == "0.00012");
    CHECK(format("{}", 123.456) == "123.456");
}

TEST_CASE("a float's spec rounds it exactly to fixed, scientific or general form") {
    CHECK(format("{:.2f}", 3.14159) == "3.14");
    CHECK(format("{:.1} {:.3}", 84.25, 84.25) == "8e+01 84.2");
    CHECK(format("{:f}", 2.5) == "2.500000");
    CHECK(format("{:.0f} {:.0f}", 2.5, 3.5) == "2 4");
    CHECK(format("{:.2f}", 1.005) == "1.00");
    CHECK(format("{:05.1f}", 9.25) == "009.2");
    CHECK(format("{:+.0f}", 2.0) == "+2");
    CHECK(format("{:#.0f}", 2.0) == "2.");
    CHECK(format("{:>8.2f}", -1.0) == "   -1.00");
    CHECK(format("{:.3f}", 0.0004) == "0.000");
    CHECK(format("{:.2f}", 0.995) == "0.99");
    CHECK(format("{:.2f}", 9.996) == "10.00");
    CHECK(format("{:e}", 1234.5) == "1.234500e+03");
    CHECK(format("{:.2E}", 0.000123) == "1.23E-04");
    CHECK(format("{:.0e}", 5e300) == "5e+300");
    CHECK(format("{:g} {:g} {:g}", 1234567.0, 0.0001, 100.0) == "1.23457e+06 0.0001 100");
    CHECK(format("{:#g}", 100.0) == "100.000");
    CHECK(format("{:.3G}", 0.00001234) == "1.23E-05");
}

TEST_CASE("an infinity keeps its sign, and a NaN has none") {
    double infinity = kPrF64Infinity;
    CHECK(format("{} {} {:+}", infinity, -infinity, infinity) == "inf -inf +inf");
    CHECK(format("{:05}", infinity) == "  inf");
    CHECK(format("{:.2f}", kPrF64Nan) == "nan");
}

TEST_CASE("text, characters, booleans and pointers are padded and cut to their spec") {
    CHECK(format("{:>6}|{:<6}|", "ab", "cd") == "    ab|cd    |");
    CHECK(format("{:.3}", "abcdef") == "abc");
    CHECK(format("{:-^7}", "mid") == "--mid--");
    CHECK(format("{:>3}", 'x') == "  x");
    CHECK(format("{:>6}", false) == " false");
    CHECK(format("{}", nullptr) == "nullptr");
    const int* none = nullptr;
    CHECK(format("{}", none) == "nullptr");
    CHECK(format("{}", reinterpret_cast<const void*>(uintptr_t(0x1f))) == "0x1f");
}

TEST_CASE("formatting to a buffer returns the text that fit") {
    char buffer[8];
    CHECK(formatToBuffer(Span(buffer), "{}-{}", 12, 34) == "12-34");
    CHECK(formatToBuffer(Span(buffer), "{} and more", 12345) == "12345 an");
}

TEST_CASE("formatting to a String appends to it") {
    String out("x=");
    formatTo(out, "{}", 1);
    formatTo(out, ", y={:.1f}", 2.25);
    CHECK(out == "x=1, y=2.2");
}
