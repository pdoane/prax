// BMP decoding over files written here: each version of the information header, each depth, bit fields, run-length
// encoding, row order, and color spaces. Icons' bitmaps are tested through lib/ico and its image codec.

#include "pr/array.h"
#include "pr/bmp.h"
#include "pr/mem.h"

#include "tools/harness/test.h"

extern "C" int prTestBmpCApi(void);

namespace pr_impl {

namespace {

void put16(Array<uint8_t>* out, uint32_t v) {
    out->push(uint8_t(v));
    out->push(uint8_t(v >> 8));
}

void put32(Array<uint8_t>* out, uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

void set32(Array<uint8_t>& bytes, uint32_t at, uint32_t v) {
    for (uint32_t i = 0; i < 4; ++i)
        bytes[at + i] = uint8_t(v >> (8 * i));
}

// What a test file holds. `masks` are written into a header from version 2 on, and after a version 1 header for the bit
// field compressions.
struct BmpLayout {
    uint32_t headerSize = 40;
    int32_t width = 2;
    int32_t height = 2;
    uint16_t bits = 24;
    uint32_t compression = 0;
    uint32_t masks[4] = {0, 0, 0, 0};
    uint32_t used = 0;
    uint32_t space = 0;
    int32_t perMeter = 0;
};

// Returns a file of `layout`, its color table `table`, of entries of four bytes or of three for a core header, its
// `pixels`, and `profile` after them for a version 5 header that embeds it.
Array<uint8_t> writeBmp(const BmpLayout& layout, const Array<uint8_t>& table, const Array<uint8_t>& pixels,
        const Array<uint8_t>& profile = {}) {
    Array<uint8_t> dib;
    put32(&dib, layout.headerSize);
    if (layout.headerSize == 12) {
        put16(&dib, uint32_t(layout.width));
        put16(&dib, uint32_t(layout.height));
        put16(&dib, 1);
        put16(&dib, layout.bits);
    } else {
        put32(&dib, uint32_t(layout.width));
        put32(&dib, uint32_t(layout.height));
        put16(&dib, 1);
        put16(&dib, layout.bits);
        put32(&dib, layout.compression);
        put32(&dib, pixels.count());
        put32(&dib, uint32_t(layout.perMeter));
        put32(&dib, uint32_t(layout.perMeter));
        put32(&dib, layout.used);
        put32(&dib, 0);
        while (dib.count() < layout.headerSize)
            dib.push(0);
        if (layout.headerSize >= 52) {
            for (uint32_t i = 0; i < (layout.headerSize >= 56 ? 4u : 3u); ++i)
                set32(dib, 40 + i * 4, layout.masks[i]);
        }
        if (layout.headerSize >= 108)
            set32(dib, 56, layout.space);
        if (layout.headerSize == 40 && (layout.compression == 3 || layout.compression == 6)) {
            for (uint32_t i = 0; i < (layout.compression == 6 ? 4u : 3u); ++i)
                put32(&dib, layout.masks[i]);
        }
    }
    dib.append(table.span());
    const uint32_t offset = 14 + dib.count();
    if (layout.headerSize == 124 && profile.count() != 0) {
        set32(dib, 112, dib.count() + pixels.count());
        set32(dib, 116, profile.count());
    }
    Array<uint8_t> out({'B', 'M'});
    put32(&out, offset + pixels.count() + profile.count());
    put32(&out, 0);
    put32(&out, offset);
    out.append(dib.span());
    out.append(pixels.span());
    out.append(profile.span());
    return out;
}

struct Decoded {
    PrStatus status = PrStatus_Ok;
    PrBmpInfo info{};
    Array<uint8_t> pixels;
};

Decoded decodeBmp(const Array<uint8_t>& file, const PrBmpLimits* limits = nullptr) {
    Decoded decoded;
    PrBmpDecoder* decoder = nullptr;
    decoded.status = prBmpDecoderCreate(PrByteView{file.data(), file.count()}, PrBmpSource_File, limits, file.count(), &decoder);
    if (decoder == nullptr)
        return decoded;
    decoded.info = *prBmpDecoderInfo(decoder);
    const PrUVec2 extent = decoded.info.extent;
    const uint32_t stride = extent.x * decoded.info.channels;
    decoded.pixels.resize(stride * extent.y);
    const PrStatus read = prBmpDecoderRead(decoder, PrPixels{decoded.pixels.data(), stride, extent});
    if (decoded.status == PrStatus_Ok)
        decoded.status = read;
    prBmpDecoderDestroy(decoder);
    return decoded;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a 24-bit bmp decodes from the top row, whichever order its rows are stored in") {
    // Rows are padded to four bytes. The bottom row is stored first: red, green, then blue, white.
    Array<uint8_t> pixels({0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0});
    Array<uint8_t> file = writeBmp(BmpLayout{.perMeter = 2835}, {}, pixels);
    CHECK(prBmpHasSignature(PrByteView{file.data(), file.count()}));
    Decoded decoded = decodeBmp(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.channels == 3);
    CHECK(decoded.info.pixelsPerMeter.x == 2835);
    CHECK(decoded.pixels == Array<uint8_t>({0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 0}));

    decoded = decodeBmp(writeBmp(BmpLayout{.height = -2}, {}, pixels));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}));
}

