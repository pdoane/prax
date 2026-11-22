// Tests of the block-compressed formats and of the swizzled formats: a texture of blocks whose texels are known, made
// by an upload and by a copy from a buffer, is sampled texel by texel on each device that has the format, and compared
// with what the format's specification decodes the blocks to, with its channels placed as its swizzle places them.
//
// Each block is of a mode that is simple to write by hand, and the texels that it decodes to are computed beside its
// bytes, as its format's specification gives them:
//
// BC1 to BC5 -
// https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression
// BC6H and BC7 - https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc6h-format and bc7-format ETC2 and EAC -
// Khronos Data Format Specification 1.3, chapter 21 ASTC - Khronos Data Format Specification 1.3, chapter 23: a
// void-extent block, which holds one color

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/mem.h"

#include "gpu/tests/gpu_fixture.h"
#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

using namespace pr;

namespace {

// The most texels of a block: those of ASTC's largest.
constexpr uint32_t kMaxBlockTexels = 144;

// A block's bytes, and the texels that it decodes to, in rows: red, green, blue and alpha as a shader samples them.
struct Block {
    uint8_t bytes[16];
    float texels[kMaxBlockTexels][4];
};

// Writes bits from the lowest bit of the first byte.
struct BitWriter {
    uint8_t* bytes;
    uint32_t at = 0;

    void put(uint32_t value, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i, ++at) {
            if ((value >> i & 1) != 0)
                bytes[at / 8] |= uint8_t(1u << (at % 8));
        }
    }
};

float srgbToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f : float(prF64Pow((double(v) + 0.055) / 1.055, 2.4));
}

// Returns the value of the bits of a half float that is not negative, infinite or a NaN.
float halfToFloat(uint32_t half) {
    uint32_t exponent = half >> 10 & 31;
    float mantissa = float(half & 1023) / 1024.0f;
    if (exponent == 0)
        return mantissa * float(prF64Pow(2.0, -14.0));
    return (1.0f + mantissa) * float(prF64Pow(2.0, double(exponent) - 15.0));
}

// The weights of BC6H's and BC7's indices of four bits.
constexpr uint32_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// Writes the 8 bytes of BC1's colors, of four colors, and their red, green and blue.
void bc1Colors(uint8_t* bytes, uint32_t v, float texels[][4]) {
    uint32_t c0[3] = {31 - v, 40 + v * 3, 5 + v};
    uint32_t c1[3] = {3 + v, 10, 20};
    uint32_t packed0 = c0[0] << 11 | c0[1] << 5 | c0[2];
    uint32_t packed1 = c1[0] << 11 | c1[1] << 5 | c1[2];
    bytes[0] = uint8_t(packed0);
    bytes[1] = uint8_t(packed0 >> 8);
    bytes[2] = uint8_t(packed1);
    bytes[3] = uint8_t(packed1 >> 8);
    float palette[4][3];
    for (uint32_t c = 0; c < 3; ++c) {
        uint32_t bits = c == 1 ? 6 : 5;
        float a = float(c0[c] << (8 - bits) | c0[c] >> (2 * bits - 8)) / 255.0f;
        float b = float(c1[c] << (8 - bits) | c1[c] >> (2 * bits - 8)) / 255.0f;
        palette[0][c] = a;
        palette[1][c] = b;
        palette[2][c] = (2 * a + b) / 3;
        palette[3][c] = (a + 2 * b) / 3;
    }
    BitWriter indices{bytes + 4};
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t index = (i + v) % 4;
        indices.put(index, 2);
        for (uint32_t c = 0; c < 3; ++c)
            texels[i][c] = palette[index][c];
    }
}

// Writes the 8 bytes of a BC4 block of eight values, and its values into `channel` of the texels.
void bc4Values(uint8_t* bytes, uint32_t v, bool snorm, float texels[][4], uint32_t channel) {
    uint32_t e0 = snorm ? 120 - 10 * v : 200 - 10 * v;
    uint32_t e1 = snorm ? 10 + 2 * v : 30 + 5 * v;
    bytes[0] = uint8_t(e0);
    bytes[1] = uint8_t(e1);
    BitWriter indices{bytes + 2};
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t index = (i + v) % 8;
        indices.put(index, 3);
        float value = float(index == 0 ? e0 : e1);
        if (index > 1)
            value = (float(8 - index) * float(e0) + float(index - 1) * float(e1)) / 7.0f;
        texels[i][channel] = value / (snorm ? 127.0f : 255.0f);
    }
}

void bc6h(uint8_t* bytes, uint32_t v, bool isSigned, float texels[][4]) {
    // The mode of one region whose endpoints are stored whole, in 10 bits a channel.
    uint32_t a[3] = {100 + 20 * v, 480, 250 + v};
    uint32_t b[3] = {490, 150 + 10 * v, 420};
    BitWriter bits{bytes};
    bits.put(3, 5);
    for (uint32_t c = 0; c < 3; ++c) {
        a[c] = isSigned ? a[c] / 2 : a[c];
        bits.put(a[c], 10);
    }
    for (uint32_t c = 0; c < 3; ++c) {
        b[c] = isSigned ? b[c] / 2 : b[c];
        bits.put(b[c], 10);
    }
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t index = i == 0 ? v % 8 : (i + v) % 16;
        bits.put(index, i == 0 ? 3 : 4);
        for (uint32_t c = 0; c < 3; ++c) {
            uint32_t from = isSigned ? ((a[c] << 15) + 0x4000) >> 9 : ((a[c] << 16) + 0x8000) >> 10;
            uint32_t to = isSigned ? ((b[c] << 15) + 0x4000) >> 9 : ((b[c] << 16) + 0x8000) >> 10;
            uint32_t mixed = (from * (64 - kWeights4[index]) + to * kWeights4[index] + 32) >> 6;
            texels[i][c] = halfToFloat(isSigned ? mixed * 31 >> 5 : mixed * 31 >> 6);
        }
    }
}

