// What Zstandard's decoder and encoder share.

#include "zstd/src/zstd_format.h"

#include "pr/integer.h"

namespace pr_impl {

bool zstdFseSpread(const int16_t* counts, int64_t symbols, uint32_t log, uint8_t* spread) {
    int64_t size = int64_t(1) << log;
    int64_t high = size - 1;
    for (int64_t s = 0; s < symbols; ++s) {
        if (counts[s] == -1)
            spread[high--] = uint8_t(s);
    }
    int64_t step = (size >> 1) + (size >> 3) + 3;
    int64_t mask = size - 1;
    int64_t pos = 0;
    for (int64_t s = 0; s < symbols; ++s) {
        for (int64_t i = 0; i < counts[s]; ++i) {
            spread[pos] = uint8_t(s);
            pos = (pos + step) & mask;
            while (pos > high)
                pos = (pos + step) & mask;
        }
    }
    return pos == 0;
}

bool zstdBackReaderStart(const uint8_t* data, int64_t size, ZstdBackReader* r) {
    *r = ZstdBackReader{data, size, 0, 0, 0};
    if (size == 0 || data[size - 1] == 0)
        return false;
    if (size >= 8) {
        r->at = size - 8;
        r->bits = prMemReadLeU64(data + r->at);
    } else {
        r->bits = zstdLeBytes(data, size);
        r->consumed = uint64_t(8 - size) * 8;
    }
    r->consumed += 8 - prU64HighestBit(data[size - 1]);
    return true;
}

int64_t zstdReadFseCounts(const uint8_t* src, int64_t size, int16_t* counts, int64_t symbols, uint32_t maxLog, uint32_t* outLog) {
    if (size == 0)
        return -1;
    uint32_t log = (src[0] & 15u) + 5;
    if (log > maxLog)
        return -1;
    for (int64_t s = 0; s < symbols; ++s)
        counts[s] = 0;
    uint64_t pos = 4;
    // The next `n` bits from bit `pos`, first bit lowest, zero past the end.
    auto bitsAt = [&](uint32_t n) {
        uint64_t value = 0;
        int64_t byte = int64_t(pos >> 3);
        for (int64_t k = 0; k < 4; ++k) {
            if (byte + k < size)
                value |= uint64_t(src[byte + k]) << (8 * k);
        }
        return uint32_t(value >> (pos & 7) & ((uint64_t(1) << n) - 1));
    };
    int32_t remaining = (1 << log) + 1;
    int32_t threshold = 1 << log;
    uint32_t nb = log + 1;
    int64_t symbol = 0;
    while (remaining > 1) {
        if (symbol >= symbols)
            return -1;
        int32_t most = 2 * threshold - 1 - remaining;
        int32_t count = int32_t(bitsAt(nb - 1));
        if (count < most) {
            pos += nb - 1;
        } else {
            count = int32_t(bitsAt(nb));
            if (count >= threshold)
                count -= most;
            pos += nb;
        }
        count -= 1;
        counts[symbol++] = int16_t(count);
        remaining -= count < 0 ? -count : count;
        if (remaining < 1)
            return -1;
        while (remaining < threshold) {
            nb -= 1;
            threshold >>= 1;
        }
        if (count == 0) {
            // A count of zero is followed by how many more zeros follow, two bits at a time while they are 3.
            for (;;) {
                uint32_t repeat = bitsAt(2);
                pos += 2;
                symbol += repeat;
                if (repeat != 3)
                    break;
            }
        }
    }
    int64_t taken = int64_t((pos + 7) >> 3);
    if (taken > size)
        return -1;
    *outLog = log;
    return taken;
}

bool zstdBuildFse(const int16_t* counts, int64_t symbols, uint32_t log, ZstdFseCell* cells) {
    int64_t size = int64_t(1) << log;
    uint8_t spread[512];
    if (!zstdFseSpread(counts, symbols, log, spread))
        return false;
    uint16_t next[64];
    for (int64_t s = 0; s < symbols; ++s)
        next[s] = counts[s] == -1 ? uint16_t(1) : uint16_t(counts[s]);
    for (int64_t u = 0; u < size; ++u) {
        uint8_t s = spread[u];
        uint32_t state = next[s]++;
        uint32_t bits = log - prU64HighestBit(state);
        cells[u] = ZstdFseCell{s, uint8_t(bits), uint16_t((state << bits) - uint32_t(size))};
    }
    return true;
}

int64_t zstdReadWeights(const uint8_t* src, int64_t length, uint8_t* weights, int64_t* outCount) {
    if (length == 0)
        return -1;
    int64_t count = 0;
    int64_t header = src[0];
    if (header >= 128) {
        // Four bits a weight, the first in the high bits.
        count = header - 127;
        int64_t size = 1 + (count + 1) / 2;
        if (size > length)
            return -1;
        for (int64_t k = 0; k < count; ++k) {
            uint8_t b = src[1 + k / 2];
            weights[k] = k % 2 == 0 ? b >> 4 : b & 15;
        }
        *outCount = count;
        return size;
    }
    // FSE-coded, in two interleaved states.
    int64_t size = 1 + header;
    if (size > length)
        return -1;
    int16_t counts[13];
    uint32_t log = 0;
    int64_t taken = zstdReadFseCounts(src + 1, header, counts, 13, 6, &log);
    if (taken < 0)
        return -1;
    ZstdFseCell cells[64];
    if (!zstdBuildFse(counts, 13, log, cells))
        return -1;
    ZstdBackReader r;
    if (!zstdBackReaderStart(src + 1 + taken, header - taken, &r))
        return -1;
    int64_t one = int64_t(zstdBackRead(r, log));
    int64_t two = int64_t(zstdBackRead(r, log));
    for (;;) {
        if (count > 253)
            return -1;
        weights[count++] = cells[one].symbol;
        one = cells[one].baseline + int64_t(zstdBackRead(r, cells[one].bits));
        if (zstdBackReload(r) == ZstdReload_Overflow) {
            weights[count++] = cells[two].symbol;
            break;
        }
        weights[count++] = cells[two].symbol;
        two = cells[two].baseline + int64_t(zstdBackRead(r, cells[two].bits));
        if (zstdBackReload(r) == ZstdReload_Overflow) {
            weights[count++] = cells[one].symbol;
            break;
        }
    }
    *outCount = count;
    return size;
}

} // namespace pr_impl
