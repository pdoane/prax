#include "webp/src/vp8.h"

#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/memory.h"

#include "webp/src/vp8_tables.h"

namespace pr_impl {

namespace {

// The bytes of a key frame before its first partition: the frame tag, the start code and the size (rfc 6386 9.1).
constexpr size_t kFrameHeaderBytes = 10;

constexpr uint32_t kSegmentCount = 4;
constexpr uint32_t kMaxPartitions = 8;

// The prediction modes of a macroblock's luma and chroma (rfc 6386 11.2) and of a luma subblock (11.3), as their trees
// number them.
enum {
    Vp8Mode_Dc,
    Vp8Mode_Vertical,
    Vp8Mode_Horizontal,
    Vp8Mode_TrueMotion,
};
typedef uint8_t Vp8Mode;

enum {
    Vp8Subblock_Dc,
    Vp8Subblock_TrueMotion,
    Vp8Subblock_Vertical,
    Vp8Subblock_Horizontal,
    Vp8Subblock_DownLeft,
    Vp8Subblock_DownRight,
    Vp8Subblock_VerticalRight,
    Vp8Subblock_VerticalLeft,
    Vp8Subblock_HorizontalDown,
    Vp8Subblock_HorizontalUp,
};
typedef uint8_t Vp8Subblock;

// The tree of subblock modes (rfc 6386 11.4). A leaf is a mode negated.
constexpr int8_t kSubblockTree[18] = {
    -Vp8Subblock_Dc, 2,
    -Vp8Subblock_TrueMotion, 4,
    -Vp8Subblock_Vertical, 6,
    8, 12,
    -Vp8Subblock_Horizontal, 10,
    -Vp8Subblock_DownRight, -Vp8Subblock_VerticalRight,
    -Vp8Subblock_DownLeft, 14,
    -Vp8Subblock_VerticalLeft, 16,
    -Vp8Subblock_HorizontalDown, -Vp8Subblock_HorizontalUp,
};

// The coefficient band of each position (rfc 6386 13.3), and one past the last for the probabilities that follow it.
constexpr uint8_t kBands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};

// The position in a block of each coefficient in the order that the stream gives them (rfc 6386 13).
constexpr uint8_t kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

// The probabilities of the extra bits of the token categories 3 to 6, each list ending at zero (rfc 6386 13.2).
constexpr uint8_t kCategory3[] = {173, 148, 140, 0};
constexpr uint8_t kCategory4[] = {176, 155, 140, 135, 0};
constexpr uint8_t kCategory5[] = {180, 157, 141, 134, 130, 0};
constexpr uint8_t kCategory6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
constexpr const uint8_t* kCategories[4] = {kCategory3, kCategory4, kCategory5, kCategory6};

// The block types of the coefficient probabilities (rfc 6386 13.3).
enum {
    Vp8Block_LumaAfterDc,
    Vp8Block_LumaDc,
    Vp8Block_Chroma,
    Vp8Block_Luma,
};
typedef uint8_t Vp8Block;

// The boolean entropy decoder (rfc 6386 7), which holds the range less one, and the bits of the stream that it has read
// past the eight that it compares, as the reference decoder does. Past the end of its data it reads one zero byte and
// records that it did.
struct BoolDecoder {
    const uint8_t* at;
    const uint8_t* end;
    uint64_t value;
    int32_t bits;
    uint32_t range;
    bool eof;

    void init(const uint8_t* data, size_t size) {
        at = data;
        end = data + size;
        value = 0;
        bits = -8;
        range = 255 - 1;
        eof = false;
        load();
    }

    // Loads seven bytes at once while eight remain, which two big-endian words read.
    PR_FORCE_INLINE void load() {
        if (end - at >= 8) {
            const uint64_t next = (uint64_t(prMemReadBeU32(at)) << 24) | (prMemReadBeU32(at + 4) >> 8);
            at += 7;
            value = (value << 56) | next;
            bits += 56;
            return;
        }
        loadSlow();
    }

    PR_NOINLINE void loadSlow() {
        if (end - at >= 7) {
            uint64_t next = 0;
            for (int i = 0; i < 7; ++i)
                next = (next << 8) | at[i];
            at += 7;
            value = (value << 56) | next;
            bits += 56;
        } else if (at < end) {
            value = (value << 8) | *at++;
            bits += 8;
        } else if (!eof) {
            value <<= 8;
            bits += 8;
            eof = true;
        } else {
            bits = 0;
        }
    }

    // Decodes a bit without a branch on its value, which a branch would mispredict as often as the bit is uncertain.
    PR_FORCE_INLINE uint32_t bit(uint32_t probability) {
        uint32_t span = range;
        if (bits < 0)
            load();
        const uint32_t split = (span * probability) >> 8;
        const uint32_t window = uint32_t(value >> bits);
        const uint32_t one = window > split ? 1 : 0;
        const uint64_t mask = uint64_t(0) - one;
        value -= (uint64_t(split + 1) << bits) & mask;
        span = one != 0 ? span - split : split + 1;
        const uint32_t shift = 7 ^ prU64HighestBit(span);
        span <<= shift;
        bits -= int32_t(shift);
        range = span - 1;
        return one;
    }

    // Decodes a bit of probability one half and returns `magnitude` negated where it is set, as a coefficient's sign
    // is. Half of the range is never less than a quarter of the largest, so the range takes one shift alone.
    PR_FORCE_INLINE int32_t sign(int32_t magnitude) {
        if (bits < 0)
            load();
        const int32_t position = bits;
        const uint32_t split = range >> 1;
        const uint32_t window = uint32_t(value >> position);
        const int32_t mask = int32_t(split - window) >> 31;
        bits = position - 1;
        range = (range + uint32_t(mask)) | 1;
        value -= uint64_t((split + 1) & uint32_t(mask)) << position;
        return (magnitude ^ mask) - mask;
    }

    uint32_t literal(uint32_t count) {
        uint32_t out = 0;
        while (count-- > 0)
            out |= bit(0x80) << count;
        return out;
    }

    int32_t signedLiteral(uint32_t count) {
        const int32_t magnitude = int32_t(literal(count));
        return bit(0x80) != 0 ? -magnitude : magnitude;
    }

    // An optional signed field: a flag, then the field if the flag is set.
    int32_t optionalSigned(uint32_t count) {
        return bit(0x80) != 0 ? signedLiteral(count) : 0;
    }
};

// The dequantization factors of a segment's DC and AC coefficients of each kind of block (rfc 6386 14.1).
struct Quant {
    int32_t luma[2];
    int32_t lumaDc[2];
    int32_t chroma[2];
};

// How the loop filter treats a macroblock (rfc 6386 15).
struct FilterInfo {
    uint8_t limit;      // the edge limit of subblock edges, or zero for an unfiltered macroblock
    uint8_t interior;
    uint8_t hevThreshold;
    uint8_t inner;      // whether the subblock edges are filtered
};

// What the first partition says of a macroblock.
struct Macroblock {
    uint8_t segment;
    uint8_t skip;
    uint8_t subblocks;      // whether luma predicts by subblock
    uint8_t lumaMode;
    uint8_t chromaMode;
    uint8_t modes[16];
};

// The nonzero flags of a macroblock's edge, for the contexts of the coefficients of its neighbours: one bit for each
// luma subblock column or row, then two for each chroma plane's, and the luma DC.
struct Nonzero {
    uint32_t blocks;
    uint32_t dc;
};

int32_t clip(int32_t value, int32_t most) {
    return value < 0 ? 0 : value > most ? most : value;
}

uint8_t clip8(int32_t value) {
    return uint8_t(value < 0 ? 0 : value > 255 ? 255 : value);
}

struct Header {
    bool simpleFilter;
    uint32_t filterLevel;
    uint32_t sharpness;
    bool useSegments;
    bool updateMap;
    bool absoluteDelta;
    int32_t segmentQuant[kSegmentCount];
    int32_t segmentFilter[kSegmentCount];
    uint8_t segmentProbabilities[3];
    bool useDeltas;
    int32_t referenceDelta[4];
    int32_t modeDelta[4];
    bool useSkip;
    uint8_t skipProbability;
    Quant quant[kSegmentCount];
    FilterInfo filters[kSegmentCount][2];
    uint8_t probabilities[4][8][3][11];

