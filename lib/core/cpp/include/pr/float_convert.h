// Exact conversion between decimal text and doubles. Text parses to the nearest double, ties to even. A double formats
// as the shortest digits that parse back to it.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The decimal `digits` * 10^`exponent`, with no trailing zeros in `digits`.
struct ShortestDecimal {
    uint64_t digits;
    int32_t exponent;
};

// Returns the shortest decimal that parses back to `value`. `value` must be finite and not zero.
ShortestDecimal shortestDecimalF64(double value);

// Parses the decimal number at the start of `text` into `*out`, correctly rounded. The syntax is an optional sign,
// digits with an optional fractional part, and an optional exponent. Bytes after the number are ignored. Returns false,
// and leaves `*out` unchanged, if `text` has no digits.
bool decimalToF64(PrStringView text, double* out);

} // namespace pr

PR_ASSUME_NONNULL_END
