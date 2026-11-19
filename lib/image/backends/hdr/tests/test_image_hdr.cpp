// The built-in codec's Radiance pictures: what lib/hdr's adapter reports of them, and how they read through a decoder.

#include "pr/float.h"
#include "pr/mem.h"

#include "tools/harness/image_test.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// A pixel as a Radiance picture stores it: three mantissas and their shared exponent.
struct Rgbe {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t e;
};

// Returns a file of `header`, an empty line, the resolution line `resolution`, and then `scanlines`.
Array<uint8_t> writeHdr(PrStringView header, PrStringView resolution, const Array<uint8_t>& scanlines) {
    Array<uint8_t> out;
    out.append(PrByteView{reinterpret_cast<const uint8_t*>(header.data), header.size});
    out.push('\n');
    out.append(PrByteView{reinterpret_cast<const uint8_t*>(resolution.data), resolution.size});
    out.push('\n');
    out.append(scanlines.span());
    return out;
}

// The header that most of the test files have.
constexpr PrStringView kHeader = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n";

// Appends pixels as four bytes each, which is a flat scanline.
void appendFlat(Array<uint8_t>* out, const Array<Rgbe>& pixels) {
    for (const Rgbe& pixel : pixels) {
        const uint8_t bytes[4] = {pixel.r, pixel.g, pixel.b, pixel.e};
        out->append(PrByteView{bytes, 4});
    }
}

// The pixels of a decoded picture: its floats, three a pixel.
struct Decoded {
    PrStatus status = PrStatus_Ok;
    PrImageInfo info{};
    Array<float> pixels;
};

// Opens `file` and reads its pixels in its own format. The status is the open's if the picture does not open, and the
// read's otherwise.
Decoded decodeHdr(const Array<uint8_t>& file, const PrImageLimits* PR_NULLABLE limits = nullptr) {
    Decoded decoded;
    PrImageSource source{};
    source.memory = PrByteView{file.data(), file.count()};
    PrImageDecoder* decoder = nullptr;
    decoded.status = testDecoderOpen(testCodec(), source, limits, &decoder);
    if (decoder == nullptr)
        return decoded;
    decoded.info = *prImageDecoderInfo(decoder);
    PrUVec2 extent = decoded.info.extent;
    decoded.pixels.resize(extent.x * extent.y * 3);
    PrPixels output{reinterpret_cast<uint8_t*>(decoded.pixels.data()), size_t(extent.x) * 3 * sizeof(float), extent};
    decoded.status = prImageDecoderRead(decoder, 0, output);
    prImageDecoderDestroy(decoder);
    return decoded;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an hdr is linear sRGB floats without alpha, of one frame") {
    Array<Rgbe> pixels({{128, 128, 128, 128}, {255, 0, 64, 129}, {0, 0, 0, 0}});
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, pixels);
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/vnd.radiance"));
    Decoded decoded = decodeHdr(writeHdr(kHeader, "-Y 1 +X 3", scanlines));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.extent == PrUVec2{3, 1});
    CHECK(decoded.info.format.model == PrColorModel_Rgb);
    CHECK(decoded.info.format.sample == PrImageSample_F32);
    CHECK(decoded.info.format.alpha == PrImageAlpha_None);
    CHECK(decoded.info.format.color.encoding == PrImageColorEncoding_Named);
    CHECK(decoded.info.format.color.space == PrColorSpace_SrgbLinear);
    CHECK(decoded.info.orientation == PrImageOrientation_Identity);
    CHECK(decoded.info.frameCount == 1);
    CHECK(prImageFormatSize(&decoded.info.format) == 12);
    CHECK(decoded.pixels[0] == 128.5f / 256);
    // Bytes that do not begin as a picture's header are not the codec's.
    CHECK(decodeHdr(writeHdr("#!RADIANCE\n", "-Y 1 +X 3", scanlines)).status == PrStatus_NotSupported);
}

