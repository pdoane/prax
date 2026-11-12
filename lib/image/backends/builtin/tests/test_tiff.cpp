// TIFF decode tests over files written here, uncompressed with 8-bit samples.

#include "image/backends/builtin/tests/image_builtin_test.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// How a test file is laid out.
struct TiffLayout {
    uint32_t width;
    uint32_t height;
    uint32_t samples;
    uint32_t photometric;           // 0 white is zero, 1 black is zero, 2 RGB
    uint32_t extra;                 // ExtraSamples' value, or 0xffff for none
    uint32_t rowsPerStrip;
    uint32_t compression;
    bool bigEndian;
};

// Returns a file of one image whose strips hold `pixels`, in the byte order of the layout.
Array<uint8_t> writeTiff(const TiffLayout& layout, const Array<uint8_t>& pixels) {
    Array<uint8_t> out;
    auto put16 = [&](uint32_t v) {
        if (layout.bigEndian) {
            out.push(uint8_t(v >> 8));
            out.push(uint8_t(v));
        } else {
            out.push(uint8_t(v));
            out.push(uint8_t(v >> 8));
        }
    };
    auto put32 = [&](uint32_t v) {
        if (layout.bigEndian) {
            put16(v >> 16);
            put16(v & 0xffff);
        } else {
            put16(v & 0xffff);
            put16(v >> 16);
        }
    };
    uint32_t rowBytes = layout.width * layout.samples;
    uint32_t strips = (layout.height + layout.rowsPerStrip - 1) / layout.rowsPerStrip;
    // The header, the offsets and counts of the strips, then the pixels, then the directory.
    out.push(layout.bigEndian ? 'M' : 'I');
    out.push(layout.bigEndian ? 'M' : 'I');
    put16(42);
    uint32_t arrays = 8;
    uint32_t data = arrays + strips * 8;
    uint32_t directory = data + uint32_t(pixels.count());
    put32(directory);
    for (uint32_t s = 0; s < strips; ++s)
        put32(data + s * layout.rowsPerStrip * rowBytes);
    for (uint32_t s = 0; s < strips; ++s) {
        uint32_t rows = layout.height - s * layout.rowsPerStrip;
        put32((rows < layout.rowsPerStrip ? rows : layout.rowsPerStrip) * rowBytes);
    }
    out.append(pixels.span());
    struct Entry {
        uint16_t tag;
        uint16_t type;
        uint32_t count;
        uint32_t value;
    };
    Array<Entry> entries({
        {256, 4, 1, layout.width},
        {257, 4, 1, layout.height},
        {258, 3, 1, 8},
        {259, 3, 1, layout.compression},
        {262, 3, 1, layout.photometric},
        {273, 4, strips, strips == 1 ? data : arrays},
        {277, 3, 1, layout.samples},
        {278, 4, 1, layout.rowsPerStrip},
        {279, 4, strips, strips == 1 ? uint32_t(pixels.count()) : arrays + strips * 4},
    });
    if (layout.samples > 1)
        entries[2].count = 0;
    if (layout.extra != 0xffff)
        entries.push(Entry{338, 3, 1, layout.extra});
    put16(uint32_t(entries.count()));
    for (const Entry& e : entries) {
        put16(e.tag);
        put16(e.type);
        put32(e.count);
        if (e.type == 3 && e.count == 1) {
            put16(e.value);
            put16(0);
        } else {
            put32(e.value);
        }
    }
    put32(0);
    return out;
}

Array<uint8_t> readRgba8(const Array<uint8_t>& file, PrImageAlpha alpha, uint32_t* width, uint32_t* height) {
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    *width = info.extent.x;
    *height = info.extent.y;
    Array<uint8_t> pixels(size_t(info.extent.x) * info.extent.y * 4);
    REQUIRE(testReadRgba8(decoder, 0, false, alpha, pixels.data(), pixels.count()) == PrStatus_Ok);
    prImageDecoderDestroy(decoder);
    return pixels;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an RGBA tiff with straight alpha decodes in either byte order") {
    Array<uint8_t> pixels({255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 10, 20, 30, 255});
    const bool bigEndians[] = {false, true};
    for (bool bigEndian : bigEndians) {
        Array<uint8_t> file = writeTiff(TiffLayout{2, 2, 4, 2, 2, 2, 1, bigEndian}, pixels);
        CHECK(prImageCodecSupportsType(testCodec(), "image/tiff"));
        uint32_t width = 0;
        uint32_t height = 0;
        Array<uint8_t> rgba = readRgba8(file, PrImageAlpha_Straight, &width, &height);
        CHECK(width == 2);
        CHECK(height == 2);
        CHECK(rgba == pixels);
    }
}

TEST_CASE("a gray tiff in several strips decodes each strip, and white-is-zero inverts") {
    Array<uint8_t> pixels({0, 50, 100, 150, 200, 250});
    Array<uint8_t> file = writeTiff(TiffLayout{2, 3, 1, 1, 0xffff, 1, 1, false}, pixels);
    uint32_t width = 0;
    uint32_t height = 0;
    Array<uint8_t> rgba = readRgba8(file, PrImageAlpha_Straight, &width, &height);
    REQUIRE(height == 3);
    CHECK(rgba[4 * 4] == 200);
    CHECK(rgba[4 * 4 + 3] == 255);
    file = writeTiff(TiffLayout{2, 3, 1, 0, 0xffff, 2, 1, true}, pixels);
    rgba = readRgba8(file, PrImageAlpha_Straight, &width, &height);
    CHECK(rgba[0] == 255);
    CHECK(rgba[5 * 4] == 5);
}

TEST_CASE("a compressed tiff is not supported, and one whose strips run past the file does not open") {
    Array<uint8_t> pixels;
    for (uint32_t i = 0; i < 12; ++i)
        pixels.push(7);
    Array<uint8_t> file = writeTiff(TiffLayout{2, 2, 3, 2, 0xffff, 2, 5, false}, pixels);
    PrImageSource source{};
    source.memory = file;
    PrImageDecoder* decoder = nullptr;
    CHECK(testDecoderOpen(testCodec(), source, nullptr, &decoder) == PrStatus_NotSupported);
    file = writeTiff(TiffLayout{2, 2, 3, 2, 0xffff, 2, 1, false}, pixels);
    // The strip offset is the value of the sixth entry of the directory. The directory follows the header, the strip
    // arrays and the pixels.
    uint32_t offset = 8 + 8 + pixels.count() + 2 + 5 * 12 + 8;
    file[offset] = 0xff;
    file[offset + 1] = 0xff;
    decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    Array<uint8_t> rgba(16);
    CHECK(testReadRgba8(decoder, 0, false, PrImageAlpha_Straight, rgba.data(), rgba.count()) == PrStatus_InvalidData);
    prImageDecoderDestroy(decoder);
}
