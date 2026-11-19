// OpenEXR decoding over small files written here, attribute by attribute: how channels map to the pixels that a part
// decodes to, the display and data windows, the channels and attributes, and the validation of headers, offset tables
// and chunks. tools/suite/exr compares the compressions with OpenEXR's own decoder.

#include "pr/array.h"
#include "pr/color_space.h"
#include "pr/defer.h"
#include "pr/exr.h"
#include "pr/float16.h"
#include "pr/mem.h"

#include "exr/tests/exr_fixture.h"

#include "tools/harness/test.h"

extern "C" int prTestExrCApi(void);

namespace pr_impl {

namespace {

// The bytes as an array of their own.
Array<uint8_t> copyBytes(PrByteView bytes) {
    Array<uint8_t> out;
    out.append(bytes);
    return out;
}

// An open image, which the test closes with closeImage.
struct Opened {
    PrExrImage* PR_NULLABLE image = nullptr;
    PrStatus status = PrStatus_Ok;

    const PrExrPartInfo& info() const { return *prExrImagePartInfo(image); }
};

// Opens the file, which must outlive the image.
Opened openImage(const Array<uint8_t>& file) {
    Opened opened;
    opened.status =
        prExrImageCreate(PrByteView{file.data(), file.count()}, nullptr, nullptr, file.count(), &opened.image);
    return opened;
}

void closeImage(Opened& opened) {
    prExrImageDestroy(opened.image);
    opened.image = nullptr;
}

// Reads the open image's first level in its own format, as floats.
Array<float> readFloats(Opened& opened, PrStatus* status) {
    const PrExrPartInfo& info = opened.info();
    uint32_t samples = info.colors + (info.alpha ? 1 : 0);
    uint32_t size = samples * (info.sample == PrExrSample_Half ? 2 : 4);
    Array<uint8_t> stored(uint32_t(size_t(info.extent.x) * info.extent.y * size));
    *status = prExrImageRead(opened.image, 0, PrPixels{stored.data(), size_t(info.extent.x) * size, info.extent});
    Array<float> values;
    for (uint32_t i = 0; i < info.extent.x * info.extent.y * samples; ++i) {
        if (info.sample == PrExrSample_Half) {
            PrFloat16 h{};
            prMemCopy(&h, stored.data() + i * 2, 2);
            values.push(prFloat16ToFloat(h));
        } else {
            float f = 0;
            prMemCopy(&f, stored.data() + i * 4, 4);
            values.push(f);
        }
    }
    return values;
}

// The half nearest the value of channel `c` at (x, y).
float halfValue(uint32_t c, int32_t x, int32_t y) {
    return prFloat16ToFloat(prFloat16FromFloat(sampleValue(c, x, y)));
}

constexpr TestChannel kRgba[] = {{"A", 1}, {"B", 1}, {"G", 1}, {"R", 1}};

} // namespace

} // namespace pr_impl

using namespace pr;
using namespace pr_impl;

TEST_CASE("exr RGBA halves decode as halves with alpha, in the primaries of BT.709") {
    TestImage image{.channels = kRgba, .channelCount = 4, .data = {0, 0, 4, 2}, .display = {0, 0, 4, 2}};
    Array<uint8_t> file1 = writeImage(image);
    Opened opened = openImage(file1);
    Defer closeOpened([&] { closeImage(opened); });
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(prExrHasSignature(PrByteView{file1.data(), file1.count()}));
    const PrExrPartInfo& info = opened.info();
    CHECK(info.extent.x == 5);
    CHECK(info.extent.y == 3);
    CHECK(info.colors == 3);
    CHECK(info.sample == PrExrSample_Half);
    CHECK(info.alpha);
    CHECK(prColorPrimariesFromChromaticities(&info.chromaticities, 1e-4f) == PrColorPrimaries_Rec709);
    CHECK(info.channelCount == 4);
    CHECK(info.levels == PrExrLevels_One);
    CHECK(info.levelCount == 1);
    CHECK(prExrImagePartCount(opened.image) == 1);
    PrStatus status = PrStatus_Ok;
    Array<float> values = readFloats(opened, &status);
    CHECK(status == PrStatus_Ok);
    // R, G, B and A are channels 3, 2, 1 and 0 of the list.
    CHECK(values[(1 * 5 + 2) * 4 + 0] == halfValue(3, 2, 1));
    CHECK(values[(1 * 5 + 2) * 4 + 1] == halfValue(2, 2, 1));
    CHECK(values[(1 * 5 + 2) * 4 + 2] == halfValue(1, 2, 1));
    CHECK(values[(1 * 5 + 2) * 4 + 3] == halfValue(0, 2, 1));
}