    // The probabilities of each kind of block by the position of a token, through its band, which the tokens read in
    // one step.
    uint8_t positions[4][17][3][11];
};

void readSegments(BoolDecoder& br, Header& header) {
    header.useSegments = br.bit(0x80) != 0;
    header.absoluteDelta = true;
    header.segmentProbabilities[0] = header.segmentProbabilities[1] = header.segmentProbabilities[2] = 255;
    if (!header.useSegments)
        return;
    header.updateMap = br.bit(0x80) != 0;
    if (br.bit(0x80) != 0) {
        header.absoluteDelta = br.bit(0x80) != 0;
        for (uint32_t s = 0; s < kSegmentCount; ++s)
            header.segmentQuant[s] = br.optionalSigned(7);
        for (uint32_t s = 0; s < kSegmentCount; ++s)
            header.segmentFilter[s] = br.optionalSigned(6);
    }
    if (header.updateMap) {
        for (uint32_t s = 0; s < 3; ++s)
            header.segmentProbabilities[s] = uint8_t(br.bit(0x80) != 0 ? br.literal(8) : 255);
    }
}

void readFilter(BoolDecoder& br, Header& header) {
    header.simpleFilter = br.bit(0x80) != 0;
    header.filterLevel = br.literal(6);
    header.sharpness = br.literal(3);
    header.useDeltas = br.bit(0x80) != 0;
    if (header.useDeltas && br.bit(0x80) != 0) {
        for (uint32_t i = 0; i < 4; ++i) {
            if (br.bit(0x80) != 0)
                header.referenceDelta[i] = br.signedLiteral(6);
        }
        for (uint32_t i = 0; i < 4; ++i) {
            if (br.bit(0x80) != 0)
                header.modeDelta[i] = br.signedLiteral(6);
        }
    }
}

// Reads the quantizer indices (rfc 6386 9.6) and sets each segment's factors, as dixie.c derives them (rfc 6386 20.4):
// the luma DC block's DC doubled and its AC scaled by 155/100 to at least 8, and the chroma DC's index at most 117.
void readQuant(BoolDecoder& br, Header& header) {
    const int32_t base = int32_t(br.literal(7));
    const int32_t lumaDc = br.optionalSigned(4);
    const int32_t secondDc = br.optionalSigned(4);
    const int32_t secondAc = br.optionalSigned(4);
    const int32_t chromaDc = br.optionalSigned(4);
    const int32_t chromaAc = br.optionalSigned(4);
    for (uint32_t s = 0; s < kSegmentCount; ++s) {
        int32_t q = base;
        if (header.useSegments)
            q = header.segmentQuant[s] + (header.absoluteDelta ? 0 : base);
        Quant& quant = header.quant[s];
        quant.luma[0] = kVp8DcQuant[clip(q + lumaDc, 127)];
        quant.luma[1] = kVp8AcQuant[clip(q, 127)];
        quant.lumaDc[0] = kVp8DcQuant[clip(q + secondDc, 127)] * 2;
        // x * 155 / 100 is (x * 101581) >> 16 for every factor of the table.
        quant.lumaDc[1] = (kVp8AcQuant[clip(q + secondAc, 127)] * 101581) >> 16;
        if (quant.lumaDc[1] < 8)
            quant.lumaDc[1] = 8;
        quant.chroma[0] = kVp8DcQuant[clip(q + chromaDc, 117)];
        quant.chroma[1] = kVp8AcQuant[clip(q + chromaAc, 127)];
    }
}

// The loop filter's strengths of each segment, for macroblocks that predict whole and by subblock (rfc 6386 9.3, 9.6,
// 15.2).
void setFilters(Header& header) {
    for (uint32_t s = 0; s < kSegmentCount; ++s) {
        int32_t base = int32_t(header.filterLevel);
        if (header.useSegments)
            base = header.segmentFilter[s] + (header.absoluteDelta ? 0 : int32_t(header.filterLevel));
        for (uint32_t subblocks = 0; subblocks < 2; ++subblocks) {
            FilterInfo& info = header.filters[s][subblocks];
            int32_t level = base;
            if (header.useDeltas) {
                // A key frame's macroblocks are all intra, whose reference frame delta is the first, and a macroblock
                // that predicts by subblock adds the first mode delta.
                level += header.referenceDelta[0];
                if (subblocks != 0)
                    level += header.modeDelta[0];
            }
            level = clip(level, 63);
            info = {};
            info.inner = uint8_t(subblocks);
            if (level == 0)
                continue;
            int32_t interior = level;
            if (header.sharpness > 0) {
                interior >>= header.sharpness > 4 ? 2 : 1;
                if (interior > 9 - int32_t(header.sharpness))
                    interior = 9 - int32_t(header.sharpness);
            }
            if (interior < 1)
                interior = 1;
            info.interior = uint8_t(interior);
            info.limit = uint8_t(2 * level + interior);
            info.hevThreshold = uint8_t(level >= 40 ? 2 : level >= 15 ? 1 : 0);
        }
    }
}

void readMacroblock(BoolDecoder& br, const Header& header, uint8_t* top, uint8_t* left, Macroblock& mb) {
    mb.segment = 0;
    if (header.updateMap) {
        if (br.bit(header.segmentProbabilities[0]) == 0)
            mb.segment = uint8_t(br.bit(header.segmentProbabilities[1]));
        else
            mb.segment = uint8_t(2 + br.bit(header.segmentProbabilities[2]));
    }
    mb.skip = header.useSkip ? uint8_t(br.bit(header.skipProbability)) : 0;
    mb.subblocks = br.bit(145) == 0 ? 1 : 0;
    if (mb.subblocks == 0) {
        // The key frame luma tree (rfc 6386 11.2), whose modes stand for the subblock modes of their contexts.
        uint8_t context = 0;
        if (br.bit(156) != 0) {
            mb.lumaMode = br.bit(128) != 0 ? Vp8Mode_TrueMotion : Vp8Mode_Horizontal;
            context = mb.lumaMode == Vp8Mode_TrueMotion ? Vp8Subblock_TrueMotion : Vp8Subblock_Horizontal;
        } else {
            mb.lumaMode = br.bit(163) != 0 ? Vp8Mode_Vertical : Vp8Mode_Dc;
            context = mb.lumaMode == Vp8Mode_Vertical ? Vp8Subblock_Vertical : Vp8Subblock_Dc;
        }
        prMemFill(top, context, 4);
        prMemFill(left, context, 4);
    } else {
        for (uint32_t y = 0; y < 4; ++y) {
            uint8_t mode = left[y];
            for (uint32_t x = 0; x < 4; ++x) {
                const uint8_t* probabilities = kVp8SubblockModeProbabilities[top[x]][mode];
                int32_t i = kSubblockTree[br.bit(probabilities[0])];
                while (i > 0)
                    i = kSubblockTree[i + int32_t(br.bit(probabilities[i >> 1]))];
                mode = uint8_t(-i);
                top[x] = mode;
                mb.modes[y * 4 + x] = mode;
            }
            left[y] = mode;
        }
    }
    // The key frame chroma tree (rfc 6386 11.2).
    if (br.bit(142) == 0)
        mb.chromaMode = Vp8Mode_Dc;
    else if (br.bit(114) == 0)
        mb.chromaMode = Vp8Mode_Vertical;
    else
        mb.chromaMode = br.bit(183) != 0 ? Vp8Mode_TrueMotion : Vp8Mode_Horizontal;
}

// Reads the extra bits of a token of category 3 or more, or the value of a token from 2 to 4 (rfc 6386 13.2).
PR_FORCE_INLINE int32_t largeValue(BoolDecoder& br, const uint8_t* p) {
    if (br.bit(p[3]) == 0) {
        if (br.bit(p[4]) == 0)
            return 2;
        return 3 + int32_t(br.bit(p[5]));
    }
    if (br.bit(p[6]) == 0) {
        if (br.bit(p[7]) == 0)
            return 5 + int32_t(br.bit(159));
        int32_t v = 7 + 2 * int32_t(br.bit(165));
        return v + int32_t(br.bit(145));
    }
    const uint32_t high = br.bit(p[8]);
    const uint32_t low = br.bit(p[9 + high]);
    const uint32_t category = 2 * high + low;
    int32_t v = 0;
    for (const uint8_t* table = kCategories[category]; *table != 0; ++table)
        v += v + int32_t(br.bit(*table));
    return v + 3 + (8 << category);
}

// Reads the tokens of one block from position `first` (rfc 6386 13), with the context of its neighbours' flags, and
// writes its dequantized coefficients. Returns the position after the last token, which is `first` for a block of none.
// A coefficient keeps the low 16 bits of its value, as the reference decoder keeps it.
PR_FORCE_INLINE uint32_t readCoefficients(BoolDecoder& br, const uint8_t (*positions)[3][11], uint32_t context,
        const int32_t* dq, uint32_t first, int16_t* out) {
    const uint8_t* p = positions[first][context];
    uint32_t n = first;
    for (; n < 16; ++n) {
        if (br.bit(p[0]) == 0)
            return n;
        while (br.bit(p[1]) == 0) {
            ++n;
            p = positions[n][0];
            if (n == 16)
                return 16;
        }
        int32_t v = 0;
        if (br.bit(p[2]) == 0) {
            v = 1;
            p = positions[n + 1][1];
        } else {
            v = largeValue(br, p);
            p = positions[n + 1][2];
        }
        v = br.sign(v);
        out[kZigzag[n]] = int16_t(uint16_t(uint32_t(v * dq[n > 0 ? 1 : 0])));
    }
    return 16;
}

// The inverse Walsh-Hadamard transform of the luma DC block (rfc 6386 14.3), whose outputs are the DC of each luma
// subblock, 16 coefficients apart.
void inverseWht(const int16_t* in, int16_t* out) {
    int32_t tmp[16];
    for (uint32_t i = 0; i < 4; ++i) {
        const int32_t a0 = in[0 + i] + in[12 + i];
        const int32_t a1 = in[4 + i] + in[8 + i];
        const int32_t a2 = in[4 + i] - in[8 + i];
        const int32_t a3 = in[0 + i] - in[12 + i];
        tmp[0 + i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        const int32_t dc = tmp[0 + i * 4] + 3;
        const int32_t a0 = dc + tmp[3 + i * 4];
        const int32_t a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        const int32_t a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
        const int32_t a3 = dc - tmp[3 + i * 4];
        out[0] = int16_t(uint16_t(uint32_t((a0 + a1) >> 3)));
        out[16] = int16_t(uint16_t(uint32_t((a3 + a2) >> 3)));
        out[32] = int16_t(uint16_t(uint32_t((a0 - a1) >> 3)));
        out[48] = int16_t(uint16_t(uint32_t((a3 - a2) >> 3)));
        out += 64;
    }
}

int32_t mulCos(int32_t a) {
    return ((a * 20091) >> 16) + a;
}

int32_t mulSin(int32_t a) {
    return (a * 35468) >> 16;
}

// Adds the inverse DCT of a block's coefficients (rfc 6386 14.4) to the prediction at `dst`, `stride` bytes a row.
void inverseDctAdd(const int16_t* in, uint8_t* dst, size_t stride) {
    int32_t tmp[16];
    for (uint32_t i = 0; i < 4; ++i) {
        const int32_t a = in[i] + in[8 + i];
        const int32_t b = in[i] - in[8 + i];
        const int32_t c = mulSin(in[4 + i]) - mulCos(in[12 + i]);
        const int32_t d = mulCos(in[4 + i]) + mulSin(in[12 + i]);
        tmp[i * 4 + 0] = a + d;
        tmp[i * 4 + 1] = b + c;
        tmp[i * 4 + 2] = b - c;
        tmp[i * 4 + 3] = a - d;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        const int32_t dc = tmp[i] + 4;
        const int32_t a = dc + tmp[8 + i];
        const int32_t b = dc - tmp[8 + i];
        const int32_t c = mulSin(tmp[4 + i]) - mulCos(tmp[12 + i]);
        const int32_t d = mulCos(tmp[4 + i]) + mulSin(tmp[12 + i]);
        uint8_t* row = dst + i * stride;
        row[0] = clip8(row[0] + ((a + d) >> 3));
        row[1] = clip8(row[1] + ((b + c) >> 3));
        row[2] = clip8(row[2] + ((b - c) >> 3));
        row[3] = clip8(row[3] + ((a - d) >> 3));
    }
}

// Adds the inverse DCT of a block whose only coefficient is its DC, which is the DC's own share at every pixel, and
// zeroes the block.
void inverseDcAdd(int16_t* in, uint8_t* dst, size_t stride) {
    const int32_t dc = (in[0] + 4) >> 3;
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x)
            dst[y * stride + x] = clip8(dst[y * stride + x] + dc);
    }
    in[0] = 0;
}

