// Targa decoding over files written here: each image type, stored and run-length encoded, the depths of their pixels
// and color maps, and the footer and extension area of TGA 2.0.

#include "pr/array.h"
#include "pr/mem.h"
#include "pr/tga.h"

#include "tools/harness/test.h"

extern "C" int prTestTgaCApi(void);

namespace pr_impl {

namespace {

// The fields of a header that a test sets.
struct TgaHeader {
    uint8_t type;
    uint16_t width;
    uint16_t height;
    uint8_t depth;
    uint8_t descriptor = 0;
    uint8_t mapType = 0;
    uint16_t mapFirst = 0;
    uint16_t mapLength = 0;
    uint8_t mapEntryBits = 0;
    uint8_t idLength = 0;
};

void put16(Array<uint8_t>* out, uint32_t v) {
    out->push(uint8_t(v));
    out->push(uint8_t(v >> 8));
}

void put32(Array<uint8_t>* out, uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

// Returns a file of the header, an image ID of its length, `map` and `pixels`.
Array<uint8_t> writeTga(const TgaHeader& h, const Array<uint8_t>& map, const Array<uint8_t>& pixels) {
    Array<uint8_t> out;
    out.push(h.idLength);
    out.push(h.mapType);
    out.push(h.type);
    put16(&out, h.mapFirst);
    put16(&out, h.mapLength);
    out.push(h.mapEntryBits);
    put16(&out, 0);
    put16(&out, 0);
    put16(&out, h.width);
    put16(&out, h.height);
    out.push(h.depth);
    out.push(h.descriptor);
    for (uint32_t i = 0; i < h.idLength; ++i)
        out.push(uint8_t('a' + i));
    out.append(map.span());
    out.append(pixels.span());
    return out;
}

// Appends an extension area with `attributes` as its attributes type, and the footer that locates it.
void appendFooter(Array<uint8_t>* file, uint8_t attributes) {
    const uint32_t offset = file->count();
    put16(file, 495);
    for (uint32_t i = 2; i < 494; ++i)
        file->push(0);
    file->push(attributes);
    put32(file, offset);
    put32(file, 0);
    const char signature[] = "TRUEVISION-XFILE.";
    file->append(PrByteView{reinterpret_cast<const uint8_t*>(signature), sizeof(signature)});
}

// Returns the pixels of `count` stored pixels of `bytes` bytes each, run-length encoded: each run of equal pixels of
// two or more as a run packet, and the pixels between as raw packets. Packets cross rows.
Array<uint8_t> encodeRle(const Array<uint8_t>& stored, uint32_t bytes) {
    Array<uint8_t> out;
    const uint32_t count = stored.count() / bytes;
    auto same = [&](uint32_t a, uint32_t b) { return prMemEquals(stored.data() + a * bytes, stored.data() + b * bytes, bytes); };
    for (uint32_t i = 0; i < count;) {
        uint32_t run = 1;
        while (i + run < count && run < 128 && same(i, i + run))
            ++run;
        if (run >= 2) {
            out.push(uint8_t(0x80 | (run - 1)));
            out.append(PrByteView{stored.data() + i * bytes, bytes});
            i += run;
            continue;
        }
        uint32_t raw = 1;
        while (i + raw < count && raw < 128 && !(i + raw + 1 < count && same(i + raw, i + raw + 1)))
            ++raw;
        out.push(uint8_t(raw - 1));
        out.append(PrByteView{stored.data() + i * bytes, raw * bytes});
        i += raw;
    }
    return out;
}

struct Decoded {
    PrStatus status = PrStatus_Ok;
    PrTgaInfo info{};
    Array<uint8_t> pixels;
};

Decoded decodeTga(const Array<uint8_t>& file, const PrTgaLimits* limits = nullptr) {
    Decoded decoded;
    PrTgaDecoder* decoder = nullptr;
    decoded.status = prTgaDecoderCreate(PrByteView{file.data(), file.count()}, limits, file.count(), &decoder);
    if (decoded.status != PrStatus_Ok)
        return decoded;
    decoded.info = *prTgaDecoderInfo(decoder);
    const PrUVec2 extent = decoded.info.extent;
    const uint32_t stride = extent.x * decoded.info.channels;
    decoded.pixels.resize(stride * extent.y);
    decoded.status = prTgaDecoderRead(decoder, PrPixels{decoded.pixels.data(), stride, extent});
    prTgaDecoderDestroy(decoder);
    return decoded;
}

// Twelve BGRA pixels that differ, with runs among them.
Array<uint8_t> bgraPixels() {
    return Array<uint8_t>({
        1, 2, 3, 255, 1, 2, 3, 255, 1, 2, 3, 255, 40, 50, 60, 128,
        7, 8, 9, 0, 10, 11, 12, 13, 10, 11, 12, 13, 99, 98, 97, 96,
        5, 5, 5, 5, 5, 5, 5, 5, 200, 201, 202, 203, 1, 1, 1, 1,
    });
}

// The pixels as RGBA, or RGB without alpha.
Array<uint8_t> fromBgra(const Array<uint8_t>& bgra, bool alpha) {
    Array<uint8_t> out;
    for (uint32_t i = 0; i < bgra.count(); i += 4) {
        out.push(bgra[i + 2]);
        out.push(bgra[i + 1]);
        out.push(bgra[i]);
        if (alpha)
            out.push(bgra[i + 3]);
    }
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a true-color tga of 24 bits decodes, with rows from the bottom by default") {
    Array<uint8_t> bgra = bgraPixels();
    Array<uint8_t> bgr;
    for (uint32_t i = 0; i < bgra.count(); i += 4)
        bgr.append(PrByteView{bgra.data() + i, 3});
    Array<uint8_t> file = writeTga(TgaHeader{.type = 2, .width = 4, .height = 3, .depth = 24, .idLength = 5}, {}, bgr);
    CHECK(prTgaHasHeader(PrByteView{file.data(), file.count()}));
    CHECK(!prTgaHasFooter(PrByteView{file.data(), file.count()}));
    Decoded decoded = decodeTga(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.extent == PrUVec2{4, 3});
    CHECK(decoded.info.channels == 3);
    CHECK(decoded.info.alpha == PrTgaAlpha_None);
    CHECK(decoded.info.fromBottom);
    CHECK(!decoded.info.fromRight);
    CHECK(decoded.pixels == fromBgra(bgra, false));

    file = writeTga(TgaHeader{.type = 2, .width = 4, .height = 3, .depth = 24, .descriptor = 0x30}, {}, bgr);
    decoded = decodeTga(file);
    CHECK(!decoded.info.fromBottom);
    CHECK(decoded.info.fromRight);
}

TEST_CASE("a 32-bit tga has alpha by its attribute bits, and its extension area says what kind") {
    Array<uint8_t> bgra = bgraPixels();
    Array<uint8_t> file = writeTga(TgaHeader{.type = 2, .width = 4, .height = 3, .depth = 32, .descriptor = 8}, {}, bgra);
    Decoded decoded = decodeTga(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.alpha == PrTgaAlpha_Straight);
    CHECK(decoded.pixels == fromBgra(bgra, true));

    file = writeTga(TgaHeader{.type = 2, .width = 4, .height = 3, .depth = 32}, {}, bgra);
    decoded = decodeTga(file);
    CHECK(decoded.info.alpha == PrTgaAlpha_None);
    CHECK(decoded.pixels == fromBgra(bgra, false));

    const uint8_t types[] = {0, 3, 4};
    const PrTgaAlpha alphas[] = {PrTgaAlpha_None, PrTgaAlpha_Straight, PrTgaAlpha_Premultiplied};
    for (uint32_t i = 0; i < 3; ++i) {
        file = writeTga(TgaHeader{.type = 2, .width = 4, .height = 3, .depth = 32, .descriptor = 8}, {}, bgra);
        appendFooter(&file, types[i]);
        CHECK(prTgaHasFooter(PrByteView{file.data(), file.count()}));
        decoded = decodeTga(file);
        REQUIRE(decoded.status == PrStatus_Ok);
        CHECK(decoded.info.alpha == alphas[i]);
    }
}

TEST_CASE("a run-length encoded tga decodes packets that cross rows") {
    Array<uint8_t> bgra = bgraPixels();
    Array<uint8_t> file =
        writeTga(TgaHeader{.type = 10, .width = 4, .height = 3, .depth = 32, .descriptor = 8}, {}, encodeRle(bgra, 4));
    Decoded decoded = decodeTga(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == fromBgra(bgra, true));
}

TEST_CASE("a color-mapped tga looks its indices up from the map's first entry") {
    // Three entries of 24 bits, the first of them entry 5.
    Array<uint8_t> map({10, 20, 30, 40, 50, 60, 70, 80, 90});
    Array<uint8_t> indices({5, 6, 7, 7, 6, 5});
    TgaHeader h{
        .type = 1,
        .width = 3,
        .height = 2,
        .depth = 8,
        .mapType = 1,
        .mapFirst = 5,
        .mapLength = 3,
        .mapEntryBits = 24,
    };
    Decoded decoded = decodeTga(writeTga(h, map, indices));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({30, 20, 10, 60, 50, 40, 90, 80, 70, 90, 80, 70, 60, 50, 40, 30, 20, 10}));

    h.type = 9;
    decoded = decodeTga(writeTga(h, map, encodeRle(indices, 1)));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels[9] == 90);