TEST_CASE("exr data window decodes into the display window's frame") {
    // The data window lies partly outside the display window, which is larger elsewhere.
    TestImage image{.channels = kRgba + 3, .channelCount = 1, .data = {-2, 1, 3, 2}, .display = {0, 0, 5, 3}};
    Array<uint8_t> file2 = writeImage(image);
    Opened opened = openImage(file2);
    Defer closeOpened([&] { closeImage(opened); });
    REQUIRE(opened.status == PrStatus_Ok);
    const PrExrPartInfo& info = opened.info();
    CHECK(info.extent.x == 6);
    CHECK(info.extent.y == 4);
    CHECK(info.colors == 3);
    PrStatus status = PrStatus_Ok;
    Array<float> values = readFloats(opened, &status);
    CHECK(status == PrStatus_Ok);
    for (int32_t y = 0; y < 4; ++y) {
        for (int32_t x = 0; x < 6; ++x) {
            bool inside = x <= 3 && y >= 1 && y <= 2;
            float r = values[uint32_t((y * 6 + x) * 3)];
            CHECK(r == (inside ? halfValue(0, x, y) : 0.0f));
            // Green and blue are missing, and zero.
            CHECK(values[uint32_t((y * 6 + x) * 3 + 1)] == 0.0f);
        }
    }
    PrExrHeader header{};
    REQUIRE(prExrImageHeaderRead(opened.image, &header) == PrStatus_Ok);
    CHECK(header.dataWindow.xMin == -2);
    CHECK(header.dataWindow.yMax == 2);
    CHECK(header.displayWindow.xMax == 5);
    CHECK(header.pixelAspectRatio == 1.0f);
    CHECK(header.compression == PrExrCompression_None);
}

TEST_CASE("exr floats and integers decode as floats by value, and every channel reads by name") {
    constexpr TestChannel channels[] = {{"G", 1}, {"R", 2}, {"id", 0}};
    TestImage image{.channels = channels, .channelCount = 3, .data = {0, 0, 2, 1}, .display = {0, 0, 2, 1}};
    Array<uint8_t> file3 = writeImage(image);
    Opened opened = openImage(file3);
    Defer closeOpened([&] { closeImage(opened); });
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info().sample == PrExrSample_Float);
    CHECK(opened.info().channelCount == 3);
    PrExrChannel channel = prExrImageChannel(opened.image, 2);
    CHECK(channel.name == "id");
    CHECK(channel.sample == PrExrSample_Uint);
    PrStatus status = PrStatus_Ok;
    Array<float> values = readFloats(opened, &status);
    CHECK(values[(1 * 3 + 2) * 3 + 0] == sampleValue(1, 2, 1));
    CHECK(values[(1 * 3 + 2) * 3 + 1] == halfValue(0, 2, 1));

    uint32_t ids[3 * 2] = {};
    const uint32_t which = 2;
    REQUIRE(prExrImageReadChannels(opened.image, 0, &which, 1, PrExrSample_Uint,
        PrPixels{reinterpret_cast<uint8_t*>(ids), 3 * 4, PrUVec2{3, 2}}) == PrStatus_Ok);
    CHECK(ids[1 * 3 + 2] == uint32_t(sampleValue(2, 2, 1) * 16));
    const uint32_t half = 0;
    CHECK(prExrImageReadChannels(opened.image, 0, &half, 1, PrExrSample_Uint,
        PrPixels{reinterpret_cast<uint8_t*>(ids), 3 * 4, PrUVec2{3, 2}}) == PrStatus_NotSupported);
    float floats[3 * 2 * 2] = {};
    const uint32_t both[2] = {2, 0};
    REQUIRE(prExrImageReadChannels(opened.image, 0, both, 2, PrExrSample_Float,
        PrPixels{reinterpret_cast<uint8_t*>(floats), 3 * 8, PrUVec2{3, 2}}) == PrStatus_Ok);
    CHECK(floats[(1 * 3 + 1) * 2] == float(uint32_t(sampleValue(2, 1, 1) * 16)));
    CHECK(floats[(1 * 3 + 1) * 2 + 1] == halfValue(0, 1, 1));
}

