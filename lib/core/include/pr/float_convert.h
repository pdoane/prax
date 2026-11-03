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

// Returns the shortest decimal that parses back to `value`. `value` must be finite and not zero.
PrF64Decimal prF64ShortestDecimal(double value);

// Parses the decimal number at the start of `text` into `*out`, correctly rounded. The syntax is an optional sign,
// digits with an optional fractional part, and an optional exponent. Bytes after the number are ignored. Returns false,
// and leaves `*out` unchanged, if `text` has no digits.
bool prF64ParsePrefix(PrStringView text, double* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
