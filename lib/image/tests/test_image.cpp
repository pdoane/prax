#include "pr/image.h"
#include "pr/mem.h"

#include "tools/harness/test.h"

extern "C" int prTestImageCApi(void);

namespace pr_impl {

namespace {

PrImageFormat rgbaFormat() {
    return {
        PrColorModel_Rgb,
        PrImageSample_U8,
        PrImageAlpha_Straight,
        {PrImageColorEncoding_Named, PrColorSpace_Srgb, {nullptr, 0}},
    };
}

struct Reader {
    uint8_t bytes[8] = {'t', 'e', 's', 't', 1, 2, 3, 4};
    uint32_t reads = 0;
    uint64_t lastOffset = 0;
    size_t lastSize = 0;
    PrStatus result = PrStatus_Ok;

    static PrStatus read(void* context, uint64_t offset, void* out, size_t size) {
        Reader& reader = *static_cast<Reader*>(context);
        ++reader.reads;
        reader.lastOffset = offset;
        reader.lastSize = size;
        if (reader.result != PrStatus_Ok) {
            return reader.result;
        }
        if (offset > sizeof(reader.bytes) || size > sizeof(reader.bytes) - size_t(offset)) {
            return PrStatus_Io;
        }
        prMemCopy(out, reader.bytes + offset, size);
        return PrStatus_Ok;
    }

    PrImageSource source() {
        return {.readAt = read, .stream = {this, sizeof(bytes)}};
    }
};

// A codec whose record is its first member, and which hands out its single reader.
struct FakeCodec {
    PrImageCodec record = {};
    PrImageReader reader = {};
    bool matches = true;
    uint32_t classifications = 0;
    size_t prefixSize = 0;
    uint32_t opens = 0;
    uint32_t closes = 0;
    uint32_t decodes = 0;
    bool rereadPrefix = false;
    PrStatus openResult = PrStatus_Ok;
    PrImageInfo info = {{2, 2}, rgbaFormat(), PrImageOrientation_Identity, {0, 0}, 1, 1, 0, false};

    static FakeCodec& of(PrImageCodec* codec) {
        return *reinterpret_cast<FakeCodec*>(codec);
    }

    static bool supportsType(PrImageCodec*, PrStringView type) {
        return type.size == 10 && prMemCompare(type.data, "image/test", 10) == 0;
    }

    static bool recognize(PrImageCodec* codec, PrByteView prefix) {
        FakeCodec& fake = of(codec);
        ++fake.classifications;
        fake.prefixSize = prefix.size;
        return fake.matches && prefix.size != 0 && prefix.data[0] == 't';
    }

    static PrStatus open(PrImageCodec* codec, const PrImageSource* source, const PrImageLimits*,
            PrImageReader** out, PrImageInfo* info) {
        FakeCodec& fake = of(codec);
        ++fake.opens;
        *out = nullptr;
        if (fake.rereadPrefix) {
            uint8_t prefix[4] = {};
            PrStatus read = prImageSourceRead(source, 0, prefix, sizeof(prefix));
            if (read != PrStatus_Ok) {
                return read;
            }
        }
        if (fake.openResult == PrStatus_Ok || fake.openResult == PrStatus_Truncated) {
            fake.reader.codec = codec;
            *out = &fake.reader;
            *info = fake.info;
        }
        return fake.openResult;
    }

    static void close(PrImageCodec* codec, PrImageReader*) {
        ++of(codec).closes;
    }

    static PrStatus frameInfo(PrImageCodec*, PrImageReader*, uint32_t frame, PrImageFrameInfo* info) {
        info->durationMs = frame;
        return PrStatus_Ok;
    }

