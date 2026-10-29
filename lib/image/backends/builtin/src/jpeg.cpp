#include "image/backends/builtin/src/jpeg.h"

#include "pr/mem.h"
#include "pr/memory.h"

#include "image/backends/builtin/src/metadata.h"

namespace pr_impl {
namespace {

// The maximum decoded pixel count. Larger frames are rejected before allocation.
constexpr uint64_t kMaxPixels = 64u * 1024u * 1024u;

constexpr uint32_t kMaxComponents = 4;
constexpr uint32_t kBlockSamples = 64;
constexpr uint32_t kTableCount = 4;

// The natural position of each coefficient, in the order in which a block codes them (T.81 figure A.6).
constexpr uint8_t kZigZag[kBlockSamples] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// One Huffman table in the form that the decoder reads: the canonical code ranges per code length, and an index on the
// first eight bits for codes short enough to resolve in one step (T.81 annex C, F.2.2.3).
struct HuffTable {
    uint8_t symbols[256];
    int32_t minCode[17];
    int32_t maxCode[17];
    int32_t valueOffset[17];
    uint8_t fastLength[256];
    uint8_t fastSymbol[256];
    bool present;
};

// One frame component and the storage for its coefficients and samples. `coefficients` holds whole blocks in natural
// order, and `samples` holds the plane that the inverse transform writes. Both are padded to whole MCUs.
struct Component {
    int16_t* coefficients;
    uint8_t* samples;
    uint32_t blocksPerLine;
    uint32_t blocksPerColumn;
    uint32_t blocksPerLineForMcu;
    uint32_t blocksPerColumnForMcu;
    uint32_t downsampledWidth;
    uint32_t downsampledHeight;
    int32_t dcPrediction;
    uint8_t id;
    uint8_t h;
    uint8_t v;
    uint8_t quantIndex;
    uint8_t dcIndex;
    uint8_t acIndex;
};

// The frame as its headers describe it, with the tables that the scans decode against.
struct Jpeg {
    uint16_t quant[kTableCount][kBlockSamples];
    bool quantPresent[kTableCount];
    HuffTable dcTables[kTableCount];
    HuffTable acTables[kTableCount];
    Component components[kMaxComponents];
    uint32_t componentCount;
    uint32_t width;
    uint32_t height;
    uint32_t maxH;
    uint32_t maxV;
    uint32_t mcusPerLine;
    uint32_t mcusPerColumn;
    uint32_t restartInterval;
    int32_t adobeTransform;
    bool progressive;
    bool frame;
};

// The entropy-coded bit stream of one scan. Bytes are read through the 0xFF00 stuffing that the format inserts. If the
// data stops, at a marker or at the end of the input, the fill supplies zero bits so that every read terminates.
// `zeroBits` counts the zero bits at the low end of the buffer that no input byte backs. `usedPadding` records that a
// read consumed one of them, which means the scan asked for data that the file does not hold. Decoding one code peeks
// eight bits ahead, so peeking past the last code is normal, but consuming past it is not.
struct BitReader {
    const uint8_t* bytes;
    size_t size;
    size_t at;
    uint32_t buffer;
    uint32_t count;
    uint32_t zeroBits;
    bool usedPadding;
    bool marker;

    void fill(uint32_t need) {
        while (count < need) {
            uint32_t next = 0;
            if (marker || at >= size) {
                marker = true;
                zeroBits += 8;
            } else if (bytes[at] != 0xFF) {
                next = bytes[at];
                at += 1;
            } else if (at + 1 < size && bytes[at + 1] == 0x00) {
                next = 0xFF;
                at += 2;
            } else {
                marker = true;
                zeroBits += 8;
            }
            buffer = (buffer << 8) | next;
            count += 8;
        }
    }

    void consume(uint32_t bits) {
        count -= bits;
        if (count < zeroBits) {
            usedPadding = true;
            zeroBits = count;
        }
    }

    // Returns the next `bits` bits of the stream, at most sixteen, as an unsigned value.
    uint32_t read(uint32_t bits) {
        if (bits == 0) {
            return 0;
        }
        fill(bits);
        consume(bits);
        return (buffer >> count) & ((1u << bits) - 1);
    }

    uint32_t readBit() {
        return read(1);
    }

    // Returns the next eight bits without consuming them, for the Huffman index.
    uint32_t peek8() {
        fill(8);
        return (buffer >> (count - 8)) & 0xFF;
    }

