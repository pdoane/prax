#include "pr/float_convert.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/bit_cast.h"
#include "pr/integer.h"

#include "core/src/float_convert_tables.h"

namespace pr_impl {
namespace {

constexpr uint64_t kU64Max = 0xffffffffffffffffull;
constexpr uint32_t kU32Max = 0xffffffffu;

// Returns the index of the highest set bit. `value` must be nonzero.
uint32_t log2Floor(uint64_t value) {
    PR_ASSERT(value != 0);
    return prU64HighestBit(value);
}

// floor(e * log10(2)), floor(e * log2(10)) and floor(e * log10(2) - log10(4/3)), in fixed point, exact over the
// exponents asserted. The shifts of negative products round toward negative infinity.
int32_t floorLog10Pow2(int32_t e) {
    PR_ASSERT(e >= -2620 && e <= 2620);
    return (e * 315653) >> 20;
}

int32_t floorLog2Pow10(int32_t e) {
    PR_ASSERT(e >= -1233 && e <= 1233);
    return (e * 1741647) >> 19;
}

int32_t floorLog10Pow2MinusLog10FourThirds(int32_t e) {
    PR_ASSERT(e >= -2985 && e <= 2936);
    return (e * 631305 - 261663) >> 21;
}

// The table's entry for 5^q, which is exact for q in [0, 55] and truncated elsewhere.
const Pow5& pow5(int32_t q) {
    PR_ASSERT(q >= kPow5MinExp10 && q <= kPow5MaxExp10);
    return kPow5[q - kPow5MinExp10];
}

bool pow5Exact(int32_t q) {
    return q >= 0 && q <= 55;
}

uint32_t rotateRight(uint32_t value, uint32_t n) {
    return value >> n | value << (32 - n);
}

uint64_t rotateRight(uint64_t value, uint32_t n) {
    return value >> n | value << (64 - n);
}

// Remove the trailing zeros of `n`, which must not be zero, and return how many they removed. A multiplication by the
// inverse of 25 is the quotient exactly when n is a multiple of 25, and the rotation leaves it at most max / 100 only
// when n is also a multiple of four.
int32_t removeTrailingZeros(uint32_t* n) {
    PR_ASSERT(*n != 0);
    constexpr uint32_t kInverse5 = 0xcccccccdu;
    constexpr uint32_t kInverse25 = kInverse5 * kInverse5;
    int32_t removed = 0;
    for (;;) {
        uint32_t q = rotateRight(*n * kInverse25, 2);
        if (q > kU32Max / 100)
            break;
        *n = q;
        removed += 2;
    }
    uint32_t q = rotateRight(*n * kInverse5, 1);
    if (q <= kU32Max / 10) {
        *n = q;
        removed |= 1;
    }
    return removed;
}

// `n` must be below 10^17. A multiple of 10^8 continues in 32 bits.
int32_t removeTrailingZeros(uint64_t* n) {
    PR_ASSERT(*n != 0);
    constexpr uint64_t kMagic = 12379400392853802749ull;   // ceil(2^90 / 10^8)
    uint64_t high = 0;
    uint64_t low = prU64Mul128(*n, kMagic, &high);
    if ((high & ((1ull << (90 - 64)) - 1)) == 0 && low < kMagic) {
        uint32_t quotient = uint32_t(high >> (90 - 64));
        int32_t removed = 8 + removeTrailingZeros(&quotient);
        *n = quotient;
        return removed;
    }
    constexpr uint64_t kInverse5 = 0xcccccccccccccccdull;
    constexpr uint64_t kInverse25 = kInverse5 * kInverse5;
    int32_t removed = 0;
    for (;;) {
        uint64_t q = rotateRight(*n * kInverse25, 2);
        if (q > kU64Max / 100)
            break;
        *n = q;
        removed += 2;
    }
    uint64_t q = rotateRight(*n * kInverse5, 1);
    if (q <= kU64Max / 10) {
        *n = q;
        removed |= 1;
    }
    return removed;
}

// ---------------------------------------------------------------------------------------------------------------------
// Binary to decimal, shortest (Dragonbox)
//
// Junekey Jeon, "Dragonbox: A New Floating-Point Binary-to-Decimal Conversion Algorithm", 2020. The value's rounding
// interval is scaled by a power of ten, chosen so that the scaled interval holds a multiple of 10^(kappa + 1) whenever
// a decimal of the shortest length lies in it. One multiplication by the power of ten, rounded up, finds that multiple,
// or rules it out and finds the decimal one digit longer, and the trailing zeros of the result are removed in steps of
// eight and two digits. Ties in either direction round to even: a decimal parses to the even neighbor, so an even
// value's interval includes its endpoints.

struct Mul {
    uint64_t integer;
    bool isInteger;
};

struct MulParity {
    bool parity;
    bool isInteger;
};

struct U128 {
    uint64_t hi;
    uint64_t lo;
};

// The binary formats, each with the arithmetic of its width. A cache entry is 10^k with its top bit set, rounded up to
// the width of the format's multiplications. beta is the shift that the entry's implied exponent leaves.
struct Binary64 {
    using Carrier = uint64_t;
    using Cache = U128;
    static constexpr int32_t kSignificandBits = 52;
    static constexpr int32_t kExponentBias = -1023;
    static constexpr int32_t kMinExponent = -1022;
    static constexpr int32_t kKappa = 2;

