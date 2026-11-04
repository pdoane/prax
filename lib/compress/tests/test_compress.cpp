#include "pr/compress_builtin.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestCompressCApi(void);

namespace pr_impl {

namespace {

// "the quick brown fox jumps over the lazy dog " twenty times, zlib-framed.
constexpr uint8_t kZlib[] = {
    0x78, 0xda, 0x2b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56, 0x48, 0x2a, 0xca,
    0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a, 0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f,
    0x4b, 0x2d, 0x52, 0x28, 0x01, 0x4a, 0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0x83,
    0x39, 0xa3, 0x6a, 0x47, 0xd5, 0x8e, 0xaa, 0x25, 0x5f, 0x2d, 0x00, 0xaa, 0x6a, 0x42, 0x04,
};

// "hello hello hello" as bare Deflate.
constexpr uint8_t kDeflate[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90, 0x00};

// Two gzip members, "abc" and "def".
constexpr uint8_t kGzip[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x4b, 0x4c,
    0x4a, 0x06, 0x00, 0xc2, 0x41, 0x24, 0x35, 0x03, 0x00, 0x00, 0x00, 0x1f,
    0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x4b, 0x49, 0x4d,
    0x03, 0x00, 0x61, 0xe1, 0xc4, 0x0c, 0x03, 0x00, 0x00, 0x00,
};

// The same text as kZlib, as Brotli.
constexpr uint8_t kBrotli[] = {
    0x1f, 0x6f, 0x03, 0x88, 0x9c, 0x09, 0x36, 0x4e, 0xa8, 0x77, 0x37, 0xbc, 0x26, 0x33,
    0x63, 0xa3, 0xd3, 0x20, 0x67, 0x78, 0x7a, 0xf2, 0x0d, 0x2d, 0xac, 0x41, 0xca, 0xe6,
    0xf2, 0xc2, 0x06, 0x1c, 0x38, 0x24, 0x90, 0xb7, 0x09, 0xdf, 0xa1, 0xd3, 0x0a, 0x67,
    0x1f, 0x1e, 0x27, 0x4a, 0x29, 0x38, 0xc2, 0xfb, 0xab, 0x0f, 0xa2,
};

std::string expected() {
    std::string text;
    for (int i = 0; i < 20; ++i)
        text += "the quick brown fox jumps over the lazy dog ";
    return text;
}

// Decodes a whole stream at once into room for `capacity` bytes.
PrStatus decode(PrCompressFormat format, const uint8_t* bytes, size_t size, std::string* out,
        size_t capacity = 4096, PrCompressRead* read = nullptr) {
    std::vector<uint8_t> output(capacity);
    PrCompressRead local{};
    PrStatus status = prCompressCodecDecode(prCompressBuiltinCreate(), format, PrByteView{bytes, size}, output.data(),
        output.size(), read != nullptr ? read : &local);
    const PrCompressRead& r = read != nullptr ? *read : local;
    out->assign(reinterpret_cast<const char*>(output.data()), r.written);
    return status;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestCompressCApi() == 0);
}

TEST_CASE("each format decodes whole") {
    std::string text;
    PrCompressRead read{};
    CHECK(decode(PrCompressFormat_Zlib, kZlib, sizeof(kZlib), &text, 4096, &read) == PrStatus_Ok);
    CHECK(read.progress == PrCompressProgress_Done);
    CHECK(read.consumed == sizeof(kZlib));
    CHECK(text == expected());
    CHECK(decode(PrCompressFormat_Deflate, kDeflate, sizeof(kDeflate), &text) == PrStatus_Ok);
    CHECK(text == "hello hello hello");
    CHECK(decode(PrCompressFormat_Gzip, kGzip, sizeof(kGzip), &text) == PrStatus_Ok);
    CHECK(text == "abcdef");
    CHECK(decode(PrCompressFormat_Brotli, kBrotli, sizeof(kBrotli), &text, 4096, &read) == PrStatus_Ok);
    CHECK(read.progress == PrCompressProgress_Done);
    CHECK(read.consumed == sizeof(kBrotli));
    CHECK(text == expected());
    CHECK(decode(PrCompressFormat_Brotli, kBrotli, sizeof(kBrotli) - 1, &text) == PrStatus_Truncated);
}

TEST_CASE("a stream decodes a byte of input at a time into little room") {
    PrCompressDecoder* decoder = nullptr;
    REQUIRE(prCompressDecoderCreate(prCompressBuiltinCreate(), PrCompressFormat_Zlib, &decoder) == PrStatus_Ok);
    std::string text;
    uint8_t room[7];
    size_t at = 0;
    PrCompressRead read{};
    do {
        PrByteView input{kZlib + at, at < sizeof(kZlib) ? 1u : 0u};
        REQUIRE(prCompressDecoderRead(decoder, input, at + 1 >= sizeof(kZlib), room, sizeof(room), &read) ==
            PrStatus_Ok);
        at += read.consumed;
        text.append(reinterpret_cast<const char*>(room), read.written);
    } while (read.progress != PrCompressProgress_Done);
    prCompressDecoderDestroy(decoder);
    CHECK(text == expected());
    CHECK(at == sizeof(kZlib));
}

TEST_CASE("output that fills first asks for room") {
    std::string text;
    PrCompressRead read{};
    CHECK(decode(PrCompressFormat_Zlib, kZlib, sizeof(kZlib), &text, 100, &read) == PrStatus_Ok);
    CHECK(read.progress == PrCompressProgress_Output);
    CHECK(text == expected().substr(0, 100));
}

TEST_CASE("a stream that ends early is truncated, and a corrupt one fails") {
    std::string text;
    CHECK(decode(PrCompressFormat_Zlib, kZlib, sizeof(kZlib) - 3, &text) == PrStatus_Truncated);
    uint8_t corrupt[sizeof(kZlib)];
    for (size_t i = 0; i < sizeof(kZlib); ++i)
        corrupt[i] = kZlib[i];
    corrupt[sizeof(kZlib) - 1] ^= 1;
    CHECK(decode(PrCompressFormat_Zlib, corrupt, sizeof(corrupt), &text) == PrStatus_InvalidData);
    const uint8_t badHeader[] = {0x78, 0x00};
    CHECK(decode(PrCompressFormat_Zlib, badHeader, sizeof(badHeader), &text) == PrStatus_InvalidData);
}

TEST_CASE("a zlib stream that needs a preset dictionary is not supported") {
    // FDICT set, with the check bits that make the header a multiple of 31.
    const uint8_t header[] = {0x78, 0xbb, 0x00, 0x00, 0x00, 0x01};
    std::string text;
    CHECK(decode(PrCompressFormat_Zlib, header, sizeof(header), &text) == PrStatus_NotSupported);
}