    void skip(uint32_t bits) {
        consume(bits);
    }
};

// Moves the reader to the byte after the next restart marker, dropping the bits that pad the interval to a byte.
// Returns false if the bytes that follow are not a restart marker.
bool readRestart(BitReader& reader) {
    reader.buffer = 0;
    reader.count = 0;
    reader.zeroBits = 0;
    size_t scan = reader.at;
    while (scan + 1 < reader.size
           && !(reader.bytes[scan] == 0xFF && reader.bytes[scan + 1] != 0x00)) {
        scan += 1;
    }
    if (scan + 1 >= reader.size) {
        return false;
    }
    const uint8_t code = reader.bytes[scan + 1];
    if (code < 0xD0 || code > 0xD7) {
        return false;
    }
    reader.at = scan + 2;
    reader.marker = false;
    return true;
}

// Derives the code ranges and the fast index from the code lengths and symbols in a DHT segment. Returns false for
// lengths that describe more codes than a Huffman tree of sixteen levels holds (T.81 F.2.2.3).
bool buildHuffTable(HuffTable& table, const uint8_t* counts, const uint8_t* symbols,
                    uint32_t symbolCount) {
    for (uint32_t index = 0; index < symbolCount; ++index) {
        table.symbols[index] = symbols[index];
    }
    int32_t code = 0;
    uint32_t written = 0;
    for (uint32_t length = 1; length <= 16; ++length) {
        const uint32_t count = counts[length - 1];
        table.valueOffset[length] = int32_t(written) - code;
        table.minCode[length] = code;
        table.maxCode[length] = count != 0 ? code + int32_t(count) - 1 : -1;
        written += count;
        code = (code + int32_t(count)) << 1;
        // More codes of this length than the tree holds leave no room for the next length, and the symbol index would
        // read past the end.
        if (code > (1 << (length + 1))) {
            return false;
        }
    }
    for (uint32_t index = 0; index < 256; ++index) {
        table.fastLength[index] = 0;
    }
    uint32_t symbol = 0;
    code = 0;
    for (uint32_t length = 1; length <= 16; ++length) {
        for (uint32_t index = 0; index < counts[length - 1]; ++index) {
            if (length <= 8) {
                const uint32_t first = uint32_t(code) << (8 - length);
                for (uint32_t entry = 0; entry < (1u << (8 - length)); ++entry) {
                    table.fastLength[first + entry] = uint8_t(length);
                    table.fastSymbol[first + entry] = symbols[symbol];
                }
            }
            code += 1;
            symbol += 1;
        }
        code <<= 1;
    }
    table.present = true;
    return true;
}

// Returns the symbol of the next code in the stream, or -1 for bits that match no code of the table.
int32_t decodeHuff(BitReader& reader, const HuffTable& table) {
    const uint32_t index = reader.peek8();
    const uint32_t fast = table.fastLength[index];
    if (fast != 0) {
        reader.skip(fast);
        return table.fastSymbol[index];
    }
    int32_t code = int32_t(reader.read(1));
    for (uint32_t length = 1; length <= 16; ++length) {
        if (table.maxCode[length] >= code) {
            return table.symbols[uint32_t(code + table.valueOffset[length])];
        }
        code = (code << 1) | int32_t(reader.read(1));
    }
    return -1;
}

// Returns the signed value encoded by a magnitude category and its bits (T.81 F.2.2.1).
int32_t receiveAndExtend(BitReader& reader, uint32_t category) {
    if (category == 0) {
        return 0;
    }
    const int32_t value = int32_t(reader.read(category));
    return value < (1 << (category - 1)) ? value - (1 << category) + 1 : value;
}

// The state of one scan: the components that it interleaves, the coefficient band that it covers, and the successive
// approximation that it refines (T.81 B.2.3, G.1.1.1).
struct Scan {
    Component* components[kMaxComponents];
    uint32_t count;
    uint32_t spectralStart;
    uint32_t spectralEnd;
    uint32_t approximationHigh;
    uint32_t approximationLow;
    uint32_t eobRun;
};

// Adds one difference to the DC prediction. A malformed file can code an unbounded chain, so the sum wraps instead of
// going out of range.
int32_t advancePrediction(const Component& component, int32_t difference) {
    return int32_t(uint32_t(component.dcPrediction) + uint32_t(difference));
}

bool decodeBaselineBlock(Jpeg& jpeg, Component& component, BitReader& reader, int16_t* block) {
    const int32_t category = decodeHuff(reader, jpeg.dcTables[component.dcIndex]);
    if (category < 0 || category > 16) {
        return false;
    }
    component.dcPrediction =
        advancePrediction(component, receiveAndExtend(reader, uint32_t(category)));
    block[0] = int16_t(component.dcPrediction);
    uint32_t index = 1;
    while (index < kBlockSamples) {
        const int32_t symbol = decodeHuff(reader, jpeg.acTables[component.acIndex]);
        if (symbol < 0) {
            return false;
        }
        const uint32_t size = uint32_t(symbol) & 15;
        const uint32_t run = uint32_t(symbol) >> 4;
        if (size == 0) {
            if (run != 15) {
                break;
            }
            index += 16;
            continue;
        }
        index += run;
        if (index >= kBlockSamples) {
            return false;
        }
        block[kZigZag[index]] = int16_t(receiveAndExtend(reader, size));
        index += 1;
    }
    return true;
}

bool decodeDcFirst(Jpeg& jpeg, Component& component, BitReader& reader, Scan& scan,
                   int16_t* block) {
    const int32_t category = decodeHuff(reader, jpeg.dcTables[component.dcIndex]);
    if (category < 0 || category > 16) {
        return false;
    }
    component.dcPrediction =
        advancePrediction(component, receiveAndExtend(reader, uint32_t(category)));
    block[0] = int16_t(uint32_t(component.dcPrediction) << scan.approximationLow);
    return true;
}

bool decodeDcRefine(BitReader& reader, Scan& scan, int16_t* block) {
    if (reader.readBit() != 0) {
        block[0] = int16_t(uint32_t(block[0]) | (1u << scan.approximationLow));
    }
    return true;
}

bool decodeAcFirst(Jpeg& jpeg, Component& component, BitReader& reader, Scan& scan,
                   int16_t* block) {
    if (scan.eobRun > 0) {
        scan.eobRun -= 1;
        return true;
    }
    uint32_t index = scan.spectralStart;
    while (index <= scan.spectralEnd) {
        const int32_t symbol = decodeHuff(reader, jpeg.acTables[component.acIndex]);
        if (symbol < 0) {
            return false;
        }
        const uint32_t size = uint32_t(symbol) & 15;
        const uint32_t run = uint32_t(symbol) >> 4;
        if (size == 0) {
            if (run != 15) {
                scan.eobRun = (1u << run) - 1 + reader.read(run);
                break;
            }
            index += 16;
            continue;
        }
        index += run;
        if (index > scan.spectralEnd || index >= kBlockSamples) {
            return false;
        }
        block[kZigZag[index]] =
            int16_t(uint32_t(receiveAndExtend(reader, size)) << scan.approximationLow);
        index += 1;
    }
    return true;
}

// The correction pass over one band. Every coefficient that earlier scans made nonzero gets one more bit, and the runs
// between them place the newly nonzero ones (T.81 G.1.2.3).
bool decodeAcRefine(Jpeg& jpeg, Component& component, BitReader& reader, Scan& scan,
                    int16_t* block) {
    const int32_t bit = 1 << scan.approximationLow;
    uint32_t index = scan.spectralStart;
    if (scan.eobRun > 0) {
        scan.eobRun -= 1;
        while (index <= scan.spectralEnd) {
            int16_t& coefficient = block[kZigZag[index]];
            if (coefficient != 0 && reader.readBit() != 0 && (coefficient & bit) == 0) {
                coefficient = int16_t(coefficient >= 0 ? coefficient + bit : coefficient - bit);
            }
            index += 1;
        }
        return true;
    }
    while (index <= scan.spectralEnd) {
        const int32_t symbol = decodeHuff(reader, jpeg.acTables[component.acIndex]);
        if (symbol < 0) {
            return false;
        }
        const uint32_t size = uint32_t(symbol) & 15;
        uint32_t run = uint32_t(symbol) >> 4;
        int32_t value = 0;
        if (size == 0) {
            if (run != 15) {
                scan.eobRun = (1u << run) + reader.read(run);
                break;
            }
        } else {
            if (size != 1) {
                return false;
            }
            value = reader.readBit() != 0 ? bit : -bit;
        }
        while (index <= scan.spectralEnd) {
            int16_t& coefficient = block[kZigZag[index]];
            index += 1;
            if (coefficient != 0) {
                if (reader.readBit() != 0 && (coefficient & bit) == 0) {
                    coefficient = int16_t(coefficient >= 0 ? coefficient + bit : coefficient - bit);
                }
                continue;
            }
            if (run == 0) {
                if (value != 0) {
                    coefficient = int16_t(value);
                }
                break;
            }
            run -= 1;
        }
    }
    if (scan.eobRun > 0) {
        scan.eobRun -= 1;
        while (index <= scan.spectralEnd) {
            int16_t& coefficient = block[kZigZag[index]];
            if (coefficient != 0 && reader.readBit() != 0 && (coefficient & bit) == 0) {
                coefficient = int16_t(coefficient >= 0 ? coefficient + bit : coefficient - bit);
            }
            index += 1;
        }
    }
    return true;
}

int16_t* blockAt(const Component& component, uint32_t row, uint32_t column) {
    return component.coefficients
           + size_t(row * component.blocksPerLineForMcu + column) * kBlockSamples;
}

bool decodeBlock(Jpeg& jpeg, Scan& scan, Component& component, BitReader& reader, int16_t* block) {
    if (!jpeg.progressive) {
        return decodeBaselineBlock(jpeg, component, reader, block);
    }
    if (scan.spectralStart == 0) {
        return scan.approximationHigh == 0 ? decodeDcFirst(jpeg, component, reader, scan, block)
                                           : decodeDcRefine(reader, scan, block);
    }
    return scan.approximationHigh == 0 ? decodeAcFirst(jpeg, component, reader, scan, block)
                                       : decodeAcRefine(jpeg, component, reader, scan, block);
}

// Decodes the entropy-coded data of one scan. A single component walks its own blocks row by row. Several components
// walk the interleaved MCUs (T.81 A.2).
bool decodeScanData(Jpeg& jpeg, Scan& scan, BitReader& reader) {
    for (uint32_t index = 0; index < scan.count; ++index) {
        scan.components[index]->dcPrediction = 0;
    }
    scan.eobRun = 0;
    const bool single = scan.count == 1;
    const uint32_t unitsWide = single ? scan.components[0]->blocksPerLine : jpeg.mcusPerLine;
    const uint32_t unitsHigh = single ? scan.components[0]->blocksPerColumn : jpeg.mcusPerColumn;
    const uint32_t total = unitsWide * unitsHigh;
    const uint32_t interval = jpeg.restartInterval != 0 ? jpeg.restartInterval : total;
    uint32_t unit = 0;
    while (unit < total) {
        const uint32_t end = unit + interval < total ? unit + interval : total;
        for (; unit < end; ++unit) {
            const uint32_t unitRow = unit / unitsWide;
            const uint32_t unitColumn = unit % unitsWide;
            if (single) {
                Component& component = *scan.components[0];
                if (!decodeBlock(jpeg, scan, component, reader,
                                 blockAt(component, unitRow, unitColumn))) {
                    return false;
                }
                continue;
            }
            for (uint32_t index = 0; index < scan.count; ++index) {
                Component& component = *scan.components[index];
                for (uint32_t row = 0; row < component.v; ++row) {
                    for (uint32_t column = 0; column < component.h; ++column) {
                        int16_t* block = blockAt(component, unitRow * component.v + row,
                                                 unitColumn * component.h + column);
                        if (!decodeBlock(jpeg, scan, component, reader, block)) {
                            return false;
                        }
                    }
                }
            }
        }
        if (unit < total) {
            if (!readRestart(reader)) {
                return false;
            }
            for (uint32_t index = 0; index < scan.count; ++index) {
                scan.components[index]->dcPrediction = 0;
            }
            scan.eobRun = 0;
        }
    }
    return !reader.usedPadding;
}

// The fixed-point constants of the inverse transform, with thirteen fractional bits (T.81 A.3.3, computed as a
// separable integer transform by the Loeffler-Ligtenberg-Moschytz factorization).
constexpr int64_t kFix0_298631336 = 2446;
constexpr int64_t kFix0_390180644 = 3196;
constexpr int64_t kFix0_541196100 = 4433;
constexpr int64_t kFix0_765366865 = 6270;
constexpr int64_t kFix0_899976223 = 7373;
constexpr int64_t kFix1_175875602 = 9633;
constexpr int64_t kFix1_501321110 = 12299;
constexpr int64_t kFix1_847759065 = 15137;
constexpr int64_t kFix1_961570560 = 16069;
constexpr int64_t kFix2_053119869 = 16819;
constexpr int64_t kFix2_562915447 = 20995;
constexpr int64_t kFix3_072711026 = 25172;

constexpr uint32_t kConstBits = 13;
constexpr uint32_t kPassBits = 2;

int64_t descale(int64_t value, uint32_t bits) {
    return (value + (int64_t(1) << (bits - 1))) >> bits;
}

uint8_t clampSample(int64_t value) {
    if (value < 0) {
        return 0;
    }
    return value > 255 ? 255 : uint8_t(value);
}

// Converts the dequantized block back to samples. One pass over the columns writes a workspace with two extra
// fractional bits, and one pass over the rows writes the plane and applies the level shift of the format. The
// intermediates are 64-bit so that coefficients from a malformed file stay in range.
void inverseTransform(const int16_t* block, const uint16_t* quant, uint8_t* out, size_t stride) {
    int64_t work[kBlockSamples];
    for (uint32_t column = 0; column < 8; ++column) {
        const int16_t* in = block + column;
        const uint16_t* q = quant + column;
        if (in[8] == 0 && in[16] == 0 && in[24] == 0 && in[32] == 0 && in[40] == 0 && in[48] == 0
            && in[56] == 0) {
            const int64_t value = int64_t(in[0]) * q[0] << kPassBits;
            for (uint32_t row = 0; row < 8; ++row) {
                work[row * 8 + column] = value;
            }
            continue;
        }
        int64_t z2 = int64_t(in[16]) * q[16];
        int64_t z3 = int64_t(in[48]) * q[48];
        int64_t z1 = (z2 + z3) * kFix0_541196100;
        int64_t tmp2 = z1 - z3 * kFix1_847759065;
        int64_t tmp3 = z1 + z2 * kFix0_765366865;
        z2 = int64_t(in[0]) * q[0];
        z3 = int64_t(in[32]) * q[32];
        int64_t tmp0 = (z2 + z3) << kConstBits;
        int64_t tmp1 = (z2 - z3) << kConstBits;
        const int64_t even0 = tmp0 + tmp3;
        const int64_t even3 = tmp0 - tmp3;
        const int64_t even1 = tmp1 + tmp2;
        const int64_t even2 = tmp1 - tmp2;

        tmp0 = int64_t(in[56]) * q[56];
        tmp1 = int64_t(in[40]) * q[40];
        tmp2 = int64_t(in[24]) * q[24];
        tmp3 = int64_t(in[8]) * q[8];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        int64_t z4 = tmp1 + tmp3;
        const int64_t z5 = (z3 + z4) * kFix1_175875602;
        tmp0 *= kFix0_298631336;
        tmp1 *= kFix2_053119869;
        tmp2 *= kFix3_072711026;
        tmp3 *= kFix1_501321110;
        z1 *= -kFix0_899976223;
        z2 *= -kFix2_562915447;
        z3 = z5 - z3 * kFix1_961570560;
        z4 = z5 - z4 * kFix0_390180644;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        work[column] = descale(even0 + tmp3, kConstBits - kPassBits);
        work[7 * 8 + column] = descale(even0 - tmp3, kConstBits - kPassBits);
        work[8 + column] = descale(even1 + tmp2, kConstBits - kPassBits);
        work[6 * 8 + column] = descale(even1 - tmp2, kConstBits - kPassBits);
        work[2 * 8 + column] = descale(even2 + tmp1, kConstBits - kPassBits);
        work[5 * 8 + column] = descale(even2 - tmp1, kConstBits - kPassBits);
        work[3 * 8 + column] = descale(even3 + tmp0, kConstBits - kPassBits);
        work[4 * 8 + column] = descale(even3 - tmp0, kConstBits - kPassBits);
    }
    for (uint32_t row = 0; row < 8; ++row) {
        const int64_t* in = work + row * 8;
        uint8_t* line = out + row * stride;
        int64_t z2 = in[2];
        int64_t z3 = in[6];
        int64_t z1 = (z2 + z3) * kFix0_541196100;
        int64_t tmp2 = z1 - z3 * kFix1_847759065;
        int64_t tmp3 = z1 + z2 * kFix0_765366865;
        int64_t tmp0 = (in[0] + in[4]) << kConstBits;
        int64_t tmp1 = (in[0] - in[4]) << kConstBits;
        const int64_t even0 = tmp0 + tmp3;
        const int64_t even3 = tmp0 - tmp3;
        const int64_t even1 = tmp1 + tmp2;
        const int64_t even2 = tmp1 - tmp2;

        tmp0 = in[7];
        tmp1 = in[5];
        tmp2 = in[3];
        tmp3 = in[1];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        int64_t z4 = tmp1 + tmp3;
        const int64_t z5 = (z3 + z4) * kFix1_175875602;
        tmp0 *= kFix0_298631336;
        tmp1 *= kFix2_053119869;
        tmp2 *= kFix3_072711026;
        tmp3 *= kFix1_501321110;
        z1 *= -kFix0_899976223;
        z2 *= -kFix2_562915447;
        z3 = z5 - z3 * kFix1_961570560;
        z4 = z5 - z4 * kFix0_390180644;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        const uint32_t shift = kConstBits + kPassBits + 3;
        line[0] = clampSample(descale(even0 + tmp3, shift) + 128);
        line[7] = clampSample(descale(even0 - tmp3, shift) + 128);
        line[1] = clampSample(descale(even1 + tmp2, shift) + 128);
        line[6] = clampSample(descale(even1 - tmp2, shift) + 128);
        line[2] = clampSample(descale(even2 + tmp1, shift) + 128);
        line[5] = clampSample(descale(even2 - tmp1, shift) + 128);
        line[3] = clampSample(descale(even3 + tmp0, shift) + 128);
        line[4] = clampSample(descale(even3 - tmp0, shift) + 128);
    }
}

void transformComponent(const Jpeg& jpeg, Component& component) {
    const size_t stride = size_t(component.blocksPerLineForMcu) * 8;
    for (uint32_t row = 0; row < component.blocksPerColumnForMcu; ++row) {
        for (uint32_t column = 0; column < component.blocksPerLineForMcu; ++column) {
            const int16_t* block = blockAt(component, row, column);
            uint8_t* out = component.samples + size_t(row) * 8 * stride + size_t(column) * 8;
            inverseTransform(block, jpeg.quant[component.quantIndex], out, stride);
        }
    }
}

// Writes one output row of a component at full frame resolution. A component at the resolution of the frame on an axis,
// or at half of it, uses a triangle filter for upsampling. The column sums mix the two nearest rows with weights three
// and one. Each pair of output columns mixes the two nearest sums the same way. Any other ratio repeats the nearest
// sample. At the frame edge, the edge row and column repeat, which leaves the outermost samples unmixed.
void upsampleRow(const Component& component, const Jpeg& jpeg, uint32_t y, uint8_t* out) {
    const size_t stride = size_t(component.blocksPerLineForMcu) * 8;
    const uint32_t sourceWidth = component.downsampledWidth;
    const uint32_t lastRow = component.downsampledHeight - 1;
    const bool halfWidth = component.h * 2 == jpeg.maxH;
    const bool halfHeight = component.v * 2 == jpeg.maxV;
    const bool fullWidth = component.h == jpeg.maxH;
    const bool fullHeight = component.v == jpeg.maxV;

    if (!(fullWidth || halfWidth) || !(fullHeight || halfHeight)) {
        const uint32_t row = uint32_t(uint64_t(y) * component.v / jpeg.maxV);
        const uint8_t* line = component.samples + size_t(row < lastRow ? row : lastRow) * stride;
        for (uint32_t x = 0; x < jpeg.width; ++x) {
            const uint32_t source = uint32_t(uint64_t(x) * component.h / jpeg.maxH);
            out[x] = line[source < sourceWidth ? source : sourceWidth - 1];
        }
        return;
    }

    uint32_t nearRow = halfHeight ? y / 2 : y;
    nearRow = nearRow < lastRow ? nearRow : lastRow;
    uint32_t farRow = nearRow;
    if (halfHeight) {
        if ((y & 1) == 0) {
            farRow = nearRow > 0 ? nearRow - 1 : 0;
        } else {
            farRow = nearRow < lastRow ? nearRow + 1 : lastRow;
        }
    }
    const uint8_t* nearLine = component.samples + size_t(nearRow) * stride;
    const uint8_t* farLine = component.samples + size_t(farRow) * stride;
    const uint32_t nearWeight = halfHeight ? 3 : 4;
    const uint32_t farWeight = halfHeight ? 1 : 0;

    // The two output samples of a filtered pair round halves in opposite directions, so expanding an axis adds no bias
    // to the plane.
    if (!halfWidth) {
        const uint32_t bias = halfHeight && (y & 1) == 0 ? 1 : 2;
        for (uint32_t x = 0; x < jpeg.width; ++x) {
            const uint32_t source = x < sourceWidth ? x : sourceWidth - 1;
            out[x] =
                uint8_t((nearLine[source] * nearWeight + farLine[source] * farWeight + bias) >> 2);
        }
        return;
    }
    const uint32_t leftBias = halfHeight ? 8 : 4;
    const uint32_t rightBias = halfHeight ? 7 : 8;
    uint32_t current = nearLine[0] * nearWeight + farLine[0] * farWeight;
    uint32_t previous = current;
    uint32_t next = sourceWidth > 1 ? nearLine[1] * nearWeight + farLine[1] * farWeight : current;
    for (uint32_t x = 0; x < sourceWidth && x * 2 < jpeg.width; ++x) {
        out[x * 2] = uint8_t((current * 3 + previous + leftBias) >> 4);
        if (x * 2 + 1 < jpeg.width) {
            out[x * 2 + 1] = uint8_t((current * 3 + next + rightBias) >> 4);
        }
        previous = current;
        current = next;
        const uint32_t ahead = x + 2;
        next = ahead < sourceWidth ? nearLine[ahead] * nearWeight + farLine[ahead] * farWeight
                                   : current;
    }
}

// The color conversion constants at sixteen fractional bits (JFIF 7), and the half that rounds a product of them.
constexpr int32_t kRedFromCr = 91881;
constexpr int32_t kBlueFromCb = 116130;
constexpr int32_t kGreenFromCb = -22554;
constexpr int32_t kGreenFromCr = -46802;
constexpr int32_t kColorHalf = 1 << 15;

void yccToRgb(int32_t y, int32_t cb, int32_t cr, uint8_t* out) {
    const int32_t blue = cb - 128;
    const int32_t red = cr - 128;
    out[0] = clampSample(y + ((kRedFromCr * red + kColorHalf) >> 16));
    out[1] = clampSample(y + ((kGreenFromCb * blue + kGreenFromCr * red + kColorHalf) >> 16));
    out[2] = clampSample(y + ((kBlueFromCb * blue + kColorHalf) >> 16));
}

// Returns true if three components are stored as red, green and blue instead of luma and chroma. An Adobe marker that
// names no transform indicates this, as do the conventional RGB component ids.
bool componentsAreRgb(const Jpeg& jpeg) {
    if (jpeg.adobeTransform >= 0) {
        return jpeg.adobeTransform == 0;
    }
    return jpeg.components[0].id == 'R' && jpeg.components[1].id == 'G'
           && jpeg.components[2].id == 'B';
}

// Writes canonical native rows, upsampling each component to full resolution.
void writePixels(const Jpeg& jpeg, uint8_t* pixels) {
    size_t rowSize = size_t(jpeg.width) + 8;
    uint8_t* storage = static_cast<uint8_t*>(prMemoryAlloc(rowSize * jpeg.componentCount));
    uint8_t* rows[kMaxComponents] = {};
    for (uint32_t index = 0; index < jpeg.componentCount; ++index) {
        rows[index] = storage + rowSize * index;
    }
    const bool rgb = jpeg.componentCount == 3 && componentsAreRgb(jpeg);
    const bool ycck = jpeg.componentCount == 4 && jpeg.adobeTransform == 2;
    const bool inverted = jpeg.componentCount == 4 && jpeg.adobeTransform >= 0;
    for (uint32_t y = 0; y < jpeg.height; ++y) {
        for (uint32_t index = 0; index < jpeg.componentCount; ++index) {
            upsampleRow(jpeg.components[index], jpeg, y, rows[index]);
        }
        uint8_t* out = pixels + size_t(y) * jpeg.width * jpeg.componentCount;
        for (uint32_t x = 0; x < jpeg.width; ++x) {
            uint8_t* pixel = out + size_t(x) * jpeg.componentCount;
            if (jpeg.componentCount == 4) {
                if (ycck) {
                    yccToRgb(rows[0][x], rows[1][x], rows[2][x], pixel);
                    pixel[3] = uint8_t(255 - rows[3][x]);
                } else {
                    for (uint32_t channel = 0; channel < 4; ++channel) {
                        pixel[channel] =
                            inverted ? uint8_t(255 - rows[channel][x]) : rows[channel][x];
                    }
                }
            } else if (jpeg.componentCount == 3) {
                if (rgb) {
                    pixel[0] = rows[0][x];
                    pixel[1] = rows[1][x];
                    pixel[2] = rows[2][x];
                } else {
                    yccToRgb(rows[0][x], rows[1][x], rows[2][x], pixel);
                }
            } else {
                pixel[0] = rows[0][x];
            }
        }
    }
    prMemoryFree(storage);
}

// Reads a frame header: the sample precision, the dimensions, and the components with their sampling factors. Together
// these determine the geometry of every plane (T.81 B.2.2).
bool readFrame(Jpeg& jpeg, const uint8_t* segment, uint32_t size, uint8_t marker) {
    if (jpeg.frame || size < 6 || segment[0] != 8) {
        return false;
    }
    jpeg.progressive = marker == 0xC2;
    jpeg.height = readBeU16(segment + 1);
    jpeg.width = readBeU16(segment + 3);
    jpeg.componentCount = segment[5];
    if (jpeg.width == 0 || jpeg.height == 0 || jpeg.componentCount == 0
        || jpeg.componentCount > kMaxComponents
        || uint64_t(jpeg.width) * uint64_t(jpeg.height) > kMaxPixels
        || size < 6 + jpeg.componentCount * 3) {
        return false;
    }
    jpeg.maxH = 1;
    jpeg.maxV = 1;
    for (uint32_t index = 0; index < jpeg.componentCount; ++index) {
        Component& component = jpeg.components[index];
        const uint8_t* entry = segment + 6 + index * 3;
        component.id = entry[0];
        component.h = entry[1] >> 4;
        component.v = entry[1] & 15;
        component.quantIndex = entry[2];
        if (component.h == 0 || component.h > 4 || component.v == 0 || component.v > 4
            || component.quantIndex >= kTableCount) {
            return false;
        }
        jpeg.maxH = component.h > jpeg.maxH ? component.h : jpeg.maxH;
        jpeg.maxV = component.v > jpeg.maxV ? component.v : jpeg.maxV;
    }
    jpeg.mcusPerLine = (jpeg.width + jpeg.maxH * 8 - 1) / (jpeg.maxH * 8);
    jpeg.mcusPerColumn = (jpeg.height + jpeg.maxV * 8 - 1) / (jpeg.maxV * 8);
    jpeg.frame = true;
    return true;
}

// Allocates the coefficient and sample planes for every component of the frame, sized to whole MCUs so that an
// interleaved scan writes inside them.
bool allocatePlanes(Jpeg& jpeg) {
    for (uint32_t index = 0; index < jpeg.componentCount; ++index) {
        Component& component = jpeg.components[index];
        component.downsampledWidth =
            uint32_t((uint64_t(jpeg.width) * component.h + jpeg.maxH - 1) / jpeg.maxH);
        component.downsampledHeight =
            uint32_t((uint64_t(jpeg.height) * component.v + jpeg.maxV - 1) / jpeg.maxV);
        component.blocksPerLine = (component.downsampledWidth + 7) / 8;
        component.blocksPerColumn = (component.downsampledHeight + 7) / 8;
        component.blocksPerLineForMcu = jpeg.mcusPerLine * component.h;
        component.blocksPerColumnForMcu = jpeg.mcusPerColumn * component.v;
        const uint64_t blocks =
            uint64_t(component.blocksPerLineForMcu) * uint64_t(component.blocksPerColumnForMcu);
        if (blocks * kBlockSamples > kMaxPixels * 4) {
            return false;
        }
        prMemoryFree(component.coefficients);
        prMemoryFree(component.samples);
        component.coefficients = static_cast<int16_t*>(
            prMemoryAllocZeroed(size_t(blocks) * kBlockSamples * sizeof(int16_t)));
        component.samples = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(blocks) * kBlockSamples));
    }
    return true;
}

