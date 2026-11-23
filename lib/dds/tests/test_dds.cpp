// DDS over files written here: the DX10 header and the legacy pixel formats, levels, cubes, arrays and volumes, and the
// pixels that each layout decodes to.

#include "pr/array.h"
#include "pr/dds.h"
#include "pr/float16.h"
#include "pr/mem.h"

#include "dds/tests/dds_fixture.h"

#include "tools/harness/test.h"

extern "C" int prTestDdsCApi(void);

namespace pr_impl {

namespace {

// The legacy pixel format flags.
constexpr uint32_t kAlphaPixels = 0x1;
constexpr uint32_t kAlpha = 0x2;
constexpr uint32_t kFourCc = 0x4;
constexpr uint32_t kRgb = 0x40;
constexpr uint32_t kLuminance = 0x20000;
constexpr uint32_t kBumpDuDv = 0x80000;

// A BC1 block whose ends are white, so that every texel is white and opaque.
Array<uint8_t> whiteBc1() {
    return Array<uint8_t>({0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

struct Opened {
    PrStatus status = PrStatus_Ok;
    PrDdsInfo info{};
};

Opened openDds(const Array<uint8_t>& file) {
    Opened opened;
    PrDdsTexture* texture = nullptr;
    opened.status = prDdsTextureCreate(PrByteView{file.data(), file.count()}, &texture);
    if (texture != nullptr)
        opened.info = *prDdsTextureInfo(texture);
    prDdsTextureDestroy(texture);
    return opened;
}

uint32_t sampleBytes(PrDdsSample sample) {
    return sample == PrDdsSample_U8 ? 1 : sample == PrDdsSample_F32 ? 4 : 2;
}

// Decodes `level` of `frame` of `file` to the pixels of its info.
Array<uint8_t> decodeLevel(const Array<uint8_t>& file, uint32_t frame = 0, uint32_t level = 0,
        PrStatus expected = PrStatus_Ok) {
    PrDdsTexture* texture = nullptr;
    const PrStatus opened = prDdsTextureCreate(PrByteView{file.data(), file.count()}, &texture);
    REQUIRE((opened == PrStatus_Ok || opened == PrStatus_Truncated));
    const PrDdsInfo& info = *prDdsTextureInfo(texture);
    const PrUVec2 extent = prDdsLevelExtent(info.extent, level);
    const uint32_t depth = info.depth >> level != 0 ? info.depth >> level : 1;
    const uint32_t stride = extent.x * info.channels * sampleBytes(info.sample);
    Array<uint8_t> pixels(stride * extent.y * depth);
    CHECK(prDdsTextureRead(texture, frame, level, PrPixels{pixels.data(), stride, {extent.x, extent.y * depth}}) == expected);
    prDdsTextureDestroy(texture);
    return pixels;
}

float floatAt(const Array<uint8_t>& pixels, uint32_t index) {
    float value = 0;
    prMemCopy(&value, pixels.data() + index * 4, 4);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a DX10 texture of blocks has its levels, reads them as stored, and decodes them") {
    // Three levels of 8 by 8, 4 by 4 and 2 by 2: four blocks, then one, then one.
    Array<uint8_t> texels;
    for (uint32_t i = 0; i < 6; ++i)
        texels.append(whiteBc1().span());
    Array<uint8_t> file = ddsFile(DdsLayout{.width = 8, .height = 8, .levels = 3, .dxgi = 72}, texels);
    CHECK(prDdsHasSignature(PrByteView{file.data(), file.count()}));
    PrDdsTexture* texture = nullptr;
    REQUIRE(prDdsTextureCreate(PrByteView{file.data(), file.count()}, &texture) == PrStatus_Ok);
    const PrDdsInfo& info = *prDdsTextureInfo(texture);
    CHECK(info.extent == PrUVec2{8, 8});
    CHECK(info.levelCount == 3);
    CHECK(info.blocks == PrGpuFormat_Bc1);
    CHECK(info.dxgiFormat == 72);
    CHECK(info.srgb);
    CHECK(info.decodes);
    CHECK(info.channels == 4);
    CHECK(info.alpha == PrDdsAlpha_Straight);
    CHECK(prDdsTextureLevelSize(texture, 0) == 32);
    CHECK(prDdsTextureLevelSize(texture, 2) == 8);
    uint8_t stored[8] = {};
    REQUIRE(prDdsTextureReadStored(texture, 0, 2, stored, 8) == PrStatus_Ok);
    CHECK(prMemEquals(stored, whiteBc1().data(), 8));
    uint8_t pixels[2 * 2 * 4] = {};
    REQUIRE(prDdsTextureRead(texture, 0, 2, PrPixels{pixels, 8, {2, 2}}) == PrStatus_Ok);
    for (uint8_t value : pixels)
        CHECK(value == 255);
    prDdsTextureDestroy(texture);
}

TEST_CASE("the legacy FourCCs name the BC formats, and DXT2 and DXT4 are premultiplied") {
    struct Case {
        uint32_t fourCc;
        PrGpuFormat blocks;
        PrDdsAlpha alpha;
        uint8_t channels;
    };
    const Case kCases[] = {
        {ddsFourCc('D', 'X', 'T', '1'), PrGpuFormat_Bc1, PrDdsAlpha_Straight, 4},
        {ddsFourCc('D', 'X', 'T', '2'), PrGpuFormat_Bc2, PrDdsAlpha_Premultiplied, 4},
        {ddsFourCc('D', 'X', 'T', '3'), PrGpuFormat_Bc2, PrDdsAlpha_Straight, 4},
        {ddsFourCc('D', 'X', 'T', '4'), PrGpuFormat_Bc3, PrDdsAlpha_Premultiplied, 4},
        {ddsFourCc('D', 'X', 'T', '5'), PrGpuFormat_Bc3, PrDdsAlpha_Straight, 4},
        {ddsFourCc('A', 'T', 'I', '1'), PrGpuFormat_Bc4, PrDdsAlpha_None, 1},
        {ddsFourCc('B', 'C', '4', 'U'), PrGpuFormat_Bc4, PrDdsAlpha_None, 1},
        {ddsFourCc('B', 'C', '4', 'S'), PrGpuFormat_Bc4Snorm, PrDdsAlpha_None, 1},
        {ddsFourCc('A', 'T', 'I', '2'), PrGpuFormat_Bc5, PrDdsAlpha_None, 3},
        {ddsFourCc('B', 'C', '5', 'U'), PrGpuFormat_Bc5, PrDdsAlpha_None, 3},
        {ddsFourCc('B', 'C', '5', 'S'), PrGpuFormat_Bc5Snorm, PrDdsAlpha_None, 3},
    };
    for (const Case& c : kCases) {
        CAPTURE(c.fourCc);
        Array<uint8_t> texels(prGpuFormatBlock(c.blocks).bytes);
        Opened opened = openDds(ddsFile(DdsLayout{.pixelFlags = kFourCc, .fourCc = c.fourCc}, texels));
        REQUIRE(opened.status == PrStatus_Ok);
        CHECK(opened.info.blocks == c.blocks);
        CHECK(opened.info.alpha == c.alpha);
        CHECK(opened.info.channels == c.channels);
    }
    // YUV FourCCs have no format.
    CHECK(openDds(ddsFile(DdsLayout{.pixelFlags = kFourCc, .fourCc = ddsFourCc('Y', 'U', 'Y', '2')}, {})).status ==
        PrStatus_NotSupported);
}

TEST_CASE("legacy bit masks take the DXGI format of the same masks, and others decode by their masks") {
    // A8R8G8B8 is B8G8R8A8, and X8R8G8B8 is B8G8R8X8 without alpha.
    Array<uint8_t> bgra({10, 20, 30, 40});
    DdsLayout argb{
        .width = 1,
        .height = 1,
        .pixelFlags = kRgb | kAlphaPixels,
        .bits = 32,
        .masks = {0xff0000, 0xff00, 0xff, 0xff000000},
    };
    Opened opened = openDds(ddsFile(argb, bgra));
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info.dxgiFormat == 87);
    CHECK(opened.info.channels == 4);
    CHECK(decodeLevel(ddsFile(argb, bgra)) == Array<uint8_t>({30, 20, 10, 40}));
    DdsLayout xrgb = argb;
    xrgb.pixelFlags = kRgb;
    opened = openDds(ddsFile(xrgb, bgra));
    CHECK(opened.info.dxgiFormat == 88);
    CHECK(decodeLevel(ddsFile(xrgb, bgra)) == Array<uint8_t>({30, 20, 10}));

    // R5G6B5: full red and green at 32 of 63.
    const uint32_t v565 = 31u << 11 | 32u << 5;
    DdsLayout r5g6b5{.width = 1, .height = 1, .pixelFlags = kRgb, .bits = 16, .masks = {0xf800, 0x07e0, 0x001f, 0}};
    Array<uint8_t> file = ddsFile(r5g6b5, Array<uint8_t>({uint8_t(v565), uint8_t(v565 >> 8)}));
    CHECK(openDds(file).info.dxgiFormat == 85);
    CHECK(decodeLevel(file) == Array<uint8_t>({255, 130, 0}));

    // A4R4G4B4 and A1R5G5B5.
    DdsLayout argb4{
        .width = 1,
        .height = 1,
        .pixelFlags = kRgb | kAlphaPixels,
        .bits = 16,
        .masks = {0x0f00, 0x00f0, 0x000f, 0xf000},
    };
    file = ddsFile(argb4, Array<uint8_t>({0x0f, 0x8f}));
    CHECK(openDds(file).info.dxgiFormat == 115);
    CHECK(decodeLevel(file) == Array<uint8_t>({255, 0, 255, 136}));
    DdsLayout argb1{
        .width = 1,
        .height = 1,
        .pixelFlags = kRgb | kAlphaPixels,
        .bits = 16,
        .masks = {0x7c00, 0x03e0, 0x001f, 0x8000},
    };
    CHECK(openDds(ddsFile(argb1, Array<uint8_t>({0, 0}))).info.dxgiFormat == 86);

    // 24-bit RGB has no DXGI format, and decodes.
    DdsLayout rgb24{.width = 1, .height = 1, .pixelFlags = kRgb, .bits = 24, .masks = {0xff0000, 0xff00, 0xff, 0}};
    file = ddsFile(rgb24, Array<uint8_t>({1, 2, 3}));
    opened = openDds(file);
    CHECK(opened.info.dxgiFormat == 0);
    CHECK(opened.info.decodes);
    CHECK(decodeLevel(file) == Array<uint8_t>({3, 2, 1}));

    // G16R16 is R16G16, of 16-bit samples.
    DdsLayout g16r16{.width = 1, .height = 1, .pixelFlags = kRgb, .bits = 32, .masks = {0xffff, 0xffff0000, 0, 0}};
    file = ddsFile(g16r16, Array<uint8_t>({0xff, 0xff, 0, 0x80}));
    opened = openDds(file);
    CHECK(opened.info.dxgiFormat == 35);
    CHECK(opened.info.sample == PrDdsSample_U16);
    Array<uint8_t> wide = decodeLevel(file);
    CHECK(prMemReadLeU16(wide.data()) == 65535);
}

TEST_CASE("luminance, alpha and bump masks read through their swizzles and signs") {
    Opened opened = openDds(ddsFile(DdsLayout{
        .width = 1,
        .height = 1,
        .pixelFlags = kLuminance,
        .bits = 8,
        .masks = {0xff, 0, 0, 0},
    }, Array<uint8_t>({77})));
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info.dxgiFormat == 61);
    CHECK(opened.info.swizzle == PrDdsSwizzle_Luminance);
    CHECK(opened.info.channels == 1);

    Array<uint8_t> file = ddsFile(DdsLayout{
        .width = 1,
        .height = 1,
        .pixelFlags = kLuminance | kAlphaPixels,
        .bits = 16,
        .masks = {0xff, 0, 0, 0xff00},
    }, Array<uint8_t>({77, 99}));
    opened = openDds(file);
    CHECK(opened.info.dxgiFormat == 49);
    CHECK(opened.info.swizzle == PrDdsSwizzle_LuminanceAlpha);
    CHECK(opened.info.channels == 2);
    CHECK(decodeLevel(file) == Array<uint8_t>({77, 99}));

    file = ddsFile(DdsLayout{.width = 1, .height = 1, .pixelFlags = kAlpha, .bits = 8, .masks = {0, 0, 0, 0xff}},
        Array<uint8_t>({200}));
    opened = openDds(file);
    CHECK(opened.info.dxgiFormat == 65);
    CHECK(decodeLevel(file) == Array<uint8_t>({0, 0, 0, 200}));

    // V8U8 is signed, and decodes to floats.
    file = ddsFile(DdsLayout{.width = 1, .height = 1, .pixelFlags = kBumpDuDv, .bits = 16, .masks = {0xff, 0xff00, 0, 0}},
        Array<uint8_t>({0x81, 0x7f}));
    opened = openDds(file);
    CHECK(opened.info.dxgiFormat == 51);
    CHECK(opened.info.sample == PrDdsSample_F32);
    Array<uint8_t> values = decodeLevel(file);
    CHECK(floatAt(values, 0) == -1.0f);
    CHECK(floatAt(values, 1) == 1.0f);
    CHECK(floatAt(values, 2) == 0.0f);
}

TEST_CASE("float formats decode to their values") {
    // A16B16G16R16F, as the D3DFORMAT number in place of a FourCC.
    Array<uint8_t> halves;
    const float values[4] = {0.5f, -2.0f, 1024.0f, 1.0f};
    for (float value : values) {
        PrFloat16 half = prFloat16FromFloat(value);
        uint8_t bytes[2];
        prMemCopy(bytes, &half, 2);
        halves.append(PrByteView{bytes, 2});
    }
    Array<uint8_t> file = ddsFile(DdsLayout{.width = 1, .height = 1, .pixelFlags = kFourCc, .fourCc = 113}, halves);
    Opened opened = openDds(file);
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info.dxgiFormat == 10);
    CHECK(opened.info.sample == PrDdsSample_F16);
    CHECK(decodeLevel(file) == halves);

    // R11G11B10: red 1.0, green 2.0 and blue 0.5.
    const uint32_t packed = (15u << 6) | (16u << 6) << 11 | (14u << 5) << 22;
    file = ddsFile(DdsLayout{.width = 1, .height = 1, .dxgi = 26},
        Array<uint8_t>({uint8_t(packed), uint8_t(packed >> 8), uint8_t(packed >> 16), uint8_t(packed >> 24)}));
    Array<uint8_t> decoded = decodeLevel(file);
    CHECK(floatAt(decoded, 0) == 1.0f);
    CHECK(floatAt(decoded, 1) == 2.0f);
    CHECK(floatAt(decoded, 2) == 0.5f);

    // R9G9B9E5: mantissas of 256, 128 and 0 at an exponent of 16 are 1, 0.5 and 0.
    const uint32_t shared = 256u | 128u << 9 | 16u << 27;
    file = ddsFile(DdsLayout{.width = 1, .height = 1, .dxgi = 67},
        Array<uint8_t>({uint8_t(shared), uint8_t(shared >> 8), uint8_t(shared >> 16), uint8_t(shared >> 24)}));
    decoded = decodeLevel(file);
    CHECK(floatAt(decoded, 0) == 1.0f);
    CHECK(floatAt(decoded, 1) == 0.5f);
    CHECK(floatAt(decoded, 2) == 0.0f);
}

TEST_CASE("cubes, arrays and volumes have their frames and slices") {
    // A legacy cube of all six faces, one block each.
    Array<uint8_t> faces;
    for (uint32_t i = 0; i < 6; ++i)
        faces.append(whiteBc1().span());
    DdsLayout cube{.pixelFlags = kFourCc, .fourCc = ddsFourCc('D', 'X', 'T', '1'), .caps2 = 0x200 | 0xfc00};
    Opened opened = openDds(ddsFile(cube, faces));
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info.faceCount == 6);
    cube.caps2 = 0x200 | 0x400;
    CHECK(openDds(ddsFile(cube, faces)).status == PrStatus_NotSupported);