// The work area of one macroblock's reconstruction, laid out as the reference decoder lays it out: each plane with the
// row above it and the column to its left, and the luma's row above four pixels longer, for the subblocks above and to
// the right.
constexpr size_t kBps = 32;
constexpr size_t kLumaOffset = kBps + 8;
constexpr size_t kUOffset = kLumaOffset + kBps * 16 + kBps;
constexpr size_t kVOffset = kUOffset + 16;
constexpr size_t kWorkSize = kBps * 17 + kBps * 9;

uint8_t avg3(int32_t a, int32_t b, int32_t c) {
    return uint8_t((a + 2 * b + c + 2) >> 2);
}

uint8_t avg2(int32_t a, int32_t b) {
    return uint8_t((a + b + 1) >> 1);
}

void fillBlock(uint8_t* dst, uint32_t size, uint8_t value) {
    for (uint32_t y = 0; y < size; ++y)
        prMemFill(dst + y * kBps, value, size);
}

void trueMotion(uint8_t* dst, uint32_t size) {
    const uint8_t* top = dst - kBps;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x)
            dst[y * kBps + x] = clip8(int32_t(top[x]) + (dst - 1)[y * kBps] - top[-1]);
    }
}

// Predicts a whole luma or chroma block of `size` (rfc 6386 12.2). DC prediction averages the edges that are inside the
// frame, and predicts 128 at the top-left corner.
void predictBlock(uint8_t* dst, uint32_t size, uint8_t mode, bool hasTop, bool hasLeft) {
    const uint8_t* top = dst - kBps;
    if (mode == Vp8Mode_Vertical) {
        for (uint32_t y = 0; y < size; ++y)
            prMemCopy(dst + y * kBps, top, size);
    } else if (mode == Vp8Mode_Horizontal) {
        for (uint32_t y = 0; y < size; ++y)
            prMemFill(dst + y * kBps, (dst - 1)[y * kBps], size);
    } else if (mode == Vp8Mode_TrueMotion) {
        trueMotion(dst, size);
    } else {
        const uint32_t shift = size == 16 ? 4 : 3;
        uint32_t sum = 0;
        if (hasTop) {
            for (uint32_t x = 0; x < size; ++x)
                sum += top[x];
        }
        if (hasLeft) {
            for (uint32_t y = 0; y < size; ++y)
                sum += (dst - 1)[y * kBps];
        }
        uint8_t dc = 0x80;
        if (hasTop && hasLeft)
            dc = uint8_t((sum + size) >> (shift + 1));
        else if (hasTop || hasLeft)
            dc = uint8_t((sum + (size >> 1)) >> shift);
        fillBlock(dst, size, dc);
    }
}

