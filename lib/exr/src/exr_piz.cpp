// PIZ, OpenEXR's lossless wavelet compression, and the Huffman coding of 16-bit values that it shares with DWA.
//
// A PIZ chunk holds the bitmap of the 16-bit values that its pixels use, then the length of a Huffman coding and the
// coding. The values are remapped to the dense range of those that are used, each channel is transformed by a 2D Haar
// wavelet, and the coefficients are Huffman coded. The decoder runs the steps in reverse.

#include "exr/src/exr_piz.h"

#include "pr/mem.h"
#include "pr/memory.h"

namespace pr_impl {

// The symbols of the coding: every 16-bit value, and one more that stands for a run.
constexpr uint32_t kHuffmanSymbols = 65537;

// The longest code, and the bits of a code that the decoding table resolves at once.
constexpr uint32_t kHuffmanMaxLength = 58;
constexpr uint32_t kHuffmanTableBits = 14;

// A decoding table entry: a symbol of a code no longer than the table's bits, in the high bits, and its length in the
// low six. Zero is the prefix of a longer code.
typedef uint32_t HuffmanEntry;

// The decoding tables of one coding. Codes of each length are consecutive values, ordered by symbol: `first` is the
// value of the first, `count` the number of them and `start` the index in `sorted` of its symbol. `coded` holds the
// symbols that have codes, and `lengths` the length of each, in the order of the symbols.
struct ExrHuffman {
    uint32_t coded[kHuffmanSymbols];
    uint8_t lengths[kHuffmanSymbols];
    HuffmanEntry table[1u << kHuffmanTableBits];
    uint64_t first[kHuffmanMaxLength + 1];
    uint32_t count[kHuffmanMaxLength + 1];
    uint32_t start[kHuffmanMaxLength + 1];
    uint32_t sorted[kHuffmanSymbols];
};

namespace {

// The bitmap of the values that a chunk uses, one bit for each 16-bit value.
constexpr uint32_t kBitmapBytes = 65536 / 8;

// The table of code lengths stores runs of zero lengths: a length from 59 through 62 is a run of two through five
// zeros, and 63 is a run of the next eight bits plus six.
constexpr uint32_t kShortZeroRun = 59;
constexpr uint32_t kLongZeroRun = 63;
constexpr uint32_t kShortestLongRun = 2 + kLongZeroRun - kShortZeroRun;

// Reads bits from the most significant down, from bytes that are followed by at least eight readable bytes.
struct BitReader {
    const uint8_t* bytes;
    uint64_t at;                // bits read
    uint64_t size;              // bits that the coding holds

    // Returns the next `count` bits, at most 57, without reading them.
    uint64_t peek(uint32_t count) const {
        uint64_t window = prMemReadBeU64(bytes + (at >> 3)) << (at & 7);
        return window >> (64 - count);
    }
};

// Reads bits from the most significant down from `size` bytes, a byte at a time.
struct ByteBits {
    const uint8_t* bytes;
    size_t size;
    size_t next;                // the next byte to read
    uint64_t buffer;            // the bits read and not consumed, in the low `count` bits
    uint32_t count;

