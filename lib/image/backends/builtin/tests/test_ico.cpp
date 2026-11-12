#include "image/backends/builtin/tests/image_builtin_test.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

void put16(Array<uint8_t>& bytes, uint32_t at, uint16_t value) {
    bytes[at] = uint8_t(value);
    bytes[at + 1] = uint8_t(value >> 8);
}

void put32(Array<uint8_t>& bytes, uint32_t at, uint32_t value) {
    put16(bytes, at, uint16_t(value));
    put16(bytes, at + 2, uint16_t(value >> 16));
}

Array<uint8_t> container(const Array<uint8_t>& image, uint8_t width, uint8_t height, uint16_t bits) {
    Array<uint8_t> bytes(22 + image.count());
    put16(bytes, 2, 1);
    put16(bytes, 4, 1);
    bytes[6] = width;
    bytes[7] = height;
    put16(bytes, 10, 1);
    put16(bytes, 12, bits);
    put32(bytes, 14, uint32_t(image.count()));
    put32(bytes, 18, 22);
    for (uint32_t i = 0; i < image.count(); ++i) {
        bytes[22 + i] = image[i];
    }
    return bytes;
}

Array<uint8_t> bitmap(uint16_t bits) {
    const uint32_t colors = bits <= 8 ? 2 : 0;
    const uint32_t rowBytes = ((2 * bits + 31) / 32) * 4;
    const uint32_t offset = 40 + colors * 4;
    Array<uint8_t> bytes(offset + rowBytes * 2 + 8);
    put32(bytes, 0, 40);
    put32(bytes, 4, 2);
    put32(bytes, 8, 4);
    put16(bytes, 12, 1);
    put16(bytes, 14, bits);
    put32(bytes, 32, uint32_t(colors));
    if (colors != 0) {
        bytes[40] = 255;
        bytes[46] = 255;
        bytes[offset] = bits == 8 ? 0 : uint8_t(1u << (8 - bits * 2));
        bytes[offset + rowBytes] = uint8_t(1u << (8 - bits));
        if (bits == 8) {
            bytes[offset] = 0;
            bytes[offset + 1] = 1;
        }
    } else {
        bytes[offset] = 255;
        bytes[offset + bits / 8 + 2] = 255;
        bytes[offset + rowBytes + 2] = 255;
        bytes[offset + rowBytes + bits / 8] = 255;
    }
    bytes[offset + rowBytes * 2 + 4] = 0x40;
    return container(bytes, 2, 2, bits);
}

PrStatus open(const Array<uint8_t>& bytes, PrImageDecoder** decoder, const PrImageLimits* limits = nullptr) {
    PrImageCodec* codec = testCodec();
    return testDecoderOpen(codec, {.memory = {bytes.data(), bytes.count()}}, limits, decoder);
}