#define DST(x, y) dst[(x) + (y) * kBps]

// Predicts a luma subblock (rfc 6386 12.3), from the row above it, which goes four pixels to the right, the column to
// its left, and the pixel above and to the left.
void predictSubblock(uint8_t* dst, uint8_t mode) {
    const uint8_t* top = dst - kBps;
    const int32_t X = top[-1];
    const int32_t A = top[0], B = top[1], C = top[2], D = top[3];
    const int32_t E = top[4], F = top[5], G = top[6], H = top[7];
    const uint8_t* left = dst - 1;
    const int32_t I = left[0], J = left[kBps], K = left[2 * kBps], L = left[3 * kBps];
    switch (mode) {
        case Vp8Subblock_Dc: {
            uint32_t dc = 4;
            for (uint32_t i = 0; i < 4; ++i)
                dc += top[i] + left[i * kBps];
            fillBlock(dst, 4, uint8_t(dc >> 3));
            break;
        }
        case Vp8Subblock_TrueMotion:
            trueMotion(dst, 4);
            break;
        case Vp8Subblock_Vertical: {
            const uint8_t values[4] = {avg3(X, A, B), avg3(A, B, C), avg3(B, C, D), avg3(C, D, E)};
            for (uint32_t y = 0; y < 4; ++y)
                prMemCopy(dst + y * kBps, values, 4);
            break;
        }
        case Vp8Subblock_Horizontal:
            prMemFill(dst, avg3(X, I, J), 4);
            prMemFill(dst + kBps, avg3(I, J, K), 4);
            prMemFill(dst + 2 * kBps, avg3(J, K, L), 4);
            prMemFill(dst + 3 * kBps, avg3(K, L, L), 4);
            break;
        case Vp8Subblock_DownRight:
            DST(0, 3) = avg3(J, K, L);
            DST(1, 3) = DST(0, 2) = avg3(I, J, K);
            DST(2, 3) = DST(1, 2) = DST(0, 1) = avg3(X, I, J);
            DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = avg3(A, X, I);
            DST(3, 2) = DST(2, 1) = DST(1, 0) = avg3(B, A, X);
            DST(3, 1) = DST(2, 0) = avg3(C, B, A);
            DST(3, 0) = avg3(D, C, B);
            break;
        case Vp8Subblock_DownLeft:
            DST(0, 0) = avg3(A, B, C);
            DST(1, 0) = DST(0, 1) = avg3(B, C, D);
            DST(2, 0) = DST(1, 1) = DST(0, 2) = avg3(C, D, E);
            DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = avg3(D, E, F);
            DST(3, 1) = DST(2, 2) = DST(1, 3) = avg3(E, F, G);
            DST(3, 2) = DST(2, 3) = avg3(F, G, H);
            DST(3, 3) = avg3(G, H, H);
            break;
        case Vp8Subblock_VerticalRight:
            DST(0, 0) = DST(1, 2) = avg2(X, A);
            DST(1, 0) = DST(2, 2) = avg2(A, B);
            DST(2, 0) = DST(3, 2) = avg2(B, C);
            DST(3, 0) = avg2(C, D);
            DST(0, 3) = avg3(K, J, I);
            DST(0, 2) = avg3(J, I, X);
            DST(0, 1) = DST(1, 3) = avg3(I, X, A);
            DST(1, 1) = DST(2, 3) = avg3(X, A, B);
            DST(2, 1) = DST(3, 3) = avg3(A, B, C);
            DST(3, 1) = avg3(B, C, D);
            break;
        case Vp8Subblock_VerticalLeft:
            DST(0, 0) = avg2(A, B);
            DST(1, 0) = DST(0, 2) = avg2(B, C);
            DST(2, 0) = DST(1, 2) = avg2(C, D);
            DST(3, 0) = DST(2, 2) = avg2(D, E);
            DST(0, 1) = avg3(A, B, C);
            DST(1, 1) = DST(0, 3) = avg3(B, C, D);
            DST(2, 1) = DST(1, 3) = avg3(C, D, E);
            DST(3, 1) = DST(2, 3) = avg3(D, E, F);
            DST(3, 2) = avg3(E, F, G);
            DST(3, 3) = avg3(F, G, H);
            break;
        case Vp8Subblock_HorizontalDown:
            DST(0, 0) = DST(2, 1) = avg2(I, X);
            DST(0, 1) = DST(2, 2) = avg2(J, I);
            DST(0, 2) = DST(2, 3) = avg2(K, J);
            DST(0, 3) = avg2(L, K);
            DST(3, 0) = avg3(A, B, C);
            DST(2, 0) = avg3(X, A, B);
            DST(1, 0) = DST(3, 1) = avg3(I, X, A);
            DST(1, 1) = DST(3, 2) = avg3(J, I, X);
            DST(1, 2) = DST(3, 3) = avg3(K, J, I);
            DST(1, 3) = avg3(L, K, J);
            break;
        default:
            DST(0, 0) = avg2(I, J);
            DST(2, 0) = DST(0, 1) = avg2(J, K);
            DST(2, 1) = DST(0, 2) = avg2(K, L);
            DST(1, 0) = avg3(I, J, K);
            DST(3, 0) = DST(1, 1) = avg3(J, K, L);
            DST(3, 1) = DST(1, 2) = avg3(K, L, L);
            DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = uint8_t(L);
            break;
    }
}

#undef DST

// The loop filter (rfc 6386 15.2 to 15.4), in the reference decoder's arithmetic. An edge's positions are filtered
// together: the eight pixels across the edge at each position are gathered into rows of lanes, p3 to q3, each filter's
// result is computed for every lane without branches, and each lane keeps the result that its masks choose. The lanes
// of a row are contiguous, which the compiler vectorizes.
constexpr uint32_t kEdgeLanes = 16;

// What a lane of an edge applies: the simple filter, or the normal filter of a subblock edge or of a macroblock edge.
enum {
    Vp8Edge_Simple,
    Vp8Edge_Inner,
    Vp8Edge_Macroblock,
};
typedef uint8_t Vp8Edge;