TEST_CASE("exr luminance decodes as gray, and with chroma as RGB") {
    constexpr TestChannel luminance[] = {{"Y", 1}};
    TestImage gray{.channels = luminance, .channelCount = 1, .data = {0, 0, 3, 1}, .display = {0, 0, 3, 1}};
    Array<uint8_t> file4 = writeImage(gray);
    Opened grayOpened = openImage(file4);
    Defer closeGrayOpened([&] { closeImage(grayOpened); });
    REQUIRE(grayOpened.status == PrStatus_Ok);
    CHECK(grayOpened.info().colors == 1);
    CHECK(!grayOpened.info().alpha);

    // Chroma of zero is a gray of the luminance, whatever the primaries.
    constexpr TestChannel chroma[] = {{"BY", 1, 2}, {"RY", 1, 2}, {"Y", 1}};
    TestImage image{.channels = chroma, .channelCount = 3, .data = {0, 0, 3, 1}, .display = {0, 0, 3, 1}};
    Array<uint8_t> file = writeImage(image);
    Opened opened = openImage(file);
    Defer closeOpened([&] { closeImage(opened); });
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info().colors == 3);
    PrStatus status = PrStatus_Ok;
    Array<float> values = readFloats(opened, &status);
    CHECK(status == PrStatus_Ok);
    // At (1, 1), the chroma sample of (0, 0) covers the pixel: R = (RY + 1) Y and B = (BY + 1) Y.
    float y = halfValue(2, 1, 1);
    float ry = halfValue(1, 0, 0);
    float by = halfValue(0, 0, 0);
    CHECK(values[(1 * 4 + 1) * 3 + 0] == doctest::Approx((ry + 1) * y).epsilon(0.002));
    CHECK(values[(1 * 4 + 1) * 3 + 2] == doctest::Approx((by + 1) * y).epsilon(0.002));
}

TEST_CASE("exr color comes from the first layer that has it, or else the first channel") {
    constexpr TestChannel layered[] = {{"depth.Z", 2}, {"diffuse.B", 1}, {"diffuse.G", 1}, {"diffuse.R", 1}};
    TestImage image{.channels = layered, .channelCount = 4, .data = {0, 0, 1, 1}, .display = {0, 0, 1, 1}};
    Array<uint8_t> file6 = writeImage(image);
    Opened opened = openImage(file6);
    Defer closeOpened([&] { closeImage(opened); });
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(opened.info().colors == 3);
    CHECK(opened.info().sample == PrExrSample_Half);
    PrStatus status = PrStatus_Ok;
    Array<float> values = readFloats(opened, &status);
    CHECK(values[0] == halfValue(3, 0, 0));

    constexpr TestChannel depth[] = {{"Z", 2}};
    TestImage alone{.channels = depth, .channelCount = 1, .data = {0, 0, 1, 1}, .display = {0, 0, 1, 1}};
    Array<uint8_t> file7 = writeImage(alone);
    Opened depthOpened = openImage(file7);
    Defer closeDepthOpened([&] { closeImage(depthOpened); });
    REQUIRE(depthOpened.status == PrStatus_Ok);
    CHECK(depthOpened.info().colors == 1);
    CHECK(depthOpened.info().sample == PrExrSample_Float);
}