void bc7(uint8_t* bytes, uint32_t v, float texels[][4]) {
    // Mode 6: one region, endpoints of seven bits and a bit of their own, and indices of four bits.
    uint32_t e0[4] = {20 + v * 8, 100, 127 - v, 127};
    uint32_t e1[4] = {110, 5 + v, 60, 40 + v};
    uint32_t p0 = v & 1;
    uint32_t p1 = 1;
    BitWriter bits{bytes};
    bits.put(0x40, 7);
    for (uint32_t c = 0; c < 4; ++c) {
        bits.put(e0[c], 7);
        bits.put(e1[c], 7);
    }
    bits.put(p0, 1);
    bits.put(p1, 1);
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t index = i == 0 ? v % 8 : (i + v) % 16;
        bits.put(index, i == 0 ? 3 : 4);
        for (uint32_t c = 0; c < 4; ++c) {
            uint32_t from = e0[c] << 1 | p0;
            uint32_t to = e1[c] << 1 | p1;
            texels[i][c] = float((from * (64 - kWeights4[index]) + to * kWeights4[index] + 32) >> 6) / 255.0f;
        }
    }
}

// The modifiers of ETC1's tables, and of EAC's.
constexpr int kEtcModifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
constexpr int kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9}, {-2, -4, -8, -10, 1, 3, 7, 9}, {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9}, {-1, -2, -3, -10, 0, 1, 2, 9}, {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

enum {
    EtcMode_Individual,     // two colors of four bits a channel
    EtcMode_Opaque,         // a color of five bits a channel and a difference, in a block with alpha that is opaque
    EtcMode_Punchthrough,   // the same, in a block where one index is a transparent texel
};
typedef uint8_t EtcMode;

// Writes the 8 bytes of ETC2's colors, in a mode that ETC1 has, and their red, green, blue and alpha.
void etc2Colors(uint8_t* bytes, uint32_t v, EtcMode mode, float texels[][4]) {
    int base[2][3];
    if (mode == EtcMode_Individual) {
        uint32_t first[3] = {(3 + v) & 15, 9, 5};
        uint32_t second[3] = {12, (2 + v) & 15, 14};
        for (uint32_t c = 0; c < 3; ++c) {
            bytes[c] = uint8_t(first[c] << 4 | second[c]);
            base[0][c] = int(first[c] * 17);
            base[1][c] = int(second[c] * 17);
        }
    } else {
        uint32_t color[3] = {10 + v, 20, 7 + v};
        int difference[3] = {1, -1, 0};
        for (uint32_t c = 0; c < 3; ++c) {
            bytes[c] = uint8_t(color[c] << 3 | (uint32_t(difference[c]) & 7));
            uint32_t other = uint32_t(int(color[c]) + difference[c]);
            base[0][c] = int(color[c] << 3 | color[c] >> 2);
            base[1][c] = int(other << 3 | other >> 2);
        }
    }
    uint32_t tables[2] = {v % 8, (v + 3) % 8};
    uint32_t flip = v & 1;
    uint32_t flag = mode == EtcMode_Opaque ? 1 : 0;
    bytes[3] = uint8_t(tables[0] << 5 | tables[1] << 2 | flag << 1 | flip);
    uint32_t word = 0;
    for (uint32_t x = 0; x < 4; ++x) {
        for (uint32_t y = 0; y < 4; ++y) {
            uint32_t pixel = x * 4 + y;
            uint32_t index = (pixel + v) % 4;
            word |= (index >> 1) << (16 + pixel) | (index & 1) << pixel;
            uint32_t half = (flip != 0 ? y : x) < 2 ? 0 : 1;
            int small = mode == EtcMode_Punchthrough ? 0 : kEtcModifiers[tables[half]][0];
            int large = kEtcModifiers[tables[half]][1];
            int modifier = (index & 1) != 0 ? large : small;
            modifier = (index & 2) != 0 ? -modifier : modifier;
            float* texel = texels[y * 4 + x];
            bool transparent = mode == EtcMode_Punchthrough && index == 2;
            for (uint32_t c = 0; c < 3; ++c) {
                int value = base[half][c] + modifier;
                value = value < 0 ? 0 : value > 255 ? 255 : value;
                texel[c] = transparent ? 0.0f : float(value) / 255.0f;
            }
            texel[3] = transparent ? 0.0f : 1.0f;
        }
    }
    bytes[4] = uint8_t(word >> 24);
    bytes[5] = uint8_t(word >> 16);
    bytes[6] = uint8_t(word >> 8);
    bytes[7] = uint8_t(word);
}

enum {
    EacKind_Alpha8,
    EacKind_R11,
    EacKind_R11Snorm,
};
typedef uint8_t EacKind;

// Writes the 8 bytes of an EAC block, and its values into `channel` of the texels.
void eacValues(uint8_t* bytes, uint32_t v, EacKind kind, float texels[][4], uint32_t channel) {
    int base = kind == EacKind_R11Snorm ? int(40 + v * 5) : int(120 + v * 10);
    int multiplier = int(1 + v % 3);
    uint32_t table = (v * 5 + 1) % 16;
    bytes[0] = uint8_t(base);
    bytes[1] = uint8_t(uint32_t(multiplier) << 4 | table);
    uint64_t indices = 0;
    for (uint32_t x = 0; x < 4; ++x) {
        for (uint32_t y = 0; y < 4; ++y) {
            uint32_t pixel = x * 4 + y;
            uint32_t index = (pixel + v) % 8;
            indices |= uint64_t(index) << (45 - 3 * pixel);
            int modifier = kEacModifiers[table][index] * multiplier;
            float value = 0;
            if (kind == EacKind_Alpha8) {
                int alpha = base + modifier;
                value = float(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha) / 255.0f;
            } else if (kind == EacKind_R11) {
                int red = base * 8 + 4 + modifier * 8;
                value = float(red < 0 ? 0 : red > 2047 ? 2047 : red) / 2047.0f;
            } else {
                int red = base * 8 + modifier * 8;
                value = float(red < -1023 ? -1023 : red > 1023 ? 1023 : red) / 1023.0f;
            }
            texels[y * 4 + x][channel] = value;
        }
    }
    for (uint32_t i = 0; i < 6; ++i)
        bytes[2 + i] = uint8_t(indices >> (40 - 8 * i));
}

// Writes an ASTC block of one color for every texel: of 16 bits a channel in the LDR profile, whose high and low bytes
// are the same so that a decoder to 8 bits and one to 16 agree, or of half floats in the HDR profile.
void astcVoidExtent(uint8_t* bytes, uint32_t v, bool hdr, uint32_t texelCount, float texels[][4]) {
    constexpr uint32_t kHalves[] = {0x3C00, 0x3800, 0x3400, 0x3A00, 0x3000, 0x3900, 0x3B00, 0x3600, 0x0000};
    constexpr float kHalfValues[] = {1.0f, 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.875f, 0.375f, 0.0f};
    bytes[0] = 0xFC;
    bytes[1] = hdr ? 0xFF : 0xFD;
    for (uint32_t i = 2; i < 8; ++i)
        bytes[i] = 0xFF;
    float color[4];
    for (uint32_t c = 0; c < 4; ++c) {
        uint32_t byte = (40 + v * 37 + c * 61) % 256;
        uint32_t pick = (v + c * 2) % 9;
        uint32_t stored = hdr ? kHalves[pick] : byte * 257;
        bytes[8 + c * 2] = uint8_t(stored);
        bytes[9 + c * 2] = uint8_t(stored >> 8);
        color[c] = hdr ? kHalfValues[pick] : float(byte) / 255.0f;
    }
    for (uint32_t i = 0; i < texelCount; ++i)
        prMemCopy(texels[i], color, sizeof(color));
}

// Places the channels of `count` texels as `swizzle` does.
void swizzleTexels(PrGpuSwizzle swizzle, float texels[][4], uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        float red = texels[i][0];
        float green = texels[i][1];
        const float kPlaced[PrGpuSwizzle_Count][4] = {
            {red, green, texels[i][2], texels[i][3]}, {0, 0, 0, red}, {red, red, red, 1}, {red, red, red, red},
            {1, 1, 1, red}, {red, red, red, green},
        };
        prMemCopy(texels[i], kPlaced[swizzle], sizeof(texels[i]));
    }
}

