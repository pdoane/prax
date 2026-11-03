#include "pr/float_convert.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/bit_cast.h"
#include "pr/count_of.h"
#include "pr/integer.h"

#include "core/src/float_convert_tables.h"

namespace pr_impl {
namespace {

constexpr uint64_t kU64Max = 0xffffffffffffffffull;

// Returns the index of the highest set bit. `value` must be nonzero.
uint32_t log2Floor(uint64_t value) {
    PR_ASSERT(value != 0);
    uint32_t index = 0;
    for (uint32_t shift = 32; shift != 0; shift >>= 1) {
        if ((value >> shift) != 0) {
            value >>= shift;
            index += shift;
        }
    }
    return index;
}

uint64_t shiftRight128(uint64_t low, uint64_t high, uint32_t n) {
    PR_ASSERT(n > 0 && n < 64);
    return high << (64 - n) | low >> n;
}

// Returns the bit length of 5^e, which is exact for e in [0, 3528].
int32_t pow5Bits(int32_t e) {
    PR_ASSERT(e >= 0 && e <= 3528);
    return int32_t((uint32_t(e) * 1217359u) >> 19) + 1;
}

int32_t log10Pow2(int32_t e) {
    PR_ASSERT(e >= 0 && e <= 1650);
    return int32_t((uint32_t(e) * 78913u) >> 18);
}

int32_t log10Pow5(int32_t e) {
    PR_ASSERT(e >= 0 && e <= 2620);
    return int32_t((uint32_t(e) * 732923u) >> 20);
}

uint32_t pow5Factor(uint64_t value) {
    uint32_t count = 0;
    while (value % 5 == 0) {
        value /= 5;
        ++count;
    }
    return count;
}

bool multipleOfPowerOf5(uint64_t value, uint32_t p) {
    return pow5Factor(value) >= p;
}

bool multipleOfPowerOf2(uint64_t value, uint32_t p) {
    PR_ASSERT(p < 64);
    return (value & ((1ull << p) - 1)) == 0;
}

// Returns (m * factor) >> shift. The product takes 192 bits, and the shift discards the low 64.
uint64_t mulShift(uint64_t m, const Pow5* factor, int32_t shift) {
    PR_ASSERT(shift > 64 && shift < 128);
    uint64_t loHigh = 0;
    prU64Mul128(m, factor->lo, &loHigh);
    uint64_t hiHigh = 0;
    uint64_t hiLow = prU64Mul128(m, factor->hi, &hiHigh);
    uint64_t sum = loHigh + hiLow;
    uint64_t carry = sum < loHigh ? 1 : 0;
    return shiftRight128(sum, hiHigh + carry, uint32_t(shift - 64));
}

// ---------------------------------------------------------------------------------------------------------------------
// Binary to decimal (Ryu)
//
// Computes the shortest decimal that identifies a binary float, using the Ryu tables. Their precision covers every
// binary format. `mantissaBits` and `bias` identify the format.

PrF64Decimal shortestDecimal(uint32_t mantissaBits, int32_t bias, uint64_t ieeeMantissa, uint32_t ieeeExponent) {
    int32_t e2;
    uint64_t m2;
    if (ieeeExponent == 0) {
        e2 = 1 - bias - int32_t(mantissaBits) - 2;
        m2 = ieeeMantissa;
    } else {
        e2 = int32_t(ieeeExponent) - bias - int32_t(mantissaBits) - 2;
        m2 = 1ull << mantissaBits | ieeeMantissa;
    }
    bool acceptBounds = (m2 & 1) == 0;

    // The half-ulp bounds, scaled by 4 to stay integral.
    uint64_t mv = 4 * m2;
    uint32_t mmShift = (ieeeMantissa != 0 || ieeeExponent <= 1) ? 1 : 0;
    uint64_t mp = 4 * m2 + 2;
    uint64_t mm = 4 * m2 - 1 - mmShift;

    uint64_t vr;
    uint64_t vp;
    uint64_t vm;
    int32_t e10;
    bool vmIsTrailingZeros = false;
    bool vrIsTrailingZeros = false;
    uint8_t lastRemovedDigit = 0;

    if (e2 >= 0) {
        uint32_t q = uint32_t(log10Pow2(e2));
        e10 = int32_t(q);
        int32_t k = kRyuPow5InvBitcount + pow5Bits(int32_t(q)) - 1;
        int32_t i = -e2 + int32_t(q) + k;
        PR_ASSERT(q < countOf(kRyuPow5InvSplit));
        vr = mulShift(mv, &kRyuPow5InvSplit[q], i);
        vp = mulShift(mp, &kRyuPow5InvSplit[q], i);
        vm = mulShift(mm, &kRyuPow5InvSplit[q], i);
        if (q != 0 && (vp - 1) / 10 <= vm / 10) {
            // The loop below removes a digit even if it does not run. This computes that digit, using the power one
            // lower.
            int32_t l = kRyuPow5InvBitcount + pow5Bits(int32_t(q - 1)) - 1;
            lastRemovedDigit = uint8_t(mulShift(mv, &kRyuPow5InvSplit[q - 1], -e2 + int32_t(q) - 1 + l) % 10);
        }
        // Above 24, 5^q exceeds mv and divides none of the three.
        if (q <= 24) {
            // mm, mv and mp span a range of 3, so at most one of them is a multiple of 5.
            if (mv % 5 == 0)
                vrIsTrailingZeros = multipleOfPowerOf5(mv, q);
            else if (acceptBounds)
                vmIsTrailingZeros = multipleOfPowerOf5(mm, q);
            else
                vp -= multipleOfPowerOf5(mp, q) ? 1 : 0;
        }
    } else {
        uint32_t q = uint32_t(log10Pow5(-e2));
        e10 = int32_t(q) + e2;
        int32_t i = -e2 - int32_t(q);
        int32_t k = pow5Bits(i) - kRyuPow5Bitcount;
        int32_t j = int32_t(q) - k;
        PR_ASSERT(size_t(i) < countOf(kRyuPow5Split));
        vr = mulShift(mv, &kRyuPow5Split[i], j);
        vp = mulShift(mp, &kRyuPow5Split[i], j);
        vm = mulShift(mm, &kRyuPow5Split[i], j);
        if (q != 0 && (vp - 1) / 10 <= vm / 10) {
            j = int32_t(q) - 1 - (pow5Bits(i + 1) - kRyuPow5Bitcount);
            PR_ASSERT(size_t(i + 1) < countOf(kRyuPow5Split));
            lastRemovedDigit = uint8_t(mulShift(mv, &kRyuPow5Split[i + 1], j) % 10);
        }
        if (q <= 1) {
            // mv is 4 * m2 and has two trailing zero bits. mm has one if mmShift is 1, and mp always has one.
            vrIsTrailingZeros = true;
            if (acceptBounds)
                vmIsTrailingZeros = mmShift == 1;
            else
                --vp;
        } else if (q < 63) {
            vrIsTrailingZeros = multipleOfPowerOf2(mv, q - 1);
        }
    }

    int32_t removed = 0;
    uint64_t output;
    if (vmIsTrailingZeros || vrIsTrailingZeros) {
        while (vp / 10 > vm / 10) {
            vmIsTrailingZeros &= vm % 10 == 0;
            vrIsTrailingZeros &= lastRemovedDigit == 0;
            lastRemovedDigit = uint8_t(vr % 10);
            vr /= 10;
            vp /= 10;
            vm /= 10;
            ++removed;
        }
        if (vmIsTrailingZeros) {
            while (vm % 10 == 0) {
                vrIsTrailingZeros &= lastRemovedDigit == 0;
                lastRemovedDigit = uint8_t(vr % 10);
                vr /= 10;
                vp /= 10;
                vm /= 10;
                ++removed;
            }
        }
        if (vrIsTrailingZeros && lastRemovedDigit == 5 && vr % 2 == 0)
            lastRemovedDigit = 4;   // exactly halfway, which rounds to even
        bool roundUp = (vr == vm && (!acceptBounds || !vmIsTrailingZeros)) || lastRemovedDigit >= 5;
        output = vr + (roundUp ? 1 : 0);
    } else {
        while (vp / 10 > vm / 10) {
            lastRemovedDigit = uint8_t(vr % 10);
            vr /= 10;
            vp /= 10;
            vm /= 10;
            ++removed;
        }
        output = vr + ((vr == vm || lastRemovedDigit >= 5) ? 1 : 0);
    }

    return {output, e10 + removed};
}

// ---------------------------------------------------------------------------------------------------------------------
// Decimal to binary (Eisel-Lemire)
//
// Multiplies the significand by a 128-bit power of five, and rounds the product. If the product cannot decide the
// rounding, the result is kApproximationFailed, and the exact path below decides it.

struct BinaryFormat {
    uint32_t mantissaBits;     // the explicit bits, without the implicit leading one
    int32_t minExponent;       // the exponent bias
    int32_t infinitePower;     // the biased exponent of infinity and NaN
    int32_t minExp10;          // the lowest decimal exponent of a nonzero value
    int32_t maxExp10;          // the highest decimal exponent of a finite value
    int32_t minRoundToEven;    // the decimal exponents whose 5^q product is exact, and so can
    int32_t maxRoundToEven;    // land exactly halfway between two floats
    int32_t minDecimalPoint;   // the positions of the decimal point outside which the value
    int32_t maxDecimalPoint;   // is zero or infinity regardless of its digits
};

constexpr BinaryFormat kF64Format = {52, -1023, 0x7ff, -342, 308, -4, 23, -342, 310};

// The exponent of the least significant bit of the smallest subnormal.
int32_t minLowBit(const BinaryFormat* format) {
    return format->minExponent - int32_t(format->mantissaBits) + 1;
}

// A mantissa and a biased exponent, ready to be packed into the format. A negative `power2` is kApproximationFailed.
struct AdjustedMantissa {
    uint64_t mantissa;
    int32_t power2;
};

constexpr int32_t kApproximationFailed = -1;

// Returns floor(log2(10^q)) + 63, across the table's exponent range.
int32_t pow10BinaryExponent(int32_t q) {
    return ((152170 + 65536) * q >> 16) + 63;
}

AdjustedMantissa eiselLemire(const BinaryFormat* format, int32_t q, uint64_t w) {
    if (w == 0 || q < format->minExp10)
        return {0, 0};
    if (q > format->maxExp10)
        return {0, format->infinitePower};

    uint32_t lz = 63 - log2Floor(w);
    w <<= lz;

    uint32_t precision = format->mantissaBits + 3;
    uint64_t precisionMask = kU64Max >> precision;
    const Pow5* factor = &kEiselLemirePow5[q - kEiselLemireMinExp10];

    uint64_t high = 0;
    uint64_t low = prU64Mul128(w, factor->hi, &high);
    if ((high & precisionMask) == precisionMask) {
        // The truncated factor left the result ambiguous. The factor's low half resolves it.
        uint64_t secondHigh = 0;
        prU64Mul128(w, factor->lo, &secondHigh);
        low += secondHigh;
        if (secondHigh > low)
            ++high;
    }
    if (low == kU64Max && (q < -27 || q > 55)) {
        // 5^q is inexact here, so the carry into the rounding bits is unknown.
        return {0, kApproximationFailed};
    }

    uint32_t upperBit = uint32_t(high >> 63);
    uint64_t mantissa = high >> (upperBit + 64 - format->mantissaBits - 3);
    int32_t power2 = pow10BinaryExponent(q) + int32_t(upperBit) - int32_t(lz) - format->minExponent;

    if (power2 <= 0) {
        if (-power2 + 1 >= 64)
            return {0, 0};
        mantissa >>= -power2 + 1;
        mantissa += mantissa & 1;
        mantissa >>= 1;
        // Rounding up turns the largest subnormal into the smallest normal.
        power2 = mantissa < 1ull << format->mantissaBits ? 0 : 1;
        return {mantissa, power2};
    }

    if (low <= 1 && q >= format->minRoundToEven && q <= format->maxRoundToEven && (mantissa & 3) == 1) {
        // Exactly halfway between two floats, which rounds to even.
        if (mantissa << (upperBit + 64 - format->mantissaBits - 3) == high)
            mantissa &= ~1ull;
    }

    mantissa += mantissa & 1;
    mantissa >>= 1;
    if (mantissa >= 2ull << format->mantissaBits) {
        mantissa = 1ull << format->mantissaBits;
        ++power2;
    }
    mantissa &= ~(1ull << format->mantissaBits);
    if (power2 >= format->infinitePower)
        return {0, format->infinitePower};
    return {mantissa, power2};
}

// ---------------------------------------------------------------------------------------------------------------------
// Decimal to binary, exactly
//
// The whole significand times 5^|exponent|, divided out with big integers.

// The number of significant digits beyond which the value can no longer move to another float, except to break an exact
// tie. Digits beyond it fold into a sticky bit.
constexpr size_t kMaxExactDigits = 768;

// A little-endian big integer. The limbs at `count` and above are indeterminate.
template <size_t Limbs>
struct BigUint {
    uint64_t limbs[Limbs];
    size_t count = 0;
};

constexpr size_t kParseLimbs = 96;

template <size_t N>
bool isZero(const BigUint<N>* v) {
    return v->count == 0;
}

template <size_t N>
uint32_t bitLength(const BigUint<N>* v) {
    if (v->count == 0)
        return 0;
    return uint32_t((v->count - 1) * 64) + log2Floor(v->limbs[v->count - 1]) + 1;
}

template <size_t N>
void trim(BigUint<N>* v) {
    while (v->count != 0 && v->limbs[v->count - 1] == 0)
        --v->count;
}

// Computes v = v * multiplier + addend.
template <size_t N>
void mulAdd(BigUint<N>* v, uint64_t multiplier, uint64_t addend) {
    uint64_t carry = addend;
    for (size_t i = 0; i < v->count; ++i) {
        uint64_t high = 0;
        uint64_t low = prU64Mul128(v->limbs[i], multiplier, &high);
        v->limbs[i] = low + carry;
        carry = high + (v->limbs[i] < low ? 1 : 0);
    }
    if (carry != 0) {
        PR_ASSERT(v->count < N);
        v->limbs[v->count++] = carry;
    }
}

template <size_t N>
void setU64(BigUint<N>* v, uint64_t value) {
    v->count = 0;
    if (value != 0) {
        v->limbs[0] = value;
        v->count = 1;
    }
}

template <size_t N>
void mulPow5(BigUint<N>* v, uint32_t exponent) {
    constexpr uint32_t kChunk = 27;                          // 5^27 is the largest power below 2^64
    constexpr uint64_t kPow5Chunk = 7450580596923828125ull;  // 5^27
    while (exponent >= kChunk) {
        mulAdd(v, kPow5Chunk, 0);
        exponent -= kChunk;
    }
    if (exponent != 0) {
        uint64_t factor = 1;
        for (uint32_t i = 0; i < exponent; ++i)
            factor *= 5;
        mulAdd(v, factor, 0);
    }
}

template <size_t N>
void shiftLeft(BigUint<N>* v, uint32_t shift) {
    if (v->count == 0 || shift == 0)
        return;
    size_t limbShift = shift / 64;
    uint32_t bitShift = shift % 64;

    size_t newCount = v->count + limbShift + (bitShift != 0 ? 1 : 0);
    PR_ASSERT(newCount <= N);

    if (bitShift == 0) {
        for (size_t i = v->count; i-- > 0;)
            v->limbs[i + limbShift] = v->limbs[i];
    } else {
        for (size_t i = v->count + 1; i-- > 0;) {
            uint64_t high = i < v->count ? v->limbs[i] << bitShift : 0;
            uint64_t low = i > 0 ? v->limbs[i - 1] >> (64 - bitShift) : 0;
            v->limbs[i + limbShift] = high | low;
        }
    }
    for (size_t i = 0; i < limbShift; ++i)
        v->limbs[i] = 0;
    v->count = newCount;
    trim(v);
}

template <size_t N>
void shiftRight1(BigUint<N>* v) {
    for (size_t i = 0; i < v->count; ++i) {
        uint64_t low = i + 1 < v->count ? v->limbs[i + 1] << 63 : 0;
        v->limbs[i] = v->limbs[i] >> 1 | low;
    }
    trim(v);
}

// Returns a negative value if a < b, zero if they are equal, and a positive value if a > b.
template <size_t N>
int32_t compare(const BigUint<N>* a, const BigUint<N>* b) {
    if (a->count != b->count)
        return a->count < b->count ? -1 : 1;
    for (size_t i = a->count; i-- > 0;) {
        if (a->limbs[i] != b->limbs[i])
            return a->limbs[i] < b->limbs[i] ? -1 : 1;
    }
    return 0;
}

// Computes a -= b. `a` must be at least `b`.
template <size_t N>
void subtract(BigUint<N>* a, const BigUint<N>* b) {
    uint64_t underflow = 0;
    for (size_t i = 0; i < a->count; ++i) {
        uint64_t rhs = i < b->count ? b->limbs[i] : 0;
        uint64_t diff = a->limbs[i] - rhs - underflow;
        underflow = a->limbs[i] < rhs + underflow || (rhs == kU64Max && underflow == 1) ? 1 : 0;
        a->limbs[i] = diff;
    }
    PR_ASSERT(underflow == 0);
    trim(a);
}

// Returns floor(n / d), which fits in 64 bits, and leaves the remainder in `n`.
template <size_t N>
uint64_t divide(BigUint<N>* n, const BigUint<N>* d) {
    PR_ASSERT(!isZero(d));
    uint32_t nBits = bitLength(n);
    uint32_t dBits = bitLength(d);
    if (nBits < dBits)
        return 0;
    uint32_t shift = nBits - dBits;
    PR_ASSERT(shift < 64);

    BigUint<N> scaled = *d;
    shiftLeft(&scaled, shift);

    uint64_t quotient = 0;
    for (uint32_t i = shift + 1; i-- > 0;) {
        if (compare(n, &scaled) >= 0) {
            subtract(n, &scaled);
            quotient |= 1ull << i;
        }
        shiftRight1(&scaled);
    }
    return quotient;
}

// The significand as an exact integer, and the decimal exponent that scales it.
struct ExactDecimal {
    BigUint<kParseLimbs> significand;
    size_t digits = 0;
    int32_t exponent10 = 0;
    bool sticky = false;       // a nonzero digit was dropped past kMaxExactDigits
};

// Up to 19 decimal digits, which fit in a uint64_t, collected before they are added to a significand.
struct DigitChunk {
    uint64_t value;
    uint32_t digits;
};

// Adds the chunk's digits to the end of `significand`, and empties the chunk.
void flushDigits(BigUint<kParseLimbs>* significand, DigitChunk* chunk) {
    if (chunk->digits == 0)
        return;
    uint64_t scale = 1;
    for (uint32_t i = 0; i < chunk->digits; ++i)
        scale *= 10;
    mulAdd(significand, scale, chunk->value);
    *chunk = DigitChunk{0, 0};
}

// Reads `input` again for the exact path, whose significand is the whole input.
ExactDecimal scanExact(PrStringView input) {
    ExactDecimal out;

    size_t pos = 0;
    if (pos < input.size && (input.data[pos] == '-' || input.data[pos] == '+'))
        ++pos;

    bool seenPoint = false;
    DigitChunk chunk = {0, 0};
    while (pos < input.size) {
        char c = input.data[pos];
        if (c == '.' && !seenPoint) {
            seenPoint = true;
            ++pos;
            continue;
        }
        if (!prAsciiIsDigit(c))
            break;
        if (out.digits == 0 && c == '0') {
            // A leading zero is not significant, and only moves the point.
            if (seenPoint)
                --out.exponent10;
            ++pos;
            continue;
        }
        if (out.digits < kMaxExactDigits) {
            chunk.value = chunk.value * 10 + uint64_t(c - '0');
            if (++chunk.digits == 19)
                flushDigits(&out.significand, &chunk);
            ++out.digits;
            if (seenPoint)
                --out.exponent10;
        } else {
            out.sticky |= c != '0';
            if (!seenPoint)
                ++out.exponent10;
        }
        ++pos;
    }
    flushDigits(&out.significand, &chunk);

    if (pos < input.size && (input.data[pos] == 'e' || input.data[pos] == 'E')) {
        size_t expPos = pos + 1;
        bool expNegative = false;
        if (expPos < input.size && (input.data[expPos] == '-' || input.data[expPos] == '+')) {
            expNegative = input.data[expPos] == '-';
            ++expPos;
        }
        if (expPos < input.size && prAsciiIsDigit(input.data[expPos])) {
            int32_t exponent = 0;
            while (expPos < input.size && prAsciiIsDigit(input.data[expPos])) {
                if (exponent < 100000)
                    exponent = exponent * 10 + (input.data[expPos] - '0');
                ++expPos;
            }
            out.exponent10 += expNegative ? -exponent : exponent;
        }
    }
    return out;
}

// The function is kept out of line, because its big integers take kilobytes of stack.
PR_NOINLINE AdjustedMantissa exactDecimalToBinary(const BinaryFormat* format, PrStringView input) {
    ExactDecimal decimal = scanExact(input);
    if (isZero(&decimal.significand))
        return {0, 0};

    int32_t decimalPoint = decimal.exponent10 + int32_t(decimal.digits);
    if (decimalPoint < format->minDecimalPoint)
        return {0, 0};
    if (decimalPoint > format->maxDecimalPoint)
        return {0, format->infinitePower};

    // The exact value is num / den * 2^scale, give or take the sticky tail.
    BigUint<kParseLimbs> num = decimal.significand;
    BigUint<kParseLimbs> den;
    setU64(&den, 1);
    int32_t scale = decimal.exponent10;
    if (scale >= 0)
        mulPow5(&num, uint32_t(scale));
    else
        mulPow5(&den, uint32_t(-scale));

    // lowBit is the exponent of the mantissa's least significant bit. The operands' widths estimate it to within one
    // bit either way, and the correction never overshoots.
    int32_t floorBit = minLowBit(format);
    int32_t lowBit = int32_t(bitLength(&num)) - int32_t(bitLength(&den)) + scale - int32_t(format->mantissaBits);

    uint64_t mantissa = 0;
    BigUint<kParseLimbs> remainder;
    BigUint<kParseLimbs> divisor;
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        if (lowBit < floorBit)
            lowBit = floorBit;

        remainder = num;
        divisor = den;
        int32_t shift = scale - lowBit;
        if (shift > 0)
            shiftLeft(&remainder, uint32_t(shift));
        else if (shift < 0)
            shiftLeft(&divisor, uint32_t(-shift));

        mantissa = divide(&remainder, &divisor);
        if (mantissa >= 2ull << format->mantissaBits) {
            ++lowBit;   // it overflowed into the next binade
            continue;
        }
        if (mantissa < 1ull << format->mantissaBits && lowBit > floorBit) {
            --lowBit;   // there is room to normalize
            continue;
        }
        break;
    }
    PR_ASSERT(mantissa < 2ull << format->mantissaBits);

