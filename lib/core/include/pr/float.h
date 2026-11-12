// Floating point: constants and functions of IEEE doubles (F64) and floats (F32), and their decimal text.

#pragma once

#include "pr/base.h"
#include "pr/mem.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The C library's functions, through Clang's and GCC's builtins, or under MSVC, which has none, through the library's
// own functions. The library imports some of them from its DLL in a build that links it dynamically.
#if defined(_MSC_VER) && !defined(__clang__)
double __cdecl acos(double x);
double __cdecl asin(double x);
double __cdecl atan(double x);
double __cdecl cos(double x);
double __cdecl exp(double x);
double __cdecl fabs(double x);
double __cdecl log(double x);
double __cdecl sin(double x);
double __cdecl sqrt(double x);
double __cdecl tan(double x);
double __cdecl atan2(double x, double y);
double __cdecl fmod(double x, double y);
double __cdecl pow(double x, double y);
PR_CRT_IMPORT double __cdecl cbrt(double x);
PR_CRT_IMPORT double __cdecl ceil(double x);
PR_CRT_IMPORT double __cdecl exp2(double x);
PR_CRT_IMPORT double __cdecl log2(double x);
PR_CRT_IMPORT float __cdecl exp2f(float x);
PR_CRT_IMPORT double __cdecl floor(double x);
PR_CRT_IMPORT double __cdecl round(double x);
PR_CRT_IMPORT double __cdecl hypot(double x, double y);
PR_CRT_IMPORT double __cdecl erf(double x);
PR_CRT_IMPORT double __cdecl nextafter(double x, double y);
PR_CRT_IMPORT float __cdecl nextafterf(float x, float y);
PR_CRT_IMPORT float __cdecl acosf(float x);
PR_CRT_IMPORT float __cdecl atanf(float x);
PR_CRT_IMPORT float __cdecl ceilf(float x);
PR_CRT_IMPORT float __cdecl cosf(float x);
PR_CRT_IMPORT float __cdecl expf(float x);
PR_CRT_IMPORT float __cdecl floorf(float x);
PR_CRT_IMPORT float __cdecl log2f(float x);
PR_CRT_IMPORT float __cdecl logf(float x);
PR_CRT_IMPORT float __cdecl roundf(float x);
PR_CRT_IMPORT float __cdecl sinf(float x);
PR_CRT_IMPORT float __cdecl sqrtf(float x);
PR_CRT_IMPORT float __cdecl tanf(float x);
PR_CRT_IMPORT float __cdecl atan2f(float x, float y);
PR_CRT_IMPORT float __cdecl fmodf(float x, float y);
PR_CRT_IMPORT float __cdecl powf(float x, float y);
#define PR_MATH_BUILTIN(name) name
#else
#define PR_MATH_BUILTIN(name) __builtin_##name
#endif

// The largest finite double and float, pi, e and the square root of 2.
PR_CONSTANT double kPrF64Max = 1.7976931348623157e308;
PR_CONSTANT double kPrF64Pi = 3.14159265358979323846;
PR_CONSTANT double kPrF64E = 2.71828182845904523536;
PR_CONSTANT float kPrF32Max = 3.40282346638528859812e38f;
PR_CONSTANT float kPrF32Pi = 3.14159265358979323846f;
PR_CONSTANT float kPrF32Sqrt2 = 1.41421356237309504880f;

// Infinity and NaN. C under MSVC has no builtins for them, so it folds them from an overflowing product, as the C
// library's own headers do.
#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)
#pragma warning(push)
#pragma warning(disable : 4056 4756)
PR_CONSTANT double kPrF64Infinity = 1e300 * 1e300;
PR_CONSTANT double kPrF64Nan = 1e300 * 1e300 * 0.0;
PR_CONSTANT float kPrF32Infinity = (float)(1e300 * 1e300);
PR_CONSTANT float kPrF32Nan = (float)(1e300 * 1e300 * 0.0);
#pragma warning(pop)
#else
PR_CONSTANT double kPrF64Infinity = __builtin_huge_val();
PR_CONSTANT double kPrF64Nan = __builtin_nan("");
PR_CONSTANT float kPrF32Infinity = __builtin_huge_valf();
PR_CONSTANT float kPrF32Nan = __builtin_nanf("");
#endif

