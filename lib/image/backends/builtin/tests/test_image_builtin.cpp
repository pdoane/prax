#include "image/backends/builtin/tests/image_builtin_test.h"

#include "tools/harness/test.h"

extern "C" int prTestImageBuiltinCApi(void);

namespace pr_impl {

namespace {

Array<uint8_t> pngHeader(uint32_t width, uint32_t height) {
    Array<uint8_t> bytes({0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0,    0,   0,   13,  'I',  'H',  'D',  'R'});
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push(uint8_t((width >> shift) & 0xFF));
    }
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push(uint8_t((height >> shift) & 0xFF));
    }
    bytes.append(Array<uint8_t>({8, 6, 0, 0, 0}).span());
    return bytes;
}

Array<uint8_t> gifHeader(uint32_t width, uint32_t height) {
    Array<uint8_t> bytes({'G', 'I', 'F', '8', '9', 'a'});
    bytes.push(uint8_t(width & 0xFF));
    bytes.push(uint8_t((width >> 8) & 0xFF));
    bytes.push(uint8_t(height & 0xFF));
    bytes.push(uint8_t((height >> 8) & 0xFF));
    bytes.append(Array<uint8_t>({0, 0, 0}).span());
    return bytes;
}

// A JPEG with one application segment before the frame header, which specifies the dimensions.
Array<uint8_t> jpegHeader(uint8_t frameMarker, uint32_t width, uint32_t height) {
    Array<uint8_t> bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 'J', 'F'});
    bytes.append(Array<uint8_t>({0xFF, frameMarker, 0, 17, 8}).span());
    bytes.push(uint8_t((height >> 8) & 0xFF));
    bytes.push(uint8_t(height & 0xFF));
    bytes.push(uint8_t((width >> 8) & 0xFF));
    bytes.push(uint8_t(width & 0xFF));
    bytes.append(Array<uint8_t>({3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1}).span());
    return bytes;
}

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;
};

bool dimensions(const Array<uint8_t>& bytes, Size* out) {
    return imageSize(bytes.data(), bytes.count(), Orientation_FromImage, &out->width, &out->height);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestImageBuiltinCApi() == 0);
}

TEST_CASE("png dimensions come from the IHDR chunk") {
    Size size;
    REQUIRE(dimensions(pngHeader(16, 16), &size));
    CHECK(size.width == 16);
    CHECK(size.height == 16);

    REQUIRE(dimensions(pngHeader(4096, 7), &size));
    CHECK(size.width == 4096);
    CHECK(size.height == 7);
}

TEST_CASE("a png that declares no size reads as none") {
    Size size;
    Array<uint8_t> zero = pngHeader(0, 16);
    CHECK(!dimensions(zero, &size));

    Array<uint8_t> truncated = pngHeader(16, 16);
    truncated.resize(20);
    CHECK(!dimensions(truncated, &size));

    Array<uint8_t> renamed = pngHeader(16, 16);
    renamed[13] = 'X';
    CHECK(!dimensions(renamed, &size));

    Array<uint8_t> wrongSignature = pngHeader(16, 16);
    wrongSignature[1] = 'Q';
    CHECK(!dimensions(wrongSignature, &size));
}

TEST_CASE("gif dimensions come from the logical screen descriptor") {
    Size size;
    REQUIRE(dimensions(gifHeader(16, 16), &size));
    CHECK(size.width == 16);
    CHECK(size.height == 16);

    Array<uint8_t> old = gifHeader(300, 200);
    old[4] = '7';
    REQUIRE(dimensions(old, &size));
    CHECK(size.width == 300);
    CHECK(size.height == 200);
}

TEST_CASE("a gif header that is short or misspelled reads as none") {
    Size size;
    Array<uint8_t> truncated = gifHeader(16, 16);
    truncated.resize(9);
    CHECK(!dimensions(truncated, &size));

    Array<uint8_t> version = gifHeader(16, 16);
    version[4] = '5';
    CHECK(!dimensions(version, &size));

    Array<uint8_t> suffix = gifHeader(16, 16);
    suffix[5] = 'b';
    CHECK(!dimensions(suffix, &size));
}

TEST_CASE("jpeg dimensions come from the frame header") {
    const int markers[] = {0xC0, 0xC1, 0xC2};
    for (int marker : markers) {
        Size size;
        REQUIRE(dimensions(jpegHeader(uint8_t(marker), 640, 480), &size));
        CHECK(size.width == 640);
        CHECK(size.height == 480);
    }
}

TEST_CASE("a jpeg with no frame header reads as none") {
    Size size;
    Array<uint8_t> truncated = jpegHeader(0xC0, 640, 480);
    truncated.resize(14);
    CHECK(!dimensions(truncated, &size));

    Array<uint8_t> headerless({0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 'J', 'F'});
    CHECK(!dimensions(headerless, &size));

    Array<uint8_t> notJpeg = jpegHeader(0xC0, 640, 480);
    notJpeg[1] = 0xD9;
    CHECK(!dimensions(notJpeg, &size));
}

TEST_CASE("bytes of no known encoding read as none") {
    Size size;
    CHECK(!imageSize(nullptr, 0, Orientation_FromImage, &size.width, &size.height));

    const Array<uint8_t> text({'<', 'h', 't', 'm', 'l', '>', 'x', 'y', 'z', 'w'});
    CHECK(!dimensions(text, &size));

    const Array<uint8_t> webp({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'});
    CHECK(!dimensions(webp, &size));
}

TEST_CASE("an encoding only the header reader knows decodes nothing") {
    Array<uint8_t> pixels(size_t(48) * 32 * 4);
    const Array<uint8_t> jpeg = jpegHeader(0xC0, 48, 32);
    CHECK(!decodeImage(jpeg.data(), jpeg.count(), Orientation_FromImage, pixels.data(), pixels.count()));

    const Array<uint8_t> gif = gifHeader(48, 32);
    CHECK(!decodeImage(gif.data(), gif.count(), Orientation_FromImage, pixels.data(), pixels.count()));

    const Array<uint8_t> webp({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'});
    CHECK(!decodeImage(webp.data(), webp.count(), Orientation_FromImage, pixels.data(), pixels.count()));
    CHECK(!decodeImage(nullptr, 0, Orientation_FromImage, pixels.data(), pixels.count()));
}
