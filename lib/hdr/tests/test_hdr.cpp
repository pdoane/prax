// Radiance HDR decoding over small pictures written here, byte by byte.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/hdr.h"
#include "pr/mem.h"

#include "tools/harness/test.h"

extern "C" int prTestHdrCApi(void);

namespace pr_impl {

namespace {

// A pixel as a Radiance picture stores it: three mantissas and their shared exponent.
struct Rgbe {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t e;
};

// Returns the value that a mantissa and an exponent stand for.
float valueOf(uint8_t mantissa, uint8_t exponent) {
    if (exponent == 0)
        return 0;
    return (float(mantissa) + 0.5f) * prF32Exp2(float(exponent) - 136);
}

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
    PrHdrInfo image{};
    Array<float> pixels;
};

// Reads the header of `file` and decodes its pixels. The status is the header's if it does not read, and InvalidData if
// the pixels do not decode.
Decoded decodeHdr(const Array<uint8_t>& file) {
    Decoded decoded;
    PrHdrDecoder* decoder = nullptr;
    decoded.status = prHdrDecoderCreate(PrByteView{file.data(), file.count()}, nullptr, file.count(), &decoder);
    if (decoded.status != PrStatus_Ok)
        return decoded;
    decoded.image = *prHdrDecoderInfo(decoder);
    PrUVec2 extent = decoded.image.extent;
    decoded.pixels.resize(extent.x * extent.y * 3);
    PrPixels output{reinterpret_cast<uint8_t*>(decoded.pixels.data()), size_t(extent.x) * 3 * sizeof(float), extent};
    decoded.status = prHdrDecoderRead(decoder, output);
    prHdrDecoderDestroy(decoder);
    return decoded;
}

// The bytes of `text`.
Array<uint8_t> textBytes(PrStringView text) {
    Array<uint8_t> out;
    out.append(PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    return out;
}

// The bytes as an array of their own.
Array<uint8_t> copyBytes(PrByteView bytes) {
    Array<uint8_t> out;
    out.append(bytes);
    return out;
}

// Returns true if a decoded picture's pixels are those that `pixels` stand for, each over `exposure`.
bool holds(const Decoded& decoded, const Array<Rgbe>& pixels, float exposure = 1) {
    if (decoded.pixels.count() != pixels.count() * 3)
        return false;
    for (uint32_t i = 0; i < pixels.count(); ++i) {
        const Rgbe& p = pixels[i];
        const float expected[3] = {valueOf(p.r, p.e) / exposure, valueOf(p.g, p.e) / exposure, valueOf(p.b, p.e) / exposure};
        for (uint32_t channel = 0; channel < 3; ++channel) {
            float actual = decoded.pixels[i * 3 + channel];
            if (prF32Abs(actual - expected[channel]) > prF32Abs(expected[channel]) * 1e-6f)
                return false;
        }
    }
    return true;
}

// Sixteen pixels that differ in every component, for scanlines of eight.
Array<Rgbe> samplePixels() {
    Array<Rgbe> pixels;
    for (uint8_t i = 0; i < 16; ++i)
        pixels.push(Rgbe{uint8_t(10 + i * 3), uint8_t(200 - i * 7), uint8_t(i * i), uint8_t(120 + i)});
    return pixels;
}

// Appends one scanline of `width` pixels in the newer run-length encoding, with each component stored as literal bytes.
void appendLiteralRuns(Array<uint8_t>* out, const Rgbe* pixels, uint32_t width) {
    const uint8_t marker[4] = {2, 2, uint8_t(width >> 8), uint8_t(width)};
    out->append(PrByteView{marker, 4});
    for (uint32_t component = 0; component < 4; ++component) {
        out->push(uint8_t(width));
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t bytes[4] = {pixels[x].r, pixels[x].g, pixels[x].b, pixels[x].e};
            out->push(bytes[component]);
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a flat hdr decodes to linear floats: mantissa plus a half, times two to the exponent less 136") {
    Array<Rgbe> pixels({
        {128, 128, 128, 128}, {255, 0, 64, 129}, {0, 0, 0, 0}, {12, 200, 90, 140}, {1, 2, 3, 100},
        {9, 9, 9, 1},
    });
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, pixels);
    Array<uint8_t> file = writeHdr(kHeader, "-Y 2 +X 3", scanlines);
    CHECK(prHdrHasSignature(PrByteView{file.data(), file.count()}));
    Decoded decoded = decodeHdr(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.image.extent == PrUVec2{3, 2});
    CHECK(decoded.image.exposure == 1);
    CHECK(holds(decoded, pixels));
    // A mantissa of 128 at an exponent of 128 is one half and a bit, and an exponent of zero is black.
    CHECK(decoded.pixels[0] == 128.5f / 256);
    CHECK(decoded.pixels[3] == 255.5f / 128);
    CHECK(decoded.pixels[6] == 0);
    CHECK(decoded.pixels[7] == 0);
    CHECK(decoded.pixels[9] == 12.5f * 16);
}

TEST_CASE("an hdr's run-length encoded scanlines decode, as runs, as literal bytes and as both") {
    Array<Rgbe> pixels = samplePixels();
    // The first scanline stores each component as literal bytes.
    Array<uint8_t> scanlines;
    appendLiteralRuns(&scanlines, pixels.data(), 8);
    // The second stores red as one run of 8, green as a run of 3 and 5 literal bytes, blue as two literal bytes and a
    // run of 6, and the exponent as runs of 1 and of 7.
    for (uint32_t x = 8; x < 16; ++x) {
        pixels[x].r = 77;
        pixels[x].g = x < 11 ? 5 : pixels[x].g;
        pixels[x].b = x < 10 ? pixels[x].b : 250;
        pixels[x].e = x < 9 ? 130 : 131;
    }
    const uint8_t second[] = {
        2, 2, 0, 8,
        128 + 8, 77,
        128 + 3, 5, 5, pixels[11].g, pixels[12].g, pixels[13].g, pixels[14].g, pixels[15].g,
        2, pixels[8].b, pixels[9].b, 128 + 6, 250,
        128 + 1, 130, 128 + 7, 131,
    };
    scanlines.append(PrByteView{second, sizeof(second)});
    Decoded decoded = decodeHdr(writeHdr(kHeader, "-Y 2 +X 8", scanlines));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.image.extent == PrUVec2{8, 2});
    CHECK(holds(decoded, pixels));
}

TEST_CASE("an hdr may mix flat and run-length encoded scanlines") {
    Array<Rgbe> pixels = samplePixels();
    Array<uint8_t> scanlines;
    Array<Rgbe> first;
    for (uint32_t x = 0; x < 8; ++x)
        first.push(pixels[x]);
    appendFlat(&scanlines, first);
    appendLiteralRuns(&scanlines, pixels.data() + 8, 8);
    Decoded decoded = decodeHdr(writeHdr(kHeader, "-Y 2 +X 8", scanlines));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(holds(decoded, pixels));

    // A scanline too narrow for the newer encoding is flat, even if its first pixel looks like the marker.
    Array<Rgbe> narrow({{2, 2, 0, 4}, {5, 6, 7, 130}, {8, 9, 10, 131}, {11, 12, 13, 132}});
    Array<uint8_t> narrowScanlines;
    appendFlat(&narrowScanlines, narrow);
    Decoded small = decodeHdr(writeHdr(kHeader, "-Y 1 +X 4", narrowScanlines));
    REQUIRE(small.status == PrStatus_Ok);
    CHECK(holds(small, narrow));
}

TEST_CASE("an hdr's older run-length encoding repeats the pixel before a pixel of mantissas one") {
    // A pixel, three repeats of it, another pixel, and 2 + 256 repeats of that: two repeat pixels in a row count in
    // steps of 256.
    Array<Rgbe> stored({{40, 50, 60, 128}, {1, 1, 1, 3}, {7, 8, 9, 127}, {1, 1, 1, 2}, {1, 1, 1, 1}});
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, stored);
    Array<Rgbe> expected;
    for (uint32_t i = 0; i < 4; ++i)
        expected.push(Rgbe{40, 50, 60, 128});
    for (uint32_t i = 0; i < 1 + 2 + 256; ++i)
        expected.push(Rgbe{7, 8, 9, 127});
    Decoded decoded = decodeHdr(writeHdr(kHeader, "-Y 1 +X 263", scanlines));
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(holds(decoded, expected));

