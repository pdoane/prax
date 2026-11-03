// Hashing bytes of any length.
//
// wyhash - https://github.com/wangyi-fudan/wyhash
//
// The hash is for keys of an open set, such as the interned strings or cache keys that a hash index looks up. A fixed
// set of names uses a perfect hash instead (name_table.h).

#pragma once

#include "pr/base.h"
#include "pr/integer.h"
#include "pr/mem.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the two halves of the 128-bit product of `a` and `b`, folded together.
static inline uint64_t prHashFold(uint64_t a, uint64_t b) {
    uint64_t high = 0;
    uint64_t low = prU64Mul128(a, b, &high);
    return low ^ high;
}

// Returns the hash of the `size` bytes at `bytes` with `seed`.
static inline uint64_t prHashBytes(const void* PR_NULLABLE bytes, size_t size, uint64_t seed) {
    const uint8_t* data = PR_VALUE_CAST(const uint8_t*, bytes);
    const uint64_t p0 = 0xa0761d6478bd642full;
    const uint64_t p1 = 0xe7037ed1a0b428dbull;
    const uint64_t p2 = 0x8ebc6af09c88c6e3ull;
    const uint64_t p3 = 0x589965cc75374cc3ull;
    seed ^= prHashFold(seed ^ p0, p1);
    uint64_t a = 0;
    uint64_t b = 0;
    if (size <= 16) {
        if (size >= 4) {
            size_t middle = (size >> 3) << 2;
            a = (PR_VALUE_CAST(uint64_t, prMemReadU32(data)) << 32) | prMemReadU32(data + middle);
            b = (PR_VALUE_CAST(uint64_t, prMemReadU32(data + size - 4)) << 32) | prMemReadU32(data + size - 4 - middle);
        } else if (size > 0) {
            a = (PR_VALUE_CAST(uint64_t, data[0]) << 16) | (PR_VALUE_CAST(uint64_t, data[size >> 1]) << 8) | data[size - 1];
        }
    } else {
        const uint8_t* at = data;
        size_t left = size;
        if (left > 48) {
            // Three lanes whose products are independent of each other.
            uint64_t second = seed;
            uint64_t third = seed;
            do {
                seed = prHashFold(prMemReadU64(at) ^ p1, prMemReadU64(at + 8) ^ seed);
                second = prHashFold(prMemReadU64(at + 16) ^ p2, prMemReadU64(at + 24) ^ second);
                third = prHashFold(prMemReadU64(at + 32) ^ p3, prMemReadU64(at + 40) ^ third);
                at += 48;
                left -= 48;
            } while (left > 48);
            seed ^= second ^ third;
        }
        while (left > 16) {
            seed = prHashFold(prMemReadU64(at) ^ p1, prMemReadU64(at + 8) ^ seed);
            at += 16;
            left -= 16;
        }
        a = prMemReadU64(at + left - 16);
        b = prMemReadU64(at + left - 8);
    }
    uint64_t high = 0;
    uint64_t low = prU64Mul128(a ^ p1, b ^ seed, &high);
    return prHashFold(low ^ p0 ^ size, high ^ p1);
}

// Returns the hash of `pointer` and `salt`, for a table keyed by both. `salt` distinguishes keys that share an address.
// The result is the high half of a product that every bit of the key affects.
static inline uint32_t prHashPointer(const void* pointer, uint64_t salt) {
    return PR_VALUE_CAST(uint32_t, ((PR_POINTER_CAST(uintptr_t, pointer) ^ salt) * 0x9e3779b97f4a7c15ull) >> 32);
}

// Returns a key that mixes the keys `a` and `b`, never 0, so that 0 can stand for no key. Mixing keys that are unique
// among their siblings with a parent's key gives keys that are unique in the tree.
PR_CONSTEXPR uint64_t prHashMixKeys(uint64_t a, uint64_t b) {
    uint64_t h = (a ^ 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull;
    h ^= (b + 0x94d049bb133111ebull) * 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h != 0 ? h : 1;
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