    indices[2] = 8;
    h.type = 1;
    CHECK(decodeTga(writeTga(h, map, indices)).status == PrStatus_InvalidData);
    indices[2] = 4;
    CHECK(decodeTga(writeTga(h, map, indices)).status == PrStatus_InvalidData);
}

TEST_CASE("15-bit and 16-bit colors widen to 8 bits, and the top bit of 16 is alpha") {
    // Red at full, green at half, blue at zero, with the top bit set, then black without it.
    const uint32_t first = 1u << 15 | 31u << 10 | 16u << 5;
    Array<uint8_t> pixels({uint8_t(first), uint8_t(first >> 8), 0, 0});
    Decoded decoded = decodeTga(writeTga(TgaHeader{.type = 2, .width = 2, .height = 1, .depth = 16, .descriptor = 1},
        {}, pixels));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.channels == 4);
    CHECK(decoded.pixels == Array<uint8_t>({255, 132, 0, 255, 0, 0, 0, 0}));

    decoded = decodeTga(writeTga(TgaHeader{.type = 2, .width = 2, .height = 1, .depth = 15}, {}, pixels));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.channels == 3);
    CHECK(decoded.pixels == Array<uint8_t>({255, 132, 0, 0, 0, 0}));

    // A map of 16-bit entries.
    Array<uint8_t> map({uint8_t(first), uint8_t(first >> 8)});
    TgaHeader h{
        .type = 1,
        .width = 1,
        .height = 1,
        .depth = 8,
        .descriptor = 1,
        .mapType = 1,
        .mapLength = 1,
        .mapEntryBits = 16,
    };
    decoded = decodeTga(writeTga(h, map, Array<uint8_t>({0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({255, 132, 0, 255}));
}

TEST_CASE("a black and white tga decodes to gray, with alpha at 16 bits") {
    Array<uint8_t> gray({0, 50, 100, 150, 200, 250});
    Decoded decoded = decodeTga(writeTga(TgaHeader{.type = 3, .width = 3, .height = 2, .depth = 8}, {}, gray));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.gray);
    CHECK(decoded.info.channels == 1);
    CHECK(decoded.pixels == gray);

    decoded = decodeTga(writeTga(TgaHeader{.type = 11, .width = 3, .height = 2, .depth = 8}, {}, encodeRle(gray, 1)));
    CHECK(decoded.pixels == gray);

    Array<uint8_t> grayAlpha({10, 255, 20, 128});
    decoded = decodeTga(writeTga(TgaHeader{.type = 3, .width = 2, .height = 1, .depth = 16, .descriptor = 8}, {},
        grayAlpha));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.channels == 2);
    CHECK(decoded.pixels == grayAlpha);
}