    // The binary exponent at which the shorter interval's rounded-up center can be a tie.
    static constexpr int32_t kShorterIntervalTie = -77;

    static Cache cache(int32_t k) {
        const Pow5& entry = pow5(k);
        if (pow5Exact(k))
            return {entry.hi, entry.lo};
        uint64_t lo = entry.lo + 1;
        return {entry.hi + (lo == 0 ? 1 : 0), lo};
    }

    // The upper 128 bits of the 192-bit product u * cache: its integer part, and whether the rest is zero.
    static Mul mul(Carrier u, const Cache& cache) {
        uint64_t high = 0;
        uint64_t low = prU64Mul128(u, cache.hi, &high);
        uint64_t carry = 0;
        prU64Mul128(u, cache.lo, &carry);
        uint64_t sum = low + carry;
        high += sum < low ? 1 : 0;
        return {high, sum == 0};
    }

    static uint32_t delta(const Cache& cache, int32_t beta) {
        return uint32_t(cache.hi >> (63 - beta));
    }

    // The parity of the integer part of the lower 128 bits of twoF * cache, shifted by beta, and whether its fraction
    // is zero.
    static MulParity mulParity(Carrier twoF, const Cache& cache, int32_t beta) {
        PR_ASSERT(beta >= 1 && beta < 64);
        uint64_t carry = 0;
        uint64_t low = prU64Mul128(twoF, cache.lo, &carry);
        uint64_t high = twoF * cache.hi + carry;
        return {((high >> (64 - beta)) & 1) != 0, ((high << beta) | (low >> (64 - beta))) == 0};
    }

    static Carrier leftEndpoint(const Cache& cache, int32_t beta) {
        return (cache.hi - (cache.hi >> (kSignificandBits + 2))) >> (64 - kSignificandBits - 1 - beta);
    }

    static Carrier rightEndpoint(const Cache& cache, int32_t beta) {
        return (cache.hi + (cache.hi >> (kSignificandBits + 1))) >> (64 - kSignificandBits - 1 - beta);
    }

    static Carrier roundUp(const Cache& cache, int32_t beta) {
        return ((cache.hi >> (64 - kSignificandBits - 2 - beta)) + 1) / 2;
    }
};

struct Binary32 {
    using Carrier = uint32_t;
    using Cache = uint64_t;
    static constexpr int32_t kSignificandBits = 23;
    static constexpr int32_t kExponentBias = -127;
    static constexpr int32_t kMinExponent = -126;
    static constexpr int32_t kKappa = 1;
    static constexpr int32_t kShorterIntervalTie = -35;

    static Cache cache(int32_t k) {
        // The top 64 bits of 5^k are exact for k in [0, 27], whose power fits in them.
        return pow5(k).hi + (k >= 0 && k <= 27 ? 0 : 1);
    }

    // The upper 64 bits of the 96-bit product u * cache: its integer part, and whether the rest is zero.
    static Mul mul(Carrier u, Cache cache) {
        uint64_t high = 0;
        uint64_t low = prU64Mul128(u, cache, &high);
        uint64_t upper = high << 32 | low >> 32;
        return {upper >> 32, uint32_t(upper) == 0};
    }

    static uint32_t delta(Cache cache, int32_t beta) {
        return uint32_t(cache >> (63 - beta));
    }