Array<uint8_t> decode(PrImageDecoder* decoder) {
    const PrImageInfo info = *prImageDecoderInfo(decoder);
    Array<uint8_t> pixels(size_t(info.extent.x) * info.extent.y * 4);
    const PrPixels output{pixels.data(), size_t(info.extent.x) * 4, info.extent};
    CHECK(prImageDecoderRead(decoder, 0, output) == PrStatus_Ok);
    return pixels;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("ICO bitmap palettes, row orientation, and transparency masks") {
    const uint16_t depths[] = {1, 4, 8, 24, 32};
    for (uint16_t bits : depths) {
        CAPTURE(bits);
        const Array<uint8_t> bytes = bitmap(bits);
        PrImageDecoder* decoder = nullptr;
        REQUIRE(open(bytes, &decoder) == PrStatus_Ok);
        CHECK(prImageDecoderInfo(decoder)->extent.x == 2);
        CHECK(prImageDecoderInfo(decoder)->extent.y == 2);
        CHECK(prImageDecoderInfo(decoder)->format.alpha == PrImageAlpha_Straight);
        const Array<uint8_t> expected({255, 0, 0,   255, 0,   0, 255, 0, 0,   0, 255, 255, 255, 0, 0,   255});
        CHECK(decode(decoder) == expected);
        prImageDecoderDestroy(decoder);
    }
}

TEST_CASE("ICO 32-bit alpha overrides its legacy transparency mask") {
    Array<uint8_t> bytes = bitmap(32);
    const uint32_t bitmapStart = 22 + 40;
    bytes[bitmapStart + 3] = 128;
    bytes[bitmapStart + 7] = 255;
    bytes[bitmapStart + 11] = 255;
    bytes[bitmapStart + 15] = 64;
    PrImageDecoder* decoder = nullptr;
    REQUIRE(open(bytes, &decoder) == PrStatus_Ok);
    const Array<uint8_t> pixels = decode(decoder);
    CHECK(pixels[3] == 255);
    CHECK(pixels[7] == 64);
    CHECK(pixels[11] == 128);
    CHECK(pixels[15] == 255);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("ICO PNG payloads use the native PNG representation") {
    const uint8_t pixels[] = {255, 0, 0, 255, 0, 0, 255, 128};
    Array<uint8_t> png(testEncodePng(PrPixelView{pixels, size_t(2) * 4, {2, 1}}, nullptr, 0));
    testEncodePng(PrPixelView{pixels, size_t(2) * 4, {2, 1}}, png.data(), png.count());
    const Array<uint8_t> bytes = container(png, 2, 1, 32);
    PrImageDecoder* decoder = nullptr;
    REQUIRE(open(bytes, &decoder) == PrStatus_Ok);
    CHECK(decode(decoder) == Array<uint8_t>(pixels));
    prImageDecoderDestroy(decoder);
}

TEST_CASE("ICO rejects incomplete bitmaps, out-of-range resources, and excessive output") {
    const Array<uint8_t> bytes = bitmap(1);
    for (uint32_t size = 0; size < bytes.count(); ++size) {
        PrImageDecoder* decoder = nullptr;
        Array<uint8_t> prefix;
        prefix.append(bytes.data(), size);
        CHECK(open(prefix, &decoder) != PrStatus_Ok);
        CHECK(decoder == nullptr);
    }
    Array<uint8_t> invalid = bytes;
    put32(invalid, 18, 0xFFFFFFFFu);
    PrImageDecoder* decoder = nullptr;
    CHECK(open(invalid, &decoder) == PrStatus_InvalidData);
    CHECK(decoder == nullptr);
    PrImageLimits limits = prImageLimitsDefault();
    limits.outputBytes = 15;
    CHECK(open(bytes, &decoder, &limits) == PrStatus_Limit);
    CHECK(decoder == nullptr);
}

TEST_CASE("ICO chooses higher bit depth among equally sized directory images") {
    const Array<uint8_t> indexed = bitmap(1);
    Array<uint8_t> rgba = bitmap(32);
    rgba[22 + 40 + 11] = 64;
    Array<uint8_t> bytes(38 + indexed.count() - 22 + rgba.count() - 22);
    put16(bytes, 2, 1);
    put16(bytes, 4, 2);
    for (uint32_t i = 0; i < 16; ++i) {
        bytes[6 + i] = indexed[6 + i];
        bytes[22 + i] = rgba[6 + i];
    }
    const uint32_t rgbaOffset = 38 + indexed.count() - 22;
    put32(bytes, 18, 38);
    put32(bytes, 34, uint32_t(rgbaOffset));
    for (uint32_t i = 22; i < indexed.count(); ++i) {
        bytes[38 + i - 22] = indexed[i];
    }
    for (uint32_t i = 22; i < rgba.count(); ++i) {
        bytes[rgbaOffset + i - 22] = rgba[i];
    }
    PrImageDecoder* decoder = nullptr;
    REQUIRE(open(bytes, &decoder) == PrStatus_Ok);
    CHECK(decode(decoder)[3] == 64);
    prImageDecoderDestroy(decoder);
}