bool readQuantTables(Jpeg& jpeg, const uint8_t* segment, uint32_t size) {
    uint32_t at = 0;
    while (at < size) {
        const uint32_t precision = segment[at] >> 4;
        const uint32_t index = segment[at] & 15;
        at += 1;
        const uint32_t bytes = precision == 0 ? 64 : 128;
        if (index >= kTableCount || precision > 1 || at + bytes > size) {
            return false;
        }
        for (uint32_t entry = 0; entry < kBlockSamples; ++entry) {
            jpeg.quant[index][kZigZag[entry]] = precision == 0
                                                    ? segment[at + entry]
                                                    : uint16_t(readBeU16(segment + at + entry * 2));
        }
        jpeg.quantPresent[index] = true;
        at += bytes;
    }
    return true;
}

bool readHuffTables(Jpeg& jpeg, const uint8_t* segment, uint32_t size) {
    uint32_t at = 0;
    while (at < size) {
        const uint32_t kind = segment[at] >> 4;
        const uint32_t index = segment[at] & 15;
        at += 1;
        if (kind > 1 || index >= kTableCount || at + 16 > size) {
            return false;
        }
        uint32_t symbols = 0;
        for (uint32_t length = 0; length < 16; ++length) {
            symbols += segment[at + length];
        }
        if (symbols > 256 || at + 16 + symbols > size) {
            return false;
        }
        HuffTable& table = kind == 0 ? jpeg.dcTables[index] : jpeg.acTables[index];
        if (!buildHuffTable(table, segment + at, segment + at + 16, symbols)) {
            return false;
        }
        at += 16 + symbols;
    }
    return true;
}

