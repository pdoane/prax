// 16-bit floats: IEEE binary16, as a native type if the compiler has one.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A 16-bit float. It is _Float16 if the compiler has that type. Otherwise it holds the bits of the value, which the
// conversions below compute. In C++, such a value is a record that reads as a float implicitly, and a number does not
// convert to it, so that storing a number without prFloat16FromFloat fails to compile rather than storing the number as
// bits. The bits are a scoped enum, which no integer initializes implicitly, even in a braced list of channels. The
// record has no constructor, so that it passes and returns as C's integer does.
#if defined(__clang__) || defined(__GNUC__)
#define PR_FLOAT16_NATIVE 1
typedef _Float16 PrFloat16;
#elif defined(__cplusplus)
#define PR_FLOAT16_NATIVE 0
enum class PrFloat16Bits : uint16_t {};
typedef struct PrFloat16 {
    PrFloat16Bits bits;

    constexpr operator float() const;
} PrFloat16;
#else
#define PR_FLOAT16_NATIVE 0
typedef uint16_t PrFloat16;
#endif

// The bits of a float, and of a 16-bit float, read as an integer and back. C reads them through a union.
#if !PR_FLOAT16_NATIVE && defined(__cplusplus)
#define PR_FLOAT16_FLOAT_BITS(value) __builtin_bit_cast(uint32_t, value)
#define PR_FLOAT16_BITS_FLOAT(bits) __builtin_bit_cast(float, bits)
#define PR_FLOAT16_OF_BITS(bits) __builtin_bit_cast(PrFloat16, PR_VALUE_CAST(uint16_t, bits))
#define PR_FLOAT16_BITS_OF(value) __builtin_bit_cast(uint16_t, value)
#elif !PR_FLOAT16_NATIVE
typedef union PrFloat16FloatBits {
    float f;
    uint32_t u;
} PrFloat16FloatBits;
#define PR_FLOAT16_FLOAT_BITS(value) ((PrFloat16FloatBits){.f = (value)}).u
#define PR_FLOAT16_BITS_FLOAT(bits) ((PrFloat16FloatBits){.u = (bits)}).f
#define PR_FLOAT16_OF_BITS(bits) PR_VALUE_CAST(PrFloat16, bits)
#define PR_FLOAT16_BITS_OF(value) (value)
#endif

// Converts a float to the nearest 16-bit float, with ties to even, and converts a 16-bit float to a float exactly.
#if PR_FLOAT16_NATIVE
PR_CONSTEXPR PrFloat16 prFloat16FromFloat(float value) {
    return PR_VALUE_CAST(PrFloat16, value);
}
PR_CONSTEXPR float prFloat16ToFloat(PrFloat16 value) {
    return PR_VALUE_CAST(float, value);
}
#else
PR_CONSTEXPR PrFloat16 prFloat16FromFloat(float value) {
    const uint32_t bits = PR_FLOAT16_FLOAT_BITS(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exponent = (bits >> 23) & 255u;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent == 255)
        return PR_FLOAT16_OF_BITS(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u));
    if (exponent >= 143)
        return PR_FLOAT16_OF_BITS(sign | 0x7C00u);
    if (exponent < 102)
        return PR_FLOAT16_OF_BITS(sign);
    if (exponent < 113) {
        const uint32_t shift = 126 - exponent;
        const uint32_t significand = mantissa | 0x800000u;
        const uint32_t rounded = significand + ((1u << (shift - 1)) - 1) + ((significand >> shift) & 1u);
        return PR_FLOAT16_OF_BITS(sign | (rounded >> shift));
    }
    const uint32_t rounded = mantissa + 0xFFFu + ((mantissa >> 13) & 1u);
    return PR_FLOAT16_OF_BITS(sign | (((exponent - 112) << 10) + (rounded >> 13)));
}
PR_CONSTEXPR float prFloat16ToFloat(PrFloat16 value) {
    const uint32_t bits = PR_FLOAT16_BITS_OF(value);
    const uint32_t sign = (bits & 0x8000u) << 16;
    uint32_t exponent = (bits >> 10) & 31u;
    uint32_t mantissa = bits & 1023u;
    if (exponent == 0) {
        if (mantissa == 0)
            return PR_FLOAT16_BITS_FLOAT(sign);
        uint32_t shift = 0;
        while ((mantissa & 1024u) == 0) {
            mantissa <<= 1;
            ++shift;
        }
        return PR_FLOAT16_BITS_FLOAT(sign | ((113 - shift) << 23) | ((mantissa & 1023u) << 13));
    }
    if (exponent == 31)
        return PR_FLOAT16_BITS_FLOAT(sign | 0x7F800000u | (mantissa << 13));
    return PR_FLOAT16_BITS_FLOAT(sign | ((exponent + 112) << 23) | (mantissa << 13));
}
#endif

#if defined(__cplusplus)
}
#endif

#if defined(__cplusplus) && !PR_FLOAT16_NATIVE
// The conversion that PrFloat16 declares.
constexpr PrFloat16::operator float() const {
    return prFloat16ToFloat(*this);
}
#endif

PR_ASSUME_NONNULL_END
