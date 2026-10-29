// Hashing bytes of any length.
//
// wyhash - https://github.com/wangyi-fudan/wyhash
//
// The hash is for keys of an open set, such as the interned strings or cache keys that a HashIndex looks up. A fixed
// set of names uses a perfect hash instead (name_table.h).

#pragma once

#include "pr/base.h"
#include "pr/bit_cast.h"
#include "pr/int128.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the two halves of the 128-bit product of `a` and `b`, folded together.
inline uint64_t hashFold(uint64_t a, uint64_t b) {
    uint64_t high = 0;
    uint64_t low = mul128(a, b, &high);
    return low ^ high;
}

// Returns the hash of the `size` bytes at `bytes` with `seed`.
inline uint64_t hashBytes(const void* PR_NULLABLE bytes, size_t size, uint64_t seed) {
    const uint8_t* data = static_cast<const uint8_t*>(bytes);
    constexpr uint64_t kP0 = 0xa0761d6478bd642full;
    constexpr uint64_t kP1 = 0xe7037ed1a0b428dbull;
    constexpr uint64_t kP2 = 0x8ebc6af09c88c6e3ull;
    constexpr uint64_t kP3 = 0x589965cc75374cc3ull;
    seed ^= hashFold(seed ^ kP0, kP1);
    uint64_t a = 0;
    uint64_t b = 0;
    if (size <= 16) {
        if (size >= 4) {
            size_t middle = (size >> 3) << 2;
            a = (uint64_t(loadUnaligned<uint32_t>(data)) << 32)
                | loadUnaligned<uint32_t>(data + middle);
            b = (uint64_t(loadUnaligned<uint32_t>(data + size - 4)) << 32)
                | loadUnaligned<uint32_t>(data + size - 4 - middle);
        } else if (size > 0) {
            a = (uint64_t(data[0]) << 16) | (uint64_t(data[size >> 1]) << 8) | data[size - 1];
        }
    } else {
        const uint8_t* at = data;
        size_t left = size;
        if (left > 48) {
            // Three lanes whose products are independent of each other.
            uint64_t second = seed;
            uint64_t third = seed;
            do {
                seed = hashFold(loadUnaligned<uint64_t>(at) ^ kP1,
                                loadUnaligned<uint64_t>(at + 8) ^ seed);
                second = hashFold(loadUnaligned<uint64_t>(at + 16) ^ kP2,
                                  loadUnaligned<uint64_t>(at + 24) ^ second);
                third = hashFold(loadUnaligned<uint64_t>(at + 32) ^ kP3,
                                 loadUnaligned<uint64_t>(at + 40) ^ third);
                at += 48;
                left -= 48;
            } while (left > 48);
            seed ^= second ^ third;
        }
        while (left > 16) {
            seed =
                hashFold(loadUnaligned<uint64_t>(at) ^ kP1, loadUnaligned<uint64_t>(at + 8) ^ seed);
            at += 16;
            left -= 16;
        }
        a = loadUnaligned<uint64_t>(at + left - 16);
        b = loadUnaligned<uint64_t>(at + left - 8);
    }
    uint64_t high = 0;
    uint64_t low = mul128(a ^ kP1, b ^ seed, &high);
    return hashFold(low ^ kP0 ^ size, high ^ kP1);
}

// Returns the hash of `pointer` and `salt`, for a table keyed by both. `salt` distinguishes keys that share an address.
// The result is the high half of a product that every bit of the key affects.
inline uint32_t hashPointer(const void* pointer, uint64_t salt) {
    return uint32_t(((uint64_t(uintptr_t(pointer)) ^ salt) * 0x9e3779b97f4a7c15ull) >> 32);
}

// Returns a key that mixes the keys `a` and `b`, never 0, so that 0 can stand for no key. Mixing keys that are unique
// among their siblings with a parent's key gives keys that are unique in the tree.
inline uint64_t hashMixKeys(uint64_t a, uint64_t b) {
    uint64_t h = (a ^ 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull;
    h ^= (b + 0x94d049bb133111ebull) * 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h != 0 ? h : 1;
}

} // namespace pr

PR_ASSUME_NONNULL_END