// Fills `out` with a block of `format` that differs with `v`, and the texels that it decodes to, before any decoding of
// sRGB. A swizzled format has the block of the format that it stores, with the texels placed as it places them.
void makeBlock(PrGpuFormat format, uint32_t v, Block* out) {
    if (prGpuFormatSwizzle(format) != PrGpuSwizzle_None) {
        makeBlock(prGpuFormatStored(format), v, out);
        PrGpuFormatBlock block = prGpuFormatBlock(format);
        swizzleTexels(prGpuFormatSwizzle(format), out->texels, uint32_t(block.width) * block.height);
        return;
    }
    *out = Block{};
    PrGpuFormatBlock block = prGpuFormatBlock(format);
    uint32_t texelCount = uint32_t(block.width) * block.height;
    for (uint32_t i = 0; i < texelCount; ++i)
        out->texels[i][3] = 1;
    if (prGpuFormatFamily(format) == PrGpuFormatFamily_Astc || prGpuFormatFamily(format) == PrGpuFormatFamily_AstcHdr) {
        astcVoidExtent(out->bytes, v, prGpuFormatFamily(format) == PrGpuFormatFamily_AstcHdr, texelCount, out->texels);
        return;
    }
    switch (prGpuFormatPlain(format)) {
        case PrGpuFormat_Bc1:
            bc1Colors(out->bytes, v, out->texels);
            break;
        case PrGpuFormat_Bc2: {
            BitWriter alpha{out->bytes};
            for (uint32_t i = 0; i < 16; ++i) {
                alpha.put((i + v) % 16, 4);
                out->texels[i][3] = float((i + v) % 16) / 15.0f;
            }
            bc1Colors(out->bytes + 8, v, out->texels);
            break;
        }
        case PrGpuFormat_Bc3:
            bc4Values(out->bytes, v, false, out->texels, 3);
            bc1Colors(out->bytes + 8, v, out->texels);
            break;
        case PrGpuFormat_Bc4:
        case PrGpuFormat_Bc4Snorm:
            bc4Values(out->bytes, v, format == PrGpuFormat_Bc4Snorm, out->texels, 0);
            break;
        case PrGpuFormat_Bc5:
        case PrGpuFormat_Bc5Snorm:
            bc4Values(out->bytes, v, format == PrGpuFormat_Bc5Snorm, out->texels, 0);
            bc4Values(out->bytes + 8, v + 1, format == PrGpuFormat_Bc5Snorm, out->texels, 1);
            break;
        case PrGpuFormat_Bc6HUfloat:
        case PrGpuFormat_Bc6HSfloat:
            bc6h(out->bytes, v, format == PrGpuFormat_Bc6HSfloat, out->texels);
            break;
        case PrGpuFormat_Bc7:
            bc7(out->bytes, v, out->texels);
            break;
        case PrGpuFormat_Etc2Rgb8:
            etc2Colors(out->bytes, v, EtcMode_Individual, out->texels);
            break;
        case PrGpuFormat_Etc2Rgb8A1:
            etc2Colors(out->bytes, v, v % 2 == 0 ? EtcMode_Punchthrough : EtcMode_Opaque, out->texels);
            break;
        case PrGpuFormat_Etc2Rgba8:
            etc2Colors(out->bytes + 8, v, EtcMode_Individual, out->texels);
            eacValues(out->bytes, v, EacKind_Alpha8, out->texels, 3);
            break;
        case PrGpuFormat_EacR11:
        case PrGpuFormat_EacR11Snorm:
            eacValues(out->bytes, v, format == PrGpuFormat_EacR11 ? EacKind_R11 : EacKind_R11Snorm, out->texels, 0);
            break;
        case PrGpuFormat_EacRg11:
        case PrGpuFormat_EacRg11Snorm: {
            EacKind kind = format == PrGpuFormat_EacRg11 ? EacKind_R11 : EacKind_R11Snorm;
            eacValues(out->bytes, v, kind, out->texels, 0);
            eacValues(out->bytes + 8, v + 1, kind, out->texels, 1);
            break;
        }
        default:
            FAIL("a compressed format without a test block");
    }
}

// Returns the half float of `value`, which one represents exactly, as a normal number or zero.
uint16_t halfOf(float value) {
    uint32_t bits;
    prMemCopy(&bits, &value, sizeof(bits));
    if ((bits & 0x7FFFFFFF) == 0)
        return uint16_t(bits >> 16);
    return uint16_t((bits >> 16 & 0x8000) | ((bits >> 23 & 0xFF) - 112) << 10 | (bits >> 13 & 0x3FF));
}

