// Floating point: constants and functions of IEEE doubles and floats.

#pragma once

#include "pr/base.h"

// The C library's functions, through Clang's and GCC's builtins, or under MSVC, which has none, through the library's
// own functions. The library imports some of them from its DLL in a build that links it dynamically.
#if defined(_MSC_VER) && !defined(__clang__)
extern "C" double __cdecl acos(double x);
extern "C" double __cdecl asin(double x);
extern "C" double __cdecl atan(double x);
extern "C" double __cdecl cos(double x);
extern "C" double __cdecl exp(double x);
extern "C" double __cdecl fabs(double x);
extern "C" double __cdecl log(double x);
extern "C" double __cdecl sin(double x);
extern "C" double __cdecl sqrt(double x);
extern "C" double __cdecl tan(double x);
extern "C" double __cdecl atan2(double x, double y);
extern "C" double __cdecl fmod(double x, double y);
extern "C" double __cdecl pow(double x, double y);
extern "C" PR_CRT_IMPORT double __cdecl cbrt(double x);
extern "C" PR_CRT_IMPORT double __cdecl ceil(double x);
extern "C" PR_CRT_IMPORT double __cdecl floor(double x);
extern "C" PR_CRT_IMPORT double __cdecl round(double x);
extern "C" PR_CRT_IMPORT double __cdecl hypot(double x, double y);
extern "C" PR_CRT_IMPORT float __cdecl acosf(float x);
extern "C" PR_CRT_IMPORT float __cdecl atanf(float x);
extern "C" PR_CRT_IMPORT float __cdecl ceilf(float x);
extern "C" PR_CRT_IMPORT float __cdecl cosf(float x);
extern "C" PR_CRT_IMPORT float __cdecl expf(float x);
extern "C" PR_CRT_IMPORT float __cdecl floorf(float x);
extern "C" PR_CRT_IMPORT float __cdecl log2f(float x);
extern "C" PR_CRT_IMPORT float __cdecl logf(float x);
extern "C" PR_CRT_IMPORT float __cdecl roundf(float x);
extern "C" PR_CRT_IMPORT float __cdecl sinf(float x);
extern "C" PR_CRT_IMPORT float __cdecl sqrtf(float x);
extern "C" PR_CRT_IMPORT float __cdecl tanf(float x);
extern "C" PR_CRT_IMPORT float __cdecl atan2f(float x, float y);
extern "C" PR_CRT_IMPORT float __cdecl fmodf(float x, float y);
extern "C" PR_CRT_IMPORT float __cdecl powf(float x, float y);
#define PR_MATH_BUILTIN(name) ::name
#else
#define PR_MATH_BUILTIN(name) __builtin_##name
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The largest finite double.
constexpr double kF64Max = 1.7976931348623157e308;

// Infinity, NaN, pi and e, as doubles.
constexpr double kInfinityF64 = __builtin_huge_val();
constexpr double kNanF64 = __builtin_nan("");
constexpr double kPiF64 = 3.14159265358979323846;
constexpr double kEF64 = 2.71828182845904523536;

// Returns true if `value` is NaN.
inline bool isNanF64(double value) {
    return value != value;
}

// Implements ECMAScript's SameValueZero: returns true if `a` and `b` are equal, or both NaN.
inline bool sameValueZeroF64(double a, double b) {
    return a == b || (isNanF64(a) && isNanF64(b));
}

// Returns true if `value` is neither infinite nor NaN.
inline bool isFiniteF64(double value) {
    return value >= -kF64Max && value <= kF64Max;
}

// Returns the value `t` of the way from `a` to `b`: `a` at 0 and `b` at 1.
inline double lerpF64(double a, double b, double t) {
    return a + (b - a) * t;
}

// Returns an angle of `degrees` in radians.
inline double degreesToRadiansF64(double degrees) {
    return degrees * kPiF64 / 180;
}

// Returns true if the sign bit of `value` is set, so it is true for -0 and false for 0.
inline bool signBitF64(double value) {
    return __builtin_bit_cast(uint64_t, value) >> 63 != 0;
}

// The absolute value of `value`.
inline double absF64(double value) {
    return PR_MATH_BUILTIN(fabs)(value);
}

// Return the lesser or greater of `a` and `b`. If they compare equal or either is NaN, the result is `a`.
inline double minF64(double a, double b) {
    return b < a ? b : a;
}
inline double maxF64(double a, double b) {
    return b > a ? b : a;
}

