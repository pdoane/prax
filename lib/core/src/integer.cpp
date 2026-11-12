#include "pr/integer.h"

#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {
namespace {

constexpr uint64_t kPow10[kPrU64MaxPow10 + 1] = {
    1ull,
    10ull,
    100ull,
    1000ull,
    10000ull,
    100000ull,
    1000000ull,
    10000000ull,
    100000000ull,
    1000000000ull,
    10000000000ull,
    100000000000ull,
    1000000000000ull,
    10000000000000ull,
    100000000000000ull,
    1000000000000000ull,
    10000000000000000ull,
    100000000000000000ull,
    1000000000000000000ull,
    10000000000000000000ull,
};

// Write the two, four and eight digits of `value`, with leading zeros, and return the end of what they wrote. Each
// value must be below the block's power of ten.
PR_FORCE_INLINE char* writePair(uint32_t value, char* out) {
    prMemCopy(out, prU64DigitPair(value), 2);
    return out + 2;
}

PR_FORCE_INLINE char* writeFour(uint32_t value, char* out) {
    return writePair(value % 100, writePair(value / 100, out));
}

// value / 10^6 is held in fixed point with 47 fraction bits, rounded up, and each multiplication of the fraction by 100
// brings the next two digits into the integer part. The error stays below a unit of the last pair, so each pair is
// exact.
PR_FORCE_INLINE char* writeEight(uint32_t value, char* out) {
    constexpr uint64_t kMask = (1ull << 47) - 1;
    uint64_t fixed = uint64_t(value) * 140737489ull;   // ceil(2^47 / 10^6)
    writePair(uint32_t(fixed >> 47), out);
    fixed = (fixed & kMask) * 100;
    writePair(uint32_t(fixed >> 47), out + 2);
    fixed = (fixed & kMask) * 100;
    writePair(uint32_t(fixed >> 47), out + 4);
    fixed = (fixed & kMask) * 100;
    writePair(uint32_t(fixed >> 47), out + 6);
    return out + 8;
}

size_t formatPowerOfTwo(uint64_t value, char* out, uint32_t bits) {
    const char* digits = "0123456789abcdef";
    uint32_t mask = (1u << bits) - 1;
    size_t count = (prU64HighestBit(value | 1) + bits) / bits;
    for (size_t i = count; i-- > 0;) {
        out[i] = digits[value & mask];
        value >>= bits;
    }
    return count;
}

size_t formatAnyRadix(uint64_t value, char* out, uint32_t radix) {
    const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    char reversed[kPrU64TextMaxBytes];
    size_t size = 0;
    do {
        reversed[size++] = digits[value % radix];
        value /= radix;
    } while (value != 0);
    for (size_t i = 0; i < size; ++i)
        out[i] = reversed[size - i - 1];
    return size;
}

uint32_t digitValue(char c) {
    uint32_t decimal = uint32_t(uint8_t(c)) - '0';
    if (decimal < 10)
        return decimal;
    // Setting the bit that tells the cases apart folds the uppercase letters onto the lowercase ones.
    uint32_t letter = (uint32_t(uint8_t(c)) | 0x20) - 'a';
    return letter < 26 ? letter + 10 : kPrU64MaxRadix;
}

// Parses digits in `radix`, saturating at UINT64_MAX. A constant radix makes the bound of each step a constant.
template <uint32_t Radix>
size_t parseDigits(const char* data, size_t size, uint64_t* out, uint32_t radix) {
    uint32_t base = Radix != 0 ? Radix : radix;
    uint64_t limit = UINT64_MAX / base;
    uint32_t limitDigit = uint32_t(UINT64_MAX % base);
    uint64_t value = 0;
    size_t i = 0;
    // Nineteen decimal digits, or as many of any radix as the first step after them can reach, never overflow.
    size_t safe = Radix == 10 ? 19 : 0;
    for (; i < size && i < safe; ++i) {
        uint32_t digit = uint32_t(uint8_t(data[i])) - '0';
        if (digit >= 10)
            break;
        value = value * 10 + digit;
    }
    if (i == safe || safe == 0) {
        for (; i < size; ++i) {
            uint32_t digit = digitValue(data[i]);
            if (digit >= base)
                break;
            if (value > limit || (value == limit && digit > limitDigit))
                value = UINT64_MAX;
            else
                value = value * base + digit;
        }
    }
    if (i != 0)
        *out = value;
    return i;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint64_t prU64Pow10(uint32_t exponent) {
    PR_ASSERT(exponent <= kPrU64MaxPow10);
    return kPow10[exponent];
}

uint32_t prU64DecimalDigits(uint64_t value) {
    if (value == 0)
        return 1;
    // A value of b bits has floor(b * log10(2)) or one more digits; 1233 / 4096 is log10(2) closely enough for 64 bits.
    uint32_t estimate = ((prU64HighestBit(value) + 1) * 1233) >> 12;
    return estimate + (value >= kPow10[estimate] ? 1 : 0);
}

// The digits are written at a fixed width for the value's band, with leading zeros, and then copied from the first
// significant digit by a copy of that fixed width. The copy runs past the digits into the rest of `out`, which holds
// kPrU64TextMaxBytes, so that no branch depends on the count of digits, which a run of values varies at random.
size_t prU64FormatDecimalWide(uint64_t value, char* out) {
    PR_ASSERT(value >= 100);
    char digits[24];
    uint32_t count = prU64DecimalDigits(value);
    if (value < 10000) {
        writeFour(uint32_t(value), digits);
        prMemCopy(out, digits + 4 - count, 4);
    } else if (value < 100000000ull) {
        writeEight(uint32_t(value), digits);
        prMemCopy(out, digits + 8 - count, 8);
    } else if (value < 10000000000000000ull) {
        uint64_t high = value / 100000000ull;
        writeEight(uint32_t(value - high * 100000000ull), writeEight(uint32_t(high), digits));
        prMemCopy(out, digits + 16 - count, 16);
    } else {
        // Both quotients come from the value, so that neither division waits on the other.
        uint64_t high = value / 100000000ull;
        uint64_t top = value / 10000000000000000ull;
        char* end = writeFour(uint32_t(top), digits);
        writeEight(uint32_t(value - high * 100000000ull), writeEight(uint32_t(high - top * 100000000ull), end));
        prMemCopy(out, digits + 20 - count, 20);
    }
    return count;
}

size_t prU64FormatRadix(uint64_t value, char* out, uint32_t radix) {
    PR_ASSERT(radix >= 2 && radix <= kPrU64MaxRadix);
    if (radix == 10)
        return prU64FormatDecimal(value, out);
    if (radix == 16)
        return formatPowerOfTwo(value, out, 4);
    if (radix == 2)
        return formatPowerOfTwo(value, out, 1);
    if (radix == 8)
        return formatPowerOfTwo(value, out, 3);

    return formatAnyRadix(value, out, radix);
}

bool prU64Parse(const char* data, size_t size, uint64_t* out, uint32_t radix) {
    uint64_t value = 0;
    if (size == 0 || prU64ParsePrefix(data, size, &value, radix) != size)
        return false;
    *out = value;
    return true;
}

size_t prU64ParsePrefix(const char* data, size_t size, uint64_t* out, uint32_t radix) {
    PR_ASSERT(radix >= 2 && radix <= kPrU64MaxRadix);
    if (radix == 10)
        return parseDigits<10>(data, size, out, radix);
    if (radix == 16)
        return parseDigits<16>(data, size, out, radix);
    return parseDigits<0>(data, size, out, radix);
}