// Writes the value `value`, of 0 to 1, in a channel of `format`, a format that is not compressed, at `out`.
void putChannel(PrGpuFormat format, float value, uint8_t* out) {
    switch (format) {
        case PrGpuFormat_R8:
        case PrGpuFormat_Rg8:
            out[0] = uint8_t(value * 255.0f + 0.5f);
            break;
        case PrGpuFormat_R16:
        case PrGpuFormat_Rg16:
            prMemWriteLeU16(out, uint16_t(value * 65535.0f + 0.5f));
            break;
        case PrGpuFormat_R16F:
        case PrGpuFormat_Rg16F:
            prMemWriteLeU16(out, halfOf(value));
            break;
        default:
            prMemCopy(out, &value, sizeof(value));
            break;
    }
}

// The blocks of the test texture's first level, in each direction. Its second level ends inside its last blocks.
constexpr uint32_t kColumns = 3;
constexpr uint32_t kRows = 2;

// Draws `texture` into a target of its extent, texel for texel, and returns the target's texels.
Array<uint8_t> sample(Fixture& f, PrGpuTexture texture) {
    PrGpuSampler nearest{};
    PrGpuSamplerDesc samplerDesc = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Clamp);
    REQUIRE(prGpuSamplerIntern(f.device, &samplerDesc, &nearest) == PrStatus_Ok);
    PrGpuTexture target = f.target(prGpuTextureExtent(f.device, texture));
    PrGpuPipeline pipeline = f.pipeline("quadVertex", f.bindless ? "sampled" : "slotSampled");
    Push push = fill(0, 0, 0, 0);
    push.texture = texture.id;
    push.sampler = nearest.id;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
    if (!f.bindless)
        prGpuCommandsSetTexture(commands, 2, texture, nearest);
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuPassDraw(pass, 4, 1, 0, 0);
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_ShaderRead);
    gpuSubmitAndWait(commands);
    Array<uint8_t> out = f.read(target);
    prGpuPipelineRelease(f.device, pipeline);
    prGpuTextureRelease(f.device, target);
    return out;
}

// Returns `value`, a channel of `format` between 0 and 1, as the render target receives it. An sRGB channel is decoded
// to linear.
float targetValue(PrGpuFormat format, uint32_t channel, float value) {
    value = value < 0 ? 0 : value > 1 ? 1 : value;
    return channel < 3 && prGpuFormatIsSrgb(format) ? srgbToLinear(value) : value;
}

// Returns the largest error that a decoder of `format` may make in `channel` of `block`, before sRGB decoding. Returns
// a negative value if the format has no such bound. The Direct3D 11.3 functional specification (19.5.2) bounds the
// error of BC1 to BC5 by an absolute error plus 3% of the distance between a block's two endpoints. Every test block
// uses both endpoints, so that distance is the range of the block's values. BC2 stores alpha explicitly, so its alpha
// has only the absolute error.
float decoderError(PrGpuFormat format, const Block& block, uint32_t channel) {
    PrGpuFormat stored = prGpuFormatPlain(prGpuFormatStored(format));
    bool bc13 = stored == PrGpuFormat_Bc1 || stored == PrGpuFormat_Bc2 || stored == PrGpuFormat_Bc3;
    bool bc45 = stored == PrGpuFormat_Bc4 || stored == PrGpuFormat_Bc4Snorm || stored == PrGpuFormat_Bc5 ||
        stored == PrGpuFormat_Bc5Snorm;
    if (!bc13 && !bc45)
        return -1;
    bool snorm = stored == PrGpuFormat_Bc4Snorm || stored == PrGpuFormat_Bc5Snorm;
    float absolute = bc13 ? 1.0f / 255.0f : snorm ? 1.0f / 32767.0f : 1.0f / 65535.0f;
    if (stored == PrGpuFormat_Bc2 && channel == 3)
        return absolute;
    PrGpuFormatBlock size = prGpuFormatBlock(format);
    float low = block.texels[0][channel];
    float high = low;
    for (uint32_t i = 1; i < uint32_t(size.width) * size.height; ++i) {
        low = block.texels[i][channel] < low ? block.texels[i][channel] : low;
        high = block.texels[i][channel] > high ? block.texels[i][channel] : high;
    }
    return absolute + 0.03f * (high - low);
}