// Returns true if `value` is NaN.
PR_CONSTEXPR bool prF64IsNan(double value) {
    return value != value;
}

// Implements ECMAScript's SameValueZero: returns true if `a` and `b` are equal, or both NaN.
PR_CONSTEXPR bool prF64SameValueZero(double a, double b) {
    return a == b || (prF64IsNan(a) && prF64IsNan(b));
}

// Returns true if `value` is neither infinite nor NaN.
PR_CONSTEXPR bool prF64IsFinite(double value) {
    return value >= -kPrF64Max && value <= kPrF64Max;
}

// Returns the value `t` of the way from `a` to `b`: `a` at 0 and `b` at 1.
PR_CONSTEXPR double prF64Lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

// Returns an angle of `degrees` in radians.
PR_CONSTEXPR double prF64DegreesToRadians(double degrees) {
    return degrees * kPrF64Pi / 180;
}

// Returns true if the sign bit of `value` is set, so it is true for -0 and false for 0.
static inline bool prF64SignBit(double value) {
    uint64_t bits;
    prMemCopy(&bits, &value, sizeof(bits));
    return bits >> 63 != 0;
}

// The absolute value of `value`.
static inline double prF64Abs(double value) {
    return PR_MATH_BUILTIN(fabs)(value);
}

// Return the lesser or greater of `a` and `b`. If they compare equal or either is NaN, the result is `a`.
PR_CONSTEXPR double prF64Min(double a, double b) {
    return b < a ? b : a;
}
PR_CONSTEXPR double prF64Max(double a, double b) {
    return b > a ? b : a;
}

// Wrappers for the C library's IEEE-754 functions. They return the C library's results for zeros, infinities and NaN.
static inline double prF64Sin(double value) {
    return PR_MATH_BUILTIN(sin)(value);
}
static inline double prF64Cos(double value) {
    return PR_MATH_BUILTIN(cos)(value);
}
static inline double prF64Tan(double value) {
    return PR_MATH_BUILTIN(tan)(value);
}
static inline double prF64Asin(double value) {
    return PR_MATH_BUILTIN(asin)(value);
}
static inline double prF64Acos(double value) {
    return PR_MATH_BUILTIN(acos)(value);
}
static inline double prF64Atan(double value) {
    return PR_MATH_BUILTIN(atan)(value);
}
static inline double prF64Atan2(double y, double x) {
    return PR_MATH_BUILTIN(atan2)(y, x);
}
static inline double prF64Pow(double base, double exponent) {
    return PR_MATH_BUILTIN(pow)(base, exponent);
}
static inline double prF64Sqrt(double value) {
    return PR_MATH_BUILTIN(sqrt)(value);
}
static inline double prF64Cbrt(double value) {
    return PR_MATH_BUILTIN(cbrt)(value);
}
static inline double prF64Hypot(double x, double y) {
    return PR_MATH_BUILTIN(hypot)(x, y);
}
static inline double prF64Erf(double value) {
    return PR_MATH_BUILTIN(erf)(value);
}
// Returns the double after `from` in the direction of `toward`, or `toward` if they are equal.
static inline double prF64NextAfter(double from, double toward) {
    return PR_MATH_BUILTIN(nextafter)(from, toward);
}
static inline double prF64Log(double value) {
    return PR_MATH_BUILTIN(log)(value);
}
static inline double prF64Exp(double value) {
    return PR_MATH_BUILTIN(exp)(value);
}
static inline double prF64Exp2(double value) {
    return PR_MATH_BUILTIN(exp2)(value);
}
static inline double prF64Log2(double value) {
    return PR_MATH_BUILTIN(log2)(value);
}
static inline double prF64Floor(double value) {
    return PR_MATH_BUILTIN(floor)(value);
}
static inline double prF64Ceil(double value) {
    return PR_MATH_BUILTIN(ceil)(value);
}
static inline double prF64Round(double value) {
    return PR_MATH_BUILTIN(round)(value);
}
static inline double prF64Fmod(double x, double y) {
    return PR_MATH_BUILTIN(fmod)(x, y);
}