// Reads a scan header, which names the components that the scan interleaves and the coefficient band that it covers.
// The tables that it selects must already be defined (T.81 B.2.3).
bool readScan(Jpeg& jpeg, Scan& scan, const uint8_t* segment, uint32_t size) {
    if (!jpeg.frame || size < 1) {
        return false;
    }
    scan.count = segment[0];
    if (scan.count == 0 || scan.count > jpeg.componentCount || size < 1 + scan.count * 2 + 3) {
        return false;
    }
    for (uint32_t index = 0; index < scan.count; ++index) {
        const uint8_t id = segment[1 + index * 2];
        const uint8_t tables = segment[2 + index * 2];
        Component* found = nullptr;
        for (uint32_t search = 0; search < jpeg.componentCount; ++search) {
            if (jpeg.components[search].id == id) {
                found = &jpeg.components[search];
            }
        }
        if (found == nullptr || (tables >> 4) >= kTableCount || (tables & 15) >= kTableCount) {
            return false;
        }
        found->dcIndex = tables >> 4;
        found->acIndex = tables & 15;
        scan.components[index] = found;
    }
    const uint8_t* tail = segment + 1 + scan.count * 2;
    if (jpeg.progressive) {
        scan.spectralStart = tail[0];
        scan.spectralEnd = tail[1];
        scan.approximationHigh = tail[2] >> 4;
        scan.approximationLow = tail[2] & 15;
        // A DC scan covers only that coefficient, and an AC scan covers only one component.
        if (scan.spectralStart > scan.spectralEnd || scan.spectralEnd >= kBlockSamples
            || scan.approximationLow > 13 || (scan.spectralStart == 0 && scan.spectralEnd != 0)
            || (scan.spectralStart != 0 && scan.count != 1)) {
            return false;
        }
    } else {
        scan.spectralStart = 0;
        scan.spectralEnd = kBlockSamples - 1;
        scan.approximationHigh = 0;
        scan.approximationLow = 0;
    }
    for (uint32_t index = 0; index < scan.count; ++index) {
        const Component& component = *scan.components[index];
        const bool dc = scan.spectralStart == 0;
        if (!jpeg.quantPresent[component.quantIndex]) {
            return false;
        }
        if (dc && scan.approximationHigh == 0 && !jpeg.dcTables[component.dcIndex].present) {
            return false;
        }
        if ((!dc || !jpeg.progressive) && !jpeg.acTables[component.acIndex].present) {
            return false;
        }
    }
    return true;
}