    // A DX10 array of two cubes.
    Array<uint8_t> twelve;
    for (uint32_t i = 0; i < 12; ++i)
        twelve.append(whiteBc1().span());
    opened = openDds(ddsFile(DdsLayout{.dxgi = 71, .misc = 0x4, .arraySize = 2}, twelve));
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info.layerCount == 2);
    CHECK(opened.info.faceCount == 6);

    // A volume of 2 by 2 by 4 pixels with two levels: 16 pixels, then 1 by 1 by 2.
    Array<uint8_t> voxels;
    for (uint32_t i = 0; i < 18; ++i)
        voxels.push(uint8_t(i));
    Array<uint8_t> file = ddsFile(DdsLayout{
        .width = 2,
        .height = 2,
        .depth = 4,
        .levels = 2,
        .dxgi = 61,
        .dimension = 4,
    }, voxels);
    PrDdsTexture* texture = nullptr;
    REQUIRE(prDdsTextureCreate(PrByteView{file.data(), file.count()}, &texture) == PrStatus_Ok);
    CHECK(prDdsTextureInfo(texture)->depth == 4);
    CHECK(prDdsTextureLevelSize(texture, 0) == 16);
    CHECK(prDdsTextureLevelSize(texture, 1) == 2);
    prDdsTextureDestroy(texture);
    CHECK(decodeLevel(file, 0, 1) == Array<uint8_t>({16, 17}));
    CHECK(openDds(ddsFile(DdsLayout{.width = 2, .height = 2, .depth = 4, .dxgi = 61, .dimension = 4, .arraySize = 2},
        voxels)).status == PrStatus_NotSupported);
}