    static MulParity mulParity(Carrier twoF, Cache cache, int32_t beta) {
        PR_ASSERT(beta >= 1 && beta < 32);
        uint64_t low = uint64_t(twoF) * cache;
        return {((low >> (64 - beta)) & 1) != 0, uint32_t(low >> (32 - beta)) == 0};
    }

    static Carrier leftEndpoint(Cache cache, int32_t beta) {
        return Carrier((cache - (cache >> (kSignificandBits + 2))) >> (64 - kSignificandBits - 1 - beta));
    }

    static Carrier rightEndpoint(Cache cache, int32_t beta) {
        return Carrier((cache + (cache >> (kSignificandBits + 1))) >> (64 - kSignificandBits - 1 - beta));
    }

    static Carrier roundUp(Cache cache, int32_t beta) {
        return Carrier(((cache >> (64 - kSignificandBits - 2 - beta)) + 1) / 2);
    }
};

// The value 2^e with no fraction bits, whose interval is shorter below than above.
template <typename Format>
PrF64Decimal shorterInterval(int32_t e) {
    using Carrier = typename Format::Carrier;
    int32_t minusK = floorLog10Pow2MinusLog10FourThirds(e);
    int32_t beta = e + floorLog2Pow10(-minusK);
    typename Format::Cache cache = Format::cache(-minusK);
    Carrier left = Format::leftEndpoint(cache, beta);
    Carrier right = Format::rightEndpoint(cache, beta);
    // The interval is closed, since the significand is even. The left endpoint is an integer only for these exponents.
    if (!(e >= 2 && e <= 3))
        ++left;

    Carrier digits = right / 10;
    if (digits * 10 >= left) {
        int32_t exponent = minusK + 1 + removeTrailingZeros(&digits);
        return {digits, exponent};
    }
    // No shorter decimal lies in the interval, so the result is its center rounded up.
    digits = Format::roundUp(cache, beta);
    if ((digits & 1) != 0 && e == Format::kShorterIntervalTie)
        --digits;
    else if (digits < left)
        ++digits;
    return {digits, minusK};
}

template <typename Format>
PrF64Decimal shortestDecimal(typename Format::Carrier significand, uint32_t biasedExponent) {
    using Carrier = typename Format::Carrier;
    constexpr uint32_t kSmallDivisor = Format::kKappa == 1 ? 10 : 100;
    constexpr uint32_t kBigDivisor = kSmallDivisor * 10;

    int32_t e = 0;
    if (biasedExponent != 0) {
        e = int32_t(biasedExponent) + Format::kExponentBias - Format::kSignificandBits;
        if (significand == 0)
            return shorterInterval<Format>(e);
        significand |= Carrier(1) << Format::kSignificandBits;
    } else {
        e = Format::kMinExponent - Format::kSignificandBits;
    }
    bool closed = (significand & 1) == 0;
    Carrier twoFc = significand << 1;

    // The power of ten that scales the interval, and the scaled half-width (delta) and right endpoint (z).
    int32_t minusK = floorLog10Pow2(e) - Format::kKappa;
    typename Format::Cache cache = Format::cache(-minusK);
    int32_t beta = e + floorLog2Pow10(-minusK);
    uint32_t delta = Format::delta(cache, beta);
    Mul z = Format::mul(Carrier((twoFc | 1) << beta), cache);

    // A multiple of the big divisor within delta of z is in the interval, unless it is an excluded endpoint.
    Carrier digits = Carrier(z.integer / kBigDivisor);
    uint32_t r = uint32_t(z.integer - uint64_t(kBigDivisor) * digits);
    bool longer = false;
    if (r < delta) {
        if (r == 0 && z.isInteger && !closed) {
            --digits;
            r = kBigDivisor;
            longer = true;
        }
    } else if (r > delta) {
        longer = true;
    } else {
        // r is delta, so the candidate is the left endpoint, which the fraction of its product places.
        MulParity x = Format::mulParity(twoFc - 1, cache, beta);
        if (!(x.parity || (x.isInteger && closed)))
            longer = true;
    }
    if (!longer) {
        int32_t exponent = minusK + Format::kKappa + 1 + removeTrailingZeros(&digits);
        return {digits, exponent};
    }

    // One digit more: the value itself, rounded to the small divisor's place. A result here has no trailing zero, or
    // the shorter decimal would have been found.
    digits *= 10;
    uint32_t distance = r - (delta / 2) + (kSmallDivisor / 2);
    bool approximateParity = ((distance ^ (kSmallDivisor / 2)) & 1) != 0;
    bool divisible = distance % kSmallDivisor == 0;
    digits += Carrier(distance / kSmallDivisor);
    if (divisible) {
        // The approximation is the result or one above it, which their parities tell apart. An exact tie rounds to
        // even.
        MulParity y = Format::mulParity(twoFc, cache, beta);
        if (y.parity != approximateParity)
            --digits;
        else if ((digits & 1) != 0 && y.isInteger)
            --digits;
    }
    return {digits, minusK + Format::kKappa};
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
    const Pow5* factor = &pow5(q);

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
    size_t length = 0;         // the bytes of the number, its sign and exponent included
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
            p = q;
        }
    }

