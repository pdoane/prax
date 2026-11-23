// The built-in codec's DDS textures: what lib/dds's adapter reports of them, and how they read through a decoder.

#include "dds/tests/dds_fixture.h"
#include "tools/harness/image_test.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace pr_impl;

TEST_CASE("a DDS of sRGB blocks reads as sRGB pixels, with its levels") {
    Array<uint8_t> texels({0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0});
    Array<uint8_t> file = ddsFile(DdsLayout{.levels = 2, .dxgi = 72}, texels);
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/vnd.ms-dds"));
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.extent == PrUVec2{4, 4});
    CHECK(info.levelCount == 2);
    CHECK(info.format.color.encoding == PrImageColorEncoding_Named);
    CHECK(info.format.color.space == PrColorSpace_Srgb);
    CHECK(info.format.alpha == PrImageAlpha_Straight);
    Array<uint8_t> rgba(4 * 4 * 4);
    REQUIRE(testReadRgba8(decoder, 0, false, PrImageAlpha_Straight, rgba.data(), rgba.count()) == PrStatus_Ok);
    for (uint8_t value : rgba)
        CHECK(value == 255);
    Array<uint8_t> level(2 * 2 * 4);
    CHECK(prImageDecoderReadLevel(decoder, 0, 1, PrPixels{level.data(), 8, {2, 2}}) == PrStatus_Ok);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("a DDS of BC6H reads as floats, and a volume is not supported as an image") {
    // A block of mode 11, of one subset of 10-bit endpoints, which are zero, so every texel is zero.
    Array<uint8_t> bc6({0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    Array<uint8_t> file = ddsFile(DdsLayout{.dxgi = 95}, bc6);
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    CHECK(prImageDecoderInfo(decoder)->format.sample == PrImageSample_F32);
    Array<float> floats(4 * 4 * 3);
    CHECK(prImageDecoderRead(decoder, 0, PrPixels{reinterpret_cast<uint8_t*>(floats.data()), 48, {4, 4}}) ==
        PrStatus_Ok);
    CHECK(floats[0] == 0.0f);
    prImageDecoderDestroy(decoder);
    PrImageSource source{};
    Array<uint8_t> voxels(8);
    file = ddsFile(DdsLayout{.width = 2, .height = 2, .depth = 2, .dxgi = 61, .dimension = 4}, voxels);
    source.memory = file;
    CHECK(testDecoderOpen(testCodec(), source, nullptr, &decoder) == PrStatus_NotSupported);
}