// Returns the number of texels in `sampled` that differ from the texels that `blocks` decode to. `sampled` has the size
// `extent`, and `blocks` are laid out in rows of `columns`. A channel matches if it is within the error that
// decoderError allows, plus half a step of the 8-bit render target. For a format without that bound, the channel must
// be within `tolerance` steps of 1/255, plus the half step.
uint32_t mismatches(PrGpuFormat format, const Block* blocks, uint32_t columns, PrUVec2 extent,
        const Array<uint8_t>& sampled, int tolerance) {
    PrGpuFormatBlock block = prGpuFormatBlock(format);
    uint32_t count = 0;
    for (uint32_t y = 0; y < extent.y; ++y) {
        for (uint32_t x = 0; x < extent.x; ++x) {
            const Block& from = blocks[y / block.height * columns + x / block.width];
            const float* expected = from.texels[(y % block.height) * block.width + x % block.width];
            const uint8_t* got = texel(sampled, extent.x, x, y);
            for (uint32_t c = 0; c < 4; ++c) {
                float error = decoderError(format, from, c);
                float low = targetValue(format, c, expected[c]) - (float(tolerance) + 0.5f) / 255.0f;
                float high = targetValue(format, c, expected[c]) + (float(tolerance) + 0.5f) / 255.0f;
                if (error >= 0) {
                    low = targetValue(format, c, expected[c] - error) - 0.5f / 255.0f;
                    high = targetValue(format, c, expected[c] + error) + 0.5f / 255.0f;
                }
                if (float(got[c]) < low * 255.0f - 1e-3f || float(got[c]) > high * 255.0f + 1e-3f) {
                    ++count;
                    break;
                }
            }
        }
    }
    return count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a format's family, block and forms are those of its name") {
    CHECK(prGpuFormatFamily(PrGpuFormat_Rgba8) == PrGpuFormatFamily_Plain);
    CHECK(prGpuFormatFamily(PrGpuFormat_Bc1) == PrGpuFormatFamily_Bc);
    CHECK(prGpuFormatFamily(PrGpuFormat_Bc7Srgb) == PrGpuFormatFamily_Bc);
    CHECK(prGpuFormatFamily(PrGpuFormat_Etc2Rgb8) == PrGpuFormatFamily_Etc2);
    CHECK(prGpuFormatFamily(PrGpuFormat_EacRg11Snorm) == PrGpuFormatFamily_Etc2);
    CHECK(prGpuFormatFamily(PrGpuFormat_Astc4x4) == PrGpuFormatFamily_Astc);
    CHECK(prGpuFormatFamily(PrGpuFormat_Astc12x12Srgb) == PrGpuFormatFamily_Astc);
    CHECK(prGpuFormatFamily(PrGpuFormat_Astc12x12Hdr) == PrGpuFormatFamily_AstcHdr);
    CHECK_FALSE(prGpuFormatIsCompressed(PrGpuFormat_D32FS8));
    CHECK(prGpuFormatIsCompressed(PrGpuFormat_Bc1));

    CHECK(prGpuFormatSrgb(PrGpuFormat_Rgba8) == PrGpuFormat_Rgba8Srgb);
    CHECK(prGpuFormatSrgb(PrGpuFormat_Bc2) == PrGpuFormat_Bc2Srgb);
    CHECK(prGpuFormatSrgb(PrGpuFormat_Bc4) == PrGpuFormat_Bc4);
    CHECK(prGpuFormatSrgb(PrGpuFormat_Etc2Rgb8A1) == PrGpuFormat_Etc2Rgb8A1Srgb);
    CHECK(prGpuFormatSrgb(PrGpuFormat_EacR11) == PrGpuFormat_EacR11);
    CHECK(prGpuFormatSrgb(PrGpuFormat_Astc8x5) == PrGpuFormat_Astc8x5Srgb);
    CHECK(prGpuFormatSrgb(PrGpuFormat_Astc8x5Hdr) == PrGpuFormat_Astc8x5Hdr);
    CHECK(prGpuFormatPlain(PrGpuFormat_Astc8x5Srgb) == PrGpuFormat_Astc8x5);
    CHECK(prGpuFormatPlain(PrGpuFormat_Bc7Srgb) == PrGpuFormat_Bc7);
    CHECK(prGpuFormatPlain(PrGpuFormat_Bc5Snorm) == PrGpuFormat_Bc5Snorm);
    CHECK(prGpuFormatIsSrgb(PrGpuFormat_Etc2Rgba8Srgb));
    CHECK_FALSE(prGpuFormatIsSrgb(PrGpuFormat_Astc4x4Hdr));

    CHECK(prGpuFormatAstc(PrUVec2{4, 4}) == PrGpuFormat_Astc4x4);
    CHECK(prGpuFormatAstc(PrUVec2{10, 6}) == PrGpuFormat_Astc10x6);
    CHECK(prGpuFormatAstc(PrUVec2{12, 12}) == PrGpuFormat_Astc12x12);
    CHECK(prGpuFormatAstc(PrUVec2{6, 10}) == PrGpuFormat_None);
    CHECK(prGpuFormatBlock(PrGpuFormat_Astc12x10Hdr).width == 12);
    CHECK(prGpuFormatBlock(PrGpuFormat_Astc12x10Hdr).height == 10);
    CHECK(prGpuFormatBlock(PrGpuFormat_EacR11).bytes == 8);
    CHECK(prGpuFormatBlock(PrGpuFormat_Etc2Rgba8).bytes == 16);

    // A level holds whole blocks, down to one block for the smallest levels.
    CHECK(prGpuFormatLevelBytes(PrGpuFormat_Rgba8, PrUVec2{5, 3}) == 60);
    CHECK(prGpuFormatLevelBytes(PrGpuFormat_Bc1, PrUVec2{9, 4}) == 24);
    CHECK(prGpuFormatLevelBytes(PrGpuFormat_Bc7, PrUVec2{1, 1}) == 16);
    CHECK(prGpuFormatRowBytes(PrGpuFormat_Astc10x5, 21) == 48);
    CHECK(prGpuFormatRows(PrGpuFormat_Astc10x5, 21) == 5);
    CHECK(prGpuFormatLevelBytes(PrGpuFormat_Astc10x5, PrUVec2{21, 21}) == 240);

    // A swizzled format is of the family and the block of the format that it stores, and reads it otherwise.
    CHECK(prGpuFormatSwizzle(PrGpuFormat_R8) == PrGpuSwizzle_None);
    CHECK(prGpuFormatStored(PrGpuFormat_Rgba8) == PrGpuFormat_Rgba8);
    CHECK(prGpuFormatSwizzle(PrGpuFormat_Bc4Mask) == PrGpuSwizzle_Mask);
    CHECK(prGpuFormatStored(PrGpuFormat_Bc4Mask) == PrGpuFormat_Bc4);
    CHECK(prGpuFormatStored(PrGpuFormat_Rg16FLuminanceAlpha) == PrGpuFormat_Rg16F);
    CHECK(prGpuFormatSwizzle(PrGpuFormat_EacRg11LuminanceAlpha) == PrGpuSwizzle_LuminanceAlpha);
    CHECK(prGpuFormatFamily(PrGpuFormat_EacR11Intensity) == PrGpuFormatFamily_Etc2);
    CHECK(prGpuFormatFamily(PrGpuFormat_Bc5LuminanceAlpha) == PrGpuFormatFamily_Bc);
    CHECK(prGpuFormatFamily(PrGpuFormat_R32FAlpha) == PrGpuFormatFamily_Plain);
    CHECK(prGpuFormatBlock(PrGpuFormat_R16Luminance).bytes == 2);
    CHECK(prGpuFormatBlock(PrGpuFormat_EacR11Luminance).bytes == 8);
    CHECK(prGpuFormatSwizzled(PrGpuFormat_R16F, PrGpuSwizzle_Intensity) == PrGpuFormat_R16FIntensity);
    CHECK(prGpuFormatSwizzled(PrGpuFormat_Bc5, PrGpuSwizzle_LuminanceAlpha) == PrGpuFormat_Bc5LuminanceAlpha);
    CHECK(prGpuFormatSwizzled(PrGpuFormat_Bc5, PrGpuSwizzle_Luminance) == PrGpuFormat_None);
    CHECK(prGpuFormatSwizzled(PrGpuFormat_Rgba8, PrGpuSwizzle_Alpha) == PrGpuFormat_None);
    CHECK(prGpuFormatSwizzled(PrGpuFormat_Rgba8, PrGpuSwizzle_None) == PrGpuFormat_Rgba8);
    for (PrGpuFormat format = 1; format < PrGpuFormat_Count; ++format) {
        CAPTURE(uint32_t(format));
        PrGpuSwizzle swizzle = prGpuFormatSwizzle(format);
        CHECK(prGpuFormatSwizzled(prGpuFormatStored(format), swizzle) == format);
        CHECK(prGpuFormatSwizzle(prGpuFormatStored(format)) == PrGpuSwizzle_None);
    }
}

