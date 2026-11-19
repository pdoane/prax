// SHA-1 digests of bytes.

#include "pr/sha1.h"

#include "pr/align.h"
#include "pr/assert.h"

using namespace pr_impl;

void prSha1Digest(PrByteView bytes, uint8_t out[PR_NONNULL kPrSha1Bytes]) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    uint32_t state[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    // The message is padded with a one bit, zeros, and its length in bits, to a whole number of 64-byte blocks.
    uint64_t total = bytes.size + 9;
    uint64_t padded = alignUp(total, 64);
    for (uint64_t offset = 0; offset < padded; offset += 64) {
        uint8_t block[64];
        for (uint32_t i = 0; i < 64; ++i) {
            uint64_t at = offset + i;
            if (at < bytes.size)
                block[i] = bytes.data[at];
            else if (at == bytes.size)
                block[i] = 0x80;
            else if (at >= padded - 8)
                block[i] = uint8_t(uint64_t(bytes.size) * 8 >> (8 * (padded - 1 - at)));
            else
                block[i] = 0;
        }
        uint32_t words[80];
        for (uint32_t i = 0; i < 16; ++i)
            words[i] = uint32_t(block[4 * i]) << 24 | uint32_t(block[4 * i + 1]) << 16 |
                uint32_t(block[4 * i + 2]) << 8 | block[4 * i + 3];
        for (uint32_t i = 16; i < 80; ++i) {
            uint32_t mixed = words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16];
            words[i] = mixed << 1 | mixed >> 31;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (uint32_t i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcu;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6u;
            }
            uint32_t next = (a << 5 | a >> 27) + f + e + k + words[i];
            e = d;
            d = c;
            c = b << 30 | b >> 2;
            b = a;
            a = next;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }
    for (uint32_t i = 0; i < 5; ++i) {
        out[4 * i] = uint8_t(state[i] >> 24);
        out[4 * i + 1] = uint8_t(state[i] >> 16);
        out[4 * i + 2] = uint8_t(state[i] >> 8);
        out[4 * i + 3] = uint8_t(state[i]);
    }
}