    static PrStatus decode(PrImageCodec* codec, PrImageReader*, uint32_t, PrPixels output) {
        ++of(codec).decodes;
        for (uint32_t y = 0; y < output.extent.y; ++y) {
            for (uint32_t x = 0; x < output.extent.x; ++x) {
                uint8_t* pixel = output.data + y * output.stride + x * 4;
                pixel[0] = uint8_t(10 + y * output.extent.x + x);
                pixel[1] = 20;
                pixel[2] = 30;
                pixel[3] = 255;
            }
        }
        return PrStatus_Ok;
    }

    PrImageCodec* codec(uint32_t headerBytes = 4) {
        record = PrImageCodec{nullptr, headerBytes, supportsType, recognize, open, close, frameInfo, decode, nullptr};
        return &record;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestImageCApi() == 0);
}

TEST_CASE("a decoder reads the codec's prefix once and opens images one at a time") {
    Reader reader;
    FakeCodec fake;
    PrImageCodec* codec = fake.codec(6);
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    CHECK(fake.opens == 0);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
    CHECK(reader.reads == 1);
    CHECK(reader.lastOffset == 0);
    CHECK(reader.lastSize == 6);
    CHECK(fake.classifications == 1);
    CHECK(fake.opens == 1);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
    CHECK(fake.closes == 1);
    CHECK(fake.opens == 2);
    prImageDecoderClose(decoder);
    CHECK(fake.closes == 2);
    CHECK(prImageDecoderInfo(decoder)->frameCount == 0);
    prImageDecoderClose(decoder);
    CHECK(fake.closes == 2);
    prImageDecoderDestroy(decoder);
    CHECK(fake.closes == 2);
}

TEST_CASE("image classification shares the available short prefix") {
    Reader reader;
    PrImageSource source = reader.source();
    source.stream.size = 2;
    FakeCodec fake;
    PrImageCodec* codec = fake.codec(8);
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok);
    CHECK(reader.reads == 1);
    CHECK(reader.lastSize == 2);
    CHECK(fake.prefixSize == 2);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("codec inspection reuses the prefix without another positioned read") {
    Reader reader;
    FakeCodec fake;
    fake.rereadPrefix = true;
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
    CHECK(reader.reads == 1);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("unmatched image codecs do not open a decoder") {
    Reader reader;
    FakeCodec fake;
    fake.matches = false;
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    CHECK(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_NotSupported);
    CHECK(fake.opens == 0);
    CHECK(prImageDecoderInfo(decoder)->frameCount == 0);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("a failed open leaves no image open") {
    const PrStatus statusList[] = {PrStatus_InvalidData, PrStatus_NotSupported, PrStatus_Limit, PrStatus_Io};
    for (PrStatus status : statusList) {
        Reader reader;
        FakeCodec fake;
        PrImageCodec* codec = fake.codec();
        PrImageDecoder* decoder = prImageDecoderCreate(codec);
        REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
        fake.openResult = status;
        CHECK(prImageDecoderOpen(decoder, reader.source(), nullptr) == status);
        CHECK(fake.opens == 2);
        CHECK(fake.closes == 1);
        CHECK(prImageDecoderInfo(decoder)->frameCount == 0);
        prImageDecoderDestroy(decoder);
        CHECK(fake.closes == 1);
    }
}

TEST_CASE("image source bounds are checked before callbacks") {
    Reader reader;
    PrImageSource source = reader.source();
    uint8_t result[4] = {};
    CHECK(prImageSourceRead(&source, 6, result, 4) != PrStatus_Ok);
    CHECK(prImageSourceRead(&source, UINT64_MAX, result, 2) != PrStatus_Ok);
    CHECK(reader.reads == 0);
    REQUIRE(prImageSourceRead(&source, 4, result, 4) == PrStatus_Ok);
    CHECK(result[0] == 1);
    CHECK(result[3] == 4);
    CHECK(reader.reads == 1);
    source = {.memory = {reader.bytes, sizeof(reader.bytes)}};
    REQUIRE(prImageSourceRead(&source, 4, result, 4) == PrStatus_Ok);
    CHECK(result[0] == 1);
    CHECK(result[3] == 4);
    CHECK(prImageSourceRead(&source, 7, result, 2) != PrStatus_Ok);
}

TEST_CASE("image dispatch propagates source I/O failure without classification") {
    Reader reader;
    reader.result = PrStatus_Io;
    FakeCodec fake;
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    CHECK(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Io);
    CHECK(prImageDecoderInfo(decoder)->frameCount == 0);
    prImageDecoderDestroy(decoder);
    CHECK(fake.classifications == 0);
    CHECK(fake.opens == 0);
}

TEST_CASE("image dispatch applies input and header ceilings before reading") {
    const bool inputLimits[] = {false, true};
    for (bool inputLimit : inputLimits) {
        Reader reader;
        FakeCodec fake;
        PrImageCodec* codec = fake.codec();
        PrImageLimits limits = prImageLimitsDefault();
        if (inputLimit) {
            limits.inputBytes = 7;
        } else {
            limits.headerBytes = 3;
        }
        PrImageDecoder* decoder = prImageDecoderCreate(codec);
        CHECK(prImageDecoderOpen(decoder, reader.source(), &limits) == PrStatus_Limit);
        CHECK(prImageDecoderInfo(decoder)->frameCount == 0);
        prImageDecoderDestroy(decoder);
        CHECK(reader.reads == 0);
        CHECK(fake.opens == 0);
    }
}

TEST_CASE("image dispatch rejects malformed descriptions and closes codec handles") {
    for (int invalid = 0; invalid < 6; ++invalid) {
        Reader reader;
        FakeCodec fake;
        switch (invalid) {
            case 0:
                fake.info.extent.x = 0;
                break;
            case 1:
                fake.info.frameCount = 0;
                break;
            case 2:
                fake.info.format.sample = 255;
                break;
            case 3:
                fake.info.format.model = PrColorModel_Xyz;
                break;
            case 4:
                fake.info.orientation = 255;
                break;
            case 5:
                fake.info.format.color = {PrImageColorEncoding_Icc, 0, {nullptr, 16}};
                break;
        }
        PrImageCodec* codec = fake.codec();
        PrImageDecoder* decoder = prImageDecoderCreate(codec);
        CHECK(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_InvalidData);
        CHECK(prImageDecoderInfo(decoder)->frameCount == 0);
        prImageDecoderDestroy(decoder);
        CHECK(fake.closes == 1);
    }
}

TEST_CASE("image dispatch enforces inspected pixel frame and output limits") {
    for (int ceiling = 0; ceiling < 3; ++ceiling) {
        Reader reader;
        FakeCodec fake;
        PrImageCodec* codec = fake.codec();
        PrImageLimits limits = prImageLimitsDefault();
        if (ceiling == 0)
            limits.pixels = 3;
        if (ceiling == 1)
            limits.frames = 0;
        if (ceiling == 2)
            limits.outputBytes = 15;
        PrImageDecoder* decoder = prImageDecoderCreate(codec);
        CHECK(prImageDecoderOpen(decoder, reader.source(), &limits) == PrStatus_Limit);
        CHECK(prImageDecoderInfo(decoder)->frameCount == 0);
        prImageDecoderDestroy(decoder);
        CHECK(fake.closes == 1);
    }
}

TEST_CASE("image named color ignores inactive profile fields") {
    Reader reader;
    FakeCodec fake;
    fake.info.format.color.profile = {reader.bytes, SIZE_MAX};
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
    uint8_t pixels[16] = {};
    CHECK(prImageDecoderRead(decoder, 0, PrPixels{pixels, 8, {2, 2}}) == PrStatus_Ok);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("persistent image decoder checks the frame and respects padding") {
    Reader reader;
    FakeCodec fake;
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
    REQUIRE(prImageDecoderInfo(decoder) != nullptr);
    CHECK(prImageDecoderInfo(decoder)->extent.x == 2);
    uint8_t pixels[24];
    prMemFill(pixels, 0xA5, sizeof(pixels));
    const PrPixels output{pixels, 12, {2, 2}};
    CHECK(prImageDecoderRead(decoder, 1, output) == PrStatus_OutOfRange);
    PrImageFrameInfo frameInfo = {};
    CHECK(prImageDecoderFrameInfo(decoder, 1, &frameInfo) == PrStatus_OutOfRange);
    CHECK(fake.decodes == 0);
    REQUIRE(prImageDecoderRead(decoder, 0, output) == PrStatus_Ok);
    CHECK(pixels[0] == 10);
    CHECK(pixels[12] == 12);
    const size_t ats[] = {size_t(8), size_t(9), size_t(10), size_t(11), size_t(20), size_t(23)};
    for (size_t at : ats) {
        CHECK(pixels[at] == 0xA5);
    }
    prImageDecoderDestroy(decoder);
    CHECK(fake.closes == 1);
}

TEST_CASE("partial image open retains a usable decoder") {
    Reader reader;
    FakeCodec fake;
    fake.openResult = PrStatus_Truncated;
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Truncated);
    prImageDecoderDestroy(decoder);
    CHECK(fake.closes == 1);
}

TEST_CASE("rendering image convenience rejects independent alpha") {
    Reader reader;
    FakeCodec fake;
    fake.info.format.alpha = PrImageAlpha_Independent;
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
    uint8_t output[16] = {};
    CHECK(prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
        PrPixels{output, 8, {2, 2}}) == PrStatus_NotSupported);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("rendering image convenience rejects overflowing RGBA size before allocation") {
    if (sizeof(size_t) < sizeof(uint64_t)) {
        return;
    }
    Reader reader;
    FakeCodec fake;
    fake.info.extent.x = uint32_t(1) << 31;
    fake.info.extent.y = uint32_t(1) << 31;
    fake.info.format.model = PrColorModel_Gray;
    fake.info.format.alpha = PrImageAlpha_None;
    PrImageCodec* codec = fake.codec();
    PrImageLimits limits = prImageLimitsDefault();
    limits.pixels = UINT64_MAX;
    limits.workingBytes = UINT64_MAX;
    limits.outputBytes = UINT64_MAX;
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), &limits) == PrStatus_Ok);
    uint8_t dummy = 0xA5;
    const PrPixels huge{&dummy, size_t(4) << 31, {uint32_t(1) << 31, uint32_t(1) << 31}};
    CHECK(prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity, huge) ==
        PrStatus_Limit);
    CHECK(fake.decodes == 0);
    CHECK(dummy == 0xA5);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("orienting arranges pixels of any size as the orientation displays them") {
    // A 3x1 row of 2-byte pixels, turned a quarter turn, becomes a 1x3 column.
    const uint8_t row[] = {1, 1, 2, 2, 3, 3};
    uint8_t column[6] = {};
    prImageOrient(PrPixelView{row, sizeof(row), {3, 1}}, 2, PrImageOrientation_Rotate90, PrPixels{column, 2, {1, 3}});
    const uint8_t turned[] = {1, 1, 2, 2, 3, 3};
    CHECK(prMemCompare(column, turned, sizeof(column)) == 0);
    uint8_t flipped[6] = {};
    prImageOrient(PrPixelView{row, sizeof(row), {3, 1}}, 2, PrImageOrientation_FlipHorizontal,
        PrPixels{flipped, sizeof(flipped), {3, 1}});
    const uint8_t mirrored[] = {3, 3, 2, 2, 1, 1};
    CHECK(prMemCompare(flipped, mirrored, sizeof(flipped)) == 0);
}

TEST_CASE("an orientation's inverse arranges oriented pixels as they were stored") {
    // A 3x2 image of 1-byte pixels, numbered in stored order.
    const uint8_t stored[] = {1, 2, 3, 4, 5, 6};
    for (PrImageOrientation orientation =
            PrImageOrientation_Identity; orientation <= PrImageOrientation_Rotate270; ++orientation) {
        PrUVec2 extent = prImageOrientationSwapsAxes(orientation) ? PrUVec2{2, 3} : PrUVec2{3, 2};
        uint8_t oriented[6] = {};
        prImageOrient(PrPixelView{stored, 3, {3, 2}}, 1, orientation, PrPixels{oriented, extent.x, extent});
        uint8_t restored[6] = {};
        prImageOrient(PrPixelView{oriented, extent.x, extent}, 1, prImageOrientationInverse(orientation),
            PrPixels{restored, 3, {3, 2}});
        CHECK(prMemCompare(restored, stored, sizeof(stored)) == 0);
    }
}

TEST_CASE("rendering image convenience applies stored orientation to rectangular images") {
    Reader reader;
    FakeCodec fake;
    fake.info.extent.x = 2;
    fake.info.extent.y = 3;
    fake.info.orientation = PrImageOrientation_Rotate90;
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
    uint8_t output[24] = {};
    REQUIRE(prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Rotate90,
        PrPixels{output, 12, {3, 2}}) == PrStatus_Ok);
    const uint8_t expected[] = {14, 12, 10, 15, 13, 11};
    for (size_t pixel = 0; pixel < 6; ++pixel) {
        CHECK(output[pixel * 4] == expected[pixel]);
        CHECK(output[pixel * 4 + 3] == 255);
    }
    REQUIRE(prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
        PrPixels{output, 8, {2, 3}}) == PrStatus_Ok);
    for (size_t pixel = 0; pixel < 6; ++pixel) {
        CHECK(output[pixel * 4] == 10 + pixel);
    }
    prImageDecoderDestroy(decoder);
}