// Reads the Adobe application segment, which specifies how the components are stored: zero for no transform, one for
// luma and chroma, two for the four-ink form.
void readAdobe(Jpeg& jpeg, const uint8_t* segment, uint32_t size) {
    const uint8_t tag[5] = {'A', 'd', 'o', 'b', 'e'};
    if (size < 12) {
        return;
    }
    for (uint32_t index = 0; index < 5; ++index) {
        if (segment[index] != tag[index]) {
            return;
        }
    }
    jpeg.adobeTransform = segment[11];
}

// Reads the JFIF application segment, which records the pixel density of the stored image, in pixels per inch or per
// centimetre, or an aspect ratio that specifies no resolution (JFIF 1.02).
void readJfif(const uint8_t* segment, uint32_t size, ImageMetadata* out) {
    const uint8_t tag[5] = {'J', 'F', 'I', 'F', 0};
    if (size < 12 || !memEquals(segment, tag, sizeof(tag))) {
        return;
    }
    const uint32_t units = segment[7];
    const float densityX = float(readBeU16(segment + 8));
    const float densityY = float(readBeU16(segment + 10));
    if (units == 1) {
        out->pixelsPerMeterX = imagePixelsPerMeterFromInch(densityX);
        out->pixelsPerMeterY = imagePixelsPerMeterFromInch(densityY);
    } else if (units == 2) {
        out->pixelsPerMeterX = imagePixelsPerMeterFromCm(densityX);
        out->pixelsPerMeterY = imagePixelsPerMeterFromCm(densityY);
    }
}

