// emjc decode tests over images written here, whose planes are stored in an uncompressed LZFSE block.

#include "pr/mem.h"

#include "image/backends/builtin/tests/image_builtin_test.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// The filters of a row, as the format numbers them.
enum {
    Filter_None,
    Filter_Gradient,
    Filter_Left,
    Filter_Up,
    Filter_Average,
};
typedef uint8_t Filter;

void put16(Array<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value));
    out.push(uint8_t(value >> 8));
}

void put32(Array<uint8_t>& out, uint32_t value) {
    put16(out, value & 0xffff);
    put16(out, value >> 16);
}

int32_t absolute(int32_t value) {
    return value < 0 ? -value : value;
}

// Returns the prediction of component `k` of the pixel at (x, y) under `filter`, from the components of the pixels
// before it, three to a pixel.
int32_t predict(const Array<int32_t>& components, uint32_t width, uint32_t x, uint32_t y, Filter filter, uint32_t k) {
    const int32_t* at = components.data() + (size_t(y) * width + x) * 3;
    const int32_t* left = x > 0 ? at - 3 : nullptr;
    const int32_t* up = y > 0 ? at - size_t(width) * 3 : nullptr;
    if (filter == Filter_None || (filter == Filter_Left && left == nullptr) || (filter == Filter_Up && up == nullptr) ||
            (left == nullptr && up == nullptr))
        return 0;
    if (filter == Filter_Left)
        return left[k];
    if (filter == Filter_Up || left == nullptr)
        return up[k];
    if (up == nullptr)
        return left[k];
    if (filter == Filter_Average)
        return (left[k] + up[k] + 1) / 2;
    const int32_t* corner = up - 3;
    return absolute(left[0] - corner[0]) < absolute(up[0] - corner[0]) ? up[k] : left[k];
}

// Returns an emjc image of the straight-alpha Rgba8 `pixels`, coding row y with filters[y].
Array<uint8_t> writeEmjc(uint32_t width, uint32_t height, const Array<uint8_t>& pixels, const Filter* filters) {
    const uint32_t count = width * height;
    // The transform of each pixel's color: p = r - b, q = g - b - p / 2 rounded toward zero, and base = g - q / 2
    // rounded away from zero.
    Array<int32_t> components;
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t r = pixels[i * 4];
        const int32_t g = pixels[i * 4 + 1];
        const int32_t b = pixels[i * 4 + 2];
        const int32_t p = r - b;
        const int32_t q = g - b - p / 2;
        components.push(g - (q - q / 2));
        components.push(p);
        components.push(q);
    }
    Array<uint8_t> planes;
    for (uint32_t i = 0; i < count; ++i)
        planes.push(pixels[i * 4 + 3]);
    planes.append(filters, height);
    // Each residual's byte, and an appendix entry for each component whose residual exceeds what a byte holds.
    Array<uint8_t> appendix;
    uint32_t next = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t index = (y * width + x) * 3 + k;
                const int32_t residual = components[index] - predict(components, width, x, y, filters[y], k);
                const int32_t magnitude = absolute(residual);
                const int32_t multiple = magnitude / 128;
                const int32_t coded = magnitude - 128 * multiple;
                REQUIRE((coded >= 0 && coded <= 127 && multiple <= 3));
                planes.push(uint8_t(2 * coded + (residual < 0 ? 1 : 0)));
                if (multiple == 0)
                    continue;
                // A byte with no multiple skips 64 components.
                for (; index - next > 63; next += 64)
                    appendix.push(63 << 2);
                appendix.push(uint8_t((index - next) << 2 | uint32_t(multiple)));
                next = index + 1;
            }
        }
    }
    planes.append(appendix.span());

    Array<uint8_t> out;
    out.append(reinterpret_cast<const uint8_t*>("emj1"), 4);
    put16(out, 0x100);
    put16(out, 0xa101);
    put16(out, width);
    put16(out, height);
    put16(out, appendix.count());
    put16(out, 0);
    out.append(reinterpret_cast<const uint8_t*>("bvx-"), 4);
    put32(out, planes.count());
    out.append(planes.span());
    out.append(reinterpret_cast<const uint8_t*>("bvx$"), 4);
    return out;
}