// Wrappers for the C library's IEEE-754 functions. They return the C library's results for zeros, infinities and NaN.
inline double sinF64(double value) {
    return PR_MATH_BUILTIN(sin)(value);
}
inline double cosF64(double value) {
    return PR_MATH_BUILTIN(cos)(value);
}
inline double tanF64(double value) {
    return PR_MATH_BUILTIN(tan)(value);
}
inline double asinF64(double value) {
    return PR_MATH_BUILTIN(asin)(value);
}
inline double acosF64(double value) {
    return PR_MATH_BUILTIN(acos)(value);
}
inline double atanF64(double value) {
    return PR_MATH_BUILTIN(atan)(value);
}
inline double atan2F64(double y, double x) {
    return PR_MATH_BUILTIN(atan2)(y, x);
}
inline double powF64(double base, double exponent) {
    return PR_MATH_BUILTIN(pow)(base, exponent);
}
inline double sqrtF64(double value) {
    return PR_MATH_BUILTIN(sqrt)(value);
}
inline double cbrtF64(double value) {
    return PR_MATH_BUILTIN(cbrt)(value);
}
inline double hypotF64(double x, double y) {
    return PR_MATH_BUILTIN(hypot)(x, y);
}
inline double logF64(double value) {
    return PR_MATH_BUILTIN(log)(value);
}
inline double expF64(double value) {
    return PR_MATH_BUILTIN(exp)(value);
}
inline double floorF64(double value) {
    return PR_MATH_BUILTIN(floor)(value);
}
inline double ceilF64(double value) {
    return PR_MATH_BUILTIN(ceil)(value);
}
inline double roundF64(double value) {
    return PR_MATH_BUILTIN(round)(value);
}
inline double fmodF64(double x, double y) {
    return PR_MATH_BUILTIN(fmod)(x, y);
}

// Return the lesser and the greater of two values, by the minimum and maximum operations of IEEE 754-2019. The result
// is NaN if either value is NaN, and -0 orders below 0.
inline double minimumF64(double a, double b) {
    if (isNanF64(a) || isNanF64(b))
        return kNanF64;
    if (a == b)
        return signBitF64(a) ? a : b;
    return b < a ? b : a;
}
inline double maximumF64(double a, double b) {
    if (isNanF64(a) || isNanF64(b))
        return kNanF64;
    if (a == b)
        return signBitF64(a) ? b : a;
    return b > a ? b : a;
}

// The multiple that a value rounds to: the nearest one, with a tie going up; the one above; the one below; or the one
// toward zero.
enum {
    RoundMode_Nearest,
    RoundMode_Up,
    RoundMode_Down,
    RoundMode_TowardZero,
};
typedef uint8_t RoundMode;

// Returns `a` rounded to a multiple of `step` by `mode`. A zero result keeps the sign of `a`. The result is NaN if
// either argument is NaN, if `step` is zero, or if both are infinite. An infinite `a` is returned unchanged. An
// infinite `step` gives zero, except that Up gives +infinity for a positive `a` and Down gives -infinity for a negative
// `a`.
template <RoundMode mode>
inline double roundToMultipleF64(double a, double step) {
    if (isNanF64(a) || isNanF64(step) || step == 0 || (!isFiniteF64(a) && !isFiniteF64(step)))
        return kNanF64;
    if (!isFiniteF64(a))
        return a;
    bool negative = a < 0 || (a == 0 && signBitF64(a));
    if (!isFiniteF64(step)) {
        if constexpr (mode == RoundMode_Up)
            return a > 0 ? kInfinityF64 : negative ? -0.0 : 0.0;
        else if constexpr (mode == RoundMode_Down)
            return a < 0 ? -kInfinityF64 : negative ? -0.0 : 0.0;
        else
            return negative ? -0.0 : 0.0;
    }
    step = absF64(step);
    if (fmodF64(a, step) == 0)
        return a;
    double lower = floorF64(a / step) * step;
    double upper = ceilF64(a / step) * step;
    lower = lower == 0 ? 0.0 : lower;
    upper = upper == 0 ? -0.0 : upper;
    if constexpr (mode == RoundMode_Up)
        return upper;
    else if constexpr (mode == RoundMode_Down)
        return lower;
    else if constexpr (mode == RoundMode_TowardZero)
        return absF64(lower) < absF64(upper) ? lower : upper;
    else
        return a - lower < upper - a ? lower : upper;
}

// Returns the remainder of `a` divided by `step`, with the quotient rounded down. The result takes the sign of `step`.
// It is NaN if either argument is NaN, if `step` is zero, or if `a` is infinite. If `step` is infinite, the result is
// `a` if the two share a sign, and NaN otherwise. In contrast, fmodF64 rounds the quotient toward zero, and its result
// takes the sign of `a`.
inline double floorModF64(double a, double step) {
    if (isNanF64(a) || isNanF64(step) || step == 0 || !isFiniteF64(a))
        return kNanF64;
    if (!isFiniteF64(step))
        return signBitF64(a) != signBitF64(step) ? kNanF64 : a;
    double r = fmodF64(a, step);
    if (r != 0 && (r < 0) != (step < 0))
        r += step;
    return r == 0 ? (step < 0 ? -0.0 : 0.0) : r;
}

// The largest finite float.
constexpr float kF32Max = 3.40282346638528859812e38f;

// Infinity, NaN, pi and the square root of 2, as floats.
constexpr float kInfinityF32 = __builtin_huge_valf();
constexpr float kNanF32 = __builtin_nanf("");
constexpr float kPiF32 = 3.14159265358979323846f;
constexpr float kSqrt2F32 = 1.41421356237309504880f;