    // Round half to even. An exact halfway split rounds up if the dropped digits make the value larger.
    shiftLeft(&remainder, 1);
    int32_t order = compare(&remainder, &divisor);
    bool roundUp = order > 0 || (order == 0 && (decimal.sticky || (mantissa & 1) != 0));
    if (roundUp) {
        ++mantissa;
        if (mantissa >= 2ull << format->mantissaBits) {
            mantissa >>= 1;   // into the next binade, with a zero low bit
            ++lowBit;
        }
    }

    if (mantissa < 1ull << format->mantissaBits) {
        PR_ASSERT(lowBit == floorBit);   // a subnormal, with no implicit leading bit
        return {mantissa, 0};
    }
    int32_t power2 = lowBit + int32_t(format->mantissaBits) - format->minExponent;
    if (power2 >= format->infinitePower)
        return {0, format->infinitePower};
    return {mantissa & ~(1ull << format->mantissaBits), power2};
}

// ---------------------------------------------------------------------------------------------------------------------
// Reading the text

struct DecimalScan {
    uint64_t mantissa = 0;
    int32_t exponent10 = 0;
    bool negative = false;
    bool truncated = false;    // a nonzero digit was dropped beyond what a uint64_t holds
};

// The number of digits that a uint64_t holds without overflowing.
constexpr size_t kU64Digits = 19;

// Scans the number at the start of `input`. Returns false if it has no digits.
bool scanDecimal(PrStringView input, DecimalScan* out) {
    const char* begin = input.data;
    const char* end = begin + input.size;
    const char* p = begin;

    if (p == end)
        return false;
    if (*p == '-') {
        out->negative = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }
    if (p == end || (!prAsciiIsDigit(*p) && *p != '.'))
        return false;

    // The digits accumulate unchecked. If there are more than kU64Digits, the sum may have wrapped, and the significand
    // is read again below.
    const char* intStart = p;
    uint64_t mantissa = 0;
    while (p != end && prAsciiIsDigit(*p)) {
        mantissa = mantissa * 10 + uint64_t(*p - '0');
        ++p;
    }
    size_t intDigits = size_t(p - intStart);

    size_t fracDigits = 0;
    if (p != end && *p == '.') {
        ++p;
        const char* fracStart = p;
        while (p != end && prAsciiIsDigit(*p)) {
            mantissa = mantissa * 10 + uint64_t(*p - '0');
            ++p;
        }
        fracDigits = size_t(p - fracStart);
    }
    const char* digitEnd = p;

    if (intDigits + fracDigits == 0)
        return false;
    int32_t exponent10 = -int32_t(fracDigits);

    if (intDigits + fracDigits > kU64Digits) {
        // Keep the first kU64Digits significant digits, and a sticky bit for the rest. The rest move the value by less
        // than one unit in the last kept digit.
        mantissa = 0;
        size_t index = 0;      // the position among the digits, not counting the point
        size_t captured = 0;   // one past the last digit kept
        size_t taken = 0;
        bool started = false;
        for (const char* q = intStart; q != digitEnd; ++q) {
            char c = *q;
            if (c == '.')
                continue;
            if (!started && c == '0') {
                ++index;
                continue;
            }
            started = true;
            if (taken < kU64Digits) {
                mantissa = mantissa * 10 + uint64_t(c - '0');
                ++taken;
                captured = index + 1;
            } else {
                out->truncated |= c != '0';
            }
            ++index;
        }
        // The digit at position j has weight 10^(intDigits - 1 - j).
        exponent10 = int32_t(intDigits) - int32_t(captured);
    }

    if (p != end && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        bool expNegative = false;
        if (q != end && (*q == '-' || *q == '+')) {
            expNegative = *q == '-';
            ++q;
        }
        // An exponent marker with no digits after it is not part of the number.
        if (q != end && prAsciiIsDigit(*q)) {
            int32_t exponent = 0;
            while (q != end && prAsciiIsDigit(*q)) {
                if (exponent < 100000)
                    exponent = exponent * 10 + (*q - '0');
                ++q;
            }
            exponent10 += expNegative ? -exponent : exponent;
        }
    }

    out->mantissa = mantissa;
    out->exponent10 = exponent10;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Exact small values
//
// A significand below 2^53 and a power of ten up to 10^22 are both representable, so one multiply or divide rounds
// correctly.

constexpr double kExactPow10F64[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

bool exactF64(const DecimalScan* scan, double* out) {
    int32_t exponent = scan->exponent10;
    if (scan->truncated || scan->mantissa >> 53 != 0 || uint32_t(exponent + 22) > 44)
        return false;
    double value = double(scan->mantissa);
    if (exponent < 0)
        value /= kExactPow10F64[-exponent];
    else if (exponent > 0)
        value *= kExactPow10F64[exponent];
    *out = scan->negative ? -value : value;
    return true;
}

// Converts every value that the exact small-value path does not handle.
PR_NOINLINE AdjustedMantissa decimalToBinary(const BinaryFormat* format, const DecimalScan* scan, PrStringView input) {
    AdjustedMantissa adjusted = eiselLemire(format, scan->exponent10, scan->mantissa);
    if (scan->truncated && adjusted.power2 != kApproximationFailed) {
        // The dropped digits increase the value. The approximation holds if adding one to the significand gives the
        // same float.
        AdjustedMantissa upper = eiselLemire(format, scan->exponent10, scan->mantissa + 1);
        if (adjusted.mantissa != upper.mantissa || adjusted.power2 != upper.power2)
            adjusted.power2 = kApproximationFailed;
    }
    if (adjusted.power2 == kApproximationFailed)
        adjusted = exactDecimalToBinary(format, input);
    return adjusted;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrF64Decimal prF64ShortestDecimal(double value) {
    uint64_t bits = bitCast<uint64_t>(value);
    uint64_t mantissa = bits & 0xfffffffffffffull;
    uint32_t exponent = uint32_t((bits >> 52) & 0x7ffull);
    return shortestDecimal(52, 1023, mantissa, exponent);
}

bool prF64ParsePrefix(PrStringView text, double* out) {
    DecimalScan scan;
    if (!scanDecimal(text, &scan))
        return false;
    if (!exactF64(&scan, out)) {
        AdjustedMantissa adjusted = decimalToBinary(&kF64Format, &scan, text);
        uint64_t sign = scan.negative ? 0x8000000000000000ull : 0;
        *out = bitCast<double>(sign | (uint64_t(adjusted.power2) << 52) | adjusted.mantissa);
    }
    return true;
}
