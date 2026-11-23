// The built-in codec's QOI images: what lib/qoi's adapter reports of them, and how they read through a decoder.

#include "tools/harness/image_test.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// Returns a file of `width` by `height` pixels, each coded as an RGBA chunk of `rgba`, with `channels` and `colorspace`
// in its header.
Array<uint8_t> writeQoi(uint32_t width, uint32_t height, uint8_t channels, uint8_t colorspace, const uint8_t rgba[4]) {
    Array<uint8_t> out({'q', 'o', 'i', 'f'});
    const uint32_t sides[2] = {width, height};
    for (uint32_t side : sides) {
        out.push(uint8_t(side >> 24));
        out.push(uint8_t(side >> 16));
        out.push(uint8_t(side >> 8));
        out.push(uint8_t(side));
    }
    out.push(channels);
    out.push(colorspace);
    out.push(0xff);
    out.append(PrByteView{rgba, 4});
    // The rest repeat it.
    for (uint32_t left = width * height - 1; left != 0;) {
        const uint32_t run = left < 62 ? left : 62;
        out.push(uint8_t(0xc0 | (run - 1)));
        left -= run;
    }
    const uint8_t end[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    out.append(PrByteView{end, 8});
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an rgba qoi reads as straight-alpha srgb") {
    const uint8_t rgba[4] = {10, 20, 30, 128};
    Array<uint8_t> file = writeQoi(3, 2, 4, 0, rgba);
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/qoi"));
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.extent == PrUVec2{3, 2});
    CHECK(info.format.alpha == PrImageAlpha_Straight);
    CHECK(info.format.color.encoding == PrImageColorEncoding_Named);
    CHECK(info.format.color.space == PrColorSpace_Srgb);
    Array<uint8_t> pixels(3 * 2 * 4);
    REQUIRE(testReadRgba8(decoder, 0, false, PrImageAlpha_Straight, pixels.data(), pixels.count()) == PrStatus_Ok);
    for (uint32_t i = 0; i < 6; ++i)
        CHECK(pixels[i * 4 + 3] == 128);
    CHECK(pixels[4] == 10);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("an rgb qoi of linear color has no alpha") {
    const uint8_t rgba[4] = {10, 20, 30, 128};
    Array<uint8_t> file = writeQoi(2, 2, 3, 1, rgba);
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.format.alpha == PrImageAlpha_None);
    CHECK(info.format.color.space == PrColorSpace_SrgbLinear);
    Array<uint8_t> rgb(2 * 2 * 3);
    CHECK(prImageDecoderRead(decoder, 0, PrPixels{rgb.data(), 6, {2, 2}}) == PrStatus_Ok);
    CHECK(rgb[0] == 10);
    CHECK(rgb[11] == 30);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("a qoi past the limits does not open") {
    const uint8_t rgba[4] = {1, 2, 3, 4};
    Array<uint8_t> file = writeQoi(64, 64, 4, 0, rgba);
    PrImageLimits limits = prImageLimitsDefault();
    limits.pixels = 64 * 64 - 1;
    PrImageSource source{};
    source.memory = file;
    PrImageDecoder* decoder = nullptr;
    CHECK(testDecoderOpen(testCodec(), source, &limits, &decoder) == PrStatus_Limit);
}