// Returns true if `value` is NaN.
inline bool isNanF32(float value) {
    return value != value;
}

// Returns true if `value` is neither infinite nor NaN.
inline bool isFiniteF32(float value) {
    return value >= -kF32Max && value <= kF32Max;
}

// The absolute value of `value`.
inline float absF32(float value) {
    return __builtin_bit_cast(float, __builtin_bit_cast(uint32_t, value) & 0x7fffffffu);
}

// Return the lesser or greater of `a` and `b`. If they compare equal or either is NaN, the result is `a`.
inline float minF32(float a, float b) {
    return b < a ? b : a;
}
inline float maxF32(float a, float b) {
    return b > a ? b : a;
}

// Returns the value `t` of the way from `a` to `b`: `a` at 0 and `b` at 1.
inline float lerpF32(float a, float b, float t) {
    return a + (b - a) * t;
}

// Returns an angle of `degrees` in radians.
inline float degreesToRadiansF32(float degrees) {
    return degrees * kPiF32 / 180;
}

// Wrappers for the C library's IEEE-754 functions. They return the C library's results for zeros, infinities and NaN.
inline float sqrtF32(float value) {
    return PR_MATH_BUILTIN(sqrtf)(value);
}
inline float sinF32(float value) {
    return PR_MATH_BUILTIN(sinf)(value);
}
inline float cosF32(float value) {
    return PR_MATH_BUILTIN(cosf)(value);
}
inline float acosF32(float value) {
    return PR_MATH_BUILTIN(acosf)(value);
}
inline float atan2F32(float y, float x) {
    return PR_MATH_BUILTIN(atan2f)(y, x);
}
inline float tanF32(float value) {
    return PR_MATH_BUILTIN(tanf)(value);
}
inline float atanF32(float value) {
    return PR_MATH_BUILTIN(atanf)(value);
}
inline float powF32(float base, float exponent) {
    return PR_MATH_BUILTIN(powf)(base, exponent);
}
inline float expF32(float value) {
    return PR_MATH_BUILTIN(expf)(value);
}
inline float logF32(float value) {
    return PR_MATH_BUILTIN(logf)(value);
}
inline float log2F32(float value) {
    return PR_MATH_BUILTIN(log2f)(value);
}
inline float floorF32(float value) {
    return PR_MATH_BUILTIN(floorf)(value);
}
inline float ceilF32(float value) {
    return PR_MATH_BUILTIN(ceilf)(value);
}

// Returns `value` rounded to the nearest integer, with halves rounded away from zero.
inline float roundF32(float value) {
    return PR_MATH_BUILTIN(roundf)(value);
}

// Returns the remainder of `x` divided by `y`, with the sign of `x`, as computed by the C library.
inline float fmodF32(float x, float y) {
    return PR_MATH_BUILTIN(fmodf)(x, y);
}

// Returns `value` clamped to [`low`, `high`]. NaN stays NaN.
inline float clampF32(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

// Returns `value` truncated toward zero and clamped to the range of an int32_t, or 0 for NaN.
inline int32_t saturateI32(double value) {
    if (isNanF64(value))
        return 0;
    return value <= -2147483648.0 ? INT32_MIN : value >= 2147483647.0 ? INT32_MAX : int32_t(value);
}

// Returns `value` truncated toward zero and clamped to the range of a uint32_t, or 0 for NaN.
inline uint32_t saturateU32(double value) {
    if (isNanF64(value) || value <= 0)
        return 0;
    return value >= 4294967295.0 ? UINT32_MAX : uint32_t(value);
}

// Parses the decimal number in the `size` bytes at `data`, correctly rounded with ties to even. The syntax is an
// optional sign, digits with an optional fractional part, and an optional exponent. The bytes must hold at least one
// digit.
double parseF64(const char* data, size_t size);

// The maximum number of bytes that formatF64 writes. There is no terminator.
constexpr size_t kF64TextMaxBytes = 32;

// The range of exponents that formatF64 writes in positional notation.
constexpr int32_t kF64PositionalMinExponent = -6;
constexpr int32_t kF64PositionalMaxExponent = 21;

// The number of significant digits that tells every double apart.
constexpr uint32_t kF64MaxDigits = 17;

// Writes to `out` the shortest decimal that parses back to `value`, and returns its length: "0", "0.5", "1e+21",
// "1e-7", "Infinity", "NaN". Exponents from kF64PositionalMinExponent through kF64PositionalMaxExponent use positional
// notation, and others use an exponent. Negative zero writes as "0". `out` must hold at least kF64TextMaxBytes bytes.
// The output is not terminated.
size_t formatF64(double value, char* out);

// Writes to `out` the shortest decimal of `value` rounded to `digits` significant digits, half away from zero, in the
// notation of formatF64. 73.33859777674978 to six digits is "73.3386". `digits` must be 1 through kF64MaxDigits.
size_t formatF64Significant(double value, uint32_t digits, char* out);

} // namespace pr

PR_ASSUME_NONNULL_END