// Return the lesser and the greater of two values, by the minimum and maximum operations of IEEE 754-2019. The result
// is NaN if either value is NaN, and -0 orders below 0.
static inline double prF64Minimum(double a, double b) {
    if (prF64IsNan(a) || prF64IsNan(b))
        return kPrF64Nan;
    if (a == b)
        return prF64SignBit(a) ? a : b;
    return b < a ? b : a;
}
static inline double prF64Maximum(double a, double b) {
    if (prF64IsNan(a) || prF64IsNan(b))
        return kPrF64Nan;
    if (a == b)
        return prF64SignBit(a) ? b : a;
    return b > a ? b : a;
}

// The multiple that a value rounds to: the nearest one, with a tie going up; the one above; the one below; or the one
// toward zero.
enum {
    PrF64RoundMode_Nearest,
    PrF64RoundMode_Up,
    PrF64RoundMode_Down,
    PrF64RoundMode_TowardZero,
};
typedef uint8_t PrF64RoundMode;

// Returns `a` rounded to a multiple of `step` by `mode`. A zero result keeps the sign of `a`. The result is NaN if
// either argument is NaN, if `step` is zero, or if both are infinite. An infinite `a` is returned unchanged. An
// infinite `step` gives zero, except that Up gives +infinity for a positive `a` and Down gives -infinity for a negative
// `a`.
static inline double prF64RoundToMultiple(double a, double step, PrF64RoundMode mode) {
    if (prF64IsNan(a) || prF64IsNan(step) || step == 0 || (!prF64IsFinite(a) && !prF64IsFinite(step)))
        return kPrF64Nan;
    if (!prF64IsFinite(a))
        return a;
    bool negative = a < 0 || (a == 0 && prF64SignBit(a));
    if (!prF64IsFinite(step)) {
        if (mode == PrF64RoundMode_Up && a > 0)
            return kPrF64Infinity;
        if (mode == PrF64RoundMode_Down && a < 0)
            return -kPrF64Infinity;
        return negative ? -0.0 : 0.0;
    }
    step = prF64Abs(step);
    if (prF64Fmod(a, step) == 0)
        return a;
    double lower = prF64Floor(a / step) * step;
    double upper = prF64Ceil(a / step) * step;
    lower = lower == 0 ? 0.0 : lower;
    upper = upper == 0 ? -0.0 : upper;
    switch (mode) {
        case PrF64RoundMode_Up:
            return upper;
        case PrF64RoundMode_Down:
            return lower;
        case PrF64RoundMode_TowardZero:
            return prF64Abs(lower) < prF64Abs(upper) ? lower : upper;
        default:
            return a - lower < upper - a ? lower : upper;
    }
}

// Returns the remainder of `a` divided by `step`, with the quotient rounded down. The result takes the sign of `step`.
// It is NaN if either argument is NaN, if `step` is zero, or if `a` is infinite. If `step` is infinite, the result is
// `a` if the two share a sign, and NaN otherwise. In contrast, prF64Fmod rounds the quotient toward zero, and its
// result takes the sign of `a`.
static inline double prF64FloorMod(double a, double step) {
    if (prF64IsNan(a) || prF64IsNan(step) || step == 0 || !prF64IsFinite(a))
        return kPrF64Nan;
    if (!prF64IsFinite(step))
        return prF64SignBit(a) != prF64SignBit(step) ? kPrF64Nan : a;
    double r = prF64Fmod(a, step);
    if (r != 0 && (r < 0) != (step < 0))
        r += step;
    return r == 0 ? (step < 0 ? -0.0 : 0.0) : r;
}

