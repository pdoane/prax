// Tests of KTX 2.0 textures in the built-in codec: the info that the lib/ktx adapter reports, and reads through a
// decoder.

#include "pr/array.h"
#include "pr/mem.h"

#include "ktx/tests/ktx_fixture.h"
#include "tools/harness/image_test.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace harness;

PrStatus openStatus(const Array<uint8_t>& file) {
    PrImageDecoder* decoder = nullptr;
    PrStatus status = testDecoderOpen(testCodec(), PrImageSource{.memory = {file.data(), file.count()}}, nullptr, &decoder);
    prImageDecoderDestroy(decoder);
    return status;
}

constexpr KtxSpec kRgba8{.vkFormat = 37, .extent = {2, 2}, .levelCount = 1, .faceCount = 1, .model = 1, .transfer = 1};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an image decoder reads the levels of a ktx texture and reports its pixel format") {
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/ktx2"));
    PrImageDecoder* decoder = testImageOpen(kKtxEtc1s, sizeof(kKtxEtc1s));
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.extent == PrUVec2{12, 8});
    CHECK(info.levelCount == 4);
    CHECK(info.frameCount == 1);
    CHECK(info.faceCount == 1);
    CHECK(info.format.model == PrColorModel_Rgb);
    CHECK(info.format.sample == PrImageSample_U8);
    CHECK(info.format.alpha == PrImageAlpha_Straight);
    CHECK(info.format.color.encoding == PrImageColorEncoding_Named);
    CHECK(info.format.color.space == PrColorSpace_SrgbLinear);
    const uint8_t* expected = kKtxEtc1sPixels;
    for (uint32_t level = 0; level < info.levelCount; ++level) {
        CAPTURE(level);
        PrUVec2 extent = prImageDecoderLevelExtent(decoder, level);
        Array<uint8_t> pixels(size_t(extent.x) * extent.y * 4);
        REQUIRE(prImageDecoderReadLevel(decoder, 0, level, PrPixels{pixels.data(), extent.x * 4, extent}) ==
            PrStatus_Ok);
        CHECK(prMemEquals(pixels.data(), expected, pixels.count()));
        expected += pixels.count();
    }
    uint8_t one[4];
    CHECK(prImageDecoderReadLevel(decoder, 0, 4, PrPixels{one, 4, {1, 1}}) == PrStatus_OutOfRange);
    CHECK(prImageDecoderReadLevel(decoder, 1, 3, PrPixels{one, 4, {1, 1}}) == PrStatus_OutOfRange);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("a ktx texture takes its color and alpha from its data format descriptor, and its orientation from its "
        "metadata") {
    const uint8_t rgba[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    KtxSpec bgra = kRgba8;
    bgra.vkFormat = 50;
    bgra.transfer = 2;
    bgra.flags = 1;
    bgra.orientation = "lu";
    Array<uint8_t> file = buildKtx(bgra, bytesOf(rgba, 16));
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.format.alpha == PrImageAlpha_Premultiplied);
    CHECK(info.format.color.space == PrColorSpace_Srgb);
    CHECK(info.orientation == PrImageOrientation_Rotate180);
    prImageDecoderDestroy(decoder);
    // Two channels are red and green, and one is gray.
    KtxSpec gray = kRgba8;
    gray.vkFormat = 70;
    file = buildKtx(gray, bytesOf(rgba, 8));
    decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    CHECK(prImageDecoderInfo(decoder)->format.model == PrColorModel_Gray);
    CHECK(prImageDecoderInfo(decoder)->format.sample == PrImageSample_U16);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("a ktx texture does not open if its blocks do not decode to pixels") {
    Array<uint8_t> blocks;
    for (uint8_t i = 0; i < 32; ++i)
        blocks.push(i);
    KtxSpec spec = kRgba8;
    spec.vkFormat = 158;
    spec.extent = {8, 4};
    CHECK(openStatus(buildKtx(spec, blocks)) == PrStatus_NotSupported);
    // A block texture whose blocks decode opens.
    spec.vkFormat = 145;
    CHECK(openStatus(buildKtx(spec, blocks)) == PrStatus_Ok);
}
