#include "image/backends/builtin/tests/image_builtin_test.h"

#include "doctest/doctest.h"

#include <vector>

namespace pr_impl {

namespace {

void put16(std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
    bytes[at] = uint8_t(value);
    bytes[at + 1] = uint8_t(value >> 8);
}

void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    put16(bytes, at, uint16_t(value));
    put16(bytes, at + 2, uint16_t(value >> 16));
}

std::vector<uint8_t> container(const std::vector<uint8_t>& image, uint8_t width, uint8_t height,
                               uint16_t bits) {
    std::vector<uint8_t> bytes(22 + image.size());
    put16(bytes, 2, 1);
    put16(bytes, 4, 1);
    bytes[6] = width;
    bytes[7] = height;
    put16(bytes, 10, 1);
    put16(bytes, 12, bits);
    put32(bytes, 14, uint32_t(image.size()));
    put32(bytes, 18, 22);
    for (size_t i = 0; i < image.size(); ++i) {
        bytes[22 + i] = image[i];
    }
    return bytes;
}

std::vector<uint8_t> bitmap(uint16_t bits) {
    const size_t colors = bits <= 8 ? 2 : 0;
    const size_t rowBytes = ((2 * bits + 31) / 32) * 4;
    const size_t offset = 40 + colors * 4;
    std::vector<uint8_t> bytes(offset + rowBytes * 2 + 8);
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

PrStatus open(const std::vector<uint8_t>& bytes, PrImageDecoder** decoder,
              const PrImageLimits* limits = nullptr) {
    PrImageCodec* codec = testCodec();
    return testDecoderOpen(codec, {.memory = {bytes.data(), bytes.size()}}, limits, decoder);
}

std::vector<uint8_t> decode(PrImageDecoder* decoder) {
    const PrImageInfo info = *prImageDecoderInfo(decoder);
    std::vector<uint8_t> pixels(size_t(info.extent.x) * info.extent.y * 4);
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
        const std::vector<uint8_t> bytes = bitmap(bits);
        PrImageDecoder* decoder = nullptr;
        REQUIRE(open(bytes, &decoder) == PrStatus_Ok);
        CHECK(prImageDecoderInfo(decoder)->extent.x == 2);
        CHECK(prImageDecoderInfo(decoder)->extent.y == 2);
        CHECK(prImageDecoderInfo(decoder)->format.alpha == PrImageAlpha_Straight);
        const std::vector<uint8_t> expected = {255, 0, 0,   255, 0,   0, 255, 0,
                                               0,   0, 255, 255, 255, 0, 0,   255};
        CHECK(decode(decoder) == expected);
        prImageDecoderDestroy(decoder);
    }
}

TEST_CASE("ICO 32-bit alpha overrides its legacy transparency mask") {
    std::vector<uint8_t> bytes = bitmap(32);
    const size_t bitmapStart = 22 + 40;
    bytes[bitmapStart + 3] = 128;
    bytes[bitmapStart + 7] = 255;
    bytes[bitmapStart + 11] = 255;
    bytes[bitmapStart + 15] = 64;
    PrImageDecoder* decoder = nullptr;
    REQUIRE(open(bytes, &decoder) == PrStatus_Ok);
    const std::vector<uint8_t> pixels = decode(decoder);
    CHECK(pixels[3] == 255);
    CHECK(pixels[7] == 64);
    CHECK(pixels[11] == 128);
    CHECK(pixels[15] == 255);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("ICO PNG payloads use the native PNG representation") {
    const uint8_t pixels[] = {255, 0, 0, 255, 0, 0, 255, 128};
    std::vector<uint8_t> png(testEncodePng(PrPixelView{pixels, size_t(2) * 4, {2, 1}}, nullptr, 0));
    testEncodePng(PrPixelView{pixels, size_t(2) * 4, {2, 1}}, png.data(), png.size());
    const std::vector<uint8_t> bytes = container(png, 2, 1, 32);
    PrImageDecoder* decoder = nullptr;
    REQUIRE(open(bytes, &decoder) == PrStatus_Ok);
    CHECK(decode(decoder) == std::vector<uint8_t>(pixels, pixels + sizeof(pixels)));
    prImageDecoderDestroy(decoder);
}

TEST_CASE("ICO rejects incomplete bitmaps, out-of-range resources, and excessive output") {
    const std::vector<uint8_t> bytes = bitmap(1);
    for (size_t size = 0; size < bytes.size(); ++size) {
        PrImageDecoder* decoder = nullptr;
        const std::vector<uint8_t> prefix(bytes.data(), bytes.data() + size);
        CHECK(open(prefix, &decoder) != PrStatus_Ok);
        CHECK(decoder == nullptr);
    }
    std::vector<uint8_t> invalid = bytes;
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
    const std::vector<uint8_t> indexed = bitmap(1);
    std::vector<uint8_t> rgba = bitmap(32);
    rgba[22 + 40 + 11] = 64;
    std::vector<uint8_t> bytes(38 + indexed.size() - 22 + rgba.size() - 22);
    put16(bytes, 2, 1);
    put16(bytes, 4, 2);
    for (size_t i = 0; i < 16; ++i) {
        bytes[6 + i] = indexed[6 + i];
        bytes[22 + i] = rgba[6 + i];
    }
    const size_t rgbaOffset = 38 + indexed.size() - 22;
    put32(bytes, 18, 38);
    put32(bytes, 34, uint32_t(rgbaOffset));
    for (size_t i = 22; i < indexed.size(); ++i) {
        bytes[38 + i - 22] = indexed[i];
    }
    for (size_t i = 22; i < rgba.size(); ++i) {
        bytes[rgbaOffset + i - 22] = rgba[i];
    }
    PrImageDecoder* decoder = nullptr;
    REQUIRE(open(bytes, &decoder) == PrStatus_Ok);
    CHECK(decode(decoder)[3] == 64);
    prImageDecoderDestroy(decoder);
}
