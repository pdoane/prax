#include "image/backends/builtin/tests/image_builtin_test.h"

#include "tools/harness/test.h"

#include <vector>

extern "C" int prTestImageBuiltinCApi(void);

namespace pr_impl {

namespace {

std::vector<uint8_t> pngHeader(uint32_t width, uint32_t height) {
    std::vector<uint8_t> bytes = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
        0,    0,   0,   13,  'I',  'H',  'D',  'R',
    };
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(uint8_t((width >> shift) & 0xFF));
    }
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(uint8_t((height >> shift) & 0xFF));
    }
    bytes.insert(bytes.end(), {8, 6, 0, 0, 0});
    return bytes;
}

std::vector<uint8_t> gifHeader(uint32_t width, uint32_t height) {
    std::vector<uint8_t> bytes = {'G', 'I', 'F', '8', '9', 'a'};
    bytes.push_back(uint8_t(width & 0xFF));
    bytes.push_back(uint8_t((width >> 8) & 0xFF));
    bytes.push_back(uint8_t(height & 0xFF));
    bytes.push_back(uint8_t((height >> 8) & 0xFF));
    bytes.insert(bytes.end(), {0, 0, 0});
    return bytes;
}

// A JPEG with one application segment before the frame header, which specifies the dimensions.
std::vector<uint8_t> jpegHeader(uint8_t frameMarker, uint32_t width, uint32_t height) {
    std::vector<uint8_t> bytes = {0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 'J', 'F'};
    bytes.insert(bytes.end(), {0xFF, frameMarker, 0, 17, 8});
    bytes.push_back(uint8_t((height >> 8) & 0xFF));
    bytes.push_back(uint8_t(height & 0xFF));
    bytes.push_back(uint8_t((width >> 8) & 0xFF));
    bytes.push_back(uint8_t(width & 0xFF));
    bytes.insert(bytes.end(), {3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1});
    return bytes;
}

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;
};

bool dimensions(const std::vector<uint8_t>& bytes, Size* out) {
    return imageSize(bytes.data(), bytes.size(), Orientation_FromImage, &out->width, &out->height);
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
    std::vector<uint8_t> zero = pngHeader(0, 16);
    CHECK(!dimensions(zero, &size));

    std::vector<uint8_t> truncated = pngHeader(16, 16);
    truncated.resize(20);
    CHECK(!dimensions(truncated, &size));

    std::vector<uint8_t> renamed = pngHeader(16, 16);
    renamed[13] = 'X';
    CHECK(!dimensions(renamed, &size));

    std::vector<uint8_t> wrongSignature = pngHeader(16, 16);
    wrongSignature[1] = 'Q';
    CHECK(!dimensions(wrongSignature, &size));
}

TEST_CASE("gif dimensions come from the logical screen descriptor") {
    Size size;
    REQUIRE(dimensions(gifHeader(16, 16), &size));
    CHECK(size.width == 16);
    CHECK(size.height == 16);

    std::vector<uint8_t> old = gifHeader(300, 200);
    old[4] = '7';
    REQUIRE(dimensions(old, &size));
    CHECK(size.width == 300);
    CHECK(size.height == 200);
}

TEST_CASE("a gif header that is short or misspelled reads as none") {
    Size size;
    std::vector<uint8_t> truncated = gifHeader(16, 16);
    truncated.resize(9);
    CHECK(!dimensions(truncated, &size));

    std::vector<uint8_t> version = gifHeader(16, 16);
    version[4] = '5';
    CHECK(!dimensions(version, &size));

    std::vector<uint8_t> suffix = gifHeader(16, 16);
    suffix[5] = 'b';
    CHECK(!dimensions(suffix, &size));
}

TEST_CASE("jpeg dimensions come from the frame header") {
    for (int marker : {0xC0, 0xC1, 0xC2}) {
        Size size;
        REQUIRE(dimensions(jpegHeader(uint8_t(marker), 640, 480), &size));
        CHECK(size.width == 640);
        CHECK(size.height == 480);
    }
}

TEST_CASE("a jpeg with no frame header reads as none") {
    Size size;
    std::vector<uint8_t> truncated = jpegHeader(0xC0, 640, 480);
    truncated.resize(14);
    CHECK(!dimensions(truncated, &size));

    std::vector<uint8_t> headerless = {0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 'J', 'F'};
    CHECK(!dimensions(headerless, &size));

    std::vector<uint8_t> notJpeg = jpegHeader(0xC0, 640, 480);
    notJpeg[1] = 0xD9;
    CHECK(!dimensions(notJpeg, &size));
}

TEST_CASE("bytes of no known encoding read as none") {
    Size size;
    CHECK(!imageSize(nullptr, 0, Orientation_FromImage, &size.width, &size.height));

    const std::vector<uint8_t> text = {'<', 'h', 't', 'm', 'l', '>', 'x', 'y', 'z', 'w'};
    CHECK(!dimensions(text, &size));

    const std::vector<uint8_t> webp = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    CHECK(!dimensions(webp, &size));
}

TEST_CASE("an encoding only the header reader knows decodes nothing") {
    std::vector<uint8_t> pixels(size_t(48) * 32 * 4, 0);
    const std::vector<uint8_t> jpeg = jpegHeader(0xC0, 48, 32);
    CHECK(!decodeImage(jpeg.data(), jpeg.size(), Orientation_FromImage, pixels.data(), pixels.size()));

    const std::vector<uint8_t> gif = gifHeader(48, 32);
    CHECK(!decodeImage(gif.data(), gif.size(), Orientation_FromImage, pixels.data(), pixels.size()));

    const std::vector<uint8_t> webp = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    CHECK(!decodeImage(webp.data(), webp.size(), Orientation_FromImage, pixels.data(), pixels.size()));
    CHECK(!decodeImage(nullptr, 0, Orientation_FromImage, pixels.data(), pixels.size()));
}
