// XXH64, xxHash's 64-bit hash of bytes.

#include "pr/xxh64.h"

#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {
namespace {

// XXH64's primes.
constexpr uint64_t kXxhPrime1 = 0x9e3779b185ebca87ull;
constexpr uint64_t kXxhPrime2 = 0xc2b2ae3d27d4eb4full;
constexpr uint64_t kXxhPrime3 = 0x165667b19e3779f9ull;
constexpr uint64_t kXxhPrime4 = 0x85ebca77c2b2ae63ull;
constexpr uint64_t kXxhPrime5 = 0x27d4eb2f165667c5ull;

inline uint64_t rotate(uint64_t value, uint32_t count) {
    return value << count | value >> (64 - count);
}

inline uint64_t xxhRound(uint64_t lane, uint64_t input) {
    return rotate(lane + input * kXxhPrime2, 31) * kXxhPrime1;
}

// Takes the whole stripes of the `size` bytes at `data` into the lanes, which stay in registers across the stripes.
// Returns the bytes taken.
size_t xxhStripes(uint64_t lanes[PR_NONNULL 4], const uint8_t* data, size_t size) {
    uint64_t a = lanes[0];
    uint64_t b = lanes[1];
    uint64_t c = lanes[2];
    uint64_t d = lanes[3];
    size_t at = 0;
    for (; size - at >= 32; at += 32) {
        a = xxhRound(a, prMemReadLeU64(data + at));
        b = xxhRound(b, prMemReadLeU64(data + at + 8));
        c = xxhRound(c, prMemReadLeU64(data + at + 16));
        d = xxhRound(d, prMemReadLeU64(data + at + 24));
    }
    lanes[0] = a;
    lanes[1] = b;
    lanes[2] = c;
    lanes[3] = d;
    return at;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrXxh64 prXxh64Start(uint64_t seed) {
    PrXxh64 hash{};
    hash.lanes[0] = seed + kXxhPrime1 + kXxhPrime2;
    hash.lanes[1] = seed + kXxhPrime2;
    hash.lanes[2] = seed;
    hash.lanes[3] = seed - kXxhPrime1;
    return hash;
}

void prXxh64Update(PrXxh64* hash, PrByteView bytes) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    const uint8_t* at = bytes.data;
    size_t left = bytes.size;
    hash->total += left;
    if (hash->held != 0) {
        const size_t count = left < 32 - hash->held ? left : 32 - hash->held;
        prMemCopy(hash->stripe + hash->held, at, count);
        hash->held += uint32_t(count);
        if (hash->held < 32)
            return;
        xxhStripes(hash->lanes, hash->stripe, 32);
        hash->held = 0;
        at += count;
        left -= count;
    }
    const size_t taken = xxhStripes(hash->lanes, at, left);
    if (left != taken)
        prMemCopy(hash->stripe, at + taken, left - taken);
    hash->held = uint32_t(left - taken);
}

uint64_t prXxh64Digest(const PrXxh64* hash) {
    uint64_t h = 0;
    if (hash->total >= 32) {
        h = rotate(hash->lanes[0], 1) + rotate(hash->lanes[1], 7) + rotate(hash->lanes[2], 12) + rotate(hash->lanes[3], 18);
        for (uint64_t lane : hash->lanes)
            h = (h ^ xxhRound(0, lane)) * kXxhPrime1 + kXxhPrime4;
    } else {
        // The seed is the third lane, which no stripe has changed.
        h = hash->lanes[2] + kXxhPrime5;
    }
    h += hash->total;
    const uint8_t* at = hash->stripe;
    uint32_t left = hash->held;
    for (; left >= 8; left -= 8, at += 8)
        h = rotate(h ^ xxhRound(0, prMemReadLeU64(at)), 27) * kXxhPrime1 + kXxhPrime4;
    if (left >= 4) {
        h = rotate(h ^ prMemReadLeU32(at) * kXxhPrime1, 23) * kXxhPrime2 + kXxhPrime3;
        at += 4;
        left -= 4;
    }
    for (; left != 0; --left, ++at)
        h = rotate(h ^ *at * kXxhPrime5, 11) * kXxhPrime1;
    h ^= h >> 33;
    h *= kXxhPrime2;
    h ^= h >> 29;
    h *= kXxhPrime3;
    h ^= h >> 32;
    return h;
}

uint64_t prXxh64(PrByteView bytes, uint64_t seed) {
    PrXxh64 hash = prXxh64Start(seed);
    prXxh64Update(&hash, bytes);
    return prXxh64Digest(&hash);
}