    // A repeat with no pixel before it in its scanline, or one past the scanline's end, is malformed.
    Array<Rgbe> orphan({{1, 1, 1, 2}, {7, 8, 9, 127}, {7, 8, 9, 127}});
    Array<uint8_t> orphanScanlines;
    appendFlat(&orphanScanlines, orphan);
    CHECK(decodeHdr(writeHdr(kHeader, "-Y 1 +X 3", orphanScanlines)).status == PrStatus_InvalidData);
    Array<Rgbe> long_({{7, 8, 9, 127}, {1, 1, 1, 3}});
    Array<uint8_t> longScanlines;
    appendFlat(&longScanlines, long_);
    CHECK(decodeHdr(writeHdr(kHeader, "-Y 1 +X 3", longScanlines)).status == PrStatus_InvalidData);
}

TEST_CASE("an hdr's resolution line gives its extent and the axes of its scanlines and of their pixels") {
    Array<Rgbe> pixels({{1, 0, 0, 136}, {2, 0, 0, 136}, {3, 0, 0, 136}, {4, 0, 0, 136}, {5, 0, 0, 136}, {6, 0, 0, 136}});
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, pixels);
    struct Case {
        PrStringView resolution;
        PrUVec2 extent;
        bool scanlinesY;
        bool scanlinesDecreasing;
        bool pixelsDecreasing;
    };
    // The first axis counts the scanlines and the second the pixels of one.
    const Case cases[] = {
        {"-Y 2 +X 3", {3, 2}, true, true, false},
        {"-Y 2 -X 3", {3, 2}, true, true, true},
        {"+Y 2 -X 3", {3, 2}, true, false, true},
        {"+Y 2 +X 3", {3, 2}, true, false, false},
        {"+X 3 -Y 2", {2, 3}, false, false, true},
        {"-X 3 -Y 2", {2, 3}, false, true, true},
        {"-X 3 +Y 2", {2, 3}, false, true, false},
        {"+X 3 +Y 2", {2, 3}, false, false, false},
    };
    for (const Case& c : cases) {
        CAPTURE(c.resolution);
        Decoded decoded = decodeHdr(writeHdr(kHeader, c.resolution, scanlines));
        REQUIRE(decoded.status == PrStatus_Ok);
        CHECK(decoded.image.extent == c.extent);
        CHECK(decoded.image.scanlineAxis.y == c.scanlinesY);
        CHECK(decoded.image.scanlineAxis.decreasing == c.scanlinesDecreasing);
        CHECK(decoded.image.scanlineAxis.length == c.extent.y);
        CHECK(decoded.image.pixelAxis.y == !c.scanlinesY);
        CHECK(decoded.image.pixelAxis.decreasing == c.pixelsDecreasing);
        CHECK(decoded.image.pixelAxis.length == c.extent.x);
        // The pixels are read in their stored order, whatever the axes.
        CHECK(holds(decoded, pixels));
    }
}