TEST_CASE("an hdr's resolution line is its orientation") {
    Array<Rgbe> pixels({{1, 0, 0, 136}, {2, 0, 0, 136}, {3, 0, 0, 136}, {4, 0, 0, 136}, {5, 0, 0, 136}, {6, 0, 0, 136}});
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, pixels);
    struct Case {
        PrStringView resolution;
        PrUVec2 extent;
        PrImageOrientation orientation;
    };
    // Radiance's y runs up, so -Y is from the top down.
    const Case cases[] = {
        {"-Y 2 +X 3", {3, 2}, PrImageOrientation_Identity},
        {"-Y 2 -X 3", {3, 2}, PrImageOrientation_FlipHorizontal},
        {"+Y 2 -X 3", {3, 2}, PrImageOrientation_Rotate180},
        {"+Y 2 +X 3", {3, 2}, PrImageOrientation_FlipVertical},
        {"+X 3 -Y 2", {2, 3}, PrImageOrientation_Transpose},
        {"-X 3 -Y 2", {2, 3}, PrImageOrientation_Rotate90},
        {"-X 3 +Y 2", {2, 3}, PrImageOrientation_Transverse},
        {"+X 3 +Y 2", {2, 3}, PrImageOrientation_Rotate270},
    };
    for (const Case& c : cases) {
        CAPTURE(c.resolution);
        Decoded decoded = decodeHdr(writeHdr(kHeader, c.resolution, scanlines));
        REQUIRE(decoded.status == PrStatus_Ok);
        CHECK(decoded.info.extent == c.extent);
        CHECK(decoded.info.orientation == c.orientation);
    }

    // The displayed picture of scanlines stored from the bottom up has the last scanline on top.
    Array<uint8_t> file = writeHdr(kHeader, "+Y 2 +X 3", scanlines);
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    uint8_t shown[2 * 3 * 4];
    REQUIRE(testReadRgba8(decoder, 0, true, PrImageAlpha_Straight, shown, sizeof(shown)) == PrStatus_Ok);
    uint8_t stored[2 * 3 * 4];
    REQUIRE(testReadRgba8(decoder, 0, false, PrImageAlpha_Straight, stored, sizeof(stored)) == PrStatus_Ok);
    CHECK(prMemEquals(shown, stored + 12, 12));
    CHECK(prMemEquals(shown + 12, stored, 12));
    prImageDecoderDestroy(decoder);
}

TEST_CASE("an hdr converts to 8-bit sRGB through the decoder: linear light is encoded, and values above 1 clip") {
    // One, 0.2159 and 16 in linear light: 255, about 128 and 255 in sRGB's encoding.
    Array<Rgbe> pixels({{127, 127, 127, 129}, {110, 110, 110, 127}, {127, 127, 127, 133}, {0, 0, 0, 0}});
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, pixels);
    Array<uint8_t> file = writeHdr(kHeader, "-Y 1 +X 4", scanlines);
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    uint8_t rgba[4 * 4];
    REQUIRE(testReadRgba8(decoder, 0, false, PrImageAlpha_Straight, rgba, sizeof(rgba)) == PrStatus_Ok);
    CHECK(rgba[0] == 255);
    CHECK(rgba[3] == 255);
    CHECK(rgba[4] >= 127);
    CHECK(rgba[4] <= 129);
    CHECK(rgba[8] == 255);
    CHECK(rgba[12] == 0);
    CHECK(rgba[15] == 255);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("an hdr larger than the limits does not open") {
    Array<Rgbe> pixels({{100, 150, 200, 130}, {20, 30, 40, 126}});
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, pixels);
    Array<uint8_t> file = writeHdr(kHeader, "-Y 1 +X 2", scanlines);
    PrImageLimits limits = prImageLimitsDefault();
    limits.pixels = 1;
    CHECK(decodeHdr(file, &limits).status == PrStatus_Limit);
    limits = prImageLimitsDefault();
    limits.outputBytes = 2 * 12 - 1;
    CHECK(decodeHdr(file, &limits).status == PrStatus_Limit);
    limits.outputBytes = 2 * 12;
    CHECK(decodeHdr(file, &limits).status == PrStatus_Ok);
    // A header may claim any size: a picture of two thousand million pixels on a side is over every default limit.
    CHECK(decodeHdr(writeHdr(kHeader, "-Y 2000000000 +X 2000000000", scanlines)).status == PrStatus_Limit);
}
