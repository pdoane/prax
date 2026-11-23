// PSD decoding over documents written here: each color mode and depth, raw and PackBits images, PSB, transparency, and
// the image resources that the decoder reads.

#include "pr/array.h"
#include "pr/mem.h"
#include "pr/psd.h"

#include "psd/tests/psd_fixture.h"

#include "tools/harness/test.h"

extern "C" int prTestPsdCApi(void);

namespace pr_impl {

namespace {

struct Decoded {
    PrStatus status = PrStatus_Ok;
    PrPsdInfo info{};
    Array<uint8_t> pixels;
};

uint32_t sampleBytes(PrPsdSample sample) {
    return sample == PrPsdSample_U8 ? 1 : sample == PrPsdSample_U16 ? 2 : 4;
}

Decoded decodePsd(const Array<uint8_t>& file, const PrPsdLimits* limits = nullptr) {
    Decoded decoded;
    PrPsdDecoder* decoder = nullptr;
    decoded.status = prPsdDecoderCreate(PrByteView{file.data(), file.count()}, limits, file.count(), &decoder);
    if (decoder == nullptr)
        return decoded;
    decoded.info = *prPsdDecoderInfo(decoder);
    const PrUVec2 extent = decoded.info.extent;
    const uint32_t stride = extent.x * decoded.info.channels * sampleBytes(decoded.info.sample);
    decoded.pixels.resize(stride * extent.y);
    decoded.status = prPsdDecoderRead(decoder, PrPixels{decoded.pixels.data(), stride, extent});
    prPsdDecoderDestroy(decoder);
    return decoded;
}

float floatAt(const Array<uint8_t>& pixels, uint32_t index) {
    float value = 0;
    prMemCopy(&value, pixels.data() + index * 4, 4);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an RGB document's merged image decodes from its planes, raw and with PackBits") {
    // Red, green and blue planes of two by two pixels.
    Array<uint8_t> planes({1, 2, 3, 4, 10, 20, 30, 40, 100, 110, 120, 130});
    Array<uint8_t> file = psdFile(PsdLayout{}, {}, {}, planes);
    CHECK(prPsdHasSignature(PrByteView{file.data(), file.count()}));
    Decoded decoded = decodePsd(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.colors == PrPsdColors_Rgb);
    CHECK(decoded.info.channels == 3);
    CHECK(!decoded.info.alpha);
    const Array<uint8_t> expected({1, 10, 100, 2, 20, 110, 3, 30, 120, 4, 40, 130});
    CHECK(decoded.pixels == expected);

    decoded = decodePsd(psdFile(PsdLayout{.compression = 1}, {}, {}, psdPackBits(planes, 2, false)));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == expected);

    // Byte counts that are wrong, as some writers' are, do not matter: each row begins where the one before ends.
    Array<uint8_t> wrong = psdPackBits(planes, 2, false);
    for (uint32_t i = 0; i < 12; ++i)
        wrong[i] = 0;
    decoded = decodePsd(psdFile(PsdLayout{.compression = 1}, {}, {}, wrong));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == expected);

    // PSB widens the byte counts of the rows.
    decoded = decodePsd(psdFile(PsdLayout{.version = 2, .compression = 1}, {}, {}, psdPackBits(planes, 2, true)));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == expected);
}

TEST_CASE("16-bit and 32-bit documents decode to their samples") {
    Array<uint8_t> planes({0x12, 0x34, 0xff, 0xff});
    Decoded decoded = decodePsd(psdFile(PsdLayout{.channels = 1, .width = 2, .height = 1, .depth = 16, .mode = 1}, {},
        {}, planes));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.sample == PrPsdSample_U16);
    uint16_t value = 0;
    prMemCopy(&value, decoded.pixels.data(), 2);
    CHECK(value == 0x1234);

    // 1.5 as a big-endian float.
    Array<uint8_t> floats({0x3f, 0xc0, 0, 0});
    decoded = decodePsd(psdFile(PsdLayout{.channels = 1, .width = 1, .height = 1, .depth = 32, .mode = 1}, {}, {}, floats));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.sample == PrPsdSample_F32);
    CHECK(floatAt(decoded.pixels, 0) == 1.5f);
}