// Returns straight-alpha Rgba8 pixels of xorshift32 noise, with runs of black and white that some filters predict
// badly, so that their residuals exceed a byte. The noise of the first 30 pixels is faint, so that no residual of
// theirs does.
Array<uint8_t> testPixels(uint32_t width, uint32_t height) {
    Array<uint8_t> pixels;
    uint32_t x = 1;
    for (uint32_t i = 0; i < width * height; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        const bool faint = i < 30;
        const bool extreme = !faint && (x >> 24) < 96;
        const uint8_t level = (x >> 8) & 1 ? 255 : 0;
        for (uint32_t k = 0; k < 3; ++k) {
            const uint8_t noise = uint8_t(x >> (8 * k));
            const uint8_t extremeLevel = k == 1 ? uint8_t(255 - level) : level;
            pixels.push(faint ? uint8_t(120 + (noise & 15)) : extreme ? extremeLevel : noise);
        }
        pixels.push(uint8_t(x >> 4));
    }
    return pixels;
}

Array<uint8_t> readRgba8(const Array<uint8_t>& file, PrStatus* status) {
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.format.model == PrColorModel_Rgb);
    CHECK(info.format.alpha == PrImageAlpha_Straight);
    Array<uint8_t> pixels(info.extent.x * info.extent.y * 4);
    *status = testReadRgba8(decoder, 0, false, PrImageAlpha_Straight, pixels.data(), pixels.count());
    prImageDecoderDestroy(decoder);
    return pixels;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an emjc image decodes each row through its filter, with the residuals that the appendix offsets") {
    CHECK(prImageCodecSupportsType(testCodec(), "image/x-emjc"));
    const uint32_t width = 70;
    const uint32_t height = 10;
    const Filter filters[height] = {
        Filter_Gradient, Filter_None, Filter_Left, Filter_Up, Filter_Average, Filter_Gradient, Filter_Average,
        Filter_Left, Filter_Up, Filter_None,
    };
    const Array<uint8_t> pixels = testPixels(width, height);
    const Array<uint8_t> file = writeEmjc(width, height, pixels, filters);
    // The appendix has entries, and skips far enough to need a byte that only skips. It ends the planes, before the end
    // of the LZFSE stream.
    const uint32_t appendixSize = prMemReadLeU16(file.data() + 12);
    REQUIRE(appendixSize > 1);
    const uint8_t* appendix = file.data() + file.count() - 4 - appendixSize;
    bool skips = false;
    for (uint32_t i = 0; i < appendixSize; ++i)
        skips = skips || appendix[i] == 63 << 2;
    CHECK(skips);
    uint32_t decodedWidth = 0;
    uint32_t decodedHeight = 0;
    REQUIRE(imageSize(file.data(), file.count(), Orientation_None, &decodedWidth, &decodedHeight));
    CHECK(decodedWidth == width);
    CHECK(decodedHeight == height);
    PrStatus status = PrStatus_InvalidData;
    CHECK(readRgba8(file, &status) == pixels);
    CHECK(status == PrStatus_Ok);
}

TEST_CASE("an emjc image whose planes are malformed does not decode") {
    const Filter filters[2] = {Filter_None, Filter_Up};
    const Array<uint8_t> pixels = testPixels(3, 2);
    const Array<uint8_t> file = writeEmjc(3, 2, pixels, filters);
    PrStatus status = PrStatus_Ok;
    SUBCASE("a filter that the format does not have") {
        Array<uint8_t> bad = file;
        // The filters follow the header, the header of the LZFSE block and the alpha plane.
        bad[16 + 8 + 6 + 1] = 5;
        readRgba8(bad, &status);
        CHECK(status == PrStatus_InvalidData);
    }
    SUBCASE("planes shorter than the image") {
        Array<uint8_t> bad = file;
        prMemWriteLeU16(bad.data() + 10, 3);
        readRgba8(bad, &status);
        CHECK(status == PrStatus_InvalidData);
    }
    SUBCASE("a stream that is not LZFSE") {
        Array<uint8_t> bad = file;
        bad[16] = 'x';
        readRgba8(bad, &status);
        CHECK(status == PrStatus_InvalidData);
    }
    SUBCASE("an image of no pixels") {
        Array<uint8_t> bad = file;
        prMemWriteLeU16(bad.data() + 8, 0);
        CHECK(testImageOpen(bad.data(), bad.count()) == nullptr);
    }
    SUBCASE("a header cut short") {
        CHECK(testImageOpen(file.data(), 12) == nullptr);
    }
}