// Returns `value` truncated toward zero and clamped to the range of an int32_t, or 0 for NaN.
PR_CONSTEXPR int32_t prF64SaturateI32(double value) {
    if (prF64IsNan(value))
        return 0;
    return value <= -2147483648.0 ? INT32_MIN : value >= 2147483647.0 ? INT32_MAX : PR_VALUE_CAST(int32_t, value);
}

// Returns `value` truncated toward zero and clamped to the range of a uint32_t, or 0 for NaN.
PR_CONSTEXPR uint32_t prF64SaturateU32(double value) {
    if (prF64IsNan(value) || value <= 0)
        return 0;
    return value >= 4294967295.0 ? UINT32_MAX : PR_VALUE_CAST(uint32_t, value);
}

// Returns true if `value` is NaN.
PR_CONSTEXPR bool prF32IsNan(float value) {
    return value != value;
}

// Returns true if `value` is neither infinite nor NaN.
PR_CONSTEXPR bool prF32IsFinite(float value) {
    return value >= -kPrF32Max && value <= kPrF32Max;
}

// The absolute value of `value`.
static inline float prF32Abs(float value) {
    uint32_t bits;
    prMemCopy(&bits, &value, sizeof(bits));
    bits &= 0x7fffffffu;
    prMemCopy(&value, &bits, sizeof(value));
    return value;
}

// Return the lesser or greater of `a` and `b`. If they compare equal or either is NaN, the result is `a`.
PR_CONSTEXPR float prF32Min(float a, float b) {
    return b < a ? b : a;
}
PR_CONSTEXPR float prF32Max(float a, float b) {
    return b > a ? b : a;
}