TEST_CASE("a texture's record holds its description in 64 bits, and gives the bytes of its levels") {
    // The enumeration of formats is stored in 8 bits, with room to spare.
    CHECK(PrGpuFormat_Count == 151);
    CHECK(sizeof(PrGpuTextureRecord) == 8);
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_EacRg11, PrUVec2{16384, 12}, PrGpuTextureUsageFlag_Sampled);
    desc.kind = PrGpuTextureKind_Cube;
    desc.extent.depth = 6;
    desc.levels = 16;
    PrGpuTextureRecord record{};
    REQUIRE(prGpuTextureRecordMake(&desc, PrGpuTextureLayout_Linear, &record));
    PrGpuTextureDesc read{};
    PrGpuTextureLayout layout = PrGpuTextureLayout_DeviceOptimal;
    REQUIRE(prGpuTextureRecordRead(record, &read, &layout));
    CHECK(layout == PrGpuTextureLayout_Linear);
    CHECK(read.kind == PrGpuTextureKind_Cube);
    CHECK(read.format == PrGpuFormat_EacRg11);
    CHECK(read.extent.width == 16384);
    CHECK(read.extent.height == 12);
    CHECK(read.extent.depth == 6);
    CHECK(read.levels == 16);
    CHECK(read.samples == 1);

    // Two levels of a texture of 9 by 6 texels in blocks of 16 bytes: three blocks by two, and one by one.
    desc = prGpuTextureDesc2d(PrGpuFormat_Bc7, PrUVec2{9, 6}, PrGpuTextureUsageFlag_Sampled);
    desc.levels = 2;
    REQUIRE(prGpuTextureRecordMake(&desc, PrGpuTextureLayout_Linear, &record));
    CHECK(prGpuTextureRecordBytes(record) == 96 + 16);
    REQUIRE(prGpuTextureRecordMake(&desc, PrGpuTextureLayout_LinearPitch256, &record));
    CHECK(prGpuTextureRecordBytes(record) == 512 + 256);
    REQUIRE(prGpuTextureRecordMake(&desc, PrGpuTextureLayout_DeviceOptimal, &record));
    CHECK(prGpuTextureRecordBytes(record) == 0);

    // A record holds no texture past its fields, and no bits make a record of a format that is none.
    desc.extent.width = 16385;
    CHECK_FALSE(prGpuTextureRecordMake(&desc, PrGpuTextureLayout_Linear, &record));
    desc.extent.width = 9;
    desc.levels = 17;
    CHECK_FALSE(prGpuTextureRecordMake(&desc, PrGpuTextureLayout_Linear, &record));
    desc.levels = 1;
    desc.samples = 4;
    CHECK_FALSE(prGpuTextureRecordMake(&desc, PrGpuTextureLayout_Linear, &record));
    CHECK_FALSE(prGpuTextureRecordRead(PrGpuTextureRecord{}, &read, &layout));
    CHECK(read.format == PrGpuFormat_None);
}