    // Writes the next `bits` bits, at most 32, to `*out`. Returns false if the bytes end first.
    bool read(uint32_t bits, uint32_t* out) {
        while (count < bits) {
            if (next == size)
                return false;
            buffer = buffer << 8 | bytes[next++];
            count += 8;
        }
        count -= bits;
        *out = uint32_t(buffer >> count) & ((1u << bits) - 1);
        return true;
    }
};

// Reads the code lengths of symbols `low` through `high` from `table`, which holds `size` bytes, and makes the decoding
// tables of the canonical code that they describe: codes of one length are consecutive in the order of their symbols,
// and the codes of each length follow, as values, the codes of the next longer length shifted right by one. Writes the
// bytes that the lengths took to `*read`. Returns false for lengths that end early, run past `high`, or describe no
// prefix code.
bool huffmanTables(ExrHuffman* h, const uint8_t* table, size_t size, uint32_t low, uint32_t high, size_t* read) {
    ByteBits bits{.bytes = table, .size = size};
    uint32_t coded = 0;
    prMemZero(h->count, sizeof(h->count));
    for (uint32_t symbol = low; symbol <= high; ++symbol) {
        uint32_t length = 0;
        if (!bits.read(6, &length))
            return false;
        if (length == kLongZeroRun || length >= kShortZeroRun) {
            uint32_t run = length - kShortZeroRun + 2;
            if (length == kLongZeroRun) {
                if (!bits.read(8, &run))
                    return false;
                run += kShortestLongRun;
            }
            if (uint64_t(symbol) + run > uint64_t(high) + 1)
                return false;
            symbol += run - 1;
            continue;
        }
        if (length != 0) {
            h->coded[coded] = symbol;
            h->lengths[coded++] = uint8_t(length);
            h->count[length] += 1;
        }
    }
    *read = bits.next;

    // The codes of a prefix code fill at most the whole space of 58-bit values.
    uint64_t space = 0;
    for (uint32_t length = 1; length <= kHuffmanMaxLength; ++length)
        space += uint64_t(h->count[length]) << (kHuffmanMaxLength - length);
    if (space > uint64_t(1) << kHuffmanMaxLength)
        return false;
    uint64_t code = 0;
    uint32_t sortedAt = 0;
    for (uint32_t length = kHuffmanMaxLength; length > 0; --length) {
        h->first[length] = code;
        code = (code + h->count[length]) >> 1;
    }
    for (uint32_t length = 1; length <= kHuffmanMaxLength; ++length) {
        h->start[length] = sortedAt;
        sortedAt += h->count[length];
    }
    uint32_t next[kHuffmanMaxLength + 1];
    prMemCopy(next, h->start, sizeof(next));
    for (uint32_t i = 0; i < coded; ++i)
        h->sorted[next[h->lengths[i]]++] = h->coded[i];
    prMemZero(h->table, sizeof(h->table));
    for (uint32_t length = 1; length <= kHuffmanTableBits; ++length) {
        for (uint32_t i = 0; i < h->count[length]; ++i) {
            uint64_t value = h->first[length] + i;
            uint32_t symbol = h->sorted[h->start[length] + i];
            uint32_t span = kHuffmanTableBits - length;
            HuffmanEntry entry = symbol << 6 | length;
            HuffmanEntry* fill = h->table + (value << span);
            for (uint32_t k = 0; k < (1u << span); ++k)
                fill[k] = entry;
        }
    }
    return true;
}

// Returns the symbol of the code longer than the table's bits at the reader, and its length in `*length`, or false if
// no code is there.
bool huffmanLong(const ExrHuffman* h, const BitReader& reader, uint32_t* symbol, uint32_t* length) {
    // A window of 64 bits from the reader's byte holds at least 57 bits past the reader. Longer codes read the next
    // byte as well.
    uint64_t byte = reader.at >> 3;
    uint32_t skip = uint32_t(reader.at & 7);
    uint64_t window = prMemReadBeU64(reader.bytes + byte) << skip;
    window |= uint64_t(reader.bytes[byte + 8]) << skip >> 8;
    for (uint32_t l = kHuffmanTableBits + 1; l <= kHuffmanMaxLength; ++l) {
        uint64_t code = window >> (64 - l);
        if (h->count[l] != 0 && code >= h->first[l] && code - h->first[l] < h->count[l]) {
            *symbol = h->sorted[h->start[l] + uint32_t(code - h->first[l])];
            *length = l;
            return true;
        }
    }
    return false;
}

// The 14-bit form of PIZ's wavelet, without modular arithmetic, for values below 2^14: a pair's average and difference
// become the pair.
inline void wavelet14(uint16_t l, uint16_t h, uint16_t* a, uint16_t* b) {
    int32_t hi = int16_t(h);
    int32_t ai = int16_t(l) + (hi & 1) + (hi >> 1);
    *a = uint16_t(ai);
    *b = uint16_t(ai - hi);
}

// The 16-bit form of PIZ's wavelet, modulo 2^16.
inline void wavelet16(uint16_t l, uint16_t h, uint16_t* a, uint16_t* b) {
    int32_t m = l;
    int32_t d = h;
    int32_t bb = (m - (d >> 1)) & 0xffff;
    int32_t aa = (d + bb - 0x8000) & 0xffff;
    *b = uint16_t(bb);
    *a = uint16_t(aa);
}

// Reverses PIZ's 2D wavelet transform of the `nx` by `ny` values at `in`, `ox` values apart across and `oy` down, in
// place. The transform is 14-bit if the largest value is below 2^14.
template <bool Narrow>
void waveletDecode(uint16_t* in, uint32_t nx, uint32_t ox, uint32_t ny, uint32_t oy) {
    uint32_t n = nx > ny ? ny : nx;
    uint32_t p = 1;
    while (p <= n)
        p <<= 1;
    p >>= 1;
    uint32_t p2 = p;
    p >>= 1;
    while (p >= 1) {
        size_t oy1 = size_t(oy) * p;
        size_t oy2 = size_t(oy) * p2;
        size_t ox1 = size_t(ox) * p;
        size_t ox2 = size_t(ox) * p2;
        size_t ey = size_t(oy) * (ny - p2);
        size_t ex = size_t(ox) * (nx - p2);
        size_t py = 0;
        for (; py <= ey; py += oy2) {
            size_t px = py;
            for (; px <= py + ex; px += ox2) {
                uint16_t* p00 = in + px;
                uint16_t* p01 = p00 + ox1;
                uint16_t* p10 = p00 + oy1;
                uint16_t* p11 = p10 + ox1;
                if (Narrow) {
                    // The four values of a square, whose intermediate sums are not reduced to 16 bits.
                    int32_t a = int16_t(*p00);
                    int32_t b = int16_t(*p10);
                    int32_t c = int16_t(*p01);
                    int32_t d = int16_t(*p11);
                    int32_t s00 = a + (b & 1) + (b >> 1);
                    int32_t s10 = s00 - b;
                    int32_t s01 = c + (d & 1) + (d >> 1);
                    int32_t s11 = s01 - d;
                    int32_t top = s00 + (s01 & 1) + (s01 >> 1);
                    int32_t bottom = s10 + (s11 & 1) + (s11 >> 1);
                    *p00 = uint16_t(top);
                    *p01 = uint16_t(top - s01);
                    *p10 = uint16_t(bottom);
                    *p11 = uint16_t(bottom - s11);
                } else {
                    uint16_t i00 = 0;
                    uint16_t i01 = 0;
                    uint16_t i10 = 0;
                    uint16_t i11 = 0;
                    wavelet16(*p00, *p10, &i00, &i10);
                    wavelet16(*p01, *p11, &i01, &i11);
                    wavelet16(i00, i01, p00, p01);
                    wavelet16(i10, i11, p10, p11);
                }
            }
            // An odd column.
            if ((nx & p) != 0) {
                uint16_t* p00 = in + px;
                uint16_t* p10 = p00 + oy1;
                uint16_t i00 = 0;
                if (Narrow)
                    wavelet14(*p00, *p10, &i00, p10);
                else
                    wavelet16(*p00, *p10, &i00, p10);
                *p00 = i00;
            }
        }
        // An odd line.
        if ((ny & p) != 0) {
            for (size_t px = py; px <= py + ex; px += ox2) {
                uint16_t* p00 = in + px;
                uint16_t* p01 = p00 + ox1;
                uint16_t i00 = 0;
                if (Narrow)
                    wavelet14(*p00, *p01, &i00, p01);
                else
                    wavelet16(*p00, *p01, &i00, p01);
                *p00 = i00;
            }
        }
        p2 = p;
        p >>= 1;
    }
}

} // namespace

PrStatus exrHuffmanDecode(ExrCodec& codec, PrByteView packed, uint16_t* out, size_t count) {
    // The coding begins with the lowest and the highest symbol of the code, the length of the table of code lengths,
    // which the decoder does not need, the bits of the coded values, and a word for the future.
    constexpr size_t kHeaderBytes = 20;
    if (packed.size < kHeaderBytes)
        return count == 0 ? PrStatus_Ok : PrStatus_InvalidData;
    uint32_t low = prMemReadLeU32(packed.data);
    uint32_t high = prMemReadLeU32(packed.data + 4);
    uint64_t bits = prMemReadLeU32(packed.data + 12);
    if (low >= kHuffmanSymbols || high >= kHuffmanSymbols || low > high)
        return PrStatus_InvalidData;
    if (codec.huffman == nullptr)
        codec.huffman = static_cast<ExrHuffman*>(prMemoryAlloc(sizeof(ExrHuffman)));
    ExrHuffman* h = codec.huffman;
    size_t tableBytes = 0;
    const uint8_t* table = packed.data + kHeaderBytes;
    if (!huffmanTables(h, table, packed.size - kHeaderBytes, low, high, &tableBytes))
        return PrStatus_InvalidData;
    size_t codedBytes = size_t((bits + 7) >> 3);
    if (codedBytes > packed.size - kHeaderBytes - tableBytes)
        return PrStatus_InvalidData;
    // The reader reads whole words past the coding's end, into zeros.
    if (codedBytes + 16 > 0xffffffffu)
        return PrStatus_InvalidData;
    codec.scratch2.resize(uint32_t(codedBytes + 16));
    prMemCopy(codec.scratch2.data, table + tableBytes, codedBytes);
    prMemZero(codec.scratch2.data + codedBytes, 16);

    // The highest symbol stands for a run: eight bits follow, the times that the value before it repeats.
    uint32_t run = high;
    BitReader reader{.bytes = codec.scratch2.data, .at = 0, .size = bits};
    size_t written = 0;
    const uint8_t* bytes = reader.bytes;
    while (reader.at < reader.size) {
        // Far from the end, one window of at least 57 bits decodes symbols until fewer bits are left than a code of the
        // table's length and a run's count take. A longer code ends the window, and the step below reads it.
        constexpr uint32_t kWindowBits = 57;
        constexpr uint32_t kReserve = kHuffmanTableBits + 8;
        while (reader.size - reader.at >= 64) {
            uint64_t window = prMemReadBeU64(bytes + (reader.at >> 3)) << (reader.at & 7);
            uint32_t left = kWindowBits;
            bool longCode = false;
            do {
                HuffmanEntry entry = h->table[window >> (64 - kHuffmanTableBits)];
                uint32_t length = entry & 63;
                if (length == 0) {
                    longCode = true;
                    break;
                }
                uint32_t symbol = entry >> 6;
                window <<= length;
                left -= length;
                reader.at += length;
                if (symbol == run) {
                    uint32_t repeats = uint32_t(window >> 56);
                    window <<= 8;
                    left -= 8;
                    reader.at += 8;
                    if (written == 0 || repeats > count - written)
                        return PrStatus_InvalidData;
                    uint16_t value = out[written - 1];
                    for (uint32_t i = 0; i < repeats; ++i)
                        out[written + i] = value;
                    written += repeats;
                } else {
                    if (written == count)
                        return PrStatus_InvalidData;
                    out[written++] = uint16_t(symbol);
                }
            } while (left >= kReserve);
            if (longCode)
                break;
        }
        if (reader.at >= reader.size)
            break;
        HuffmanEntry entry = h->table[reader.peek(kHuffmanTableBits)];
        uint32_t symbol = entry >> 6;
        uint32_t length = entry & 63;
        if (length == 0 && !huffmanLong(h, reader, &symbol, &length))
            return PrStatus_InvalidData;
        if (length > reader.size - reader.at)
            return PrStatus_InvalidData;
        reader.at += length;
        if (symbol == run) {
            if (reader.size - reader.at < 8 || written == 0)
                return PrStatus_InvalidData;
            uint32_t repeats = uint32_t(reader.peek(8));
            reader.at += 8;
            if (repeats > count - written)
                return PrStatus_InvalidData;
            uint16_t value = out[written - 1];
            for (uint32_t i = 0; i < repeats; ++i)
                out[written + i] = value;
            written += repeats;
            continue;
        }
        if (written == count)
            return PrStatus_InvalidData;
        out[written++] = uint16_t(symbol);
    }
    return written == count ? PrStatus_Ok : PrStatus_InvalidData;
}

PrStatus exrPizDecompress(ExrCodec& codec, const ExrBlock& block, PrByteView packed, uint8_t* out, size_t size) {
    if ((size & 1) != 0 || size / 2 > 0xffffffffu)
        return PrStatus_InvalidData;
    // The bitmap of the values that are used, from its first nonzero byte to its last.
    if (packed.size < 4)
        return PrStatus_InvalidData;
    uint32_t minNonZero = prMemReadLeU16(packed.data);
    uint32_t maxNonZero = prMemReadLeU16(packed.data + 2);
    size_t at = 4;
    if (maxNonZero >= kBitmapBytes)
        return PrStatus_InvalidData;
    uint8_t bitmap[kBitmapBytes] = {};
    if (minNonZero <= maxNonZero) {
        size_t bytes = maxNonZero - minNonZero + 1;
        if (bytes > packed.size - at)
            return PrStatus_InvalidData;
        prMemCopy(bitmap + minNonZero, packed.data + at, bytes);
        at += bytes;
    }
    // The values that are used, which the dense codes index. Zero is always among them.
    codec.words.resize(65536 + uint32_t(size / 2));
    uint16_t* lut = codec.words.data;
    uint32_t used = 0;
    for (uint32_t i = 0; i < 65536; ++i) {
        if (i == 0 || (bitmap[i >> 3] >> (i & 7) & 1) != 0)
            lut[used++] = uint16_t(i);
    }
    for (uint32_t i = used; i < 65536; ++i)
        lut[i] = 0;
    uint32_t maxValue = used - 1;

    if (packed.size - at < 4)
        return PrStatus_InvalidData;
    uint32_t codingBytes = prMemReadLeU32(packed.data + at);
    at += 4;
    if (codingBytes > packed.size - at)
        return PrStatus_InvalidData;
    uint16_t* words = codec.words.data + 65536;
    size_t count = size / 2;
    PrStatus status = exrHuffmanDecode(codec, PrByteView{packed.data + at, codingBytes}, words, count);
    if (status != PrStatus_Ok)
        return status;

    // Each channel's samples, all its lines one after another, hold one transform for each 16-bit word of a sample.
    uint16_t* plane = words;
    for (uint32_t c = 0; c < block.channelCount; ++c) {
        const ExrBlockChannel& channel = block.channels[c];
        uint32_t lanes = exrPixelBytes(channel.pixel) / 2;
        uint32_t nx = channel.width;
        uint32_t ny = channel.height;
        if (nx != 0 && ny != 0) {
            for (uint32_t lane = 0; lane < lanes; ++lane) {
                if (maxValue < (1u << 14))
                    waveletDecode<true>(plane + lane, nx, lanes, ny, lanes * nx);
                else
                    waveletDecode<false>(plane + lane, nx, lanes, ny, lanes * nx);
            }
        }
        plane += size_t(nx) * ny * lanes;
    }
    for (size_t i = 0; i < count; ++i)
        words[i] = lut[words[i]];

    // The channels' samples become the chunk's lines.
    size_t written = 0;
    for (uint32_t row = 0; row < block.height; ++row) {
        int32_t y = block.y + int32_t(row);
        const uint16_t* channelWords = words;
        for (uint32_t c = 0; c < block.channelCount; ++c) {
            const ExrBlockChannel& channel = block.channels[c];
            size_t lineWords = size_t(channel.width) * (exrPixelBytes(channel.pixel) / 2);
            if (channel.height != 0 && exrChannelSamples(channel, y)) {
                uint32_t line = channel.ySampling == 1 ? row : uint32_t(row / channel.ySampling);
                if (line >= channel.height || lineWords * 2 > size - written)
                    return PrStatus_InvalidData;
                const uint16_t* source = channelWords + size_t(line) * lineWords;
                for (size_t i = 0; i < lineWords; ++i)
                    prMemWriteLeU16(out + written + i * 2, source[i]);
                written += lineWords * 2;
            }
            channelWords += lineWords * channel.height;
        }
    }
    return written == size ? PrStatus_Ok : PrStatus_InvalidData;
}

} // namespace pr_impl