TEST_CASE("color tables index 1, 4 and 8 bits, with three-byte entries in a core header") {
    Array<uint8_t> table({0, 0, 0, 0, 255, 255, 255, 0, 0, 0, 255, 0});
    // One row of four pixels, bottom-up of one row.
    Decoded decoded = decodeBmp(writeBmp(BmpLayout{.width = 4, .height = 1, .bits = 1, .used = 2}, table,
        Array<uint8_t>({0x50, 0, 0, 0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({0, 0, 0, 255, 255, 255, 0, 0, 0, 255, 255, 255}));

    // An index past the table is black.
    decoded = decodeBmp(writeBmp(BmpLayout{.width = 3, .height = 1, .bits = 4, .used = 3}, table,
        Array<uint8_t>({0x12, 0x50, 0, 0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({255, 255, 255, 255, 0, 0, 0, 0, 0}));

    Array<uint8_t> core({0, 0, 0, 255, 255, 255, 0, 255, 0});
    decoded = decodeBmp(writeBmp(BmpLayout{.headerSize = 12, .width = 3, .height = 1, .bits = 8}, core,
        Array<uint8_t>({2, 1, 0, 0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({0, 255, 0, 255, 255, 255, 0, 0, 0}));
}

TEST_CASE("Windows CE's 2-bit pixels index four entries, and 64-bit pixels are not supported") {
    Array<uint8_t> table({0, 0, 0, 0, 255, 255, 255, 0, 0, 0, 255, 0, 0, 255, 0, 0});
    Decoded decoded = decodeBmp(writeBmp(BmpLayout{.width = 4, .height = 1, .bits = 2, .used = 4}, table,
        Array<uint8_t>({0x1b, 0, 0, 0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 255, 0}));
    CHECK(decodeBmp(writeBmp(BmpLayout{.bits = 64}, {}, Array<uint8_t>(32))).status == PrStatus_NotSupported);
}

TEST_CASE("16-bit and 32-bit pixels read their bit fields, and alpha from an alpha mask") {
    // 5-5-5 by default: full red, and blue at 16 of 31.
    const uint32_t v555 = 31u << 10 | 16u;
    Decoded decoded = decodeBmp(writeBmp(BmpLayout{.width = 1, .height = 1, .bits = 16}, {},
        Array<uint8_t>({uint8_t(v555), uint8_t(v555 >> 8), 0, 0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({255, 0, 132}));

    // 5-6-5 in masks after a version 1 header: green at 32 of 63.
    const uint32_t v565 = 32u << 5;
    decoded = decodeBmp(writeBmp(BmpLayout{
        .width = 1,
        .height = 1,
        .bits = 16,
        .compression = 3,
        .masks = {0xf800, 0x07e0, 0x001f, 0},
    }, {}, Array<uint8_t>({uint8_t(v565), uint8_t(v565 >> 8), 0, 0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({0, 130, 0}));

    // 32 bits without bit fields have no alpha.
    decoded = decodeBmp(writeBmp(BmpLayout{.width = 1, .height = 1, .bits = 32}, {}, Array<uint8_t>({1, 2, 3, 4})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.channels == 3);
    CHECK(decoded.pixels == Array<uint8_t>({3, 2, 1}));

    // A version 4 header's alpha mask.
    const BmpLayout v4{
        .headerSize = 108,
        .bits = 32,
        .compression = 3,
        .masks = {0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000},
    };
    Array<uint8_t> pixels({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0});
    decoded = decodeBmp(writeBmp(v4, {}, pixels));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.channels == 4);
    CHECK(decoded.pixels == Array<uint8_t>({11, 10, 9, 12, 15, 14, 13, 0, 3, 2, 1, 4, 7, 6, 5, 8}));

    // Alpha of zero everywhere is opaque.
    for (uint32_t i = 3; i < 16; i += 4)
        pixels[i] = 0;
    decoded = decodeBmp(writeBmp(v4, {}, pixels));
    CHECK(decoded.pixels[3] == 255);
    CHECK(decoded.pixels[15] == 255);

    // Masks of four after a version 1 header, for the alpha bit fields compression.
    decoded = decodeBmp(writeBmp(BmpLayout{
        .width = 1,
        .height = 1,
        .bits = 32,
        .compression = 6,
        .masks = {0xff, 0xff00, 0xff0000, 0xff000000},
    }, {}, Array<uint8_t>({1, 2, 3, 128})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({1, 2, 3, 128}));

    // Bits that are not contiguous are not a field.
    decoded = decodeBmp(writeBmp(BmpLayout{
        .width = 1,
        .height = 1,
        .bits = 32,
        .compression = 3,
        .masks = {0xf0f, 0xf000, 0xff0000, 0},
    }, {}, Array<uint8_t>({1, 2, 3, 4})));
    CHECK(decoded.status == PrStatus_InvalidData);
}

TEST_CASE("run-length encoding decodes runs, absolute runs and deltas, and what it skips is transparent") {
    Array<uint8_t> table({0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0});
    // Four by two, from the bottom: a run of two of red, an absolute run of green, blue and green, whose last index
    // passes the row, and its padding, the end of the line, then a delta of two across, a run of one of blue, and the
    // end of the bitmap.
    Array<uint8_t> codes({2, 0, 0, 3, 1, 2, 1, 0, 0, 0, 0, 2, 2, 0, 1, 2, 0, 1});
    Decoded decoded = decodeBmp(writeBmp(BmpLayout{.width = 4, .height = 2, .bits = 8, .compression = 1, .used = 3},
        table, codes));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.channels == 4);
    CHECK(decoded.pixels == Array<uint8_t>({
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 0, 0,
        255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
    }));

    // RLE4: a run of alternating nibbles, then an absolute run of three nibbles padded to a word.
    Array<uint8_t> rle4({3, 0x12, 0, 3, 0x21, 0x00, 0, 1});
    decoded = decodeBmp(writeBmp(BmpLayout{.width = 6, .height = 1, .bits = 4, .compression = 2, .used = 3}, table, rle4));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels[0 * 4 + 1] == 255);
    CHECK(decoded.pixels[1 * 4 + 2] == 255);
    CHECK(decoded.pixels[2 * 4 + 1] == 255);
    CHECK(decoded.pixels[3 * 4 + 2] == 255);
    CHECK(decoded.pixels[4 * 4 + 1] == 255);
    CHECK(decoded.pixels[5 * 4 + 0] == 255);
    CHECK(decoded.pixels[5 * 4 + 3] == 255);

    // Codes that end before the end of the bitmap are truncated, and a delta past the bitmap is invalid.
    decoded = decodeBmp(writeBmp(BmpLayout{.width = 4, .height = 2, .bits = 8, .compression = 1}, table,
        Array<uint8_t>({2, 0})));
    CHECK(decoded.status == PrStatus_Truncated);
    decoded = decodeBmp(writeBmp(BmpLayout{.width = 4, .height = 2, .bits = 8, .compression = 1}, table,
        Array<uint8_t>({0, 2, 9, 0, 0, 1})));
    CHECK(decoded.status == PrStatus_InvalidData);
}

TEST_CASE("a version 4 header names sRGB, and a version 5 header embeds a profile") {
    Array<uint8_t> pixels({1, 2, 3, 0});
    Decoded decoded = decodeBmp(writeBmp(BmpLayout{.headerSize = 108, .width = 1, .height = 1, .space = 0x73524742}, {}, pixels));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.color == PrBmpColor_Srgb);

    Array<uint8_t> profile({'p', 'r', 'o', 'f', 'i', 'l', 'e'});
    decoded = decodeBmp(
        writeBmp(BmpLayout{.headerSize = 124, .width = 1, .height = 1, .space = 0x4d424544}, {}, pixels, profile));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.color == PrBmpColor_Profile);
    REQUIRE(decoded.info.profile.size == profile.count());

    decoded = decodeBmp(writeBmp(BmpLayout{.headerSize = 124, .width = 1, .height = 1, .space = 0}, {}, pixels));
    CHECK(decoded.info.color == PrBmpColor_Unknown);
}

TEST_CASE("a bmp whose rows end early is truncated, and the rest are transparent black") {
    Array<uint8_t> pixels({0, 0, 255, 0, 255, 0, 0, 0});
    Decoded decoded = decodeBmp(writeBmp(BmpLayout{}, {}, pixels));
    CHECK(decoded.status == PrStatus_Truncated);
    // The bottom row is stored, and the top row is not.
    CHECK(decoded.pixels == Array<uint8_t>({0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 255, 0}));
}

TEST_CASE("a bmp that breaks the format does not open, and an embedded jpeg or png is not supported") {
    Array<uint8_t> pixels(16);
    CHECK(decodeBmp(writeBmp(BmpLayout{.compression = 4}, {}, pixels)).status == PrStatus_NotSupported);
    CHECK(decodeBmp(writeBmp(BmpLayout{.compression = 5}, {}, pixels)).status == PrStatus_NotSupported);
    CHECK(decodeBmp(writeBmp(BmpLayout{.bits = 3}, {}, pixels)).status == PrStatus_InvalidData);
    CHECK(decodeBmp(writeBmp(BmpLayout{.width = 0}, {}, pixels)).status == PrStatus_InvalidData);
    CHECK(decodeBmp(writeBmp(BmpLayout{.bits = 24, .compression = 1}, {}, pixels)).status == PrStatus_InvalidData);
    CHECK(decodeBmp(writeBmp(BmpLayout{.height = -2, .bits = 8, .compression = 1}, {}, pixels)).status ==
        PrStatus_InvalidData);
    CHECK(decodeBmp(writeBmp(BmpLayout{.headerSize = 64, .compression = 4}, {}, pixels)).status ==
        PrStatus_NotSupported);
    Array<uint8_t> file = writeBmp(BmpLayout{}, {}, pixels);
    set32(file, 14, 41);
    CHECK(decodeBmp(file).status == PrStatus_NotSupported);
}

TEST_CASE("a bmp past the limits does not open") {
    Array<uint8_t> file = writeBmp(BmpLayout{.width = 100, .height = 100}, {}, {});
    PrBmpLimits limits = prBmpLimitsDefault();
    limits.pixels = 9999;
    CHECK(decodeBmp(file, &limits).status == PrStatus_Limit);
    limits = prBmpLimitsDefault();
    limits.outputBytes = 29999;
    CHECK(decodeBmp(file, &limits).status == PrStatus_Limit);
    limits = prBmpLimitsDefault();
    limits.work = 9999;
    CHECK(decodeBmp(file, &limits).status == PrStatus_Limit);
    limits = prBmpLimitsDefault();
    limits.workingBytes = file.count();
    CHECK(decodeBmp(file, &limits).status == PrStatus_Limit);
}

TEST_CASE("the bmp header compiles as C") {
    CHECK(prTestBmpCApi() == 0);
}