TEST_CASE("a tga whose pixels end early is truncated, and the rest are zero") {
    Array<uint8_t> gray({9, 9, 9});
    Decoded decoded = decodeTga(writeTga(TgaHeader{.type = 3, .width = 3, .height = 2, .depth = 8}, {}, gray));
    CHECK(decoded.status == PrStatus_Truncated);
    CHECK(decoded.pixels == Array<uint8_t>({9, 9, 9, 0, 0, 0}));

    // A run packet without its pixel.
    decoded = decodeTga(writeTga(TgaHeader{.type = 11, .width = 3, .height = 2, .depth = 8}, {},
        Array<uint8_t>({0x82, 7, 0x85})));
    CHECK(decoded.status == PrStatus_Truncated);
    CHECK(decoded.pixels == Array<uint8_t>({7, 7, 7, 0, 0, 0}));
}

TEST_CASE("a tga header that the specification does not allow does not open") {
    const TgaHeader bad[] = {
        {.type = 4, .width = 1, .height = 1, .depth = 8},
        {.type = 2, .width = 0, .height = 1, .depth = 24},
        {.type = 2, .width = 1, .height = 1, .depth = 12},
        {.type = 3, .width = 1, .height = 1, .depth = 24},
        {.type = 1, .width = 1, .height = 1, .depth = 8},
        {.type = 2, .width = 1, .height = 1, .depth = 24, .descriptor = 0x40},
        {.type = 2, .width = 1, .height = 1, .depth = 24, .mapLength = 4},
        {.type = 1, .width = 1, .height = 1, .depth = 8, .mapType = 1, .mapLength = 1, .mapEntryBits = 8},
    };
    for (const TgaHeader& h : bad) {
        Array<uint8_t> file = writeTga(h, {}, Array<uint8_t>({0, 0, 0, 0}));
        CHECK(!prTgaHasHeader(PrByteView{file.data(), file.count()}));
        CHECK(decodeTga(file).status == PrStatus_InvalidData);
    }
    // A color map past the end of the file.
    TgaHeader h{.type = 1, .width = 1, .height = 1, .depth = 8, .mapType = 1, .mapLength = 200, .mapEntryBits = 24};
    CHECK(decodeTga(writeTga(h, {}, {})).status == PrStatus_InvalidData);
    // Indices of 16 bits.
    h.mapLength = 1;
    h.depth = 16;
    CHECK(decodeTga(writeTga(h, Array<uint8_t>({1, 2, 3}), Array<uint8_t>({0, 0}))).status == PrStatus_NotSupported);
}

TEST_CASE("a tga past the limits does not open") {
    Array<uint8_t> file = writeTga(TgaHeader{.type = 2, .width = 100, .height = 100, .depth = 32, .descriptor = 8}, {}, {});
    PrTgaLimits limits = prTgaLimitsDefault();
    limits.pixels = 9999;
    CHECK(decodeTga(file, &limits).status == PrStatus_Limit);
    limits = prTgaLimitsDefault();
    limits.outputBytes = 39999;
    CHECK(decodeTga(file, &limits).status == PrStatus_Limit);
    limits = prTgaLimitsDefault();
    limits.work = 9999;
    CHECK(decodeTga(file, &limits).status == PrStatus_Limit);
    limits = prTgaLimitsDefault();
    limits.workingBytes = file.count();
    CHECK(decodeTga(file, &limits).status == PrStatus_Limit);
}

TEST_CASE("the tga header compiles as C") {
    CHECK(prTestTgaCApi() == 0);
}
