// Random numbers: unpredictable bytes from the platform, and a fast deterministic generator for everything that is not
// a secret.

#pragma once

#include "pr/base.h"
#include "pr/integer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Fills the `size` bytes at `out` with random bytes that are unpredictable enough for keys and nonces. A platform that
// cannot supply them fails. May be called from any thread.
void prRandomBytes(void* PR_NULLABLE out, size_t size);

// Returns a seed for a generator from prRandomBytes, so that each run draws a different sequence.
uint64_t prRandomSeed(void);

// A generator of 64-bit numbers, wyrand: a counter mixed by a 128-bit multiply. A seed gives the same sequence on every
// platform, and any state is valid, so `{.state = seed}` makes one and zeroed storage is the generator of seed zero. It
// is not for secrets, which come from prRandomBytes. A generator is not thread-safe; each thread draws from its own.
typedef struct PrRandom {
    uint64_t state;
} PrRandom;

// Returns the next 64 bits of `random`'s sequence, each bit equally likely.
static inline uint64_t prRandomU64(PrRandom* random) {
    random->state += 0xa0761d6478bd642full;
    uint64_t high = 0;
    uint64_t low = prU64Mul128(random->state, random->state ^ 0xe7037ed1a0b428dbull, &high);
    return low ^ high;
}

// Returns the high 32 bits of the next draw.
static inline uint32_t prRandomU32(PrRandom* random) {
    return PR_VALUE_CAST(uint32_t, prRandomU64(random) >> 32);
}

// Returns true or false, equally likely.
static inline bool prRandomBool(PrRandom* random) {
    return (prRandomU64(random) >> 63) != 0;
}

// Return a number in [0, `bound`), each equally likely, by Lemire's multiply-and-shift: the high half of a draw times
// the bound, with the draws that would bias it rejected. A draw is rejected with probability below bound / 2^32 or
// bound / 2^64, so the loop almost never repeats. `bound` must be greater than zero.
static inline uint32_t prRandomU32Below(PrRandom* random, uint32_t bound) {
    uint64_t product = PR_VALUE_CAST(uint64_t, prRandomU32(random)) * bound;
    uint32_t low = PR_VALUE_CAST(uint32_t, product);
    if (low < bound) {
        uint32_t threshold = (0u - bound) % bound;
        while (low < threshold) {
            product = PR_VALUE_CAST(uint64_t, prRandomU32(random)) * bound;
            low = PR_VALUE_CAST(uint32_t, product);
        }
    }
    return PR_VALUE_CAST(uint32_t, product >> 32);
}
static inline uint64_t prRandomU64Below(PrRandom* random, uint64_t bound) {
    uint64_t high = 0;
    uint64_t low = prU64Mul128(prRandomU64(random), bound, &high);
    if (low < bound) {
        uint64_t threshold = (0ull - bound) % bound;
        while (low < threshold)
            low = prU64Mul128(prRandomU64(random), bound, &high);
    }
    return high;
}

// Return a number in [`low`, `high`], each equally likely. `low` must be at most `high`.
static inline uint32_t prRandomU32Between(PrRandom* random, uint32_t low, uint32_t high) {
    uint64_t range = PR_VALUE_CAST(uint64_t, high - low) + 1;
    return low + PR_VALUE_CAST(uint32_t, prRandomU64Below(random, range));
}
static inline int32_t prRandomI32Between(PrRandom* random, int32_t low, int32_t high) {
    uint64_t range = PR_VALUE_CAST(uint64_t, PR_VALUE_CAST(int64_t, high) - low) + 1;
    return PR_VALUE_CAST(int32_t, low + PR_VALUE_CAST(int64_t, prRandomU64Below(random, range)));
}

// Return a number in [0, 1), from the top 24 or 53 bits of a draw, so that every value is a multiple of the type's
// spacing at one.
static inline float prRandomF32(PrRandom* random) {
    return PR_VALUE_CAST(float, prRandomU64(random) >> 40) * (1.0f / 16777216.0f);
}
static inline double prRandomF64(PrRandom* random) {
    return PR_VALUE_CAST(double, prRandomU64(random) >> 11) * (1.0 / 9007199254740992.0);
}

// Returns a number in [`low`, `high`).
static inline float prRandomF32Between(PrRandom* random, float low, float high) {
    return low + prRandomF32(random) * (high - low);
}

// Returns a number from an approximately normal distribution with mean 0 and standard deviation 1, from one draw: the
// sum of three uniform 21-bit numbers, rescaled. The sum is an Irwin-Hall distribution, which stays within three
// standard deviations, so it suits jitter and noise rather than statistics that need the tails.
static inline float prRandomGaussianF32(PrRandom* random) {
    uint64_t bits = prRandomU64(random);
    uint32_t sum = PR_VALUE_CAST(uint32_t, (bits & 0x1fffff) + ((bits >> 21) & 0x1fffff) + ((bits >> 42) & 0x1fffff));
    return PR_VALUE_CAST(float, sum) * (1.0f / 1048576.0f) - 3.0f;
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
