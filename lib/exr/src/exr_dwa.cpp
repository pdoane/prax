// DWAA and DWAB, OpenEXR's lossy compressions of blocks of the discrete cosine transform. The two differ only in the
// lines of a chunk.
//
// A chunk begins with eleven 64-bit sizes and counts, and from version 2 the rules that sort its channels by name and
// type into three schemes. A channel of LOSSY_DCT is stored as 8 by 8 blocks of the DCT of its values in a nonlinear
// form, the coefficients rounded to halves: each block's DC coefficient in a plane of its own, zlib compressed, and its
// AC coefficients run-length coded in zigzag order and then Huffman coded or zlib compressed. Three channels that the
// rules name red, green and blue of one layer are stored as Y'CbCr of Rec. ITU-R BT.709 instead. A channel of RLE, as
// alpha is, is stored as planes of its bytes, run-length coded and zlib compressed, and any other channel, of UNKNOWN,
// as it is, zlib compressed.
//
// The decoding is computed in floats in the order that the reference implementation computes it, so that it gives the
// same halves.

#include "exr/src/exr_dwa.h"

#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/string_view.h"

#include "exr/src/exr_piz.h"

namespace pr_impl {

namespace {

// The schemes that a channel is compressed by.
enum {
    DwaScheme_Unknown,
    DwaScheme_LossyDct,
    DwaScheme_Rle,
    DwaScheme_Count,
};
typedef uint8_t DwaScheme;

// The sizes and counts that begin a chunk, by their order.
enum {
    DwaSize_Version,
    DwaSize_UnknownUncompressed,
    DwaSize_UnknownCompressed,
    DwaSize_AcCompressed,
    DwaSize_DcCompressed,
    DwaSize_RleCompressed,
    DwaSize_RleUncompressed,
    DwaSize_RleRaw,
    DwaSize_AcCount,
    DwaSize_DcCount,
    DwaSize_AcCompression,
    DwaSize_Count,
};
typedef uint8_t DwaSize;

// How the AC coefficients are compressed.
enum {
    DwaAc_Huffman,
    DwaAc_Deflate,
};
typedef uint8_t DwaAc;

// A rule that gives the channels whose names end in `suffix`, after the last dot, and that are of `pixel`, a scheme,
// and which of red, green and blue of a layer they are, or -1.
struct DwaRule {
    PrStringView suffix;
    DwaScheme scheme;
    ExrPixel pixel;
    int8_t color;
    bool caseless;
};

// The rules of a chunk before version 2.
constexpr DwaRule kLegacyRules[] = {
    {"r", DwaScheme_LossyDct, ExrPixel_Half, 0, true},     {"r", DwaScheme_LossyDct, ExrPixel_Float, 0, true},
    {"red", DwaScheme_LossyDct, ExrPixel_Half, 0, true},   {"red", DwaScheme_LossyDct, ExrPixel_Float, 0, true},
    {"g", DwaScheme_LossyDct, ExrPixel_Half, 1, true},     {"g", DwaScheme_LossyDct, ExrPixel_Float, 1, true},
    {"grn", DwaScheme_LossyDct, ExrPixel_Half, 1, true},   {"grn", DwaScheme_LossyDct, ExrPixel_Float, 1, true},
    {"green", DwaScheme_LossyDct, ExrPixel_Half, 1, true}, {"green", DwaScheme_LossyDct, ExrPixel_Float, 1, true},
    {"b", DwaScheme_LossyDct, ExrPixel_Half, 2, true},     {"b", DwaScheme_LossyDct, ExrPixel_Float, 2, true},
    {"blu", DwaScheme_LossyDct, ExrPixel_Half, 2, true},   {"blu", DwaScheme_LossyDct, ExrPixel_Float, 2, true},
    {"blue", DwaScheme_LossyDct, ExrPixel_Half, 2, true},  {"blue", DwaScheme_LossyDct, ExrPixel_Float, 2, true},
    {"y", DwaScheme_LossyDct, ExrPixel_Half, -1, true},    {"y", DwaScheme_LossyDct, ExrPixel_Float, -1, true},
    {"by", DwaScheme_LossyDct, ExrPixel_Half, -1, true},   {"by", DwaScheme_LossyDct, ExrPixel_Float, -1, true},
    {"ry", DwaScheme_LossyDct, ExrPixel_Half, -1, true},   {"ry", DwaScheme_LossyDct, ExrPixel_Float, -1, true},
    {"a", DwaScheme_Rle, ExrPixel_Uint, -1, true},         {"a", DwaScheme_Rle, ExrPixel_Half, -1, true},
    {"a", DwaScheme_Rle, ExrPixel_Float, -1, true},
};

// The longest suffix of a rule.
constexpr size_t kMaxSuffix = 128;

// The coefficients of DWA's inverse DCT: 0.5 cos(k π / 16) for k of 4, 1, 2, 3, 5, 6 and 7, with π taken as 3.14159,
// each rounded to a float.
constexpr float kDctA = 0x1.6a09f6p-2f;
constexpr float kDctB = 0x1.f6297ep-2f;
constexpr float kDctC = 0x1.d906c2p-2f;
constexpr float kDctD = 0x1.a9b66cp-2f;
constexpr float kDctE = 0x1.1c73cap-2f;
constexpr float kDctF = 0x1.87de66p-3f;
constexpr float kDctG = 0x1.8f8c16p-4f;

// The scale of a block's DC coefficient that gives each of its values when it has no AC coefficients: a, squared.
constexpr float kDcScale = 3.535536e-01f;

// The position in zigzag order of each coefficient of a block, by its row and then its column.
constexpr uint8_t kZigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44,
    53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49,
    57, 58, 62, 63,
};

// One channel of the chunk: its scheme, and where its rows are in the chunk's pixels.
struct DwaChannel {
    DwaScheme scheme;
    bool done;
    uint32_t firstRow;      // the index in the rows of the channel's first row
};

float halfToFloat(uint16_t bits) {
    PrFloat16 half{};
    prMemCopy(&half, &bits, sizeof(half));
    return prFloat16ToFloat(half);
}

uint16_t floatToHalf(float value) {
    PrFloat16 half = prFloat16FromFloat(value);
    uint16_t bits = 0;
    prMemCopy(&bits, &half, sizeof(bits));
    return bits;
}

// Returns the bits of the linear half that DWA's nonlinear half of bits `bits` stands for: x^2.2 for a magnitude x of
// at most 1, and e^(2.2 (x - 1)) above, with e as 2.7182818, each computed in floats, and zero for an infinity or a
// NaN.
uint16_t dwaToLinear(uint16_t bits) {
    if ((bits & 0x7c00) == 0x7c00 || bits == 0)
        return 0;
    float value = halfToFloat(bits);
    float sign = value < 0 ? -1.0f : 1.0f;
    float magnitude = prF32Abs(value);
    float base = float(prF64Pow(2.7182818, 2.2));
    float linear = float(prF64Pow(double(base), double(magnitude - 1.0f)));
    if (magnitude <= 1.0f)
        linear = float(prF64Pow(double(magnitude), double(2.2f)));
    return floatToHalf(sign * linear);
}

// Returns true if `suffix` matches the rule's, in case if the rule says so.
bool suffixMatches(const DwaRule& rule, PrStringView suffix) {
    if (suffix.size != rule.suffix.size)
        return false;
    for (size_t i = 0; i < suffix.size; ++i) {
        char a = suffix.data[i];
        char b = rule.suffix.data[i];
        if (rule.caseless) {
            a = a >= 'A' && a <= 'Z' ? char(a - 'A' + 'a') : a;
            b = b >= 'A' && b <= 'Z' ? char(b - 'A' + 'a') : b;
        }
        if (a != b)
            return false;
    }
    return true;
}

// Reads the rules of a chunk of version 2 from `*data`, of `size` bytes, into `rules`, and moves past them. A 16-bit
// size of the rules, counting itself, is followed by each rule: its suffix, ended by a zero byte, a byte of its color
// index plus one, its scheme and whether its suffix ignores case, and a byte of its pixel type.
bool readRules(const uint8_t** data, size_t* size, RawArray<DwaRule>& rules) {
    if (*size <= 2)
        return false;
    size_t total = prMemReadLeU16(*data);
    if (total < 2 || total > *size)
        return false;
    const uint8_t* at = *data + 2;
    size_t left = total - 2;
    *data += total;
    *size -= total;
    while (left > 0) {
        if (left <= 3)
            return false;
        size_t length = 0;
        while (length < left - 2 && length <= kMaxSuffix && at[length] != 0)
            ++length;
        if (length >= left - 2 || length > kMaxSuffix || at[length] != 0)
            return false;
        uint8_t value = at[length + 1];
        uint8_t pixel = at[length + 2];
        int32_t color = int32_t(value >> 4) - 1;
        uint32_t scheme = value >> 2 & 3;
        if (color >= 3 || scheme >= DwaScheme_Count || pixel >= ExrPixel_Count)
            return false;
        rules.push(DwaRule{
            .suffix = PrStringView{reinterpret_cast<const char*>(at), length},
            .scheme = DwaScheme(scheme),
            .pixel = ExrPixel(pixel),
            .color = int8_t(color),
            .caseless = (value & 1) != 0,
        });
        at += length + 3;
        left -= length + 3;
    }
    return true;
}

// The inverse DCT of a block of 64 coefficients, by rows and then by columns, in place. The rows past the first 8 -
// `zeroedRows` are zero, and are skipped in the first pass.
void inverseDct(float* data, uint32_t zeroedRows) {
    constexpr float a = kDctA;
    constexpr float b = kDctB;
    constexpr float c = kDctC;
    constexpr float d = kDctD;
    constexpr float e = kDctE;
    constexpr float f = kDctF;
    constexpr float g = kDctG;
    for (uint32_t row = 0; row < 8 - zeroedRows; ++row) {
        float* p = data + row * 8;
        float alpha0 = c * p[2];
        float alpha1 = f * p[2];
        float alpha2 = c * p[6];
        float alpha3 = f * p[6];
        float beta0 = b * p[1] + d * p[3] + e * p[5] + g * p[7];
        float beta1 = d * p[1] - g * p[3] - b * p[5] - e * p[7];
        float beta2 = e * p[1] - b * p[3] + g * p[5] + d * p[7];
        float beta3 = g * p[1] - e * p[3] + d * p[5] - b * p[7];
        float theta0 = a * (p[0] + p[4]);
        float theta3 = a * (p[0] - p[4]);
        float theta1 = alpha0 + alpha3;
        float theta2 = alpha1 - alpha2;
        float gamma0 = theta0 + theta1;
        float gamma1 = theta3 + theta2;
        float gamma2 = theta3 - theta2;
        float gamma3 = theta0 - theta1;
        p[0] = gamma0 + beta0;
        p[1] = gamma1 + beta1;
        p[2] = gamma2 + beta2;
        p[3] = gamma3 + beta3;
        p[4] = gamma3 - beta3;
        p[5] = gamma2 - beta2;
        p[6] = gamma1 - beta1;
        p[7] = gamma0 - beta0;
    }
    for (uint32_t column = 0; column < 8; ++column) {
        float* p = data + column;
        float alpha0 = c * p[16];
        float alpha1 = f * p[16];
        float alpha2 = c * p[48];
        float alpha3 = f * p[48];
        float beta0 = b * p[8] + d * p[24] + e * p[40] + g * p[56];
        float beta1 = d * p[8] - g * p[24] - b * p[40] - e * p[56];
        float beta2 = e * p[8] - b * p[24] + g * p[40] + d * p[56];
        float beta3 = g * p[8] - e * p[24] + d * p[40] - b * p[56];
        float theta0 = a * (p[0] + p[32]);
        float theta3 = a * (p[0] - p[32]);
        float theta1 = alpha0 + alpha3;
        float theta2 = alpha1 - alpha2;
        float gamma0 = theta0 + theta1;
        float gamma1 = theta3 + theta2;
        float gamma2 = theta3 - theta2;
        float gamma3 = theta0 - theta1;
        p[0] = gamma0 + beta0;
        p[8] = gamma1 + beta1;
        p[16] = gamma2 + beta2;
        p[24] = gamma3 + beta3;
        p[32] = gamma3 - beta3;
        p[40] = gamma2 - beta2;
        p[48] = gamma1 - beta1;
        p[56] = gamma0 - beta0;
    }
}

// Returns the rows at the end of a block that are zero, by the last nonzero coefficient in zigzag order: each row's
// first coefficient in that order follows the last of the rows above it.
uint32_t zeroedRowsOf(uint32_t lastNonZero) {
    constexpr uint8_t kRowStarts[] = {2, 3, 9, 10, 20, 21, 35};
    for (uint32_t i = 0; i < 7; ++i) {
        if (lastNonZero < kRowStarts[i])
            return 7 - i;
    }
    return 0;
}

// Converts Y'CbCr to R'G'B' by the inverse of Rec. ITU-R BT.709's matrix.
inline void inverseCsc(float* c0, float* c1, float* c2) {
    float y = *c0;
    float cb = *c1;
    float cr = *c2;
    *c0 = y + 1.5747f * cr;
    *c1 = y - 0.1873f * cb - 0.4682f * cr;
    *c2 = y + 1.8556f * cb;
}

// What decoding one set of lossy channels reads: the AC coefficients from `ac` to `acEnd`, and the DC coefficients from
// `dc`, `dcLeft` of them.
struct DwaCoefficients {
    const uint16_t* ac;
    const uint16_t* acEnd;
    const uint16_t* dc;
    uint64_t dcLeft;
};

// Decodes `count` lossy channels, three of a set of red, green and blue or one alone, of `width` by `height` samples,
// into their rows: `rows` holds the start of each line of each channel in turn. A set converts from Y'CbCr. The values
// convert to linear through `toLinear`, or stay as they are if it is null. A channel of floats has rows of floats,
// which its halves widen to. Returns false for coefficients that end early. `blockRow` holds a row of blocks.
bool decodeLossy(const ExrBlockChannel* const* channels, uint32_t count, uint8_t* const* const* rows, uint32_t width,
        uint32_t height, const uint16_t* toLinear, DwaCoefficients* coefficients, RawArray<uint16_t>& blockRow) {
    uint32_t blocksX = (width + 7) / 8;
    uint32_t blocksY = (height + 7) / 8;
    uint32_t leftoverX = width - (blocksX - 1) * 8;
    uint32_t leftoverY = height - (blocksY - 1) * 8;
    uint64_t blocks = uint64_t(blocksX) * blocksY;
    if (coefficients->dcLeft < count * blocks)
        return false;
    blockRow.resize(count * blocksX * 64);
    const uint16_t* dc[3];
    for (uint32_t comp = 0; comp < count; ++comp)
        dc[comp] = coefficients->dc + comp * blocks;
    const uint16_t* ac = coefficients->ac;
    float dct[3][64];
    for (uint32_t by = 0; by < blocksY; ++by) {
        uint32_t maxY = by == blocksY - 1 ? leftoverY : 8;
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            bool constant = true;
            for (uint32_t comp = 0; comp < count; ++comp) {
                uint16_t zig[64] = {};
                zig[0] = *dc[comp]++;
                // The AC coefficients after the DC, in zigzag order, where 0xff00 plus n is a run of n zeros, and
                // 0xff00 ends the block.
                uint32_t at = 1;
                uint32_t lastNonZero = 0;
                while (at < 64) {
                    if (ac >= coefficients->acEnd)
                        return false;
                    uint16_t value = *ac++;
                    if ((value & 0xff00) == 0xff00) {
                        uint32_t run = value & 0xff;
                        at += run == 0 ? 64 : run;
                    } else {
                        lastNonZero = at;
                        zig[at++] = value;
                    }
                }
                float* block = dct[comp];
                if (lastNonZero == 0) {
                    float value = halfToFloat(zig[0]) * kDcScale * kDcScale;
                    for (uint32_t i = 0; i < 64; ++i)
                        block[i] = value;
                } else {
                    constant = false;
                    for (uint32_t i = 0; i < 64; ++i)
                        block[i] = halfToFloat(zig[kZigzag[i]]);
                    inverseDct(block, zeroedRowsOf(lastNonZero));
                }
            }
            if (count == 3) {
                uint32_t values = constant ? 1 : 64;
                for (uint32_t i = 0; i < values; ++i)
                    inverseCsc(&dct[0][i], &dct[1][i], &dct[2][i]);
            }
            for (uint32_t comp = 0; comp < count; ++comp) {
                uint16_t* out = blockRow.data + (size_t(comp) * blocksX + bx) * 64;
                if (constant) {
                    uint16_t value = floatToHalf(dct[comp][0]);
                    for (uint32_t i = 0; i < 64; ++i)
                        out[i] = value;
                } else {
                    for (uint32_t i = 0; i < 64; ++i)
                        out[i] = floatToHalf(dct[comp][i]);
                }
            }
        }
        // The row of blocks becomes the channels' lines.
        for (uint32_t comp = 0; comp < count; ++comp) {
            const uint16_t* lut = toLinear;
            const uint16_t* blockData = blockRow.data + size_t(comp) * blocksX * 64;
            for (uint32_t y = by * 8; y < by * 8 + maxY; ++y) {
                uint8_t* line = rows[comp][y];
                for (uint32_t bx = 0; bx < blocksX; ++bx) {
                    uint32_t columns = bx == blocksX - 1 ? leftoverX : 8;
                    const uint16_t* source = blockData + size_t(bx) * 64 + (y & 7) * 8;
                    uint8_t* out = line + size_t(bx) * 16;
                    for (uint32_t x = 0; x < columns; ++x)
                        prMemWriteLeU16(out + x * 2, lut != nullptr ? lut[source[x]] : source[x]);
                }
            }
        }
    }
    coefficients->ac = ac;
    coefficients->dc += count * blocks;
    coefficients->dcLeft -= count * blocks;
    // A channel of floats widens its halves in place, from the end of each line.
    for (uint32_t comp = 0; comp < count; ++comp) {
        if (channels[comp]->pixel != ExrPixel_Float)
            continue;
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* line = rows[comp][y];
            for (uint32_t x = width; x-- > 0;) {
                float value = halfToFloat(prMemReadLeU16(line + size_t(x) * 2));
                uint32_t bits = 0;
                prMemCopy(&bits, &value, sizeof(bits));
                prMemWriteLeU32(line + size_t(x) * 4, bits);
            }
        }
    }
    return true;
}

} // namespace

