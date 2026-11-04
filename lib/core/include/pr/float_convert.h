// Exact conversion between decimal text and doubles. Text parses to the nearest double, ties to even. A double formats
// as the shortest digits that parse back to it.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The decimal `digits` * 10^`exponent`, with no trailing zeros in `digits`.
typedef struct PrF64Decimal {
    uint64_t digits;
    int32_t exponent;
} PrF64Decimal;

// Return the shortest decimal that parses back to `value` at its own precision, so that 0.1f is 1 * 10^-1. `value` must
// be finite and not zero.
PrF64Decimal prF64ShortestDecimal(double value);
PrF64Decimal prF32ShortestDecimal(float value);

// The digits of a double's magnitude rounded to a place: |value| = 0.<digits> * 10^exponent, where the `count` digits
// are ASCII, with no sign and no point. A count of zero means that the value rounds away at that place.
typedef struct PrF64Digits {
    uint32_t count;
    int32_t exponent;
} PrF64Digits;

// The most significant digits that a double's exact decimal expansion has. A request for more is answered with this
// many, and the digits past them are zeros.
enum { kPrF64MaxExactDigits = 768 };

// Write to `out` the digits of `value`, correctly rounded with ties to even: down to the 10^-`decimals` place, or the
// first `significant` digits. `out` holds `capacity` bytes, which must be more than the digits written, and
// kPrF64MaxExactDigits + 1 is always enough. `value` must be finite and not zero.
PrF64Digits prF64FixedDigits(double value, uint32_t decimals, char* out, size_t capacity);
PrF64Digits prF64SignificantDigits(double value, uint32_t significant, char* out, size_t capacity);

// Parses the decimal number at the start of `text` into `*out`, correctly rounded. The syntax is an optional sign,
// digits with an optional fractional part, and an optional exponent. Returns the number of bytes that the number takes,
// so that a caller can tell what follows it, or zero, leaving `*out` unchanged, if `text` has no digits.
size_t prF64ParsePrefix(PrStringView text, double* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