TEST_CASE("rendering image convenience rejects ICC transforms despite fallback policy") {
    Reader reader;
    FakeCodec fake;
    uint8_t profile[128] = {};
    fake.info.format.color = {PrImageColorEncoding_Icc, 0, {profile, sizeof(profile)}};
    PrImageCodec* codec = fake.codec();
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
    uint8_t output[16] = {};
    CHECK(prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
        PrPixels{output, 8, {2, 2}}) == PrStatus_NotSupported);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("cover frame requests require the codec to advertise a separate cover") {
    Reader reader;
    const bool covers[] = {false, true};
    for (bool cover : covers) {
        FakeCodec fake;
        fake.info.hasCover = cover;
        PrImageCodec* codec = fake.codec();
        PrImageDecoder* decoder = prImageDecoderCreate(codec);
        REQUIRE(prImageDecoderOpen(decoder, reader.source(), nullptr) == PrStatus_Ok);
        uint8_t pixels[16] = {};
        const PrPixels output{pixels, 8, {2, 2}};
        const PrStatus expected = cover ? PrStatus_Ok : PrStatus_OutOfRange;
        PrImageFrameInfo info = {};
        CHECK(prImageDecoderFrameInfo(decoder, kPrImageFrameCover, &info) == expected);
        CHECK(prImageDecoderRead(decoder, kPrImageFrameCover, output) == expected);
        CHECK(prImageDecoderReadRgba8(decoder, kPrImageFrameCover, PrImageAlpha_Straight,
            PrImageOrientation_Identity, output) == expected);
        CHECK(prImageDecoderRead(decoder, 1, output) == PrStatus_OutOfRange);
        prImageDecoderDestroy(decoder);
    }
}

TEST_CASE("a codec that encodes nothing reports it") {
    FakeCodec fake;
    const uint8_t pixel[4] = {1, 2, 3, 4};
    size_t size = 1;
    CHECK(prImageCodecEncode(fake.codec(), "image/png", PrPixelView{pixel, 4, {1, 1}}, nullptr, 0, &size) ==
        PrStatus_NotSupported);
    CHECK(size == 0);
}