PrStatus exrDwaDecompress(ExrCodec& codec, const ExrBlock& block, PrByteView packed, uint8_t* out, size_t size) {
    constexpr size_t kHeaderBytes = DwaSize_Count * 8;
    if (packed.size < kHeaderBytes)
        return PrStatus_InvalidData;
    uint64_t sizes[DwaSize_Count];
    for (uint32_t i = 0; i < DwaSize_Count; ++i)
        sizes[i] = prMemReadLeU64(packed.data + i * 8);
    uint64_t compressed = 0;
    constexpr uint32_t kCompressedSizes[] = {
        DwaSize_UnknownCompressed, DwaSize_AcCompressed, DwaSize_DcCompressed, DwaSize_RleCompressed,
    };
    for (uint32_t i : kCompressedSizes) {
        if (sizes[i] > packed.size)
            return PrStatus_InvalidData;
        compressed += sizes[i];
    }
    if (packed.size < kHeaderBytes + compressed || sizes[DwaSize_UnknownUncompressed] > size ||
            sizes[DwaSize_RleRaw] > size || sizes[DwaSize_UnknownUncompressed] + sizes[DwaSize_RleRaw] > size)
        return PrStatus_InvalidData;
    for (uint64_t value : sizes) {
        if (int64_t(value) < 0)
            return PrStatus_InvalidData;
    }
    uint64_t version = sizes[DwaSize_Version];
    const uint8_t* data = packed.data + kHeaderBytes;
    size_t left = packed.size - kHeaderBytes;

    // The rules that classify the channels.
    const DwaRule* rules = kLegacyRules;
    uint32_t ruleCount = countOf(kLegacyRules);
    RawArray<DwaRule> fileRules;
    if (version >= 2) {
        if (!readRules(&data, &left, fileRules)) {
            fileRules.reset();
            return PrStatus_InvalidData;
        }
        rules = fileRules.data;
        ruleCount = fileRules.count;
    }
    if (version > 2) {
        fileRules.reset();
        return PrStatus_InvalidData;
    }

    // Each channel's scheme is that of the last rule that matches it, and the channels of one layer that rules name
    // red, green and blue form a set.
    uint32_t channelCount = block.channelCount;
    RawArray<DwaChannel> channels;
    channels.resizeZeroed(channelCount);
    RawArray<int32_t> sets;          // three channels for each layer, or -1, by layer in the order they appear
    RawArray<PrStringView> layers;   // by layer
    for (uint32_t c = 0; c < channelCount; ++c) {
        PrStringView name = block.channels[c].name;
        size_t dot = name.size;
        while (dot > 0 && name.data[dot - 1] != '.')
            --dot;
        PrStringView layer{name.data, dot};
        PrStringView suffix{name.data + dot, name.size - dot};
        uint32_t index = 0;
        while (index < layers.count && !(layers.data[index] == layer))
            ++index;
        if (index == layers.count) {
            layers.push(layer);
            for (uint32_t k = 0; k < 3; ++k)
                sets.push(-1);
        }
        for (uint32_t r = 0; r < ruleCount; ++r) {
            if (rules[r].pixel != block.channels[c].pixel || !suffixMatches(rules[r], suffix))
                continue;
            channels.data[c].scheme = rules[r].scheme;
            if (rules[r].color >= 0)
                sets.data[index * 3 + uint32_t(rules[r].color)] = int32_t(c);
        }
    }
    fileRules.reset();

    // The bounds of the coefficients and the planes, from the chunk's extent.
    uint64_t blocksAcross = (uint64_t(block.width) + 7) / 8;
    uint64_t blocksDown = (uint64_t(block.height) + 7) / 8;
    uint64_t pixels = uint64_t(block.width) * block.height;
    uint64_t lossy = 0;
    uint64_t rleBytes = 0;
    uint64_t unknownBytes = 0;
    for (uint32_t c = 0; c < channelCount; ++c) {
        uint32_t bytes = exrPixelBytes(block.channels[c].pixel);
        if (channels.data[c].scheme == DwaScheme_LossyDct)
            lossy += 1;
        else if (channels.data[c].scheme == DwaScheme_Rle)
            rleBytes += 2 * pixels * bytes;
        else
            unknownBytes += pixels * bytes;
    }
    uint64_t acCount = sizes[DwaSize_AcCount];
    uint64_t dcCount = sizes[DwaSize_DcCount];
    if (acCount > blocksAcross * blocksDown * 63 * lossy || dcCount > blocksAcross * blocksDown * lossy ||
            sizes[DwaSize_RleUncompressed] > rleBytes || sizes[DwaSize_RleRaw] > rleBytes ||
            sizes[DwaSize_UnknownUncompressed] > unknownBytes + unknownBytes / 1000 + 64 ||
            compressed > left || acCount > 0xffffffffu || dcCount > 0xffffffffu ||
            sizes[DwaSize_RleRaw] > 0xffffffffu || sizes[DwaSize_RleUncompressed] > 0xffffffffu)
        return PrStatus_InvalidData;
    const uint8_t* unknownData = data;
    const uint8_t* acData = unknownData + sizes[DwaSize_UnknownCompressed];
    const uint8_t* dcData = acData + sizes[DwaSize_AcCompressed];
    const uint8_t* rleData = dcData + sizes[DwaSize_DcCompressed];

    // The storage of the decoded parts: the UNKNOWN bytes, the AC and DC coefficients, and the RLE planes.
    uint64_t unknownSize = sizes[DwaSize_UnknownUncompressed];
    uint64_t rleRaw = sizes[DwaSize_RleRaw];
    uint64_t total = unknownSize + rleRaw + sizes[DwaSize_RleUncompressed] + (acCount + dcCount * 2) * 2;
    if (total > 0xffffffffu)
        return PrStatus_InvalidData;
    codec.scratch.resize(uint32_t(total));
    uint8_t* unknown = codec.scratch.data;
    uint8_t* rlePlanes = unknown + unknownSize;
    uint8_t* rleRuns = rlePlanes + rleRaw;
    uint16_t* acValues = reinterpret_cast<uint16_t*>(rleRuns + sizes[DwaSize_RleUncompressed]);
    uint16_t* dcValues = acValues + acCount;
    uint8_t* dcBytes = reinterpret_cast<uint8_t*>(dcValues + dcCount);
    PrStatus status = PrStatus_Ok;
    if (sizes[DwaSize_UnknownCompressed] > 0)
        status = exrInflate(codec, PrByteView{unknownData, size_t(sizes[DwaSize_UnknownCompressed])}, unknown,
            size_t(unknownSize));
    if (status == PrStatus_Ok && sizes[DwaSize_AcCompressed] > 0) {
        PrByteView acBytes{acData, size_t(sizes[DwaSize_AcCompressed])};
        if (sizes[DwaSize_AcCompression] == DwaAc_Huffman) {
            status = exrHuffmanDecode(codec, acBytes, acValues, size_t(acCount));
        } else if (sizes[DwaSize_AcCompression] == DwaAc_Deflate) {
            status = exrInflate(codec, acBytes, reinterpret_cast<uint8_t*>(acValues), size_t(acCount * 2));
            for (uint64_t i = 0; status == PrStatus_Ok && i < acCount; ++i)
                acValues[i] = prMemReadLeU16(reinterpret_cast<const uint8_t*>(acValues + i));
        } else {
            status = PrStatus_InvalidData;
        }
    }
    if (status == PrStatus_Ok && sizes[DwaSize_DcCompressed] > 0) {
        status = exrInflate(codec, PrByteView{dcData, size_t(sizes[DwaSize_DcCompressed])}, dcBytes, size_t(dcCount * 2));
        if (status == PrStatus_Ok) {
            exrUnpredict(dcBytes, reinterpret_cast<uint8_t*>(dcValues), size_t(dcCount * 2));
            for (uint64_t i = 0; i < dcCount; ++i)
                dcValues[i] = prMemReadLeU16(reinterpret_cast<const uint8_t*>(dcValues + i));
        }
    } else if (status == PrStatus_Ok && dcCount != 0) {
        status = PrStatus_InvalidData;
    }
    if (status == PrStatus_Ok && rleRaw > 0) {
        status = exrInflate(codec, PrByteView{rleData, size_t(sizes[DwaSize_RleCompressed])}, rleRuns,
            size_t(sizes[DwaSize_RleUncompressed]));
        // The runs are those of RLE compression, without its deltas.
        size_t at = 0;
        size_t written = 0;
        size_t runs = size_t(sizes[DwaSize_RleUncompressed]);
        while (status == PrStatus_Ok && at < runs) {
            int8_t n = int8_t(rleRuns[at++]);
            if (n < 0) {
                size_t copied = size_t(-int32_t(n));
                if (copied > runs - at || copied > rleRaw - written) {
                    status = PrStatus_InvalidData;
                    break;
                }
                prMemCopy(rlePlanes + written, rleRuns + at, copied);
                at += copied;
                written += copied;
            } else {
                size_t repeated = size_t(n) + 1;
                if (at == runs || repeated > rleRaw - written) {
                    status = PrStatus_InvalidData;
                    break;
                }
                prMemFill(rlePlanes + written, rleRuns[at++], repeated);
                written += repeated;
            }
        }
        if (status == PrStatus_Ok && written != rleRaw)
            status = PrStatus_InvalidData;
    }
    if (status != PrStatus_Ok)
        return status;

    // Each channel's lines in the chunk's pixels, one after another for each line of the chunk.
    prMemZero(out, size);
    uint64_t lines = 0;
    for (uint32_t c = 0; c < channelCount; ++c) {
        channels.data[c].firstRow = uint32_t(lines);
        lines += block.channels[c].height;
    }
    RawArray<uint8_t*> rows;
    rows.resize(uint32_t(lines));
    RawArray<uint32_t> filled;      // the rows of each channel placed so far, by channel
    filled.resizeZeroed(channelCount);
    uint8_t* at = out;
    for (uint32_t row = 0; row < block.height; ++row) {
        int32_t y = block.y + int32_t(row);
        for (uint32_t c = 0; c < channelCount; ++c) {
            const ExrBlockChannel& channel = block.channels[c];
            if (channel.height == 0 || !exrChannelSamples(channel, y))
                continue;
            rows.data[channels.data[c].firstRow + filled.data[c]++] = at;
            at += size_t(channel.width) * exrPixelBytes(channel.pixel);
        }
    }
    filled.reset();
    if (size_t(at - out) != size) {
        rows.reset();
        return PrStatus_InvalidData;
    }

    if (codec.dwaToLinear == nullptr) {
        codec.dwaToLinear = static_cast<uint16_t*>(prMemoryAlloc(65536 * sizeof(uint16_t)));
        for (uint32_t i = 0; i < 65536; ++i)
            codec.dwaToLinear[i] = dwaToLinear(uint16_t(i));
    }
    DwaCoefficients coefficients{.ac = acValues, .acEnd = acValues + acCount, .dc = dcValues, .dcLeft = dcCount};
    bool ok = true;
    // The sets of red, green and blue first, then the other channels in order.
    for (uint32_t s = 0; ok && s < layers.count; ++s) {
        const int32_t* set = sets.data + s * 3;
        if (set[0] < 0 || set[1] < 0 || set[2] < 0)
            continue;
        const ExrBlockChannel* members[3] = {&block.channels[set[0]], &block.channels[set[1]], &block.channels[set[2]]};
        bool sampled = true;
        for (uint32_t k = 1; k < 3; ++k)
            sampled = sampled && block.channels[set[k]].ySampling == members[0]->ySampling &&
                block.channels[set[k]].width == members[0]->width;
        if (!sampled)
            continue;
        for (uint32_t k = 0; k < 3; ++k)
            ok = ok && channels.data[set[k]].scheme == DwaScheme_LossyDct;
        if (!ok)
            break;
        uint8_t* const* setRows[3] = {
            rows.data + channels.data[set[0]].firstRow, rows.data + channels.data[set[1]].firstRow,
            rows.data + channels.data[set[2]].firstRow,
        };
        if (members[0]->width != 0 && members[0]->height != 0)
            ok = decodeLossy(members, 3, setRows, members[0]->width, members[0]->height, codec.dwaToLinear,
                &coefficients, codec.words);
        for (uint32_t k = 0; k < 3; ++k)
            channels.data[set[k]].done = true;
    }
    uint8_t* rleAt = rlePlanes;
    uint8_t* unknownAt = unknown;
    for (uint32_t c = 0; ok && c < channelCount; ++c) {
        const ExrBlockChannel& channel = block.channels[c];
        DwaChannel& state = channels.data[c];
        if (state.done || channel.width == 0 || channel.height == 0)
            continue;
        uint32_t bytes = exrPixelBytes(channel.pixel);
        uint8_t* const* channelRows = rows.data + state.firstRow;
        size_t lineBytes = size_t(channel.width) * bytes;
        if (state.scheme == DwaScheme_LossyDct) {
            const ExrBlockChannel* members[1] = {&channel};
            ok = decodeLossy(members, 1, &channelRows, channel.width, channel.height,
                channel.linear ? nullptr : codec.dwaToLinear, &coefficients, codec.words);
        } else if (state.scheme == DwaScheme_Rle) {
            // The channel's samples are stored as planes of their bytes, the first byte of each sample first.
            size_t plane = size_t(channel.width) * channel.height;
            if (plane * bytes > size_t(rlePlanes + rleRaw - rleAt)) {
                ok = false;
                break;
            }
            for (uint32_t row = 0; row < channel.height; ++row) {
                uint8_t* line = channelRows[row];
                for (uint32_t x = 0; x < channel.width; ++x) {
                    for (uint32_t k = 0; k < bytes; ++k)
                        line[x * bytes + k] = rleAt[k * plane + size_t(row) * channel.width + x];
                }
            }
            rleAt += plane * bytes;
        } else {
            for (uint32_t row = 0; row < channel.height; ++row) {
                if (lineBytes > size_t(unknown + unknownSize - unknownAt)) {
                    ok = false;
                    break;
                }
                prMemCopy(channelRows[row], unknownAt, lineBytes);
                unknownAt += lineBytes;
            }
        }
        state.done = true;
    }
    rows.reset();
    return ok ? PrStatus_Ok : PrStatus_InvalidData;
}

} // namespace pr_impl