TEST_CASE("bitmap, indexed, CMYK and Lab documents decode in their colors") {
    // A set bit is black.
    Decoded decoded = decodePsd(psdFile(PsdLayout{.channels = 1, .width = 3, .height = 1, .depth = 1, .mode = 0}, {},
        {}, Array<uint8_t>({0xa0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({0, 255, 0}));

    Array<uint8_t> table(768);
    table[2] = 200;
    table[256 + 2] = 100;
    table[512 + 2] = 50;
    decoded = decodePsd(psdFile(PsdLayout{.channels = 1, .width = 2, .height = 1, .mode = 2}, table, {},
        Array<uint8_t>({2, 0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == Array<uint8_t>({200, 100, 50, 0, 0, 0}));
    CHECK(decodePsd(psdFile(PsdLayout{.channels = 1, .mode = 2}, {}, {}, Array<uint8_t>(4))).status ==
        PrStatus_InvalidData);

    // CMYK stores 255 for no ink.
    decoded = decodePsd(psdFile(PsdLayout{.channels = 4, .width = 1, .height = 1, .mode = 4}, {}, {},
        Array<uint8_t>({255, 0, 155, 255})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.colors == PrPsdColors_Cmyk);
    CHECK(decoded.pixels == Array<uint8_t>({0, 255, 100, 0}));

    // Lab: L of 255 is 100, and a and b of 128 are zero.
    decoded = decodePsd(psdFile(PsdLayout{.channels = 3, .width = 1, .height = 1, .mode = 9}, {}, {},
        Array<uint8_t>({255, 128, 0})));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.colors == PrPsdColors_Lab);
    CHECK(floatAt(decoded.pixels, 0) == 100.0f);
    CHECK(floatAt(decoded.pixels, 1) == 0.0f);
    CHECK(floatAt(decoded.pixels, 2) == -128.0f);
}

TEST_CASE("the first extra channel is transparency if the layer count is negative, and the color is unmatted") {
    // Gray of 191 over white at half alpha is gray of 127 at half alpha, about.
    Array<uint8_t> planes({191, 128});
    Decoded decoded = decodePsd(psdFile(PsdLayout{.channels = 2, .width = 1, .height = 1, .mode = 1, .layerCount = -1},
        {}, {}, planes));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.alpha);
    REQUIRE(decoded.pixels.count() == 2);
    CHECK(decoded.pixels[0] >= 126);
    CHECK(decoded.pixels[0] <= 128);
    CHECK(decoded.pixels[1] == 128);

    // Without the negative count, the extra channel is not alpha.
    decoded = decodePsd(psdFile(PsdLayout{.channels = 2, .width = 1, .height = 1, .mode = 1, .layerCount = 1}, {}, {}, planes));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(!decoded.info.alpha);
    CHECK(decoded.pixels == Array<uint8_t>({191}));
}

TEST_CASE("the image resources give the profile, Exif, thumbnail and resolution, and a document without a merged image is not supported") {
    Array<uint8_t> resources = psdResource(0x040f, Array<uint8_t>({'i', 'c', 'c'}));
    resources.append(psdResource(0x0422, Array<uint8_t>({'M', 'M', 0, 42})).span());
    Array<uint8_t> thumbnail(28);
    thumbnail[3] = 1;
    thumbnail.append(Array<uint8_t>({0xff, 0xd8, 0xff, 0xd9}).span());
    resources.append(psdResource(0x040c, thumbnail).span());
    // 72 pixels an inch on each axis.
    Array<uint8_t> resolution;
    for (uint32_t axis = 0; axis < 2; ++axis) {
        psdPut32(&resolution, 72u << 16);
        psdPut16(&resolution, 1);
        psdPut16(&resolution, 1);
    }
    resources.append(psdResource(0x03ed, resolution).span());
    Decoded decoded = decodePsd(psdFile(PsdLayout{}, {}, resources, Array<uint8_t>(12)));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.profile.size == 3);
    CHECK(decoded.info.exif.size == 4);
    CHECK(decoded.info.thumbnail.size == 4);
    CHECK(decoded.info.pixelsPerMeter.x == doctest::Approx(72 / 0.0254));

    // Version information whose second field is zero: no real merged image.
    Array<uint8_t> version({0, 0, 0, 1, 0});
    CHECK(decodePsd(psdFile(PsdLayout{}, {}, psdResource(0x0421, version), Array<uint8_t>(12))).status ==
        PrStatus_NotSupported);
    version[4] = 1;
    CHECK(decodePsd(psdFile(PsdLayout{}, {}, psdResource(0x0421, version), Array<uint8_t>(12))).status == PrStatus_Ok);
}

TEST_CASE("a document whose image ends early is truncated, and one that breaks the format does not open") {
    Decoded decoded = decodePsd(psdFile(PsdLayout{}, {}, {}, Array<uint8_t>({1, 2, 3, 4, 5, 6})));
    CHECK(decoded.status == PrStatus_Truncated);
    CHECK(decoded.pixels[0] == 1);
    CHECK(decoded.pixels[2] == 0);

    CHECK(decodePsd(psdFile(PsdLayout{.compression = 2}, {}, {}, {})).status == PrStatus_NotSupported);
    CHECK(decodePsd(psdFile(PsdLayout{.compression = 9}, {}, {}, {})).status == PrStatus_InvalidData);
    CHECK(decodePsd(psdFile(PsdLayout{.mode = 7}, {}, {}, {})).status == PrStatus_NotSupported);
    CHECK(decodePsd(psdFile(PsdLayout{.depth = 12}, {}, {}, {})).status == PrStatus_InvalidData);
    CHECK(decodePsd(psdFile(PsdLayout{.channels = 2}, {}, {}, {})).status == PrStatus_InvalidData);
    CHECK(decodePsd(psdFile(PsdLayout{.width = 0}, {}, {}, {})).status == PrStatus_InvalidData);
    Array<uint8_t> file = psdFile(PsdLayout{}, {}, {}, {});
    file[4] = 9;
    CHECK(decodePsd(file).status == PrStatus_InvalidData);
    // A PackBits row that runs past its row.
    Array<uint8_t> packed({0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0xfd, 7});
    CHECK(decodePsd(psdFile(PsdLayout{.compression = 1}, {}, {}, packed)).status == PrStatus_InvalidData);
}

TEST_CASE("a document past the limits does not open") {
    Array<uint8_t> file = psdFile(PsdLayout{.width = 100, .height = 100}, {}, {}, {});
    PrPsdLimits limits = prPsdLimitsDefault();
    limits.pixels = 9999;
    CHECK(decodePsd(file, &limits).status == PrStatus_Limit);
    limits = prPsdLimitsDefault();
    limits.outputBytes = 29999;
    CHECK(decodePsd(file, &limits).status == PrStatus_Limit);
    limits = prPsdLimitsDefault();
    limits.work = 29999;
    CHECK(decodePsd(file, &limits).status == PrStatus_Limit);
    limits = prPsdLimitsDefault();
    limits.workingBytes = file.count();
    CHECK(decodePsd(file, &limits).status == PrStatus_Limit);
}

TEST_CASE("the psd header compiles as C") {
    CHECK(prTestPsdCApi() == 0);
}