TEST_CASE("an hdr's header: either signature, comments, unknown variables, and exposures that are divided out") {
    Array<Rgbe> pixels({{100, 150, 200, 130}, {20, 30, 40, 126}});
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, pixels);

    Decoded plain = decodeHdr(writeHdr("#?RGBE\n", "-Y 1 +X 2", scanlines));
    REQUIRE(plain.status == PrStatus_Ok);
    CHECK(holds(plain, pixels));

    PrStringView busy = "#?RADIANCE\n# made by a test\nSOFTWARE=none\nFORMAT=32-bit_rle_rgbe\r\nGAMMA=1\nEXPOSURE=2\n"
        "VIEW= -vtv -vp 0 0 0\nEXPOSURE= 0.25\nEXPOSURE=nonsense\nEXPOSURE=0\n";
    Decoded exposed = decodeHdr(writeHdr(busy, "-Y 1 +X 2", scanlines));
    REQUIRE(exposed.status == PrStatus_Ok);
    // The exposures multiply to one half, and the two that say nothing are skipped.
    CHECK(holds(exposed, pixels, 0.5f));
    CHECK_FALSE(holds(exposed, pixels));

    // A picture in CIE XYZ is not one that the codec converts.
    PrStringView xyz = "#?RADIANCE\nFORMAT=32-bit_rle_xyze\n";
    CHECK(decodeHdr(writeHdr(xyz, "-Y 1 +X 2", scanlines)).status == PrStatus_NotSupported);
    PrStringView other = "#?RADIANCE\nFORMAT=8-bit_palette\n";
    CHECK(decodeHdr(writeHdr(other, "-Y 1 +X 2", scanlines)).status == PrStatus_InvalidData);
}