TEST_CASE("a DX10 header of an unknown dimension and no array size is one 2D texture") {
    Opened opened = openDds(ddsFile(DdsLayout{.dxgi = 71, .dimension = 0, .arraySize = 0}, whiteBc1()));
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info.layerCount == 1);
    CHECK(opened.info.depth == 1);
    // An 8-bit luminance whose mask is past its bits reads the byte.
    Array<uint8_t> file = ddsFile(DdsLayout{
        .width = 1,
        .height = 1,
        .pixelFlags = kLuminance,
        .bits = 8,
        .masks = {0xff000000, 0, 0, 0},
    }, Array<uint8_t>({77}));
    CHECK(decodeLevel(file) == Array<uint8_t>({77}));
}

TEST_CASE("the DX10 alpha mode makes alpha premultiplied or opaque") {
    Array<uint8_t> pixel({1, 2, 3, 4});
    Opened opened = openDds(ddsFile(DdsLayout{.width = 1, .height = 1, .dxgi = 28, .alphaMode = 2}, pixel));
    CHECK(opened.info.alpha == PrDdsAlpha_Premultiplied);
    Array<uint8_t> file = ddsFile(DdsLayout{.width = 1, .height = 1, .dxgi = 28, .alphaMode = 3}, pixel);
    opened = openDds(file);
    CHECK(opened.info.alpha == PrDdsAlpha_None);
    CHECK(opened.info.channels == 3);
    CHECK(decodeLevel(file) == Array<uint8_t>({1, 2, 3}));
}