int32_t clampS8(int32_t v) {
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

int32_t clampS4(int32_t v) {
    return v < -16 ? -16 : v > 15 ? 15 : v;
}

int32_t clampU8(int32_t v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

int32_t absolute(int32_t v) {
    return v < 0 ? -v : v;
}

// Filters the `count` lanes of `px`, rows p3, p2, p1, p0, q0, q1, q2, q3, in place.
template <Vp8Edge kind>
void filterLanes(uint8_t (&px)[8][kEdgeLanes], uint32_t count, int32_t threshold, int32_t interior,
        int32_t hevThreshold) {
    for (uint32_t i = 0; i < kEdgeLanes; ++i) {
        const int32_t p3 = px[0][i], p2 = px[1][i], p1 = px[2][i], p0 = px[3][i];
        const int32_t q0 = px[4][i], q1 = px[5][i], q2 = px[6][i], q3 = px[7][i];
        bool filter = 4 * absolute(p0 - q0) + absolute(p1 - q1) <= threshold;
        if (kind != Vp8Edge_Simple) {
            filter = filter && absolute(p3 - p2) <= interior && absolute(p2 - p1) <= interior &&
                absolute(p1 - p0) <= interior && absolute(q3 - q2) <= interior && absolute(q2 - q1) <= interior &&
                absolute(q1 - q0) <= interior;
        }
        filter = filter && i < count;
        const bool hev = kind == Vp8Edge_Simple || absolute(p1 - p0) > hevThreshold || absolute(q1 - q0) > hevThreshold;
        // The filter of high edge variance, and of the simple filter: the outer taps, and p0 and q0 alone change.
        const int32_t outer = 3 * (q0 - p0) + clampS8(p1 - q1);
        const int32_t hevP0 = clampU8(p0 + clampS4((outer + 3) >> 3));
        const int32_t hevQ0 = clampU8(q0 - clampS4((outer + 4) >> 3));
        int32_t newP2 = p2, newP1 = p1, newP0 = hevP0, newQ0 = hevQ0, newQ1 = q1, newQ2 = q2;
        if (kind == Vp8Edge_Inner) {
            const int32_t a = 3 * (q0 - p0);
            const int32_t a1 = clampS4((a + 4) >> 3);
            const int32_t a2 = clampS4((a + 3) >> 3);
            const int32_t a3 = (a1 + 1) >> 1;
            newP1 = hev ? p1 : clampU8(p1 + a3);
            newP0 = hev ? hevP0 : clampU8(p0 + a2);
            newQ0 = hev ? hevQ0 : clampU8(q0 - a1);
            newQ1 = hev ? q1 : clampU8(q1 - a3);
        } else if (kind == Vp8Edge_Macroblock) {
            const int32_t w = clampS8(outer);
            const int32_t a1 = (27 * w + 63) >> 7;
            const int32_t a2 = (18 * w + 63) >> 7;
            const int32_t a3 = (9 * w + 63) >> 7;
            newP2 = hev ? p2 : clampU8(p2 + a3);
            newP1 = hev ? p1 : clampU8(p1 + a2);
            newP0 = hev ? hevP0 : clampU8(p0 + a1);
            newQ0 = hev ? hevQ0 : clampU8(q0 - a1);
            newQ1 = hev ? q1 : clampU8(q1 - a2);
            newQ2 = hev ? q2 : clampU8(q2 - a3);
        }
        px[1][i] = uint8_t(filter ? newP2 : p2);
        px[2][i] = uint8_t(filter ? newP1 : p1);
        px[3][i] = uint8_t(filter ? newP0 : p0);
        px[4][i] = uint8_t(filter ? newQ0 : q0);
        px[5][i] = uint8_t(filter ? newQ1 : q1);
        px[6][i] = uint8_t(filter ? newQ2 : q2);
    }
}

// Filters `count` positions of one edge, 16 for luma and 8 for chroma, which are `along` apart, across the edge at
// `step`. The limit doubles, as four times the difference across the edge and the outer difference are compared with
// it.
template <Vp8Edge kind, uint32_t count>
void filterEdge(uint8_t* p, ptrdiff_t step, ptrdiff_t along, int32_t limit, int32_t interior, int32_t hevThreshold) {
    uint8_t px[8][kEdgeLanes] = {};
    uint8_t* at = p - 4 * step;
    if (along == 1) {
        for (uint32_t k = 0; k < 8; ++k)
            prMemCopy(px[k], at + ptrdiff_t(k) * step, count);
    } else {
        // Each position's eight pixels are one word, whose bytes go to the rows.
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t lane = prMemReadLeU64(at + ptrdiff_t(i) * along);
            for (uint32_t k = 0; k < 8; ++k)
                px[k][i] = uint8_t(lane >> (8 * k));
        }
    }
    filterLanes<kind>(px, count, 2 * limit + 1, interior, hevThreshold);
    if (along == 1) {
        for (uint32_t k = 1; k < 7; ++k)
            prMemCopy(at + ptrdiff_t(k) * step, px[k], count);
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t* lane = at + ptrdiff_t(i) * along;
            for (uint32_t k = 1; k < 7; ++k)
                lane[k] = px[k][i];
        }
    }
}

template <uint32_t count>
void simpleEdge(uint8_t* p, ptrdiff_t step, ptrdiff_t along, int32_t limit) {
    filterEdge<Vp8Edge_Simple, count>(p, step, along, limit, 0, 0);
}

template <uint32_t count>
void normalEdge(uint8_t* p, ptrdiff_t step, ptrdiff_t along, int32_t limit, int32_t interior, int32_t hevThreshold,
        bool macroblockEdge) {
    if (macroblockEdge)
        filterEdge<Vp8Edge_Macroblock, count>(p, step, along, limit, interior, hevThreshold);
    else
        filterEdge<Vp8Edge_Inner, count>(p, step, along, limit, interior, hevThreshold);
}

struct Frame {
    Header header;
    uint32_t mbWidth;
    uint32_t mbHeight;
    BoolDecoder modes;
    BoolDecoder partitions[kMaxPartitions];
    uint32_t partitionMask;
};

PrStatus readHeaders(PrByteView bytes, Frame& frame) {
    const uint8_t* data = bytes.data;
    const uint32_t tag = uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16);
    const uint32_t firstSize = tag >> 5;
    const uint8_t* start = data + kFrameHeaderBytes;
    const uint8_t* end = data + bytes.size;
    BoolDecoder& br = frame.modes;
    br.init(start, firstSize);
    Header& header = frame.header;
    br.bit(0x80);   // the color space, which has one meaning
    br.bit(0x80);   // the clamping type: a decoder clamps either way
    readSegments(br, header);
    readFilter(br, header);
    const uint32_t partitions = 1u << br.literal(2);
    if (br.eof)
        return PrStatus_InvalidData;
    // The sizes of the token partitions but the last follow the first partition, three bytes each, and the last has the
    // rest (rfc 6386 9.5). A size past the end is cut to it.
    const uint8_t* sizes = start + firstSize;
    if (size_t(end - sizes) < size_t(3) * (partitions - 1))
        return PrStatus_InvalidData;
    const uint8_t* part = sizes + 3 * (partitions - 1);
    for (uint32_t p = 0; p + 1 < partitions; ++p) {
        size_t size = size_t(sizes[0]) | (size_t(sizes[1]) << 8) | (size_t(sizes[2]) << 16);
        if (size > size_t(end - part))
            size = size_t(end - part);
        frame.partitions[p].init(part, size);
        part += size;
        sizes += 3;
    }
    frame.partitions[partitions - 1].init(part, size_t(end - part));
    frame.partitionMask = partitions - 1;
    if (part >= end)
        return PrStatus_InvalidData;
    readQuant(br, header);
    br.bit(0x80);   // whether a later frame keeps the probabilities, which no later frame reads
    for (uint32_t t = 0; t < 4; ++t) {
        for (uint32_t b = 0; b < 8; ++b) {
            for (uint32_t c = 0; c < 3; ++c) {
                for (uint32_t p = 0; p < 11; ++p) {
                    uint8_t probability = kVp8CoefficientProbabilities[t][b][c][p];
                    if (br.bit(kVp8CoefficientUpdateProbabilities[t][b][c][p]) != 0)
                        probability = uint8_t(br.literal(8));
                    header.probabilities[t][b][c][p] = probability;
                }
            }
        }
        for (uint32_t n = 0; n < 17; ++n)
            prMemCopy(header.positions[t][n], header.probabilities[t][kBands[n]], sizeof(header.positions[t][n]));
    }
    header.useSkip = br.bit(0x80) != 0;
    if (header.useSkip)
        header.skipProbability = uint8_t(br.literal(8));
    setFilters(header);
    return br.eof ? PrStatus_InvalidData : PrStatus_Ok;
}

// The blocks of a macroblock that have coefficients to transform, 16 luma then 4 of each chroma plane, and of those the
// blocks whose only coefficient is the DC, a bit each. A block that is not coded holds zeros.
struct Residuals {
    uint32_t coded;
    uint32_t dcOnly;
};