// Reads the Exif application segment. It holds a TIFF header, from which the shared reader takes the orientation and
// resolution.
void readExifSegment(const uint8_t* segment, uint32_t size, ImageMetadata* out) {
    const uint8_t tag[6] = {'E', 'x', 'i', 'f', 0, 0};
    if (size <= sizeof(tag) || !memEquals(segment, tag, sizeof(tag))) {
        return;
    }
    imageReadExif(segment + sizeof(tag), size - sizeof(tag), out);
}

// Returns true if a marker is followed by a segment body. Markers 0x00, 0x01 and 0xD0 through 0xD9 have no body (T.81
// B.1).
bool hasSegment(uint8_t marker) {
    return marker != 0x00 && marker != 0x01 && !(marker >= 0xD0 && marker <= 0xD9);
}

bool decode(Jpeg& jpeg, const uint8_t* bytes, size_t size, uint8_t* pixels, size_t pixelsSize) {
    size_t at = 2;
    bool scanned = false;
    while (at + 1 < size) {
        if (bytes[at] != 0xFF) {
            at += 1;
            continue;
        }
        while (at + 1 < size && bytes[at + 1] == 0xFF) {
            at += 1;
        }
        if (at + 1 >= size) {
            break;
        }
        const uint8_t marker = bytes[at + 1];
        at += 2;
        if (marker == 0xD9) {
            break;
        }
        if (!hasSegment(marker)) {
            continue;
        }
        if (at + 2 > size) {
            return false;
        }
        const uint32_t length = readBeU16(bytes + at);
        if (length < 2 || size_t(length) > size - at) {
            return false;
        }
        const uint8_t* segment = bytes + at + 2;
        const uint32_t segmentSize = length - 2;
        at += length;
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            if (!readFrame(jpeg, segment, segmentSize, marker)
                || uint64_t(jpeg.width) * uint64_t(jpeg.height) * jpeg.componentCount
                       != uint64_t(pixelsSize)
                || !allocatePlanes(jpeg)) {
                return false;
            }
        } else if (marker == 0xDB) {
            if (!readQuantTables(jpeg, segment, segmentSize)) {
                return false;
            }
        } else if (marker == 0xC4) {
            if (!readHuffTables(jpeg, segment, segmentSize)) {
                return false;
            }
        } else if (marker == 0xDD) {
            if (segmentSize < 2) {
                return false;
            }
            jpeg.restartInterval = readBeU16(segment);
        } else if (marker == 0xEE) {
            readAdobe(jpeg, segment, segmentSize);
        } else if (marker == 0xDA) {
            Scan scan = {};
            if (!readScan(jpeg, scan, segment, segmentSize)) {
                return false;
            }
            BitReader reader = {};
            reader.bytes = bytes;
            reader.size = size;
            reader.at = at;
            if (!decodeScanData(jpeg, scan, reader)) {
                return false;
            }
            at = reader.at;
            scanned = true;
        } else if (marker >= 0xC3 && marker <= 0xCF) {
            // The frame types other than Huffman-coded sequential and progressive: lossless, hierarchical and
            // arithmetic coding.
            return false;
        }
    }
    if (!jpeg.frame || !scanned) {
        return false;
    }
    for (uint32_t index = 0; index < jpeg.componentCount; ++index) {
        transformComponent(jpeg, jpeg.components[index]);
    }
    writePixels(jpeg, pixels);
    return true;
}

} // namespace