TEST_CASE("a DDS that ends early is truncated, and malformed headers do not open") {
    Array<uint8_t> file = ddsFile(DdsLayout{.width = 8, .height = 8, .dxgi = 71}, whiteBc1());
    CHECK(openDds(file).status == PrStatus_Truncated);
    decodeLevel(file, 0, 0, PrStatus_Truncated);

    file = ddsFile(DdsLayout{.dxgi = 71}, whiteBc1());
    file[0] = 'X';
    CHECK(openDds(file).status == PrStatus_InvalidData);
    file = ddsFile(DdsLayout{.dxgi = 71}, whiteBc1());
    file[4] = 100;
    CHECK(openDds(file).status == PrStatus_InvalidData);
    CHECK(openDds(ddsFile(DdsLayout{.dxgi = 71, .dimension = 9}, whiteBc1())).status == PrStatus_InvalidData);
    CHECK(openDds(ddsFile(DdsLayout{.width = 0, .dxgi = 71}, whiteBc1())).status == PrStatus_InvalidData);
    CHECK(openDds(ddsFile(DdsLayout{.levels = 4, .dxgi = 71}, whiteBc1())).status == PrStatus_InvalidData);
    CHECK(openDds(ddsFile(DdsLayout{.dxgi = 1000}, whiteBc1())).status == PrStatus_NotSupported);
    // Palette indices.
    CHECK(openDds(ddsFile(DdsLayout{.pixelFlags = 0x20, .bits = 8}, whiteBc1())).status == PrStatus_NotSupported);
    Array<uint8_t> header = ddsFile(DdsLayout{.dxgi = 71}, {});
    header.resize(140);
    CHECK(openDds(header).status == PrStatus_InvalidData);
}

TEST_CASE("the dds header compiles as C") {
    CHECK(prTestDdsCApi() == 0);
}