// Reads the tokens of one macroblock and writes its coefficients: 16 luma blocks, then 4 of each chroma plane, 16
// coefficients each. Updates the contexts above and to the left. Returns whether any block has tokens past its first
// position or a nonzero DC, as the reference decoder decides whether the macroblock's subblock edges are filtered.
bool readResiduals(BoolDecoder& partition, const Header& header, const Macroblock& mb, Nonzero& top, Nonzero& left,
        int16_t* coefficients, Residuals* residuals) {
    // The decoder's state is local while the macroblock's tokens are read, so that it stays in registers.
    BoolDecoder br = partition;
    const Quant& quant = header.quant[mb.segment];
    uint32_t coded = 0;
    uint32_t dcOnly = 0;
    const uint8_t (*lumaBands)[3][11] = header.positions[Vp8Block_Luma];
    uint32_t first = 0;
    if (mb.subblocks == 0) {
        int16_t dc[16] = {};
        const uint32_t context = top.dc + left.dc;
        const uint32_t count = readCoefficients(br, header.positions[Vp8Block_LumaDc], context, quant.lumaDc, 0, dc);
        top.dc = left.dc = count > 0 ? 1 : 0;
        if (count > 1) {
            inverseWht(dc, coefficients);
        } else {
            const int16_t dc0 = int16_t((dc[0] + 3) >> 3);
            for (uint32_t i = 0; i < 16; ++i)
                coefficients[i * 16] = dc0;
        }
        first = 1;
        lumaBands = header.positions[Vp8Block_LumaAfterDc];
    }
    uint32_t topFlags = top.blocks;
    uint32_t leftFlags = left.blocks;
    uint32_t outTop = 0;
    uint32_t outLeft = 0;
    for (uint32_t y = 0; y < 4; ++y) {
        uint32_t l = (leftFlags >> y) & 1;
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t t = (topFlags >> x) & 1;
            int16_t* block = coefficients + (y * 4 + x) * 16;
            const uint32_t count = readCoefficients(br, lumaBands, l + t, quant.luma, first, block);
            l = count > first ? 1 : 0;
            topFlags = (topFlags & ~(1u << x)) | (l << x);
            const uint32_t n = y * 4 + x;
            coded |= uint32_t(count > 1 || block[0] != 0) << n;
            dcOnly |= uint32_t(count <= 1) << n;
        }
        outLeft |= l << y;
    }
    outTop = topFlags & 0xF;
    for (uint32_t plane = 0; plane < 2; ++plane) {
        const uint32_t shift = 4 + plane * 2;
        uint32_t topBits = (top.blocks >> shift) & 3;
        uint32_t leftBits = (left.blocks >> shift) & 3;
        uint32_t newLeft = 0;
        for (uint32_t y = 0; y < 2; ++y) {
            uint32_t l = (leftBits >> y) & 1;
            for (uint32_t x = 0; x < 2; ++x) {
                const uint32_t t = (topBits >> x) & 1;
                int16_t* block = coefficients + 256 + plane * 64 + (y * 2 + x) * 16;
                const uint32_t count = readCoefficients(br, header.positions[Vp8Block_Chroma], l + t, quant.chroma, 0, block);
                l = count > 0 ? 1 : 0;
                topBits = (topBits & ~(1u << x)) | (l << x);
                const uint32_t n = 16 + plane * 4 + y * 2 + x;
                coded |= uint32_t(count > 1 || block[0] != 0) << n;
                dcOnly |= uint32_t(count <= 1) << n;
            }
            newLeft |= l << y;
        }
        outTop |= topBits << shift;
        outLeft |= newLeft << shift;
    }
    top.blocks = outTop;
    left.blocks = outLeft;
    residuals->coded = coded;
    residuals->dcOnly = dcOnly;
    partition = br;
    return coded != 0;
}

// Copies the edges that a macroblock predicts from into the work area: the pixels of the frame above and to the left,
// which are not yet filtered, or past the frame's top 127 and past its left 129 (rfc 6386 12.2).
void loadEdges(const Vp8Planes& planes, uint32_t mbX, uint32_t mbY, uint32_t mbWidth, uint8_t* work) {
    uint8_t* y = work + kLumaOffset;
    uint8_t* u = work + kUOffset;
    uint8_t* v = work + kVOffset;
    const size_t ys = planes.yStride;
    const size_t cs = planes.uvStride;
    const uint8_t* frameY = planes.y + size_t(mbY) * 16 * ys + size_t(mbX) * 16;
    const uint8_t* frameU = planes.u + size_t(mbY) * 8 * cs + size_t(mbX) * 8;
    const uint8_t* frameV = planes.v + size_t(mbY) * 8 * cs + size_t(mbX) * 8;
    for (uint32_t j = 0; j < 16; ++j)
        (y - 1)[j * kBps] = mbX == 0 ? 129 : *(frameY + j * ys - 1);
    for (uint32_t j = 0; j < 8; ++j) {
        (u - 1)[j * kBps] = mbX == 0 ? 129 : *(frameU + j * cs - 1);
        (v - 1)[j * kBps] = mbX == 0 ? 129 : *(frameV + j * cs - 1);
    }
    if (mbY == 0) {
        prMemFill(y - kBps - 1, 127, 16 + 4 + 1);
        prMemFill(u - kBps - 1, 127, 8 + 1);
        prMemFill(v - kBps - 1, 127, 8 + 1);
    } else {
        *(y - kBps - 1) = mbX == 0 ? 129 : *(frameY - ys - 1);
        *(u - kBps - 1) = mbX == 0 ? 129 : *(frameU - cs - 1);
        *(v - kBps - 1) = mbX == 0 ? 129 : *(frameV - cs - 1);
        prMemCopy(y - kBps, frameY - ys, 16);
        prMemCopy(u - kBps, frameU - cs, 8);
        prMemCopy(v - kBps, frameV - cs, 8);
        // The pixels above and to the right are the next macroblock's, or past the right edge this one's last.
        if (mbX + 1 < mbWidth)
            prMemCopy(y - kBps + 16, frameY - ys + 16, 4);
        else
            prMemFill(y - kBps + 16, *(frameY - ys + 15), 4);
    }
}

// Adds a coded block's inverse transform to the prediction, and leaves the block zero for the next macroblock.
void addResidual(int16_t* block, uint8_t* dst, const Residuals& residuals, uint32_t n) {
    if (((residuals.coded >> n) & 1) == 0)
        return;
    if (((residuals.dcOnly >> n) & 1) != 0) {
        inverseDcAdd(block, dst, kBps);
        return;
    }
    inverseDctAdd(block, dst, kBps);
    prMemZero(block, 16 * sizeof(int16_t));
}

void reconstruct(const Macroblock& mb, int16_t* coefficients, const Residuals& residuals, uint32_t mbX, uint32_t mbY,
        uint8_t* work) {
    uint8_t* y = work + kLumaOffset;
    uint8_t* u = work + kUOffset;
    uint8_t* v = work + kVOffset;
    if (mb.subblocks != 0) {
        // The subblocks of the right column take the pixels above and to the right of the macroblock as theirs.
        uint8_t* topRight = y - kBps + 16;
        for (uint32_t row = 1; row < 4; ++row)
            prMemCopy(topRight + row * 4 * kBps, topRight, 4);
        for (uint32_t n = 0; n < 16; ++n) {
            uint8_t* dst = y + (n & 3) * 4 + (n >> 2) * 4 * kBps;
            predictSubblock(dst, mb.modes[n]);
            addResidual(coefficients + n * 16, dst, residuals, n);
        }
    } else {
        predictBlock(y, 16, mb.lumaMode, mbY > 0, mbX > 0);
        for (uint32_t n = 0; n < 16; ++n)
            addResidual(coefficients + n * 16, y + (n & 3) * 4 + (n >> 2) * 4 * kBps, residuals, n);
    }
    predictBlock(u, 8, mb.chromaMode, mbY > 0, mbX > 0);
    predictBlock(v, 8, mb.chromaMode, mbY > 0, mbX > 0);
    for (uint32_t n = 0; n < 4; ++n) {
        const size_t at = (n & 1) * 4 + (n >> 1) * 4 * kBps;
        addResidual(coefficients + 256 + n * 16, u + at, residuals, 16 + n);
        addResidual(coefficients + 320 + n * 16, v + at, residuals, 20 + n);
    }
}