void jpegMetadata(PrByteView bytes, ImageMetadata* out) {
    if (bytes.size < 4 || bytes.data[0] != 0xFF || bytes.data[1] != 0xD8) {
        return;
    }
    size_t at = 2;
    while (at + 4 <= bytes.size) {
        if (bytes.data[at] != 0xFF) {
            at += 1;
            continue;
        }
        const uint8_t marker = bytes.data[at + 1];
        at += 2;
        if (marker == 0xDA || marker == 0xD9) {
            return;
        }
        if (!hasSegment(marker)) {
            continue;
        }
        if (at + 2 > bytes.size) {
            return;
        }
        const uint32_t length = readBeU16(bytes.data + at);
        if (length < 2 || size_t(length) > bytes.size - at) {
            return;
        }
        if (marker == 0xE0) {
            readJfif(bytes.data + at + 2, length - 2, out);
        } else if (marker == 0xE1) {
            readExifSegment(bytes.data + at + 2, length - 2, out);
        }
        at += length;
    }
}

bool jpegDecode(PrByteView bytes, uint8_t* pixels, size_t pixelsSize) {
    if (bytes.size < 4 || bytes.data[0] != 0xFF || bytes.data[1] != 0xD8) {
        return false;
    }
    Jpeg jpeg = {};
    jpeg.adobeTransform = -1;
    const bool ok = decode(jpeg, bytes.data, bytes.size, pixels, pixelsSize);
    for (Component& component : jpeg.components) {
        prMemoryFree(component.coefficients);
        prMemoryFree(component.samples);
    }
    return ok;
}

} // namespace pr_impl
