// The built-in codec's BMP files: what lib/bmp's adapter reports of them, and how they read through a decoder.

#include "tools/harness/image_test.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

void put32(Array<uint8_t>* out, uint32_t v) {
    for (uint32_t i = 0; i < 4; ++i)
        out->push(uint8_t(v >> (8 * i)));
}

// Returns a file of one 32-bit row of `pixels`, in BGRA, with a version 5 header that has an alpha mask and the color
// space type `space`, and `profile` after the pixels if it embeds one.
Array<uint8_t> writeBmp(uint32_t width, const Array<uint8_t>& pixels, uint32_t space, const Array<uint8_t>& profile) {
    Array<uint8_t> dib;
    put32(&dib, 124);
    put32(&dib, width);
    put32(&dib, 1);
    dib.push(1);
    dib.push(0);
    dib.push(32);
    dib.push(0);
    put32(&dib, 3);
    put32(&dib, pixels.count());
    put32(&dib, 3780);
    put32(&dib, 3780);
    put32(&dib, 0);
    put32(&dib, 0);
    put32(&dib, 0x00ff0000);
    put32(&dib, 0x0000ff00);
    put32(&dib, 0x000000ff);
    put32(&dib, 0xff000000);
    put32(&dib, space);
    while (dib.count() < 112)
        dib.push(0);
    put32(&dib, 124 + pixels.count());
    put32(&dib, profile.count());
    put32(&dib, 0);
    Array<uint8_t> out({'B', 'M'});
    put32(&out, 14 + 124 + pixels.count() + profile.count());
    put32(&out, 0);
    put32(&out, 14 + 124);
    out.append(dib.span());
    out.append(pixels.span());
    out.append(profile.span());
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a bmp with an alpha mask reads as straight-alpha srgb with its resolution") {
    Array<uint8_t> file = writeBmp(2, Array<uint8_t>({30, 20, 10, 128, 3, 2, 1, 255}), 0x73524742, {});
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/bmp"));
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.extent == PrUVec2{2, 1});
    CHECK(info.format.alpha == PrImageAlpha_Straight);
    CHECK(info.format.color.encoding == PrImageColorEncoding_Named);
    CHECK(info.format.color.space == PrColorSpace_Srgb);
    CHECK(info.pixelsPerMeter.x == 3780);
    Array<uint8_t> rgba(8);
    REQUIRE(testReadRgba8(decoder, 0, false, PrImageAlpha_Straight, rgba.data(), rgba.count()) == PrStatus_Ok);
    CHECK(rgba == Array<uint8_t>({10, 20, 30, 128, 1, 2, 3, 255}));
    prImageDecoderDestroy(decoder);
}

TEST_CASE("a bmp's embedded profile is its color") {
    Array<uint8_t> profile({'n', 'o', 't', ' ', 'a', ' ', 'p', 'r', 'o', 'f', 'i', 'l', 'e'});
    Array<uint8_t> file = writeBmp(1, Array<uint8_t>({1, 2, 3, 4}), 0x4d424544, profile);
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageColor& color = prImageDecoderInfo(decoder)->format.color;
    CHECK(color.encoding == PrImageColorEncoding_Icc);
    CHECK(color.profile.size == profile.count());
    prImageDecoderDestroy(decoder);
}

TEST_CASE("a bmp whose rows end early opens truncated") {
    Array<uint8_t> file = writeBmp(2, Array<uint8_t>({1, 2, 3, 4}), 0, {});
    PrImageSource source{};
    source.memory = file;
    PrImageDecoder* decoder = nullptr;
    REQUIRE(testDecoderOpen(testCodec(), source, nullptr, &decoder) == PrStatus_Truncated);
    Array<uint8_t> rgba(8);
    CHECK(prImageDecoderRead(decoder, 0, PrPixels{rgba.data(), 8, {2, 1}}) == PrStatus_Truncated);
    prImageDecoderDestroy(decoder);
}