void storeMacroblock(const uint8_t* work, uint32_t mbX, uint32_t mbY, Vp8Planes& planes) {
    uint8_t* frameY = planes.y + size_t(mbY) * 16 * planes.yStride + size_t(mbX) * 16;
    uint8_t* frameU = planes.u + size_t(mbY) * 8 * planes.uvStride + size_t(mbX) * 8;
    uint8_t* frameV = planes.v + size_t(mbY) * 8 * planes.uvStride + size_t(mbX) * 8;
    for (uint32_t j = 0; j < 16; ++j)
        prMemCopy(frameY + j * planes.yStride, work + kLumaOffset + j * kBps, 16);
    for (uint32_t j = 0; j < 8; ++j) {
        prMemCopy(frameU + j * planes.uvStride, work + kUOffset + j * kBps, 8);
        prMemCopy(frameV + j * planes.uvStride, work + kVOffset + j * kBps, 8);
    }
}

// Filters one macroblock's edges (rfc 6386 15): its left edge, its inner vertical edges, its top edge and its inner
// horizontal edges, in that order.
void filterMacroblock(Vp8Planes& planes, bool simple, const FilterInfo& info, uint32_t mbX, uint32_t mbY) {
    if (info.limit == 0)
        return;
    const ptrdiff_t ys = ptrdiff_t(planes.yStride);
    const ptrdiff_t cs = ptrdiff_t(planes.uvStride);
    uint8_t* y = planes.y + size_t(mbY) * 16 * planes.yStride + size_t(mbX) * 16;
    const int32_t limit = info.limit;
    if (simple) {
        if (mbX > 0)
            simpleEdge<16>(y, 1, ys, limit + 4);
        if (info.inner != 0) {
            for (uint32_t k = 1; k < 4; ++k)
                simpleEdge<16>(y + 4 * k, 1, ys, limit);
        }
        if (mbY > 0)
            simpleEdge<16>(y, ys, 1, limit + 4);
        if (info.inner != 0) {
            for (uint32_t k = 1; k < 4; ++k)
                simpleEdge<16>(y + ptrdiff_t(4 * k) * ys, ys, 1, limit);
        }
        return;
    }
    uint8_t* planesUv[2] = {
        planes.u + size_t(mbY) * 8 * planes.uvStride + size_t(mbX) * 8,
        planes.v + size_t(mbY) * 8 * planes.uvStride + size_t(mbX) * 8,
    };
    const int32_t interior = info.interior;
    const int32_t hev = info.hevThreshold;
    if (mbX > 0) {
        normalEdge<16>(y, 1, ys, limit + 4, interior, hev, true);
        for (uint8_t* c : planesUv)
            normalEdge<8>(c, 1, cs, limit + 4, interior, hev, true);
    }
    if (info.inner != 0) {
        for (uint32_t k = 1; k < 4; ++k)
            normalEdge<16>(y + 4 * k, 1, ys, limit, interior, hev, false);
        for (uint8_t* c : planesUv)
            normalEdge<8>(c + 4, 1, cs, limit, interior, hev, false);
    }
    if (mbY > 0) {
        normalEdge<16>(y, ys, 1, limit + 4, interior, hev, true);
        for (uint8_t* c : planesUv)
            normalEdge<8>(c, cs, 1, limit + 4, interior, hev, true);
    }
    if (info.inner != 0) {
        for (uint32_t k = 1; k < 4; ++k)
            normalEdge<16>(y + ptrdiff_t(4 * k) * ys, ys, 1, limit, interior, hev, false);
        for (uint8_t* c : planesUv)
            normalEdge<8>(c + 4 * cs, cs, 1, limit, interior, hev, false);
    }
}

// The work of one macroblock row: the modes of its macroblocks, their filters, and the contexts above them.
struct RowState {
    Macroblock* macroblocks;
    Nonzero* nonzero;
    uint8_t* modesAbove;
    FilterInfo* filters;
    int16_t* coefficients;
    uint8_t* work;
};

uint64_t rowStateBytes(uint32_t mbWidth, uint32_t mbHeight) {
    // The chroma that the conversion to RGB upsamples, a row at a time, is two bytes a pixel of a row.
    return uint64_t(mbWidth) * (sizeof(Macroblock) + sizeof(Nonzero) + 4 + 2 * 16) +
        uint64_t(mbWidth) * mbHeight * sizeof(FilterInfo) + 384 * sizeof(int16_t) + kWorkSize;
}

uint64_t planeBytes(uint32_t mbWidth, uint32_t mbHeight) {
    return uint64_t(mbWidth) * mbHeight * (256 + 64 + 64);
}

// Upsamples one chroma plane for an output row (rfc 9649 leaves the method to the decoder; this is the reference
// decoder's default): each output pixel takes 9/16 of the chroma sample nearest it, 3/16 of each of the two next
// nearest, one in the row of `near` and one in the row of `far`, and 1/16 of the farthest, rounded in two steps as the
// reference decoder rounds them. The samples past the row's ends repeat its first and last. A row with one chroma row
// near it passes that row as both.
void upsampleLine(const uint8_t* near, const uint8_t* far, uint8_t* out, uint32_t width) {
    out[0] = uint8_t((3 * near[0] + far[0] + 2) >> 2);
    const uint32_t pairs = (width - 1) >> 1;
    for (uint32_t x = 1; x <= pairs; ++x) {
        const uint32_t nearLeft = near[x - 1], nearHere = near[x], farLeft = far[x - 1], farHere = far[x];
        const uint32_t average = nearLeft + nearHere + farLeft + farHere + 8;
        const uint32_t diagonal12 = (average + 2 * (nearHere + farLeft)) >> 3;
        const uint32_t diagonal03 = (average + 2 * (nearLeft + farHere)) >> 3;
        out[2 * x - 1] = uint8_t((diagonal12 + nearLeft) >> 1);
        out[2 * x] = uint8_t((diagonal03 + nearHere) >> 1);
    }
    if ((width & 1) == 0)
        out[width - 1] = uint8_t((3 * near[pairs] + far[pairs] + 2) >> 2);
}

// The conversion of BT.601 limited-range YCbCr to RGB, in the reference decoder's fixed point: 14 bits of fraction, of
// which the products keep six, clamped.
uint8_t yuvChannel(int32_t v) {
    v >>= 6;
    return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

template <uint32_t channels>
void yuvRowToRgb(const uint8_t* luma, const uint8_t* u, const uint8_t* v, uint8_t* out, uint32_t width) {
    for (uint32_t x = 0; x < width; ++x) {
        const int32_t y = (int32_t(luma[x]) * 19077) >> 8;
        const int32_t cb = u[x];
        const int32_t cr = v[x];
        out[x * channels] = yuvChannel(y + ((cr * 26149) >> 8) - 14234);
        out[x * channels + 1] = yuvChannel(y - ((cb * 6419) >> 8) - ((cr * 13320) >> 8) + 8708);
        out[x * channels + 2] = yuvChannel(y + ((cb * 33050) >> 8) - 17685);
        if (channels == 4)
            out[x * channels + 3] = 0xFF;
    }
}

template <uint32_t channels>
void yuvToRgb(const Vp8Planes& planes, uint8_t* chroma, uint8_t* out, size_t stride) {
    const uint32_t width = planes.width;
    const uint32_t height = planes.height;
    const size_t ys = planes.yStride;
    const size_t cs = planes.uvStride;
    uint8_t* u = chroma;
    uint8_t* v = chroma + width;
    // The first row has only the first chroma row near it. Each row after it lies between two chroma rows, nearer the
    // one above it if its index is odd, and an even height's last row has only the last chroma row.
    for (uint32_t row = 0; row < height; ++row) {
        const size_t nearRow = row / 2;
        size_t farRow = (row & 1) != 0 ? nearRow + 1 : nearRow - 1;
        if (row == 0 || ((row & 1) != 0 && row + 1 == height))
            farRow = nearRow;
        upsampleLine(planes.u + nearRow * cs, planes.u + farRow * cs, u, width);
        upsampleLine(planes.v + nearRow * cs, planes.v + farRow * cs, v, width);
        yuvRowToRgb<channels>(planes.y + row * ys, u, v, out + row * stride, width);
    }
}

} // namespace