// Returns `value` clamped to [`low`, `high`]. NaN stays NaN.
PR_CONSTEXPR float prF32Clamp(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

// Returns the value `t` of the way from `a` to `b`: `a` at 0 and `b` at 1.
PR_CONSTEXPR float prF32Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Returns an angle of `degrees` in radians.
PR_CONSTEXPR float prF32DegreesToRadians(float degrees) {
    return degrees * kPrF32Pi / 180;
}

// Wrappers for the C library's IEEE-754 functions. They return the C library's results for zeros, infinities and NaN.
// Returns the float after `from` in the direction of `toward`, or `toward` if they are equal.
static inline float prF32NextAfter(float from, float toward) {
    return PR_MATH_BUILTIN(nextafterf)(from, toward);
}
static inline float prF32Sqrt(float value) {
    return PR_MATH_BUILTIN(sqrtf)(value);
}
static inline float prF32Sin(float value) {
    return PR_MATH_BUILTIN(sinf)(value);
}
static inline float prF32Cos(float value) {
    return PR_MATH_BUILTIN(cosf)(value);
}
static inline float prF32Acos(float value) {
    return PR_MATH_BUILTIN(acosf)(value);
}
static inline float prF32Atan2(float y, float x) {
    return PR_MATH_BUILTIN(atan2f)(y, x);
}
static inline float prF32Tan(float value) {
    return PR_MATH_BUILTIN(tanf)(value);
}
static inline float prF32Atan(float value) {
    return PR_MATH_BUILTIN(atanf)(value);
}
static inline float prF32Pow(float base, float exponent) {
    return PR_MATH_BUILTIN(powf)(base, exponent);
}
static inline float prF32Exp(float value) {
    return PR_MATH_BUILTIN(expf)(value);
}
static inline float prF32Log(float value) {
    return PR_MATH_BUILTIN(logf)(value);
}
static inline float prF32Log2(float value) {
    return PR_MATH_BUILTIN(log2f)(value);
}
static inline float prF32Exp2(float value) {
    return PR_MATH_BUILTIN(exp2f)(value);
}

// Returns the length of the vector (`x`, `y`), without overflow or underflow in the squares. It is computed in double,
// because MSVC's C library defines hypotf only inline in its header.
static inline float prF32Hypot(float x, float y) {
    return PR_VALUE_CAST(float, prF64Hypot(PR_VALUE_CAST(double, x), PR_VALUE_CAST(double, y)));
}
static inline float prF32Floor(float value) {
    return PR_MATH_BUILTIN(floorf)(value);
}
static inline float prF32Ceil(float value) {
    return PR_MATH_BUILTIN(ceilf)(value);
}

// Returns `value` rounded to the nearest integer, with halves rounded away from zero.
static inline float prF32Round(float value) {
    return PR_MATH_BUILTIN(roundf)(value);
}

// Returns the remainder of `x` divided by `y`, with the sign of `x`, as computed by the C library.
static inline float prF32Fmod(float x, float y) {
    return PR_MATH_BUILTIN(fmodf)(x, y);
}

// The bounds of the text that prF64Format writes.
enum {
    kPrF64TextMaxBytes = 32,            // the most bytes that prF64Format writes, with no terminator
    kPrF64PositionalMinExponent = -6,   // the range of exponents that prF64Format writes in positional notation
    kPrF64PositionalMaxExponent = 21,
    kPrF64MaxDigits = 17,               // the number of significant digits that tells every double apart

    // The most bytes that prF64FormatFixed writes: a sign, the digits below 10^kPrF64PositionalMaxExponent, a point and
    // kPrF64MaxDigits places.
    kPrF64FixedMaxBytes = 1 + kPrF64PositionalMaxExponent + 1 + kPrF64MaxDigits,
};

// Parses the decimal number in the `size` bytes at `data`, correctly rounded with ties to even. The syntax is an
// optional sign, digits with an optional fractional part, and an optional exponent. The bytes must hold at least one
// digit.
double prF64Parse(const char* data, size_t size);

// Writes to `out` the shortest decimal that parses back to `value`, and returns its length: "0", "0.5", "1e+21",
// "1e-7", "Infinity", "NaN". Exponents from kPrF64PositionalMinExponent through kPrF64PositionalMaxExponent use
// positional notation, and others use an exponent. Negative zero writes as "0". `out` must hold at least
// kPrF64TextMaxBytes bytes. The output is not terminated.
size_t prF64Format(double value, char* out);

// Writes to `out` the shortest decimal that parses back to `value` at a float's precision, in the notation of
// prF64Format, and returns its length: the float nearest 1.57 writes as "1.57". A parse to a double that is then
// rounded to a float gives `value` too. `out` must hold at least kPrF64TextMaxBytes bytes. The output is not
// terminated.
size_t prF32Format(float value, char* out);

// Writes to `out` the shortest decimal of `value` rounded to `digits` significant digits, half away from zero, in the
// notation of prF64Format. 73.33859777674978 to six digits is "73.3386". `digits` must be 1 through kPrF64MaxDigits.
size_t prF64FormatSignificant(double value, uint32_t digits, char* out);

// Writes to `out` the shortest decimal of `value` rounded to `decimals` places, half away from zero, in positional
// notation with no exponent and no trailing zeros, and returns its length: 0.12345 to four places is "0.1235", and 2.5
// to none is "3". A value that rounds to zero writes as "0". Formats such as PDF, which read no exponent and bound the
// precision of a real, write their numbers this way. `value` must be finite, and its magnitude below
// 10^kPrF64PositionalMaxExponent. `decimals` must be at most kPrF64MaxDigits. `out` must hold at least
// kPrF64FixedMaxBytes bytes. The output is not terminated.
size_t prF64FormatFixed(double value, uint32_t decimals, char* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
