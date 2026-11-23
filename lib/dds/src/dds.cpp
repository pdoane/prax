// DDS textures.
//
// A file is the magic "DDS ", a 124-byte header, the DX10 extension header if the pixel format's FourCC is "DX10", and
// the texels. The texels are the levels of each frame in turn, largest first. All integers are little-endian.

#include "pr/dds.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/bc.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/pixels.h"

namespace pr_impl {

namespace {

// The flags of the header, of its pixel format, and of its capabilities.
constexpr uint32_t kFlagMipCount = 0x20000;
constexpr uint32_t kPixelAlphaPixels = 0x1;
constexpr uint32_t kPixelAlpha = 0x2;
constexpr uint32_t kPixelFourCc = 0x4;
constexpr uint32_t kPixelPaletteIndexed8 = 0x20;
constexpr uint32_t kPixelRgb = 0x40;
constexpr uint32_t kPixelYuv = 0x200;
constexpr uint32_t kPixelLuminance = 0x20000;
constexpr uint32_t kPixelBumpDuDv = 0x80000;
constexpr uint32_t kCaps2Cube = 0x200;
constexpr uint32_t kCaps2AllFaces = 0xfc00;
constexpr uint32_t kCaps2Volume = 0x200000;

// The DX10 header's resource dimensions, its flag of a cube, and its alpha modes.
constexpr uint32_t kDimension1d = 2;
constexpr uint32_t kDimension2d = 3;
constexpr uint32_t kDimension3d = 4;
constexpr uint32_t kMiscCube = 0x4;
constexpr uint32_t kAlphaModePremultiplied = 2;
constexpr uint32_t kAlphaModeOpaque = 3;

constexpr uint32_t fourCc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// How a stored pixel holds its channels.
enum {
    Kind_Blocks,
    Kind_Masks,         // unsigned fields of an integer of up to 32 bits, by mask
    Kind_Unorm16,       // 16-bit unsigned components, red first
    Kind_Snorm8,        // 8-bit signed components
    Kind_Snorm16,
    Kind_Float16,
    Kind_Float32,
    Kind_Rg11B10F,      // floats of 11, 11 and 10 bits without a sign
    Kind_Rgb9E5,        // mantissas of 9 bits and a shared exponent of 5
};
typedef uint8_t Kind;

// The layout of a stored format: its kind, the bytes of a pixel, and the masks or the components of its channels.
struct Layout {
    Kind kind;
    uint8_t bytes;
    uint8_t components;
    uint32_t masks[4];  // red, green, blue and alpha, for Kind_Masks
};

// A DXGI format that the library reads.
struct DxgiFormat {
    uint32_t value;
    Layout layout;
    PrGpuFormat blocks;
    bool srgb;
    bool alpha;
};

constexpr Layout masks(uint8_t bytes, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return Layout{Kind_Masks, bytes, 0, {r, g, b, a}};
}

constexpr Layout components(Kind kind, uint8_t bytes, uint8_t count) {
    return Layout{kind, bytes, count, {0, 0, 0, 0}};
}

constexpr Layout kBlocks = Layout{Kind_Blocks, 0, 0, {0, 0, 0, 0}};

constexpr DxgiFormat kDxgiFormats[] = {
    {2, components(Kind_Float32, 16, 4), PrGpuFormat_None, false, true},                   // R32G32B32A32_FLOAT
    {6, components(Kind_Float32, 12, 3), PrGpuFormat_None, false, false},                  // R32G32B32_FLOAT
    {10, components(Kind_Float16, 8, 4), PrGpuFormat_None, false, true},                   // R16G16B16A16_FLOAT
    {11, components(Kind_Unorm16, 8, 4), PrGpuFormat_None, false, true},                   // R16G16B16A16_UNORM
    {13, components(Kind_Snorm16, 8, 4), PrGpuFormat_None, false, true},                   // R16G16B16A16_SNORM
    {16, components(Kind_Float32, 8, 2), PrGpuFormat_None, false, false},                  // R32G32_FLOAT
    {24, masks(4, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000), PrGpuFormat_None, false, true}, // R10G10B10A2_UNORM
    {26, components(Kind_Rg11B10F, 4, 3), PrGpuFormat_None, false, false},                 // R11G11B10_FLOAT
    {27, masks(4, 0xff, 0xff00, 0xff0000, 0xff000000), PrGpuFormat_None, false, true},     // R8G8B8A8_TYPELESS
    {28, masks(4, 0xff, 0xff00, 0xff0000, 0xff000000), PrGpuFormat_None, false, true},     // R8G8B8A8_UNORM
    {29, masks(4, 0xff, 0xff00, 0xff0000, 0xff000000), PrGpuFormat_None, true, true},      // R8G8B8A8_UNORM_SRGB
    {31, components(Kind_Snorm8, 4, 4), PrGpuFormat_None, false, true},                    // R8G8B8A8_SNORM
    {34, components(Kind_Float16, 4, 2), PrGpuFormat_None, false, false},                  // R16G16_FLOAT
    {35, components(Kind_Unorm16, 4, 2), PrGpuFormat_None, false, false},                  // R16G16_UNORM
    {37, components(Kind_Snorm16, 4, 2), PrGpuFormat_None, false, false},                  // R16G16_SNORM
    {41, components(Kind_Float32, 4, 1), PrGpuFormat_None, false, false},                  // R32_FLOAT
    {49, masks(2, 0xff, 0xff00, 0, 0), PrGpuFormat_None, false, false},                    // R8G8_UNORM
    {51, components(Kind_Snorm8, 2, 2), PrGpuFormat_None, false, false},                   // R8G8_SNORM
    {54, components(Kind_Float16, 2, 1), PrGpuFormat_None, false, false},                  // R16_FLOAT
    {56, components(Kind_Unorm16, 2, 1), PrGpuFormat_None, false, false},                  // R16_UNORM
    {58, components(Kind_Snorm16, 2, 1), PrGpuFormat_None, false, false},                  // R16_SNORM
    {61, masks(1, 0xff, 0, 0, 0), PrGpuFormat_None, false, false},                         // R8_UNORM
    {63, components(Kind_Snorm8, 1, 1), PrGpuFormat_None, false, false},                   // R8_SNORM
    {65, masks(1, 0, 0, 0, 0xff), PrGpuFormat_None, false, true},                          // A8_UNORM
    {67, components(Kind_Rgb9E5, 4, 3), PrGpuFormat_None, false, false},                   // R9G9B9E5_SHAREDEXP
    {70, kBlocks, PrGpuFormat_Bc1, false, true},                                           // BC1_TYPELESS
    {71, kBlocks, PrGpuFormat_Bc1, false, true},                                           // BC1_UNORM
    {72, kBlocks, PrGpuFormat_Bc1, true, true},                                            // BC1_UNORM_SRGB
    {73, kBlocks, PrGpuFormat_Bc2, false, true},                                           // BC2_TYPELESS
    {74, kBlocks, PrGpuFormat_Bc2, false, true},                                           // BC2_UNORM
    {75, kBlocks, PrGpuFormat_Bc2, true, true},                                            // BC2_UNORM_SRGB
    {76, kBlocks, PrGpuFormat_Bc3, false, true},                                           // BC3_TYPELESS
    {77, kBlocks, PrGpuFormat_Bc3, false, true},                                           // BC3_UNORM
    {78, kBlocks, PrGpuFormat_Bc3, true, true},                                            // BC3_UNORM_SRGB
    {79, kBlocks, PrGpuFormat_Bc4, false, false},                                          // BC4_TYPELESS
    {80, kBlocks, PrGpuFormat_Bc4, false, false},                                          // BC4_UNORM
    {81, kBlocks, PrGpuFormat_Bc4Snorm, false, false},                                     // BC4_SNORM
    {82, kBlocks, PrGpuFormat_Bc5, false, false},                                          // BC5_TYPELESS
    {83, kBlocks, PrGpuFormat_Bc5, false, false},                                          // BC5_UNORM
    {84, kBlocks, PrGpuFormat_Bc5Snorm, false, false},                                     // BC5_SNORM
    {85, masks(2, 0xf800, 0x07e0, 0x001f, 0), PrGpuFormat_None, false, false},             // B5G6R5_UNORM
    {86, masks(2, 0x7c00, 0x03e0, 0x001f, 0x8000), PrGpuFormat_None, false, true},         // B5G5R5A1_UNORM
    {87, masks(4, 0xff0000, 0xff00, 0xff, 0xff000000), PrGpuFormat_None, false, true},     // B8G8R8A8_UNORM
    {88, masks(4, 0xff0000, 0xff00, 0xff, 0), PrGpuFormat_None, false, false},             // B8G8R8X8_UNORM
    {90, masks(4, 0xff0000, 0xff00, 0xff, 0xff000000), PrGpuFormat_None, false, true},     // B8G8R8A8_TYPELESS
    {91, masks(4, 0xff0000, 0xff00, 0xff, 0xff000000), PrGpuFormat_None, true, true},      // B8G8R8A8_UNORM_SRGB
    {92, masks(4, 0xff0000, 0xff00, 0xff, 0), PrGpuFormat_None, false, false},             // B8G8R8X8_TYPELESS
    {93, masks(4, 0xff0000, 0xff00, 0xff, 0), PrGpuFormat_None, true, false},              // B8G8R8X8_UNORM_SRGB
    {94, kBlocks, PrGpuFormat_Bc6HUfloat, false, false},                                   // BC6H_TYPELESS
    {95, kBlocks, PrGpuFormat_Bc6HUfloat, false, false},                                   // BC6H_UF16
    {96, kBlocks, PrGpuFormat_Bc6HSfloat, false, false},                                   // BC6H_SF16
    {97, kBlocks, PrGpuFormat_Bc7, false, true},                                           // BC7_TYPELESS
    {98, kBlocks, PrGpuFormat_Bc7, false, true},                                           // BC7_UNORM
    {99, kBlocks, PrGpuFormat_Bc7, true, true},                                            // BC7_UNORM_SRGB
    {115, masks(2, 0x0f00, 0x00f0, 0x000f, 0xf000), PrGpuFormat_None, false, true},        // B4G4R4A4_UNORM
};

const DxgiFormat* PR_NULLABLE findDxgi(uint32_t value) {
    for (const DxgiFormat& format : kDxgiFormats) {
        if (format.value == value)
            return &format;
    }
    return nullptr;
}

// What the headers say of the format of the texels.
struct Format {
    Layout layout;
    PrGpuFormat blocks;
    uint32_t dxgi;
    PrDdsSwizzle swizzle;
    bool srgb;
    bool alpha;
    bool premultiplied;
};

// Returns the format of a DXGI value. Returns NotSupported for one that the library does not read.
PrStatus formatOfDxgi(uint32_t value, Format* out) {
    const DxgiFormat* found = findDxgi(value);
    if (found == nullptr)
        return PrStatus_NotSupported;
    *out = Format{found->layout, found->blocks, value, PrDdsSwizzle_None, found->srgb, found->alpha, false};
    return PrStatus_Ok;
}

// Returns the format of a legacy pixel format: a FourCC, the D3DFORMAT number of a float or 16-bit format in place of
// one, or bit masks. Formats of bit masks that DXGI names take its value.
PrStatus formatOfLegacy(const uint8_t* pf, Format* out) {
    const uint32_t flags = prMemReadLeU32(pf + 4);
    const uint32_t code = prMemReadLeU32(pf + 8);
    const uint32_t bits = prMemReadLeU32(pf + 12);
    uint32_t m[4] = {prMemReadLeU32(pf + 16), prMemReadLeU32(pf + 20), prMemReadLeU32(pf + 24), prMemReadLeU32(pf + 28)};
    if ((flags & kPixelFourCc) != 0) {
        struct Code {
            uint32_t code;
            uint32_t dxgi;
            bool premultiplied;
        };
        const Code kCodes[] = {
            {fourCc('D', 'X', 'T', '1'), 71, false}, {fourCc('D', 'X', 'T', '2'), 74, true},
            {fourCc('D', 'X', 'T', '3'), 74, false}, {fourCc('D', 'X', 'T', '4'), 77, true},
            {fourCc('D', 'X', 'T', '5'), 77, false}, {fourCc('A', 'T', 'I', '1'), 80, false},
            {fourCc('B', 'C', '4', 'U'), 80, false}, {fourCc('B', 'C', '4', 'S'), 81, false},
            {fourCc('A', 'T', 'I', '2'), 83, false}, {fourCc('B', 'C', '5', 'U'), 83, false},
            {fourCc('B', 'C', '5', 'S'), 84, false},
            // D3DFMT_A16B16G16R16, Q16W16V16U16, R16F, G16R16F, A16B16G16R16F, R32F, G32R32F and A32B32G32R32F.
            {36, 11, false}, {110, 13, false}, {111, 54, false}, {112, 34, false}, {113, 10, false},
            {114, 41, false}, {115, 16, false}, {116, 2, false},
        };
        for (const Code& known : kCodes) {
            if (known.code != code)
                continue;
            PrStatus status = formatOfDxgi(known.dxgi, out);
            out->premultiplied = known.premultiplied;
            return status;
        }
        return PrStatus_NotSupported;
    }
    if ((flags & (kPixelYuv | kPixelPaletteIndexed8)) != 0)
        return PrStatus_NotSupported;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
        return PrStatus_InvalidData;
    const uint8_t bytes = uint8_t(bits / 8);
    *out = Format{masks(bytes, 0, 0, 0, 0), PrGpuFormat_None, 0, PrDdsSwizzle_None, false, false, false};
    if ((flags & kPixelBumpDuDv) != 0) {
        // V8U8, Q8W8V8U8 and V16U16: signed components.
        if (bits == 16 && m[0] == 0xff && m[1] == 0xff00)
            return formatOfDxgi(51, out);
        if (bits == 32 && m[0] == 0xff && m[1] == 0xff00 && m[2] == 0xff0000)
            return formatOfDxgi(31, out);
        if (bits == 32 && m[0] == 0xffff && m[1] == 0xffff0000)
            return formatOfDxgi(37, out);
        return PrStatus_NotSupported;
    }
    const bool alpha = (flags & (kPixelAlphaPixels | kPixelAlpha)) != 0 && m[3] != 0;
    if (!alpha)
        m[3] = 0;
    if ((flags & kPixelLuminance) != 0) {
        // Some writers give an 8-bit luminance a mask past its bits, which reads as the whole byte.
        if (bits == 8 && (m[0] & 0xffu) == 0)
            m[0] = 0xff;
        out->layout = masks(bytes, m[0], 0, 0, m[3]);
        out->swizzle = alpha ? PrDdsSwizzle_LuminanceAlpha : PrDdsSwizzle_Luminance;
        out->alpha = alpha;
        if (bits == 16 && m[0] == 0xffff && !alpha)
            *out = Format{components(Kind_Unorm16, 2, 1), PrGpuFormat_None, 56, PrDdsSwizzle_Luminance, false, false};
        else if (bits == 8 && m[0] == 0xff && !alpha)
            out->dxgi = 61;
        else if (bits == 16 && m[0] == 0xff && m[3] == 0xff00)
            out->dxgi = 49;
        return m[0] != 0 ? PrStatus_Ok : PrStatus_InvalidData;
    }
    if ((flags & kPixelAlpha) != 0 && (flags & kPixelRgb) == 0) {
        if (bits == 8 && m[3] == 0xff)
            return formatOfDxgi(65, out);
        out->layout = masks(bytes, 0, 0, 0, m[3]);
        out->alpha = true;
        return m[3] != 0 ? PrStatus_Ok : PrStatus_InvalidData;
    }
    if ((flags & kPixelRgb) == 0)
        return PrStatus_NotSupported;
    if (bits == 32 && m[0] == 0xffff && m[1] == 0xffff0000 && m[2] == 0)
        return formatOfDxgi(35, out);
    // A DXGI format of the same masks.
    for (const DxgiFormat& format : kDxgiFormats) {
        const Layout& l = format.layout;
        if (l.kind == Kind_Masks && l.bytes == bytes && l.masks[0] == m[0] && l.masks[1] == m[1] && l.masks[2] == m[2] &&
                l.masks[3] == m[3] && !format.srgb && format.value != 27 && format.value != 90 && format.value != 92)
            return formatOfDxgi(format.value, out);
    }
    out->layout = masks(bytes, m[0], m[1], m[2], m[3]);
    out->alpha = alpha;
    return m[0] != 0 || m[1] != 0 || m[2] != 0 ? PrStatus_Ok : PrStatus_InvalidData;
}

// Returns the lib/bc format of the GPU format `format`, a BC format.
PrBcFormat bcFormat(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_Bc1: return PrBcFormat_Bc1;
        case PrGpuFormat_Bc2: return PrBcFormat_Bc2;
        case PrGpuFormat_Bc3: return PrBcFormat_Bc3;
        case PrGpuFormat_Bc4: return PrBcFormat_Bc4;
        case PrGpuFormat_Bc4Snorm: return PrBcFormat_Bc4Snorm;
        case PrGpuFormat_Bc5: return PrBcFormat_Bc5;
        case PrGpuFormat_Bc5Snorm: return PrBcFormat_Bc5Snorm;
        case PrGpuFormat_Bc6HUfloat: return PrBcFormat_Bc6HUfloat;
        case PrGpuFormat_Bc6HSfloat: return PrBcFormat_Bc6HSfloat;
        default: return PrBcFormat_Bc7;
    }
}

// Returns true if lib/bc decodes blocks of `format`, a BC format.
bool decodesBlocks(PrGpuFormat format) {
    return prBcFormatDecodes(bcFormat(format));
}

// Returns true if blocks of `format`, a BC format, decode to floats.
bool decodesToFloats(PrGpuFormat format) {
    return prBcFormatTexels(bcFormat(format)) == PrBcTexels_F32;
}

// Decodes the block of `format` at `block`, which decodesBlocks accepts, to 16 texels of 8-bit RGBA.
void decodeBlock(PrGpuFormat format, const uint8_t* block, uint8_t* texels) {
    prBcDecode(bcFormat(format), block, texels);
}

// Returns the most bits of a mask's field.
uint32_t fieldBits(uint32_t mask) {
    if (mask == 0)
        return 0;
    while ((mask & 1) == 0)
        mask >>= 1;
    uint32_t bits = 0;
    while ((mask & 1) != 0) {
        mask >>= 1;
        ++bits;
    }
    return bits;
}

// Returns the value in [0, 1] of the field of `mask` in `v`. A field of n bits of DDS's uncompressed formats is an
// unsigned normalized value: it converts to float as the value over 2^n - 1 (Vulkan Specification, "Conversion From
// Normalized Fixed-Point to Floating-Point"), and a decode to 8 bits converts that float back to an 8-bit value rounded
// to the nearest ("Conversion From Floating-Point to Normalized Fixed-Point"), as Direct3D's rules for its formats do
// (Direct3D 11.3 Functional Specification, sections 3.2.3.5 and 3.2.3.6).
float fieldOf(uint32_t mask, uint32_t v) {
    uint32_t shift = 0;
    while (((mask >> shift) & 1) == 0)
        ++shift;
    const uint32_t max = mask >> shift;
    return float((v & mask) >> shift) / float(max);
}

// Returns the value of a float of `mantissaBits` and five exponent bits without a sign.
float smallFloat(uint32_t bits, uint32_t mantissaBits) {
    const uint32_t exponent = bits >> mantissaBits & 31;
    const uint32_t mantissa = bits & ((1u << mantissaBits) - 1);
    const float scale = float(1u << mantissaBits);
    if (exponent == 0)
        return float(mantissa) / scale * (1.0f / 16384.0f);
    if (exponent == 31)
        return mantissa == 0 ? kPrF32Max : 0.0f;
    const float value = (1.0f + float(mantissa) / scale);
    return exponent >= 15 ? value * float(1u << (exponent - 15)) : value / float(1u << (15 - exponent));
}

// Decodes the stored pixel at `at` to red, green, blue and alpha.
void decodePixel(const Layout& layout, const uint8_t* at, float* rgba) {
    rgba[0] = rgba[1] = rgba[2] = 0;
    rgba[3] = 1;
    switch (layout.kind) {
        case Kind_Masks: {
            uint32_t v = 0;
            for (uint32_t i = 0; i < layout.bytes; ++i)
                v |= uint32_t(at[i]) << (8 * i);
            for (uint32_t c = 0; c < 4; ++c) {
                if (layout.masks[c] != 0)
                    rgba[c] = fieldOf(layout.masks[c], v);
            }
            break;
        }
        case Kind_Unorm16:
            for (uint32_t c = 0; c < layout.components; ++c)
                rgba[c] = float(prMemReadLeU16(at + c * 2)) / 65535.0f;
            break;
        case Kind_Snorm8:
            for (uint32_t c = 0; c < layout.components; ++c) {
                const float v = float(int8_t(at[c])) / 127.0f;
                rgba[c] = v < -1 ? -1 : v;
            }
            break;
        case Kind_Snorm16:
            for (uint32_t c = 0; c < layout.components; ++c) {
                const float v = float(int16_t(prMemReadLeU16(at + c * 2))) / 32767.0f;
                rgba[c] = v < -1 ? -1 : v;
            }
            break;
        case Kind_Float16:
            for (uint32_t c = 0; c < layout.components; ++c) {
                PrFloat16 half{};
                prMemCopy(&half, at + c * 2, 2);
                rgba[c] = prFloat16ToFloat(half);
            }
            break;
        case Kind_Float32:
            for (uint32_t c = 0; c < layout.components; ++c)
                prMemCopy(&rgba[c], at + c * 4, 4);
            break;
        case Kind_Rg11B10F: {
            const uint32_t v = prMemReadLeU32(at);
            rgba[0] = smallFloat(v & 0x7ff, 6);
            rgba[1] = smallFloat(v >> 11 & 0x7ff, 6);
            rgba[2] = smallFloat(v >> 22 & 0x3ff, 5);
            break;
        }
        case Kind_Rgb9E5: {
            const uint32_t v = prMemReadLeU32(at);
            const int32_t exponent = int32_t(v >> 27) - 15 - 9;
            const float scale = exponent >= 0 ? float(1u << exponent) : 1.0f / float(1u << -exponent);
            for (uint32_t c = 0; c < 3; ++c)
                rgba[c] = float(v >> (9 * c) & 511) * scale;
            break;
        }
        default:
            break;
    }
}

// Writes `value` as one channel of `sample` at `out`.
void storeSample(PrDdsSample sample, float value, uint8_t* out) {
    switch (sample) {
        case PrDdsSample_U8: {
            const float v = value < 0 ? 0 : value > 1 ? 1 : value;
            *out = uint8_t(v * 255.0f + 0.5f);
            break;
        }
        case PrDdsSample_U16: {
            const float v = value < 0 ? 0 : value > 1 ? 1 : value;
            const uint16_t u = uint16_t(v * 65535.0f + 0.5f);
            prMemCopy(out, &u, 2);
            break;
        }
        case PrDdsSample_F16: {
            const PrFloat16 half = prFloat16FromFloat(value);
            prMemCopy(out, &half, 2);
            break;
        }
        default:
            prMemCopy(out, &value, 4);
            break;
    }
}

uint32_t sampleBytes(PrDdsSample sample) {
    return sample == PrDdsSample_U8 ? 1 : sample == PrDdsSample_F32 ? 4 : 2;
}

// Writes the decoded red, green, blue and alpha `rgba` as a pixel of `info`'s channels at `out`.
void storePixel(const PrDdsInfo& info, const float* rgba, uint8_t* out) {
    const uint32_t size = sampleBytes(info.sample);
    if (info.channels <= 2) {
        storeSample(info.sample, rgba[0], out);
        if (info.channels == 2)
            storeSample(info.sample, rgba[3], out + size);
        return;
    }
    for (uint32_t c = 0; c < info.channels; ++c)
        storeSample(info.sample, rgba[c], out + c * size);
}

// A texture: its bytes, its description, the layout of its texels, and where its levels are.
struct DdsTexture {
    PrByteView bytes;
    PrDdsInfo info;
    Layout layout;
    size_t dataAt;                          // the offset of the first texel
    size_t levelSizes[kPrDdsMaxLevels];     // of one frame
    size_t frameSize;
};

// Writes `a` times `b` to `*out`. Returns true if the product does not fit in 64 bits.
bool multiplyOverflows(uint64_t a, uint64_t b, uint64_t* out) {
    if (b != 0 && a > UINT64_MAX / b)
        return true;
    *out = a * b;
    return false;
}

// Returns the depth of `level` of a volume of `depth` slices.
uint32_t levelDepth(uint32_t depth, uint32_t level) {
    const uint32_t d = level < 32 ? depth >> level : 0;
    return d != 0 ? d : 1;
}

// Reads the headers of `bytes` into `texture`, as prDdsTextureCreate describes.
PrStatus readTexture(PrByteView bytes, DdsTexture* texture) {
    PrDdsInfo& info = texture->info;
    texture->bytes = bytes;
    if (!prDdsHasSignature(bytes) || bytes.size < kPrDdsHeaderBytes || prMemReadLeU32(bytes.data + 4) != 124)
        return PrStatus_InvalidData;
    const uint8_t* at = bytes.data;
    const uint32_t flags = prMemReadLeU32(at + 8);
    info.extent = PrUVec2{prMemReadLeU32(at + 16), prMemReadLeU32(at + 12)};
    uint32_t depth = prMemReadLeU32(at + 24);
    uint32_t levels = (flags & kFlagMipCount) != 0 ? prMemReadLeU32(at + 28) : 1;
    const uint32_t caps2 = prMemReadLeU32(at + 112);
    const uint8_t* pf = at + 76;
    Format format{};
    bool cube = (caps2 & kCaps2Cube) != 0;
    bool volume = (caps2 & kCaps2Volume) != 0 && depth > 1;
    uint32_t layers = 1;
    uint32_t alphaMode = 0;
    texture->dataAt = kPrDdsHeaderBytes;
    PrStatus status = PrStatus_Ok;
    if ((prMemReadLeU32(pf + 4) & kPixelFourCc) != 0 && prMemReadLeU32(pf + 8) == fourCc('D', 'X', '1', '0')) {
        if (bytes.size < kPrDdsDx10HeaderBytes)
            return PrStatus_InvalidData;
        texture->dataAt = kPrDdsDx10HeaderBytes;
        uint32_t dimension = prMemReadLeU32(at + 132);
        const uint32_t misc = prMemReadLeU32(at + 136);
        layers = prMemReadLeU32(at + 140);
        alphaMode = prMemReadLeU32(at + 144) & 7;
        // Some writers leave the dimension unknown, which is a 2D texture, and the array size zero, which is one.
        if (dimension == 0)
            dimension = kDimension2d;
        if (dimension != kDimension1d && dimension != kDimension2d && dimension != kDimension3d)
            return PrStatus_InvalidData;
        cube = dimension == kDimension2d && (misc & kMiscCube) != 0;
        volume = dimension == kDimension3d;
        if (dimension == kDimension1d)
            info.extent.y = 1;
        layers = layers != 0 ? layers : 1;
        if (volume && layers != 1)
            return PrStatus_NotSupported;
        status = formatOfDxgi(prMemReadLeU32(at + 128), &format);
    } else {
        // A legacy cube lists its faces, and a cube of fewer than six has no frames of the others.
        if (cube && (caps2 & kCaps2AllFaces) != kCaps2AllFaces)
            return PrStatus_NotSupported;
        status = formatOfLegacy(pf, &format);
    }
    if (status != PrStatus_Ok)
        return status;
    if (!volume)
        depth = 1;
    if (info.extent.x == 0 || info.extent.y == 0 || depth == 0 || (cube && info.extent.x != info.extent.y))
        return PrStatus_InvalidData;
    if (levels == 0)
        levels = 1;
    uint32_t side = info.extent.x > info.extent.y ? info.extent.x : info.extent.y;
    side = side > depth ? side : depth;
    if (levels > kPrDdsMaxLevels || side >> (levels - 1) == 0)
        return PrStatus_InvalidData;
    info.depth = depth;
    info.layerCount = layers;
    info.faceCount = cube ? 6 : 1;
    info.levelCount = levels;
    info.dxgiFormat = format.dxgi;
    info.swizzle = format.swizzle;
    info.blocks = format.blocks;
    info.srgb = format.srgb;
    texture->layout = format.layout;

    // The pixels that the texels decode to.
    const Layout& layout = format.layout;
    bool alpha = format.alpha;
    if (alphaMode == kAlphaModeOpaque)
        alpha = false;
    info.alpha = PrDdsAlpha_None;
    if (alpha) {
        const bool premultiplied = format.premultiplied || alphaMode == kAlphaModePremultiplied;
        info.alpha = premultiplied ? PrDdsAlpha_Premultiplied : PrDdsAlpha_Straight;
    }
    if (layout.kind == Kind_Blocks) {
        info.decodes = decodesBlocks(format.blocks);
        info.sample = decodesToFloats(format.blocks) ? PrDdsSample_F32 : PrDdsSample_U8;
        const bool one = format.blocks == PrGpuFormat_Bc4 || format.blocks == PrGpuFormat_Bc4Snorm;
        info.channels = one ? 1 : alpha ? 4 : 3;
    } else {
        info.decodes = true;
        uint32_t widest = 0;
        for (uint32_t c = 0; c < 4; ++c) {
            const uint32_t bits = fieldBits(layout.masks[c]);
            widest = bits > widest ? bits : widest;
        }
        switch (layout.kind) {
            case Kind_Masks: info.sample = widest > 8 ? PrDdsSample_U16 : PrDdsSample_U8; break;
            case Kind_Unorm16: info.sample = PrDdsSample_U16; break;
            case Kind_Float16: info.sample = PrDdsSample_F16; break;
            default: info.sample = PrDdsSample_F32; break;
        }
        bool single = layout.components == 1;
        if (layout.kind == Kind_Masks)
            single = layout.masks[0] != 0 && layout.masks[1] == 0 && layout.masks[2] == 0;
        if (format.swizzle == PrDdsSwizzle_LuminanceAlpha)
            info.channels = alpha ? 2 : 1;
        else if (format.swizzle == PrDdsSwizzle_Luminance || (single && !alpha))
            info.channels = 1;
        else
            info.channels = alpha ? 4 : 3;
    }

    // The levels of each frame in turn. A texture whose size does not fit in memory is no file's.
    uint64_t frame = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        const PrUVec2 extent = prDdsLevelExtent(info.extent, level);
        uint64_t slice = 0;
        if (layout.kind == Kind_Blocks) {
            const uint64_t blocks = ((uint64_t(extent.x) + 3) / 4) * ((uint64_t(extent.y) + 3) / 4);
            if (multiplyOverflows(blocks, prGpuFormatBlock(format.blocks).bytes, &slice))
                return PrStatus_InvalidData;
        } else if (multiplyOverflows(uint64_t(extent.x) * extent.y, layout.bytes, &slice)) {
            return PrStatus_InvalidData;
        }
        uint64_t size = 0;
        if (multiplyOverflows(slice, levelDepth(depth, level), &size) || size > SIZE_MAX || frame + size < frame)
            return PrStatus_InvalidData;
        texture->levelSizes[level] = size_t(size);
        frame += size;
    }
    uint64_t total = 0;
    if (multiplyOverflows(frame, uint64_t(layers) * info.faceCount, &total) || total > SIZE_MAX)
        return PrStatus_InvalidData;
    texture->frameSize = size_t(frame);
    return bytes.size - texture->dataAt < total ? PrStatus_Truncated : PrStatus_Ok;
}

