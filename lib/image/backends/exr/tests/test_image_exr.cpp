// The built-in codec's OpenEXR images: what lib/exr's adapter reports of them, and how they read through a decoder.

#include "pr/array.h"
#include "pr/color_space.h"
#include "pr/float16.h"
#include "pr/mem.h"

#include "exr/tests/exr_fixture.h"
#include "tools/harness/image_test.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace harness;

constexpr TestChannel kRgba[] = {{"A", 1}, {"B", 1}, {"G", 1}, {"R", 1}};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an exr is halves with premultiplied alpha in linear sRGB, in the display window's frame") {
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/x-exr"));
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/aces"));
    TestImage image{.channels = kRgba, .channelCount = 4, .data = {0, 0, 4, 2}, .display = {0, 0, 4, 2}};
    Array<uint8_t> file = writeImage(image);
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.extent == PrUVec2{5, 3});
    CHECK(info.format.model == PrColorModel_Rgb);
    CHECK(info.format.sample == PrImageSample_F16);
    CHECK(info.format.alpha == PrImageAlpha_Premultiplied);
    CHECK(info.format.color.encoding == PrImageColorEncoding_Named);
    CHECK(info.format.color.space == PrColorSpace_SrgbLinear);
    CHECK(info.frameCount == 1);
    CHECK(info.levelCount == 1);
    Array<uint8_t> pixels(5 * 3 * 8);
    REQUIRE(prImageDecoderRead(decoder, 0, PrPixels{pixels.data(), 5 * 8, info.extent}) == PrStatus_Ok);
    // R is channel 3 of the list.
    PrFloat16 red{};
    prMemCopy(&red, pixels.data() + (1 * 5 + 2) * 8, 2);
    CHECK(prFloat16ToFloat(red) == prFloat16ToFloat(prFloat16FromFloat(sampleValue(3, 2, 1))));
    prImageDecoderDestroy(decoder);
}

TEST_CASE("an exr's chromaticities name a linear space, or are the color's") {
    TestImage image{.channels = kRgba + 3, .channelCount = 1, .data = {0, 0, 1, 0}, .display = {0, 0, 1, 0}};
    image.chromaticities = true;
    // ACEScg's primaries and white.
    const float ap1[8] = {0.713f, 0.293f, 0.165f, 0.830f, 0.128f, 0.044f, 0.32168f, 0.33767f};
    prMemCopy(image.primaries, ap1, sizeof(ap1));
    Array<uint8_t> file = writeImage(image);
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageColor& color = prImageDecoderInfo(decoder)->format.color;
    CHECK(color.encoding == PrImageColorEncoding_Chromaticities);
    CHECK(prColorPrimariesFromChromaticities(&color.chromaticities, 1e-4f) == PrColorPrimaries_AcesAp1);
    prImageDecoderDestroy(decoder);

    const float rec2020[8] = {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
    prMemCopy(image.primaries, rec2020, sizeof(rec2020));
    file = writeImage(image);
    decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    CHECK(prImageDecoderInfo(decoder)->format.color.encoding == PrImageColorEncoding_Named);
    CHECK(prImageDecoderInfo(decoder)->format.color.space == PrColorSpace_Rec2100Linear);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("an exr whose chunks the file does not hold opens truncated") {
    TestImage image{.channels = kRgba + 3, .channelCount = 1, .data = {0, 0, 2, 3}, .display = {0, 0, 2, 3}};
    Array<uint8_t> file = writeImage(image, 2);
    PrImageDecoder* decoder = nullptr;
    CHECK(testDecoderOpen(testCodec(), PrImageSource{.memory = {file.data(), file.count()}}, nullptr, &decoder) ==
        PrStatus_Truncated);
    REQUIRE(decoder != nullptr);
    prImageDecoderDestroy(decoder);
}
