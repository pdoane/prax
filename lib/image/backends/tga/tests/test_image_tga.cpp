// The built-in codec's Targa images: what lib/tga's adapter reports of them, and how they read through a decoder.

#include "tools/harness/image_test.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// Returns a true-color file of 32-bit BGRA `pixels`, with `descriptor` as its image descriptor.
Array<uint8_t> writeTga(uint16_t width, uint16_t height, uint8_t descriptor, const Array<uint8_t>& pixels) {
    Array<uint8_t> out({0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    out.push(uint8_t(width));
    out.push(uint8_t(width >> 8));
    out.push(uint8_t(height));
    out.push(uint8_t(height >> 8));
    out.push(32);
    out.push(descriptor);
    out.append(pixels.span());
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a tga from the bottom left reads upright through its orientation") {
    // The stored rows are the bottom row, red, then the top row, blue.
    Array<uint8_t> pixels({0, 0, 255, 255, 0, 0, 255, 255, 255, 0, 0, 128, 255, 0, 0, 128});
    Array<uint8_t> file = writeTga(2, 2, 8, pixels);
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/x-tga"));
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.orientation == PrImageOrientation_FlipVertical);
    CHECK(info.format.alpha == PrImageAlpha_Straight);
    Array<uint8_t> rgba(16);
    REQUIRE(testReadRgba8(decoder, 0, true, PrImageAlpha_Straight, rgba.data(), rgba.count()) == PrStatus_Ok);
    CHECK(rgba == Array<uint8_t>({0, 0, 255, 128, 0, 0, 255, 128, 255, 0, 0, 255, 255, 0, 0, 255}));
    prImageDecoderDestroy(decoder);

    file = writeTga(2, 2, 0x38, pixels);
    decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    CHECK(prImageDecoderInfo(decoder)->orientation == PrImageOrientation_FlipHorizontal);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("bytes that no tga header allows do not open as a tga") {
    Array<uint8_t> file = writeTga(2, 2, 8, {});
    file[2] = 7;
    PrImageSource source{};
    source.memory = file;
    PrImageDecoder* decoder = nullptr;
    CHECK(testDecoderOpen(testCodec(), source, nullptr, &decoder) == PrStatus_NotSupported);
}
