// Incremental checksums of bytes.

#include "pr/checksum.h"

#include "pr/assert.h"
#include "pr/count_of.h"

namespace pr {
namespace {

struct Crc32Table {
    uint32_t entries[256];
};

constexpr Crc32Table makeCrc32Table() {
    Crc32Table table{};
    for (uint32_t index = 0; index < countOf(table.entries); ++index) {
        uint32_t value = index;
        for (uint32_t bit = 0; bit < 8; ++bit)
            value = (value & 1) != 0 ? 0xedb88320u ^ (value >> 1) : value >> 1;
        table.entries[index] = value;
    }
    return table;
}

constexpr Crc32Table kCrc32 = makeCrc32Table();
constexpr uint32_t kAdlerModulus = 65521;

// The number of bytes that both Adler sums can take before either can exceed 32 bits.
constexpr size_t kAdlerBlock = 5552;

// The number of bytes in a row. The Adler sums process the bytes of a row as independent lanes. The block is a whole
// number of rows.
constexpr uint32_t kAdlerLanes = 16;
static_assert(kAdlerBlock % kAdlerLanes == 0);

// The number of rows whose sums fit in 16 bits. The running sum of a lane over them is at most 255 times 16 * 17 / 2.
constexpr size_t kAdlerRun = 16;

} // namespace

uint32_t crc32Update(uint32_t checksum, PrByteView bytes) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    uint32_t crc = checksum ^ 0xffffffffu;
    for (size_t i = 0; i < bytes.size; ++i)
        crc = kCrc32.entries[(crc ^ bytes.data[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

uint32_t adler32Update(uint32_t checksum, PrByteView bytes) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    uint32_t low = checksum & 0xffff;
    uint32_t high = checksum >> 16;
    size_t at = 0;
    while (at < bytes.size) {
        const size_t count = bytes.size - at < kAdlerBlock ? bytes.size - at : kAdlerBlock;
        const size_t end = at + count;
        // Process whole rows of kAdlerLanes bytes, with each lane independent so that the loop vectorizes. A lane keeps
        // the sum of its bytes and the sum of those sums, and the block adds both to the Adler sums at once. Over n
        // bytes, low gains every byte. High gains n times the previous low, plus each byte once for every byte from it
        // to the end of the block.
        const size_t rows = count / kAdlerLanes;
        if (rows != 0) {
            uint32_t sums[kAdlerLanes] = {};
            uint32_t running[kAdlerLanes] = {};
            // Runs of up to kAdlerRun rows sum in 16 bits, which halves the width of the lanes. Each run is then folded
            // into the totals.
            for (size_t row = 0; row < rows;) {
                const size_t run = rows - row < kAdlerRun ? rows - row : kAdlerRun;
                uint16_t runSums[kAdlerLanes] = {};
                uint16_t runRunning[kAdlerLanes] = {};
                for (size_t k = 0; k < run; ++k, at += kAdlerLanes) {
                    for (uint32_t lane = 0; lane < kAdlerLanes; ++lane) {
                        runSums[lane] = uint16_t(runSums[lane] + bytes.data[at + lane]);
                        runRunning[lane] = uint16_t(runRunning[lane] + runSums[lane]);
                    }
                }
                for (uint32_t lane = 0; lane < kAdlerLanes; ++lane) {
                    running[lane] += uint32_t(run) * sums[lane] + runRunning[lane];
                    sums[lane] += runSums[lane];
                }
                row += run;
            }
            uint64_t sum = 0;
            uint64_t weighted = 0;
            uint64_t sumOfRunning = 0;
            for (uint32_t lane = 0; lane < kAdlerLanes; ++lane) {
                sum += sums[lane];
                weighted += uint64_t(lane) * sums[lane];
                sumOfRunning += running[lane];
            }
            const uint64_t n = rows * kAdlerLanes;
            high = uint32_t((high + n * low + kAdlerLanes * sumOfRunning - weighted) % kAdlerModulus);
            low = uint32_t((low + sum) % kAdlerModulus);
        }
        for (; at < end; ++at) {
            low += bytes.data[at];
            high += low;
        }
        low %= kAdlerModulus;
        high %= kAdlerModulus;
    }
    return (high << 16) | low;
}

void sha1Digest(PrByteView bytes, uint8_t out[PR_NONNULL kSha1Bytes]) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    uint32_t state[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    // The message is padded with a one bit, zeros, and its length in bits, to a whole number of 64-byte blocks.
    uint64_t total = bytes.size + 9;
    uint64_t padded = (total + 63) / 64 * 64;
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

} // namespace pr