TEST_CASE("exr chromaticities are the file's") {
    TestImage image{.channels = kRgba + 3, .channelCount = 1, .data = {0, 0, 1, 0}, .display = {0, 0, 1, 0}};
    image.chromaticities = true;
    // ACEScg's primaries and white.
    const float ap1[8] = {0.713f, 0.293f, 0.165f, 0.830f, 0.128f, 0.044f, 0.32168f, 0.33767f};
    prMemCopy(image.primaries, ap1, sizeof(ap1));
    Array<uint8_t> file8 = writeImage(image);
    Opened opened = openImage(file8);
    Defer closeOpened([&] { closeImage(opened); });
    REQUIRE(opened.status == PrStatus_Ok);
    const PrColorChromaticities& chromaticities = opened.info().chromaticities;
    CHECK(chromaticities.green.y == 0.830f);
    CHECK(prColorPrimariesFromChromaticities(&chromaticities, 1e-4f) == PrColorPrimaries_AcesAp1);

    const float rec2020[8] = {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
    prMemCopy(image.primaries, rec2020, sizeof(rec2020));
    Array<uint8_t> file9 = writeImage(image);
    Opened named = openImage(file9);
    Defer closeNamed([&] { closeImage(named); });
    REQUIRE(named.status == PrStatus_Ok);
    CHECK(prColorPrimariesFromChromaticities(&named.info().chromaticities, 1e-4f) == PrColorPrimaries_Rec2020);
}

TEST_CASE("exr attributes read as the file stores them") {
    TestImage image{.channels = kRgba, .channelCount = 4, .data = {0, 0, 1, 1}, .display = {0, 0, 1, 1}};
    Array<uint8_t> file10 = writeImage(image);
    Opened opened = openImage(file10);
    Defer closeOpened([&] { closeImage(opened); });
    REQUIRE(opened.status == PrStatus_Ok);
    bool found = false;
    for (uint32_t i = 0; i < opened.info().attributeCount; ++i) {
        PrExrAttribute attribute = prExrImageAttribute(opened.image, i);
        if (attribute.name == "screenWindowWidth") {
            found = true;
            CHECK(attribute.type == "float");
            CHECK(attribute.value.size == 4);
        }
    }
    CHECK(found);
}

TEST_CASE("exr chunks that a file does not hold decode to zero, and the read is Truncated") {
    TestImage image{.channels = kRgba + 3, .channelCount = 1, .data = {0, 0, 2, 3}, .display = {0, 0, 2, 3}};
    Array<uint8_t> file11 = writeImage(image, 2);
    Opened opened = openImage(file11);
    Defer closeOpened([&] { closeImage(opened); });
    CHECK(opened.status == PrStatus_Truncated);
    REQUIRE(opened.image != nullptr);
    PrStatus status = PrStatus_Ok;
    Array<float> values = readFloats(opened, &status);
    CHECK(status == PrStatus_Truncated);
    CHECK(values[(1 * 3 + 1) * 3] == halfValue(0, 1, 1));
    CHECK(values[(3 * 3 + 1) * 3] == 0.0f);
}

TEST_CASE("exr offsets that a file did not write are found from its chunks") {
    TestImage image{.channels = kRgba + 3, .channelCount = 1, .data = {0, 0, 2, 3}, .display = {0, 0, 2, 3}};
    size_t table = 0;
    Array<uint8_t> file = writeImage(image, 0, &table);
    // The offset table is zeros, as a file whose writing stopped before it wrote the table.
    prMemZero(file.data() + table, 4 * 8);
    Opened opened = openImage(file);
    Defer closeOpened([&] { closeImage(opened); });
    CHECK(opened.status == PrStatus_Ok);
    PrStatus status = PrStatus_Ok;
    Array<float> values = readFloats(opened, &status);
    CHECK(status == PrStatus_Ok);
    CHECK(values[(3 * 3 + 2) * 3] == halfValue(0, 2, 3));

    // An offset of another chunk's header is corrupt.
    Array<uint8_t> swapped = writeImage(image);
    uint64_t first = prMemReadLeU64(swapped.data() + table);
    prMemWriteLeU64(swapped.data() + table + 8, first);
    Opened corrupt = openImage(swapped);
    Defer closeCorrupt([&] { closeImage(corrupt); });
    REQUIRE(corrupt.status == PrStatus_Ok);
    readFloats(corrupt, &status);
    CHECK(status == PrStatus_InvalidData);
}

TEST_CASE("exr headers that break the file format are refused") {
    TestImage image{.channels = kRgba, .channelCount = 4, .data = {0, 0, 1, 1}, .display = {0, 0, 1, 1}};
    Array<uint8_t> good = writeImage(image);
    {
        // A version other than 2.
        Array<uint8_t> file = copyBytes(PrByteView{good.data(), good.count()});
        file[4] = 1;
        Opened opened = openImage(file);
        Defer closeOpened([&] { closeImage(opened); });
        CHECK(opened.status == PrStatus_NotSupported);
    }
    {
        // Deep data.
        Array<uint8_t> file = copyBytes(PrByteView{good.data(), good.count()});
        file[5] = 0x08;
        Opened opened = openImage(file);
        Defer closeOpened([&] { closeImage(opened); });
        CHECK(opened.status == PrStatus_NotSupported);
    }
    {
        // Channels out of the order of their names.
        constexpr TestChannel unsorted[] = {{"R", 1}, {"G", 1}};
        TestImage bad{.channels = unsorted, .channelCount = 2, .data = {0, 0, 1, 1}, .display = {0, 0, 1, 1}};
        Array<uint8_t> file16 = writeImage(bad);
        Opened opened = openImage(file16);
        Defer closeOpened([&] { closeImage(opened); });
        CHECK(opened.status == PrStatus_InvalidData);
    }
    {
        // A subsampled channel whose sampling does not divide the data window.
        constexpr TestChannel sampled[] = {{"R", 1, 2}};
        TestImage bad{.channels = sampled, .channelCount = 1, .data = {0, 0, 2, 1}, .display = {0, 0, 2, 1}};
        Array<uint8_t> file17 = writeImage(bad);
        Opened opened = openImage(file17);
        Defer closeOpened([&] { closeImage(opened); });
        CHECK(opened.status == PrStatus_InvalidData);
    }
    {
        // A file that ends within its header.
        Array<uint8_t> file = copyBytes(PrByteView{good.data(), 40});
        Opened opened = openImage(file);
        Defer closeOpened([&] { closeImage(opened); });
        CHECK(opened.status == PrStatus_InvalidData);
    }
    {
        // A compression that the decoder does not read.
        Array<uint8_t> file = copyBytes(PrByteView{good.data(), good.count()});
        for (size_t i = 0; i + 12 < file.count(); ++i) {
            if (prMemEquals(file.data() + i, "compression", 12)) {
                file[uint32_t(i + 12 + 12 + 4)] = 10;
                break;
            }
        }
        Opened opened = openImage(file);
        Defer closeOpened([&] { closeImage(opened); });
        CHECK(opened.status == PrStatus_NotSupported);
    }
}

TEST_CASE("OpenEXR's public header compiles as C") {
    CHECK(prTestExrCApi() == 0);
}