// Returns the stored bytes of `level` of `frame`, or false if the bytes end before it does.
bool storedLevel(const DdsTexture* texture, uint32_t frame, uint32_t level, PrByteView* out) {
    const PrDdsInfo& info = texture->info;
    PR_ASSERT(frame < info.layerCount * info.faceCount && level < info.levelCount);
    uint64_t offset = texture->dataAt + uint64_t(texture->frameSize) * frame;
    for (uint32_t l = 0; l < level; ++l)
        offset += texture->levelSizes[l];
    const uint64_t size = texture->levelSizes[level];
    if (offset > texture->bytes.size || texture->bytes.size - offset < size)
        return false;
    *out = PrByteView{texture->bytes.data + offset, size_t(size)};
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDdsTexture : DdsTexture {};

bool prDdsHasSignature(PrByteView bytes) {
    return bytes.size >= 4 && prMemEquals(bytes.data, "DDS ", 4);
}

PrUVec2 prDdsLevelExtent(PrUVec2 extent, uint32_t level) {
    PrUVec2 size{level < 32 ? extent.x >> level : 0, level < 32 ? extent.y >> level : 0};
    return PrUVec2{size.x != 0 ? size.x : 1, size.y != 0 ? size.y : 1};
}

PrStatus prDdsTextureCreate(PrByteView bytes, PrDdsTexture* PR_NULLABLE* PR_NONNULL out) {
    PrDdsTexture* texture = allocZeroed<PrDdsTexture>();
    PrStatus status = readTexture(bytes, texture);
    if (status != PrStatus_Ok && status != PrStatus_Truncated) {
        prMemoryFree(texture);
        texture = nullptr;
    }
    *out = texture;
    return status;
}

void prDdsTextureDestroy(PrDdsTexture* PR_NULLABLE texture) {
    prMemoryFree(texture);
}

const PrDdsInfo* prDdsTextureInfo(const PrDdsTexture* texture) {
    return &texture->info;
}

size_t prDdsTextureLevelSize(const PrDdsTexture* texture, uint32_t level) {
    PR_ASSERT(level < texture->info.levelCount);
    return texture->levelSizes[level];
}

PrStatus prDdsTextureReadStored(const PrDdsTexture* texture, uint32_t frame, uint32_t level, uint8_t* out, size_t size) {
    PR_ASSERT(size == texture->levelSizes[level]);
    PrByteView stored{};
    if (!storedLevel(texture, frame, level, &stored))
        return PrStatus_Truncated;
    prMemCopy(out, stored.data, size);
    return PrStatus_Ok;
}

PrStatus prDdsTextureRead(const PrDdsTexture* texture, uint32_t frame, uint32_t level, PrPixels output) {
    const PrDdsInfo& info = texture->info;
    const PrUVec2 extent = prDdsLevelExtent(info.extent, level);
    const uint32_t depth = levelDepth(info.depth, level);
    PR_ASSERT(output.extent.x == extent.x && output.extent.y == extent.y * depth);
    if (!info.decodes)
        return PrStatus_NotSupported;
    PrByteView stored{};
    if (!storedLevel(texture, frame, level, &stored))
        return PrStatus_Truncated;
    const uint32_t pixelBytes = info.channels * sampleBytes(info.sample);
    const Layout& layout = texture->layout;
    if (layout.kind != Kind_Blocks) {
        for (uint32_t y = 0; y < extent.y * depth; ++y) {
            const uint8_t* from = stored.data + size_t(y) * extent.x * layout.bytes;
            uint8_t* to = output.data + size_t(y) * output.stride;
            for (uint32_t x = 0; x < extent.x; ++x, from += layout.bytes, to += pixelBytes) {
                float rgba[4];
                decodePixel(layout, from, rgba);
                storePixel(info, rgba, to);
            }
        }
        return PrStatus_Ok;
    }
    const uint32_t blockBytes = prGpuFormatBlock(info.blocks).bytes;
    const uint32_t blocksX = (extent.x + 3) / 4;
    const uint32_t blocksY = (extent.y + 3) / 4;
    const uint8_t* block = stored.data;
    const bool floats = info.sample == PrDdsSample_F32;
    for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t by = 0; by < blocksY; ++by) {
            for (uint32_t bx = 0; bx < blocksX; ++bx, block += blockBytes) {
                uint8_t texels[64];
                float values[64];
                if (floats)
                    prBcDecodeF32(bcFormat(info.blocks), block, values);
                else
                    decodeBlock(info.blocks, block, texels);
                const uint32_t width = extent.x - bx * 4 < 4 ? extent.x - bx * 4 : 4;
                const uint32_t height = extent.y - by * 4 < 4 ? extent.y - by * 4 : 4;
                for (uint32_t y = 0; y < height; ++y) {
                    uint8_t* to = output.data + (size_t(z) * extent.y + by * 4 + y) * output.stride +
                        size_t(bx) * 4 * pixelBytes;
                    for (uint32_t x = 0; x < width; ++x, to += pixelBytes) {
                        if (floats)
                            storePixel(info, values + (y * 4 + x) * 4, to);
                        else
                            prMemCopy(to, texels + (y * 4 + x) * 4, pixelBytes);
                    }
                }
            }
        }
    }
    return PrStatus_Ok;
}
