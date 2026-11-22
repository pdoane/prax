// String views: the orders that are too long to inline.

#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// Returns the order of two numbers, each written as a run of ASCII digits without leading zeros. The longer run is the
// larger number. Runs of the same length order by their digits.
int compareDigits(PrStringView a, PrStringView b) {
    if (a.size != b.size)
        return a.size < b.size ? -1 : 1;
    return prStringCompare(a, b);
}

// Returns the end of the run of ASCII digits of `text` that starts at `at`.
size_t digitsEnd(PrStringView text, size_t at) {
    while (at < text.size && prAsciiIsDigit(text.data[at]))
        ++at;
    return at;
}

// Returns the end of the run of zeros of `text` from `at` to `end`.
size_t zerosEnd(PrStringView text, size_t at, size_t end) {
    while (at < end && text.data[at] == '0')
        ++at;
    return at;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

int prStringCompareNatural(PrStringView a, PrStringView b) {
    size_t i = 0;
    size_t j = 0;
    // The first difference in leading zeros. It decides between texts that are otherwise equal.
    int zeros = 0;
    while (i < a.size && j < b.size) {
        char c = a.data[i];
        char d = b.data[j];
        if (prAsciiIsDigit(c) && prAsciiIsDigit(d)) {
            size_t endA = digitsEnd(a, i);
            size_t endB = digitsEnd(b, j);
            size_t startA = zerosEnd(a, i, endA);
            size_t startB = zerosEnd(b, j, endB);
            int order = compareDigits(prStringSlice(a, startA, endA - startA), prStringSlice(b, startB, endB - startB));
            if (order != 0)
                return order;
            if (zeros == 0 && startA - i != startB - j)
                zeros = startA - i < startB - j ? -1 : 1;
            i = endA;
            j = endB;
            continue;
        }
        char foldedC = prAsciiLower(c);
        char foldedD = prAsciiLower(d);
        if (foldedC != foldedD)
            return uint8_t(foldedC) < uint8_t(foldedD) ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size || j < b.size)
        return i < a.size ? 1 : -1;
    if (zeros != 0)
        return zeros;
    return prStringCompare(a, b);
}