bool vp8ReadHeader(PrByteView bytes, Vp8Header* out) {
    if (bytes.size < kFrameHeaderBytes)
        return false;
    const uint8_t* data = bytes.data;
    const uint32_t tag = uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16);
    const bool keyFrame = (tag & 1) == 0;
    const uint32_t profile = (tag >> 1) & 7;
    const bool shown = ((tag >> 4) & 1) != 0;
    const uint32_t firstSize = tag >> 5;
    if (!keyFrame || profile > 3 || !shown || firstSize > bytes.size - kFrameHeaderBytes)
        return false;
    if (data[3] != 0x9D || data[4] != 0x01 || data[5] != 0x2A)
        return false;
    // The two high bits of each dimension scale the frame for display, which a decoder leaves to its caller.
    out->width = prMemReadLeU16(data + 6) & 0x3FFF;
    out->height = prMemReadLeU16(data + 8) & 0x3FFF;
    return out->width != 0 && out->height != 0;
}

uint64_t vp8WorkingBytes(uint32_t width, uint32_t height) {
    const uint32_t mbWidth = (width + 15) >> 4;
    const uint32_t mbHeight = (height + 15) >> 4;
    return planeBytes(mbWidth, mbHeight) + rowStateBytes(mbWidth, mbHeight);
}

PrStatus vp8Decode(PrByteView bytes, WebpBudget& budget, Vp8Planes* out) {
    *out = {};
    Vp8Header size = {};
    if (!vp8ReadHeader(bytes, &size))
        return PrStatus_InvalidData;
    Frame* frame = static_cast<Frame*>(prMemoryAllocZeroed(sizeof(Frame)));
    frame->mbWidth = (size.width + 15) >> 4;
    frame->mbHeight = (size.height + 15) >> 4;
    PrStatus status = readHeaders(bytes, *frame);
    const uint32_t mbWidth = frame->mbWidth;
    const uint32_t mbHeight = frame->mbHeight;
    const uint64_t stateBytes = rowStateBytes(mbWidth, mbHeight);
    if (status == PrStatus_Ok && !webpBudgetTake(budget, planeBytes(mbWidth, mbHeight) + stateBytes))
        status = PrStatus_Limit;
    if (status != PrStatus_Ok) {
        prMemoryFree(frame);
        return status;
    }
    out->width = size.width;
    out->height = size.height;
    out->yStride = mbWidth * 16;
    out->uvStride = mbWidth * 8;
    out->y = static_cast<uint8_t*>(prMemoryAlloc(size_t(planeBytes(mbWidth, mbHeight))));
    out->u = out->y + size_t(out->yStride) * mbHeight * 16;
    out->v = out->u + size_t(out->uvStride) * mbHeight * 8;

    RowState state = {};
    uint8_t* storage = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(stateBytes)));
    // The arrays go from the most aligned to the least.
    uint8_t* at = storage;
    state.nonzero = reinterpret_cast<Nonzero*>(at);
    at += mbWidth * sizeof(Nonzero);
    state.coefficients = reinterpret_cast<int16_t*>(at);
    at += 384 * sizeof(int16_t);
    state.macroblocks = reinterpret_cast<Macroblock*>(at);
    at += mbWidth * sizeof(Macroblock);
    state.filters = reinterpret_cast<FilterInfo*>(at);
    at += size_t(mbWidth) * mbHeight * sizeof(FilterInfo);
    state.work = at;
    at += kWorkSize;
    state.modesAbove = at;

    const Header& header = frame->header;
    // Contexts outside the frame are DC prediction and no coefficients.
    prMemFill(state.modesAbove, Vp8Subblock_Dc, size_t(mbWidth) * 4);
    for (uint32_t mbY = 0; mbY < mbHeight && status == PrStatus_Ok; ++mbY) {
        uint8_t modesLeft[4] = {Vp8Subblock_Dc, Vp8Subblock_Dc, Vp8Subblock_Dc, Vp8Subblock_Dc};
        for (uint32_t mbX = 0; mbX < mbWidth; ++mbX)
            readMacroblock(frame->modes, header, state.modesAbove + mbX * 4, modesLeft, state.macroblocks[mbX]);
        if (frame->modes.eof) {
            status = PrStatus_InvalidData;
            break;
        }
        BoolDecoder& tokens = frame->partitions[mbY & frame->partitionMask];
        Nonzero left = {};
        for (uint32_t mbX = 0; mbX < mbWidth; ++mbX) {
            const Macroblock& mb = state.macroblocks[mbX];
            Nonzero& top = state.nonzero[mbX];
            bool coded = false;
            Residuals residuals = {};
            if (mb.skip == 0) {
                coded = readResiduals(tokens, header, mb, top, left, state.coefficients, &residuals);
            } else {
                // A skipped macroblock has no coefficients. One that predicts by subblock has no luma DC block, and
                // leaves that context as it was.
                top.blocks = left.blocks = 0;
                if (mb.subblocks == 0)
                    top.dc = left.dc = 0;
            }
            if (tokens.eof) {
                status = PrStatus_InvalidData;
                break;
            }
            FilterInfo info = header.filters[mb.segment][mb.subblocks];
            info.inner = uint8_t(info.inner | (coded ? 1 : 0));
            state.filters[size_t(mbY) * mbWidth + mbX] = header.filterLevel == 0 ? FilterInfo{} : info;
            loadEdges(*out, mbX, mbY, mbWidth, state.work);
            reconstruct(mb, state.coefficients, residuals, mbX, mbY, state.work);
            storeMacroblock(state.work, mbX, mbY, *out);
        }
    }
    if (status == PrStatus_Ok && header.filterLevel != 0) {
        for (uint32_t mbY = 0; mbY < mbHeight; ++mbY) {
            for (uint32_t mbX = 0; mbX < mbWidth; ++mbX)
                filterMacroblock(*out, header.simpleFilter, state.filters[size_t(mbY) * mbWidth + mbX], mbX, mbY);
        }
    }
    prMemoryFree(storage);
    webpBudgetGive(budget, stateBytes);
    prMemoryFree(frame);
    return status;
}

void vp8PlanesFree(Vp8Planes& planes, WebpBudget& budget) {
    if (planes.y != nullptr) {
        prMemoryFree(planes.y);
        webpBudgetGive(budget, planeBytes(planes.yStride / 16, (planes.height + 15) >> 4));
    }
    planes = {};
}

void vp8ToRgb(const Vp8Planes& planes, uint8_t* out, size_t stride, uint32_t channels) {
    uint8_t* chroma = static_cast<uint8_t*>(prMemoryAlloc(size_t(planes.width) * 2));
    if (channels == 4)
        yuvToRgb<4>(planes, chroma, out, stride);
    else
        yuvToRgb<3>(planes, chroma, out, stride);
    prMemoryFree(chroma);
}

} // namespace pr_impl
