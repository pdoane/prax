// Incremental checksums of bytes.
//
// CRC-32 takes the processor's CRC-32 instructions on arm64, carry-less multiplication on x86-64, whose builds have it
// (CMakeLists.txt), and tables elsewhere, 16 bytes a step. A long input runs as lanes whose work overlaps, joined at
// the end by shifting each lane's register over the bytes after it.

#include "pr/checksum.h"

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/mem.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define PR_CRC32_INSTRUCTIONS 1
#elif defined(__PCLMUL__) || defined(_M_X64)
#include <immintrin.h>
#define PR_CRC32_CLMUL 1
#endif

namespace pr_impl {
namespace {

// The reflected IEEE polynomial.
constexpr uint32_t kCrc32Polynomial = 0xedb88320u;

// The CRC-32 register after each byte, and fifteen more tables that extend it by a zero byte at a time, so that a step
// of 16 bytes is 16 independent lookups.
struct Crc32Tables {
    uint32_t entries[16][256];
};

constexpr Crc32Tables makeCrc32Tables() {
    Crc32Tables tables{};
    for (uint32_t index = 0; index < 256; ++index) {
        uint32_t value = index;
        for (uint32_t bit = 0; bit < 8; ++bit)
            value = (value & 1) != 0 ? kCrc32Polynomial ^ (value >> 1) : value >> 1;
        tables.entries[0][index] = value;
    }
    for (uint32_t k = 1; k < 16; ++k) {
        for (uint32_t index = 0; index < 256; ++index) {
            uint32_t previous = tables.entries[k - 1][index];
            tables.entries[k][index] = previous >> 8 ^ tables.entries[0][previous & 0xff];
        }
    }
    return tables;
}

constexpr Crc32Tables kCrc32 = makeCrc32Tables();

// The bytes below which an input runs as one lane.
constexpr size_t kCrc32LaneMinimum = 4096;

// a times b, modulo the polynomial, in the reflected bit order.
uint32_t crc32Multiply(uint32_t a, uint32_t b) {
    uint32_t product = 0;
    for (uint32_t m = 0x80000000u; m != 0; m >>= 1) {
        if ((a & m) != 0)
            product ^= b;
        b = (b & 1) != 0 ? (b >> 1) ^ kCrc32Polynomial : b >> 1;
    }
    return product;
}

// The register `crc` extended by `count` zero bytes: times x^(8 count), modulo the polynomial. Two registers of
// adjacent runs, computed apart, join as the first shifted over the second's bytes, exclusive-or the second.
uint32_t crc32Shift(uint32_t crc, size_t count) {
    uint32_t power = 0x00800000u; // x^8
    for (; count != 0; count >>= 1) {
        if ((count & 1) != 0)
            crc = crc32Multiply(power, crc);
        power = crc32Multiply(power, power);
    }
    return crc;
}

// The register after 16 bytes, by the tables.
inline uint32_t crc32Step16(uint32_t crc, const uint8_t* s) {
    const uint32_t w = crc ^ prMemReadLeU32(s);
    const uint32_t (&t)[16][256] = kCrc32.entries;
    return t[15][w & 0xff] ^ t[14][w >> 8 & 0xff] ^ t[13][w >> 16 & 0xff] ^ t[12][w >> 24] ^ t[11][s[4]] ^
        t[10][s[5]] ^ t[9][s[6]] ^ t[8][s[7]] ^ t[7][s[8]] ^ t[6][s[9]] ^ t[5][s[10]] ^ t[4][s[11]] ^ t[3][s[12]] ^
        t[2][s[13]] ^ t[1][s[14]] ^ t[0][s[15]];
}

// The register after `size` bytes at `data`, by the tables: a long input as four lanes.
[[maybe_unused]] uint32_t crc32Tables(uint32_t crc, const uint8_t* data, size_t size) {
    size_t at = 0;
    if (size >= kCrc32LaneMinimum) {
        const size_t lane = size / 64 * 16;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        for (; at < lane; at += 16) {
            crc = crc32Step16(crc, data + at);
            b = crc32Step16(b, data + lane + at);
            c = crc32Step16(c, data + 2 * lane + at);
            d = crc32Step16(d, data + 3 * lane + at);
        }
        crc = crc32Shift(crc32Shift(crc32Shift(crc, lane) ^ b, lane) ^ c, lane) ^ d;
        at = 4 * lane;
    }
    for (; size - at >= 16; at += 16)
        crc = crc32Step16(crc, data + at);
    for (; at < size; ++at)
        crc = kCrc32.entries[0][(crc ^ data[at]) & 0xff] ^ (crc >> 8);
    return crc;
}

#if defined(PR_CRC32_INSTRUCTIONS)
// The register after `size` bytes at `data`, eight bytes an instruction: a long input as three lanes, whose
// instructions overlap.
uint32_t crc32Instructions(uint32_t crc, const uint8_t* data, size_t size) {
    size_t at = 0;
    if (size >= kCrc32LaneMinimum) {
        const size_t lane = size / 24 * 8;
        uint32_t b = 0;
        uint32_t c = 0;
        for (; at < lane; at += 8) {
            crc = __crc32d(crc, prMemReadLeU64(data + at));
            b = __crc32d(b, prMemReadLeU64(data + lane + at));
            c = __crc32d(c, prMemReadLeU64(data + 2 * lane + at));
        }
        crc = crc32Shift(crc32Shift(crc, lane) ^ b, lane) ^ c;
        at = 3 * lane;
    }
    for (; size - at >= 8; at += 8)
        crc = __crc32d(crc, prMemReadLeU64(data + at));
    for (; at < size; ++at)
        crc = __crc32b(crc, data[at]);
    return crc;
}
#endif

#if defined(PR_CRC32_CLMUL)
// x's two halves times the powers of x in `k`: the 128 bits that x becomes that many bits further on.
inline __m128i crc32Fold(__m128i x, __m128i k) {
    return _mm_xor_si128(_mm_clmulepi64_si128(x, k, 0x00), _mm_clmulepi64_si128(x, k, 0x11));
}

inline __m128i crc32Load(const uint8_t* data) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
}

// The low 64 bits of the carry-less product of a and b, and its high 64 bits.
inline __m128i crc32Product(uint64_t a, uint64_t b) {
    return _mm_clmulepi64_si128(_mm_cvtsi64_si128(int64_t(a)), _mm_cvtsi64_si128(int64_t(b)), 0x00);
}

inline uint64_t crc32Low(__m128i x) {
    return uint64_t(_mm_cvtsi128_si64(x));
}

inline uint64_t crc32High(__m128i x) {
    return uint64_t(_mm_extract_epi64(x, 1));
}

// The register after the first multiple of 16 bytes of `size`, at least 64, at `data`, and in `*taken` how many it
// took, by folding with carry-less multiplication: four lanes of 128 bits, each carried 512 bits on and added to the
// next 64 bytes, then folded into one, and reduced to 32 bits by Barrett's method. The constants are powers of x modulo
// the polynomial, as Intel's paper on PCLMULQDQ and Linux's crc32-pclmul derive them.
uint32_t crc32Folded(uint32_t crc, const uint8_t* data, size_t size, size_t* taken) {
    PR_ASSERT(size >= 64);
    const __m128i by512 = _mm_set_epi64x(0x1c6e41596, 0x154442bd4);
    const __m128i by128 = _mm_set_epi64x(0x0ccaa009e, 0x1751997d0);
    __m128i x0 = _mm_xor_si128(crc32Load(data + 0), _mm_cvtsi32_si128(int(crc)));
    __m128i x1 = crc32Load(data + 16);
    __m128i x2 = crc32Load(data + 32);
    __m128i x3 = crc32Load(data + 48);
    size_t at = 64;
    for (; size - at >= 64; at += 64) {
        x0 = _mm_xor_si128(crc32Fold(x0, by512), crc32Load(data + at));
        x1 = _mm_xor_si128(crc32Fold(x1, by512), crc32Load(data + at + 16));
        x2 = _mm_xor_si128(crc32Fold(x2, by512), crc32Load(data + at + 32));
        x3 = _mm_xor_si128(crc32Fold(x3, by512), crc32Load(data + at + 48));
    }
    __m128i x = _mm_xor_si128(crc32Fold(x0, by128), x1);
    x = _mm_xor_si128(crc32Fold(x, by128), x2);
    x = _mm_xor_si128(crc32Fold(x, by128), x3);
    for (; size - at >= 16; at += 16)
        x = _mm_xor_si128(crc32Fold(x, by128), crc32Load(data + at));
    // 128 bits to 64, then 64 to 32, then Barrett's reduction.
    const __m128i product = crc32Product(crc32Low(x), 0x0ccaa009e);
    const uint64_t lo = crc32Low(product) ^ crc32High(x);
    const uint64_t hi = crc32High(product);
    const uint64_t shifted = lo >> 32 | hi << 32;
    const uint64_t y = crc32Low(crc32Product(lo & 0xffffffff, 0x163cd6124)) ^ shifted;
    const uint64_t t = crc32Low(crc32Product(y & 0xffffffff, 0x1f7011641)) & 0xffffffff;
    const uint64_t r = crc32Low(crc32Product(t, 0x1db710641)) ^ y;
    *taken = at;
    return uint32_t(r >> 32);
}
#endif

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

} // namespace pr_impl

using namespace pr_impl;

uint32_t prCrc32Update(uint32_t checksum, PrByteView bytes) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    const uint32_t crc = ~checksum;
#if defined(PR_CRC32_INSTRUCTIONS)
    return ~crc32Instructions(crc, bytes.data, bytes.size);
#elif defined(PR_CRC32_CLMUL)
    if (bytes.size >= 64) {
        size_t taken = 0;
        const uint32_t folded = crc32Folded(crc, bytes.data, bytes.size, &taken);
        return ~crc32Tables(folded, bytes.data + taken, bytes.size - taken);
    }
    return ~crc32Tables(crc, bytes.data, bytes.size);
#else
    return ~crc32Tables(crc, bytes.data, bytes.size);
#endif
}

uint32_t prAdler32Update(uint32_t checksum, PrByteView bytes) {
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