TEST_CASE("a device samples the blocks of each compressed format that it has, as the format decodes them") {
    onEachBackend([](Fixture& f) {
        PrGpuFeatureFlags features = prGpuDeviceInfo(f.device).features;
        for (PrGpuFormat format = PrGpuFormat_Bc1; format < PrGpuFormat_Count; ++format) {
            CAPTURE(uint32_t(format));
            // A format is only sampled, and a device has it only with its family.
            PrGpuTextureUsageFlags drawn = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
            CHECK_FALSE(prGpuDeviceSupportsFormat(f.device, format, drawn));
            if (!prGpuDeviceSupportsFormat(f.device, format, PrGpuTextureUsageFlag_Sampled))
                continue;
            CHECK((features & prGpuFormatFamilyFeature(prGpuFormatFamily(format))) != 0);

            PrGpuFormatBlock block = prGpuFormatBlock(format);
            PrUVec2 extent{uint32_t(block.width) * kColumns, uint32_t(block.height) * kRows};
            Array<Block> blocks(kColumns * kRows);
            for (uint32_t i = 0; i < kColumns * kRows; ++i)
                makeBlock(format, i, &blocks[i]);
            // The upload's rows lie further apart than the blocks of a row are long.
            uint32_t packed = kColumns * block.bytes;
            uint32_t spaced = packed + 2 * block.bytes;
            Array<uint8_t> rows(spaced * kRows);
            Array<uint8_t> tight(packed * kRows);
            for (uint32_t i = 0; i < kColumns * kRows; ++i) {
                prMemCopy(&rows[i / kColumns * spaced + i % kColumns * block.bytes], blocks[i].bytes, block.bytes);
                prMemCopy(&tight[i / kColumns * packed + i % kColumns * block.bytes], blocks[i].bytes, block.bytes);
            }
            CHECK(prGpuFormatRowBytes(format, extent.x) == packed);
            CHECK(prGpuFormatLevelBytes(format, extent) == tight.count());
            // The second level is half the first, so its blocks reach past it: two blocks in one row.
            PrUVec2 half{extent.x / 2, extent.y / 2};
            constexpr uint32_t kHalfColumns = 2;
            Array<Block> halfBlocks(kHalfColumns);
            Array<uint8_t> halfRow(kHalfColumns * block.bytes);
            for (uint32_t i = 0; i < kHalfColumns; ++i) {
                makeBlock(format, kColumns * kRows + i, &halfBlocks[i]);
                prMemCopy(&halfRow[i * block.bytes], halfBlocks[i].bytes, block.bytes);
            }
            CHECK(prGpuFormatLevelBytes(format, half) == halfRow.count());
            // BC1 to BC5 are checked against the error that Direct3D allows (decoderError). Other formats are checked
            // against this tolerance, in steps of 1/255.
            int tolerance = 2;

            // No device takes a first level that ends inside a block.
            PrGpuTextureDesc desc = prGpuTextureDesc2d(format, PrUVec2{extent.x - 1, extent.y},
                PrGpuTextureUsageFlag_Sampled);
            PrGpuTexture uploaded{};
            CHECK(prGpuTextureCreate(f.device, &desc, &uploaded) == PrStatus_NotSupported);

            desc = prGpuTextureDesc2d(format, extent, PrGpuTextureUsageFlag_Sampled);
            desc.levels = 2;
            PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, extent);
            PrGpuTextureRegion halfRegion = prGpuTextureRegion2d(PrUVec2{0, 0}, half);
            halfRegion.level = 1;
            REQUIRE(prGpuTextureCreate(f.device, &desc, &uploaded) == PrStatus_Ok);
            prGpuTextureUpload(f.device, uploaded, &region, rows.data(), spaced, spaced * kRows);
            prGpuTextureUpload(f.device, uploaded, &halfRegion, halfRow.data(), halfRow.count(), halfRow.count());
            CHECK(mismatches(format, blocks.data(), kColumns, extent, sample(f, uploaded), tolerance) == 0);
            PrGpuTexture second{};
            PrGpuSubresources secondLevel{.firstLevel = 1, .levels = 1};
            PrGpuTextureKind flat = PrGpuTextureKind_2d;
            REQUIRE(prGpuTextureCreateView(f.device, uploaded,
                PrGpuTextureViewDesc{.kind = flat, .subresources = secondLevel}, &second) == PrStatus_Ok);
            CHECK(mismatches(format, halfBlocks.data(), kHalfColumns, half, sample(f, second), tolerance) == 0);
            prGpuTextureRelease(f.device, second);
            prGpuTextureRelease(f.device, uploaded);

            PrGpuTexture copied{};
            REQUIRE(prGpuTextureCreate(f.device, &desc, &copied) == PrStatus_Ok);
            PrGpuBufferDesc bufferDesc{.size = tight.count() + halfRow.count(), .memory = PrGpuMemory_Upload};
            PrGpuBuffer buffer{};
            REQUIRE(prGpuBufferCreate(f.device, &bufferDesc, &buffer) == PrStatus_Ok);
            uint8_t* mapped = static_cast<uint8_t*>(prGpuBufferMap(f.device, buffer));
            prMemCopy(mapped, tight.data(), tight.count());
            prMemCopy(mapped + tight.count(), halfRow.data(), halfRow.count());
            PrGpuCommands* commands = nullptr;
            REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
            prGpuCommandsBufferBarrier(commands, buffer, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyRead);
            prGpuCommandsTextureBarrier(commands, copied, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_CopyWrite);
            prGpuCommandsCopyBufferToTexture(commands, buffer, 0, packed, packed * kRows, copied, &region);
            prGpuCommandsCopyBufferToTexture(commands, buffer, tight.count(), halfRow.count(), halfRow.count(), copied,
                &halfRegion);
            prGpuCommandsTextureBarrier(commands, copied, PrGpuSubresources{}, PrGpuAccessFlag_CopyWrite,
                PrGpuAccessFlag_ShaderRead);
            gpuSubmitAndWait(commands);
            CHECK(mismatches(format, blocks.data(), kColumns, extent, sample(f, copied), tolerance) == 0);
            REQUIRE(prGpuTextureCreateView(f.device, copied,
                PrGpuTextureViewDesc{.kind = flat, .subresources = secondLevel}, &second) == PrStatus_Ok);
            CHECK(mismatches(format, halfBlocks.data(), kHalfColumns, half, sample(f, second), tolerance) == 0);
            prGpuTextureRelease(f.device, second);
            prGpuBufferRelease(f.device, buffer);
            prGpuTextureRelease(f.device, copied);
        }
    });
}

TEST_CASE("a device that places channels samples each swizzled format as its swizzle places the channels") {
    onEachBackend([](Fixture& f) {
        bool swizzles = (prGpuDeviceInfo(f.device).features & PrGpuFeatureFlag_TextureSwizzle) != 0;
        uint32_t drawn = 0;
        for (PrGpuFormat format = 1; format < PrGpuFormat_Count; ++format) {
            PrGpuSwizzle swizzle = prGpuFormatSwizzle(format);
            PrGpuFormat stored = prGpuFormatStored(format);
            if (swizzle == PrGpuSwizzle_None)
                continue;
            CAPTURE(uint32_t(format));
            // A swizzled format is only sampled, and only where the device places channels and has the stored format.
            CHECK_FALSE(prGpuDeviceSupportsFormat(f.device, format, PrGpuTextureUsageFlag_RenderTarget));
            CHECK_FALSE(prGpuDeviceSupportsFormat(f.device, format, PrGpuTextureUsageFlag_Storage));
            bool sampled = prGpuDeviceSupportsFormat(f.device, format, PrGpuTextureUsageFlag_Sampled);
            CHECK(sampled == (swizzles && prGpuDeviceSupportsFormat(f.device, stored, PrGpuTextureUsageFlag_Sampled)));
            PrGpuTextureDesc desc = prGpuTextureDesc2d(format, PrUVec2{4, 4}, PrGpuTextureUsageFlag_Sampled);
            PrGpuTexture texture{};
            if (!sampled) {
                CHECK(prGpuTextureCreate(f.device, &desc, &texture) == PrStatus_NotSupported);
                continue;
            }
            // The compressed formats are sampled with the others of their families.
            if (prGpuFormatIsCompressed(format))
                continue;
            // A texture of 4 by 2 texels whose two channels rise by eighths, the second from the other end.
            PrUVec2 extent{4, 2};
            uint32_t bytes = prGpuFormatBlock(format).bytes;
            bool two = swizzle == PrGpuSwizzle_LuminanceAlpha;
            Array<Block> texels(extent.x * extent.y);
            Array<uint8_t> data(extent.x * extent.y * bytes);
            for (uint32_t i = 0; i < extent.x * extent.y; ++i) {
                texels[i] = Block{};
                float values[2] = {float(i) / 8.0f, float(7 - i) / 8.0f};
                for (uint32_t c = 0; c < (two ? 2u : 1u); ++c) {
                    putChannel(stored, values[c], &data[i * bytes + c * (bytes / (two ? 2 : 1))]);
                    texels[i].texels[0][c] = values[c];
                }
                texels[i].texels[0][3] = 1;
                swizzleTexels(swizzle, texels[i].texels, 1);
            }
            desc = prGpuTextureDesc2d(format, extent, PrGpuTextureUsageFlag_Sampled);
            REQUIRE(prGpuTextureCreate(f.device, &desc, &texture) == PrStatus_Ok);
            PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, extent);
            prGpuTextureUpload(f.device, texture, &region, data.data(), extent.x * bytes, uint32_t(data.count()));
            CHECK(mismatches(format, texels.data(), extent.x, extent, sample(f, texture), 1) == 0);
            // A view reads the texture's texels as the texture does.
            PrGpuTexture view{};
            REQUIRE(prGpuTextureCreateView(f.device, texture,
                PrGpuTextureViewDesc{.kind = PrGpuTextureKind_2d, .subresources = PrGpuSubresources{}}, &view) ==
                PrStatus_Ok);
            CHECK(mismatches(format, texels.data(), extent.x, extent, sample(f, view), 1) == 0);
            prGpuTextureRelease(f.device, view);
            prGpuTextureRelease(f.device, texture);
            ++drawn;
        }
        // Every device that places channels has R8.
        CHECK(drawn >= (swizzles ? 5u : 0u));
    });
}