TEST_CASE("a malformed hdr header does not open") {
    Array<Rgbe> pixels({{100, 150, 200, 130}, {20, 30, 40, 126}});
    Array<uint8_t> scanlines;
    appendFlat(&scanlines, pixels);
    const PrStringView resolutions[] = {
        "", "-Y 1", "-Y 1 +X", "Y 1 X 2", "-Y 1 +Y 2", "-Y 0 +X 2", "-Y 1 +X 0", "-Y 1 +X 2 extra", "-Z 1 +X 2",
        "-Y 1 +X 99999999999", "-Y -1 +X 2", "-Y one +X 2",
    };
    for (PrStringView resolution : resolutions) {
        CAPTURE(resolution);
        CHECK(decodeHdr(writeHdr(kHeader, resolution, scanlines)).status == PrStatus_InvalidData);
    }
    // A header that never ends, one without a resolution line, and one whose first line is not a signature.
    Array<uint8_t> endless = textBytes("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n");
    CHECK(decodeHdr(endless).status == PrStatus_InvalidData);
    Array<uint8_t> headerOnly = textBytes("#?RADIANCE\n\n");
    CHECK(decodeHdr(headerOnly).status == PrStatus_InvalidData);
    Array<uint8_t> unterminated = textBytes("#?RADIANCE\n\n-Y 1 +X 2");
    CHECK(decodeHdr(unterminated).status == PrStatus_InvalidData);
    CHECK(decodeHdr(writeHdr("#?RADIANCEX\n", "-Y 1 +X 2", scanlines)).status == PrStatus_InvalidData);
    // Bytes that do not begin as a picture's header have no signature.
    Array<uint8_t> unsigned_ = writeHdr("#!RADIANCE\n", "-Y 1 +X 2", scanlines);
    CHECK_FALSE(prHdrHasSignature(PrByteView{unsigned_.data(), unsigned_.count()}));
    CHECK(decodeHdr(unsigned_).status == PrStatus_InvalidData);
}

TEST_CASE("an hdr whose scanlines end early or run past their width does not decode") {
    Array<Rgbe> pixels = samplePixels();
    Array<uint8_t> whole;
    appendLiteralRuns(&whole, pixels.data(), 8);
    appendLiteralRuns(&whole, pixels.data() + 8, 8);
    REQUIRE(decodeHdr(writeHdr(kHeader, "-Y 2 +X 8", whole)).status == PrStatus_Ok);
    // Every shorter file is cut within a scanline.
    for (size_t size = 0; size < whole.count(); size += 3) {
        CAPTURE(size);
        Array<uint8_t> cut = copyBytes(PrByteView{whole.data(), size});
        CHECK(decodeHdr(writeHdr(kHeader, "-Y 2 +X 8", cut)).status == PrStatus_InvalidData);
    }
    Array<Rgbe> flat({{5, 6, 7, 130}, {8, 9, 10, 131}});
    Array<uint8_t> flatScanlines;
    appendFlat(&flatScanlines, flat);
    flatScanlines.resize(flatScanlines.count() - 1);
    CHECK(decodeHdr(writeHdr(kHeader, "-Y 1 +X 2", flatScanlines)).status == PrStatus_InvalidData);

    // A scanline whose marker gives another width, a run of no bytes, a run past the scanline's end, and literal bytes
    // past it.
    const uint8_t wrongWidth[] = {2, 2, 0, 9, 128 + 9, 1, 128 + 9, 1, 128 + 9, 1, 128 + 9, 1};
    const uint8_t emptyRun[] = {2, 2, 0, 8, 128, 1, 128 + 8, 1, 128 + 8, 1, 128 + 8, 1, 128 + 8, 1};
    const uint8_t emptyLiteral[] = {2, 2, 0, 8, 0, 128 + 8, 1, 128 + 8, 1, 128 + 8, 1, 128 + 8, 1};
    const uint8_t longRun[] = {2, 2, 0, 8, 128 + 5, 1, 128 + 4, 1, 128 + 8, 1, 128 + 8, 1, 128 + 8, 1};
    const uint8_t longLiteral[] = {2, 2, 0, 8, 128 + 7, 1, 2, 1, 1, 128 + 8, 1, 128 + 8, 1, 128 + 8, 1};
    const PrByteView bad[] = {
        {wrongWidth, sizeof(wrongWidth)},
        {emptyRun, sizeof(emptyRun)},
        {emptyLiteral, sizeof(emptyLiteral)},
        {longRun, sizeof(longRun)},
        {longLiteral, sizeof(longLiteral)},
    };
    for (uint32_t i = 0; i < 5; ++i) {
        CAPTURE(i);
        CHECK(decodeHdr(writeHdr(kHeader, "-Y 1 +X 8", copyBytes(bad[i]))).status == PrStatus_InvalidData);
    }
}

TEST_CASE("Radiance's public header compiles as C") {
    CHECK(prTestHdrCApi() == 0);
}