    out->length = size_t(p - begin);
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

// ---------------------------------------------------------------------------------------------------------------------
// Binary to decimal, rounded to a place
//
// Steele and White's digit generation. The value is held as an exact ratio num/den, and digits are produced one at a
// time, so the rounding reads the true remainder and needs no error analysis. The operands stay within one limb for
// ordinary magnitudes, and grow only toward the ends of the exponent range.

constexpr size_t kDigitLimbs = 24;
using DigitUint = BigUint<kDigitLimbs>;

// Writes the m and e for which |value| = m * 2^e exactly. `value` must be finite and not zero.
void decompose(double value, uint64_t* m, int32_t* e) {
    uint64_t bits = bitCast<uint64_t>(value) & ~(1ull << 63);
    uint64_t mantissa = bits & 0xfffffffffffffull;
    uint32_t exponent = uint32_t(bits >> 52);
    PR_ASSERT(bits != 0 && exponent != 0x7ff);
    if (exponent == 0) {
        *m = mantissa;
        *e = -1074;
    } else {
        *m = mantissa | 1ull << 52;
        *e = int32_t(exponent) - 1075;
    }
}

// Sets num/den into [1, 10), and returns the exponent E for which |value| = 0.d1d2... * 10^E.
int32_t scaleToLeadingDigit(DigitUint* num, DigitUint* den, uint64_t m, int32_t e) {
    // The target is m * 2^e * 10^k with k = 1 - E. The ten is split into its two factors, which keeps the pair in
    // lowest terms: num = m * 5^k * 2^(e + k) and den = 5^-k * 2^-(e + k), each power taken where its exponent is
    // positive. Routing the power of two through 10^k would leave a common factor of two and double the operands'
    // width. E is estimated from log10(2) in fixed point, which lands within one, and corrected below.
    int32_t magnitude = int32_t(log2Floor(m)) + e;
    int32_t exponent = (magnitude * 78913 >> 18) + 1;
    int32_t k = 1 - exponent;

    setU64(num, m);
    setU64(den, 1);
    if (k >= 0)
        mulPow5(num, uint32_t(k));
    else
        mulPow5(den, uint32_t(-k));
    int32_t shift = e + k;
    if (shift >= 0)
        shiftLeft(num, uint32_t(shift));
    else
        shiftLeft(den, uint32_t(-shift));

    for (;;) {
        if (compare(num, den) < 0) {
            mulAdd(num, 10, 0);
            --exponent;
            continue;
        }
        DigitUint tenDen = *den;
        mulAdd(&tenDen, 10, 0);
        if (compare(num, &tenDen) >= 0) {
            *den = tenDen;
            ++exponent;
            continue;
        }
        return exponent;
    }
}

// Writes `wanted` digits, then rounds the remainder half to even and carries. A carry out of the leading digit raises
// the exponent.
PrF64Digits generateDigits(DigitUint* num, DigitUint* den, int32_t exponent, uint32_t wanted, char* out) {
    // Operands that fit in one limb give their digits by 64-bit division. The headroom keeps num * 10 from overflowing.
    constexpr uint64_t kSingleLimbMax = 1ull << 60;
    if (wanted != 0 && den->count == 1 && den->limbs[0] < kSingleLimbMax && num->count <= 1) {
        uint64_t divisor = den->limbs[0];
        uint64_t value = num->count == 1 ? num->limbs[0] : 0;
        for (uint32_t i = 0; i < wanted; ++i) {
            uint64_t digit = value / divisor;
            PR_ASSERT(digit < 10);
            out[i] = char('0' + digit);
            value -= digit * divisor;
            if (i + 1 < wanted)
                value *= 10;
        }
        setU64(num, value);
    } else if (wanted != 0) {
        // The divisor is fixed across the run, so its multiples by 2, 4 and 8 are built once, and each digit takes four
        // comparisons instead of nine subtractions.
        DigitUint multiples[3];
        for (uint32_t k = 0; k < 3; ++k) {
            multiples[k] = *den;
            shiftLeft(&multiples[k], k + 1);
        }
        for (uint32_t i = 0; i < wanted; ++i) {
            uint32_t digit = 0;
            for (uint32_t k = 3; k-- > 0;) {
                if (compare(num, &multiples[k]) >= 0) {
                    subtract(num, &multiples[k]);
                    digit += 1u << (k + 1);
                }
            }
            if (compare(num, den) >= 0) {
                subtract(num, den);
                digit += 1;
            }
            PR_ASSERT(digit < 10);
            out[i] = char('0' + digit);
            if (i + 1 < wanted)
                mulAdd(num, 10, 0);
        }
    }

    bool roundUp = false;
    if (wanted == 0) {
        // No digit was written, so the value rounds up if it passes half of the first place that would have been.
        mulAdd(num, 2, 0);
        DigitUint tenDen = *den;
        mulAdd(&tenDen, 10, 0);
        roundUp = compare(num, &tenDen) > 0;
    } else {
        shiftLeft(num, 1);
        int32_t order = compare(num, den);
        roundUp = order > 0 || (order == 0 && ((out[wanted - 1] - '0') & 1) != 0);
    }

    uint32_t count = wanted;
    if (roundUp) {
        uint32_t i = wanted;
        while (i > 0 && out[i - 1] == '9') {
            out[i - 1] = '0';
            --i;
        }
        if (i > 0) {
            ++out[i - 1];
        } else {
            // Every digit carried, so the result is a one in the next place up.
            out[0] = '1';
            if (count == 0)
                count = 1;
            ++exponent;
        }
    }
    return PrF64Digits{count, exponent};
}

// ---------------------------------------------------------------------------------------------------------------------
// Binary to decimal, rounded to a place, by one multiplication
//
// m * 2^e / 10^s is the value counted in units of 10^s. Its product with the table's 5^-s has 192 bits, and the table's
// truncation leaves the exact product between the truncated one and that plus m. Where both bounds have the same
// integer part and round the same way, that is the correctly rounded count. Where the entry is exact, so is the
// product, and a tie rounds to even. Otherwise the digits are generated exactly, as above.

struct U192 {
    uint64_t limbs[3];   // little-endian
};

U192 mul192(uint64_t m, const Pow5& factor) {
    uint64_t lowHigh = 0;
    uint64_t low = prU64Mul128(m, factor.lo, &lowHigh);
    uint64_t highHigh = 0;
    uint64_t highLow = prU64Mul128(m, factor.hi, &highHigh);
    uint64_t middle = lowHigh + highLow;
    return U192{{low, middle, highHigh + (middle < lowHigh ? 1 : 0)}};
}

U192 add192(U192 v, uint64_t addend) {
    v.limbs[0] += addend;
    if (v.limbs[0] < addend && ++v.limbs[1] == 0)
        ++v.limbs[2];
    return v;
}

// The bits of `v` from `bit` up, as 64 bits, and whether any bit above those is set.
uint64_t bitsFrom(const U192& v, uint32_t bit, bool* overflow) {
    uint32_t limb = bit / 64;
    uint32_t shift = bit % 64;
    uint64_t out = limb < 3 ? v.limbs[limb] >> shift : 0;
    if (shift != 0 && limb + 1 < 3)
        out |= v.limbs[limb + 1] << (64 - shift);
    bool above = false;
    for (uint32_t i = limb + 1; i < 3; ++i) {
        uint64_t part = i == limb + 1 && shift != 0 ? v.limbs[i] >> shift : v.limbs[i];
        above |= part != 0;
    }
    *overflow = above;
    return out;
}

// Returns whether any bit of `v` below `bit` is set.
bool anyBelow(const U192& v, uint32_t bit) {
    for (uint32_t i = 0; i < 3; ++i) {
        if (bit >= 64 * (i + 1)) {
            if (v.limbs[i] != 0)
                return true;
        } else {
            uint32_t within = bit - 64 * i;
            return within != 0 && (v.limbs[i] & ((1ull << within) - 1)) != 0;
        }
    }
    return false;
}

// Splits `v` / 2^shift into its integer part, which must fit in 64 bits, and the fraction's order against one half.
bool splitAt(const U192& v, uint32_t shift, uint64_t* integer, int32_t* half) {
    PR_ASSERT(shift >= 1 && shift < 192);
    bool overflow = false;
    *integer = bitsFrom(v, shift, &overflow);
    if (overflow)
        return false;
    uint32_t halfBit = shift - 1;
    bool set = ((v.limbs[halfBit / 64] >> (halfBit % 64)) & 1) != 0;
    *half = !set ? -1 : anyBelow(v, halfBit) ? 1 : 0;
    return true;
}

// Writes to `*truncated` and `*rounded` the integer part of m * 2^e / 10^s and that count rounded half to even, if one
// multiplication decides them. Returns false if it does not, or if they do not fit in 64 bits.
bool roundedUnits(uint64_t m, int32_t e, int32_t s, uint64_t* truncated, uint64_t* rounded) {
    int32_t q = -s;
    if (q < kPow5MinExp10 || q > kPow5MaxExp10)
        return false;
    const Pow5& factor = pow5(q);
    // 10^q = factor * 2^(q + floor(log2(5^q)) - 127), so the count is m * factor / 2^shift.
    int32_t shift = 127 - e - q - (floorLog2Pow10(q) - q);
    U192 low = mul192(m, factor);

    // The common case: the count's integer part and the top 64 bits of its fraction sit within the product, and m is
    // below the fraction's lowest kept bit, so the exact product moves that fraction by at most one. Unless the
    // fraction is at a half, or one below it, or about to carry, those bits decide the rounding.
    if (shift >= 117 && shift < 192) {
        uint32_t r = uint32_t(shift) - 64;
        uint64_t fraction = 0;
        uint64_t integer = 0;
        bool overflow = false;
        if (r < 64) {
            fraction = low.limbs[0] >> r | low.limbs[1] << (64 - r);
            integer = low.limbs[1] >> r | low.limbs[2] << (64 - r);
            overflow = (low.limbs[2] >> r) != 0;
        } else {
            uint32_t within = r - 64;
            fraction = within == 0 ? low.limbs[1] : (low.limbs[1] >> within | low.limbs[2] << (64 - within));
            integer = within == 0 ? low.limbs[2] : low.limbs[2] >> within;
        }
        if (overflow)
            return false;
        constexpr uint64_t kHalf = 1ull << 63;
        if (fraction != kHalf && fraction != kHalf - 1 && fraction != kU64Max) {
            bool up = fraction > kHalf;
            if (up && integer == kU64Max)
                return false;
            *truncated = integer;
            *rounded = integer + (up ? 1 : 0);
            return true;
        }
    }

    uint64_t integer = 0;
    int32_t half = 0;
    if (shift >= 192) {
        // The product is below 2^181, so the count is below 2^-11.
        integer = 0;
        half = -1;
    } else if (shift < 1 || !splitAt(low, uint32_t(shift), &integer, &half)) {
        return false;
    }
    bool up = false;
    if (pow5Exact(q)) {
        up = half > 0 || (half == 0 && (integer & 1) != 0);
    } else {
        // The exact product lies strictly between low and low + m.
        uint64_t upperInteger = 0;
        int32_t upperHalf = -1;
        if (shift < 192 && !splitAt(add192(low, m), uint32_t(shift), &upperInteger, &upperHalf))
            return false;
        if (upperInteger != integer)
            return false;
        if (half >= 0)
            up = true;
        else if (upperHalf > 0)
            return false;
    }
    if (up && integer == kU64Max)
        return false;
    *truncated = integer;
    *rounded = integer + (up ? 1 : 0);
    return true;
}

// Writes the digits of `units`, which has `count` of them, to `out`.
void writeUnits(uint64_t units, uint32_t count, char* out) {
    PR_ASSERT(count != 0 && count <= 20);
    size_t written = prU64Format(units, out, 10);
    PR_ASSERT(written == count);
    (void)written;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrF64Decimal prF64ShortestDecimal(double value) {
    uint64_t bits = bitCast<uint64_t>(value);
    PR_ASSERT((bits & ~(1ull << 63)) != 0 && (bits >> 52 & 0x7ff) != 0x7ff);
    return shortestDecimal<Binary64>(bits & 0xfffffffffffffull, uint32_t((bits >> 52) & 0x7ffull));
}

PrF64Decimal prF32ShortestDecimal(float value) {
    uint32_t bits = bitCast<uint32_t>(value);
    PR_ASSERT((bits & 0x7fffffffu) != 0 && (bits >> 23 & 0xff) != 0xff);
    return shortestDecimal<Binary32>(bits & 0x7fffffu, (bits >> 23) & 0xffu);
}

PrF64Digits prF64FixedDigits(double value, uint32_t decimals, char* out, size_t capacity) {
    uint64_t m = 0;
    int32_t e = 0;
    decompose(value, &m, &e);
    uint64_t truncated = 0;
    uint64_t units = 0;
    if (decimals <= uint32_t(kPow5MaxExp10) && roundedUnits(m, e, -int32_t(decimals), &truncated, &units)) {
        // Nothing is left at that place: the digits stop above it.
        if (units == 0)
            return PrF64Digits{0, -int32_t(decimals)};
        uint32_t count = prU64DecimalDigits(units);
        PR_ASSERT(count < capacity);
        writeUnits(units, count, out);
        return PrF64Digits{count, int32_t(count) - int32_t(decimals)};
    }
    DigitUint num;
    DigitUint den;
    int32_t exponent = scaleToLeadingDigit(&num, &den, m, e);
    // The digits run from the leading one down to the 10^-decimals place. Fewer than none means that the value lies
    // below half of that place, and rounds away.
    int64_t span = int64_t(exponent) + decimals;
    if (span < 0)
        return PrF64Digits{0, exponent};
    // Past the exact expansion every digit is a zero, which the caller writes.
    uint32_t wanted = span < kPrF64MaxExactDigits ? uint32_t(span) : uint32_t(kPrF64MaxExactDigits);
    PR_ASSERT(wanted < capacity);
    return generateDigits(&num, &den, exponent, wanted, out);
}

PrF64Digits prF64SignificantDigits(double value, uint32_t significant, char* out, size_t capacity) {
    uint64_t m = 0;
    int32_t e = 0;
    decompose(value, &m, &e);
    uint32_t wanted = significant < kPrF64MaxExactDigits ? significant : uint32_t(kPrF64MaxExactDigits);
    PR_ASSERT(wanted < capacity);
    if (wanted >= 1 && wanted <= 19) {
        // |value| = 0.d1d2... * 10^exponent. The estimate from the binary exponent is the exponent or one below it.
        int32_t exponent = floorLog10Pow2(int32_t(log2Floor(m)) + e) + 1;
        for (uint32_t attempt = 0; attempt < 2; ++attempt) {
            uint64_t truncated = 0;
            uint64_t units = 0;
            if (!roundedUnits(m, e, exponent - int32_t(wanted), &truncated, &units))
                break;
            if (truncated >= prU64Pow10(wanted)) {
                ++exponent;
                continue;
            }
            PR_ASSERT(truncated >= prU64Pow10(wanted - 1));
            if (units == prU64Pow10(wanted)) {
                // Every digit carried, so the result is a one in the next place up.
                units /= 10;
                ++exponent;
            }
            writeUnits(units, wanted, out);
            return PrF64Digits{wanted, exponent};
        }
    }
    DigitUint num;
    DigitUint den;
    int32_t exponent = scaleToLeadingDigit(&num, &den, m, e);
    return generateDigits(&num, &den, exponent, wanted, out);
}

size_t prF64ParsePrefix(PrStringView text, double* out) {
    DecimalScan scan;
    if (!scanDecimal(text, &scan))
        return 0;
    if (!exactF64(&scan, out)) {
        AdjustedMantissa adjusted = decimalToBinary(&kF64Format, &scan, text);
        uint64_t sign = scan.negative ? 0x8000000000000000ull : 0;
        *out = bitCast<double>(sign | (uint64_t(adjusted.power2) << 52) | adjusted.mantissa);
    }
    return scan.length;
}