TEST_CASE("a texture sampled through its Srgb form decodes its bits, and through its plain form reads them as stored") {
    onEachBackend([](Fixture& f) {
        const PrGpuFormat formats[] = {PrGpuFormat_Rgba8, PrGpuFormat_Bgra8, PrGpuFormat_R8, PrGpuFormat_Rg8};
        constexpr PrGpuTextureUsageFlags kUsage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_SrgbViews;
        // A format that has only one form has no other form to view it through.
        CHECK_FALSE(prGpuDeviceSupportsFormat(f.device, PrGpuFormat_Rgba16F, kUsage));
        CHECK_FALSE(prGpuDeviceSupportsFormat(f.device, PrGpuFormat_R8Luminance, kUsage));
        uint32_t drawn = 0;
        for (PrGpuFormat plain : formats) {
            const PrGpuFormat forms[2] = {plain, prGpuFormatSrgb(plain)};
            for (PrGpuFormat format : forms) {
                CAPTURE(uint32_t(format));
                PrGpuFormat other = format == plain ? prGpuFormatSrgb(plain) : plain;
                PrGpuTextureDesc desc = prGpuTextureDesc2d(format, PrUVec2{4, 2}, kUsage);
                PrGpuTexture texture{};
                if (!prGpuDeviceSupportsFormat(f.device, format, kUsage)) {
                    CHECK(prGpuTextureCreate(f.device, &desc, &texture) == PrStatus_NotSupported);
                    continue;
                }
                // The channels of each texel hold eighths, taken from the index of the texel.
                uint32_t bytes = prGpuFormatBlock(format).bytes;
                Array<uint8_t> data(8 * bytes);
                for (uint32_t i = 0; i < 8; ++i) {
                    for (uint32_t c = 0; c < bytes; ++c)
                        data[i * bytes + c] = c == 3 ? 255 : uint8_t(i * 32);
                }
                REQUIRE(prGpuTextureCreate(f.device, &desc, &texture) == PrStatus_Ok);
                PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, PrUVec2{4, 2});
                prGpuTextureUpload(f.device, texture, &region, data.data(), 4 * bytes, uint32_t(data.count()));
                PrGpuTexture view{};
                REQUIRE(prGpuTextureCreateView(f.device, texture, PrGpuTextureViewDesc{.format = other}, &view) ==
                    PrStatus_Ok);
                CHECK(prGpuTextureDesc(f.device, view).format == other);
                const PrGpuTexture through[2] = {texture, view};
                for (PrGpuTexture read : through) {
                    bool decodes = prGpuFormatIsSrgb(prGpuTextureDesc(f.device, read).format);
                    Array<uint8_t> drawnTexels = sample(f, read);
                    uint32_t wrong = 0;
                    for (uint32_t i = 0; i < 8; ++i) {
                        float value = float(i * 32) / 255.0f;
                        float expected = decodes ? srgbToLinear(value) : value;
                        wrong += prF32Abs(float(drawnTexels[i * 4]) - expected * 255.0f) > 1.5f ? 1 : 0;
                    }
                    CHECK(wrong == 0);
                }
                prGpuTextureRelease(f.device, view);
                prGpuTextureRelease(f.device, texture);
                ++drawn;
            }
        }
        // Every device that views a format through its other form has Rgba8 and Bgra8 in both.
        CHECK((drawn == 0 || drawn >= 4));
    });
}

TEST_CASE("a format's value never changes, since records of textures store it") {
    // The first and last format of each group. A format is added at the end of the enumeration.
    CHECK(PrGpuFormat_R8 == 1);
    CHECK(PrGpuFormat_Bgra8Srgb == 22);
    CHECK(PrGpuFormat_R16 == 23);
    CHECK(PrGpuFormat_Rgba16F == 47);
    CHECK(PrGpuFormat_R32Uint == 48);
    CHECK(PrGpuFormat_Rgba32F == 61);
    CHECK(PrGpuFormat_B5G6R5 == 62);
    CHECK(PrGpuFormat_Rgb9E5F == 69);
    CHECK(PrGpuFormat_D16 == 70);
    CHECK(PrGpuFormat_S8 == 74);
    CHECK(PrGpuFormat_Bc1 == 75);
    CHECK(PrGpuFormat_Bc7Srgb == 93);
    CHECK(PrGpuFormat_Etc2Rgb8 == 94);
    CHECK(PrGpuFormat_EacRg11LuminanceAlpha == 108);
    CHECK(PrGpuFormat_Astc4x4 == 109);
    CHECK(PrGpuFormat_Astc12x12Hdr == 150);
}
