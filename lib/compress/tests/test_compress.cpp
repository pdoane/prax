#include "pr/array.h"
#include "pr/compress_brotli.h"
#include "pr/compress_deflate.h"
#include "pr/compress_lzfse.h"
#include "pr/compress_select.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

extern "C" int prTestCompressCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

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

// The same text as kZlib, as LZFSE.
constexpr uint8_t kLzfse[] = {
    0x62, 0x76, 0x78, 0x6e, 0x70, 0x03, 0x00, 0x00, 0x43, 0x00, 0x00, 0x00, 0xe0, 0x0f, 0x74,
    0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x20,
    0x66, 0x6f, 0x78, 0x20, 0x6a, 0x75, 0x6d, 0x70, 0x73, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x20,
    0x08, 0x1f, 0xe9, 0x6c, 0x61, 0x7a, 0x79, 0x20, 0x64, 0x6f, 0x67, 0x20, 0x38, 0x2c, 0xf0,
    0xff, 0xf0, 0xff, 0xf0, 0xff, 0xf9, 0xe4, 0x64, 0x6f, 0x67, 0x20, 0x06, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x62, 0x76, 0x78, 0x24,
};

// "abc" as an LZ4 block of one sequence of literals.
constexpr uint8_t kLz4[] = {0x30, 'a', 'b', 'c'};

String expected() {
    String text;
    for (int i = 0; i < 20; ++i)
        text.append("the quick brown fox jumps over the lazy dog ");
    return text;
}

// The set of every codec, which the tests share and never free.
const PrCompressCodecSet* codecs() {
    static PrCompressCodecSet* set = prCompressCodecSetCreateDefault();
    return set;
}

// Decodes a whole stream at once into room for `capacity` bytes.
PrStatus decode(PrCompressFormat format, const uint8_t* bytes, size_t size, String* out,
        size_t capacity = 4096, PrCompressRead* read = nullptr) {
    Array<uint8_t> output(capacity);
    PrCompressRead local{};
    PrStatus status = prCompressCodecSetDecode(codecs(), format, PrByteView{bytes, size}, output.data(),
        output.count(), read != nullptr ? read : &local);
    const PrCompressRead& r = read != nullptr ? *read : local;
    out->assign(PrStringView{reinterpret_cast<const char*>(output.data()), r.written});
    return status;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestCompressCApi() == 0);
}

TEST_CASE("each format decodes whole") {
    String text;
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
    CHECK(decode(PrCompressFormat_Lzfse, kLzfse, sizeof(kLzfse), &text, 4096, &read) == PrStatus_Ok);
    CHECK(read.progress == PrCompressProgress_Done);
    CHECK(read.consumed == sizeof(kLzfse));
    CHECK(text == expected());
    CHECK(decode(PrCompressFormat_Lzfse, kLzfse, sizeof(kLzfse) - 1, &text) == PrStatus_Truncated);
    CHECK(decode(PrCompressFormat_Lzfse, kZlib, sizeof(kZlib), &text) == PrStatus_InvalidData);
    CHECK(decode(PrCompressFormat_Lz4, kLz4, sizeof(kLz4), &text) == PrStatus_Ok);
    CHECK(text == "abc");
}

TEST_CASE("the set of every codec decodes every format and encodes the Deflate ones and Zstandard") {
    for (PrCompressFormat format = 0; format < PrCompressFormat_Count; ++format) {
        CAPTURE(format);
        const bool encodes = format < PrCompressFormat_Brotli || format == PrCompressFormat_Zstd;
        CHECK(prCompressCodecSetDecodes(codecs(), format));
        CHECK(prCompressCodecSetEncodes(codecs(), format) == encodes);
        CHECK((prCompressCodecSetEncodeBound(codecs(), format, 100) != 0) == encodes);
    }
    uint8_t out[16];
    size_t written = 0;
    CHECK(prCompressCodecSetEncode(codecs(), PrCompressFormat_Lzfse, PrCompressLevel_Default, PrByteView{}, out,
        &written) == PrStatus_NotSupported);
}

TEST_CASE("a set reads only the formats of its codecs") {
    PrCompressCodec* members[] = {prCompressDeflateCreate(), prCompressLzfseCreate()};
    PrCompressCodecSet* set = prCompressCodecSetCreate(members, 2);
    CHECK(prCompressCodecSetDecodes(set, PrCompressFormat_Zlib));
    CHECK(prCompressCodecSetDecodes(set, PrCompressFormat_Lzfse));
    CHECK_FALSE(prCompressCodecSetDecodes(set, PrCompressFormat_Brotli));
    PrCompressDecoder* decoder = nullptr;
    CHECK(prCompressDecoderCreate(set, PrCompressFormat_Brotli, &decoder) == PrStatus_NotSupported);
    CHECK(decoder == nullptr);
    uint8_t out[16];
    PrCompressRead read{};
    CHECK(prCompressCodecSetDecode(set, PrCompressFormat_Brotli, PrByteView{kBrotli, sizeof(kBrotli)}, out,
        sizeof(out), &read) == PrStatus_NotSupported);
    prCompressCodecSetDestroy(set);
    PrCompressCodecSet* empty = prCompressCodecSetCreate(nullptr, 0);
    CHECK_FALSE(prCompressCodecSetDecodes(empty, PrCompressFormat_Deflate));
    CHECK_FALSE(prCompressCodecSetEncodes(empty, PrCompressFormat_Deflate));
    CHECK(prCompressCodecSetEncodeBound(empty, PrCompressFormat_Deflate, 100) == 0);
    prCompressCodecSetDestroy(empty);
}

TEST_CASE("a set decodes with its first codec that decodes a format, and encodes with its first that encodes it") {
    // Brotli decodes only, so the set encodes Deflate with the Deflate codec, which comes after it.
    PrCompressCodec* members[] = {prCompressBrotliCreate(), prCompressDeflateCreate()};
    PrCompressCodecSet* set = prCompressCodecSetCreate(members, 2);
    CHECK(prCompressCodecSetEncodes(set, PrCompressFormat_Gzip));
    const String text = expected();
    Array<uint8_t> encoded(prCompressCodecSetEncodeBound(set, PrCompressFormat_Gzip, text.count()));
    size_t written = 0;
    REQUIRE(prCompressCodecSetEncode(set, PrCompressFormat_Gzip, PrCompressLevel_Default,
        PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.count()}, encoded.data(), &written) ==
        PrStatus_Ok);
    Array<uint8_t> decoded(text.count());
    PrCompressRead read{};
    CHECK(prCompressCodecSetDecode(set, PrCompressFormat_Gzip, PrByteView{encoded.data(), written}, decoded.data(),
        decoded.count(), &read) == PrStatus_Ok);
    CHECK(read.written == text.count());
    prCompressCodecSetDestroy(set);
}

TEST_CASE("a stream decodes a byte of input at a time into little room") {
    PrCompressDecoder* decoder = nullptr;
    REQUIRE(prCompressDecoderCreate(codecs(), PrCompressFormat_Zlib, &decoder) == PrStatus_Ok);
    String text;
    uint8_t room[7];
    size_t at = 0;
    PrCompressRead read{};
    do {
        PrByteView input{kZlib + at, at < sizeof(kZlib) ? 1u : 0u};
        REQUIRE(prCompressDecoderRead(decoder, input, at + 1 >= sizeof(kZlib), room, sizeof(room), &read) ==
            PrStatus_Ok);
        at += read.consumed;
        text.append(PrStringView{reinterpret_cast<const char*>(room), read.written});
    } while (read.progress != PrCompressProgress_Done);
    prCompressDecoderDestroy(decoder);
    CHECK(text == expected());
    CHECK(at == sizeof(kZlib));
}

TEST_CASE("output that fills first asks for room") {
    String text;
    PrCompressRead read{};
    CHECK(decode(PrCompressFormat_Zlib, kZlib, sizeof(kZlib), &text, 100, &read) == PrStatus_Ok);
    CHECK(read.progress == PrCompressProgress_Output);
    String all = expected();
    CHECK(text == prStringSlice(all, 0, 100));
}

TEST_CASE("a stream that ends early is truncated, and a corrupt one fails") {
    String text;
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
    String text;
    CHECK(decode(PrCompressFormat_Zlib, header, sizeof(header), &text) == PrStatus_NotSupported);
}

TEST_CASE("a decoder decodes another stream after a reset") {
    const PrCompressFormat formats[] = {PrCompressFormat_Zlib, PrCompressFormat_Gzip, PrCompressFormat_Zstd};
    const String input = expected();
    const PrByteView bytes{reinterpret_cast<const uint8_t*>(input.data()), input.count()};
    for (PrCompressFormat format : formats) {
        CAPTURE(format);
        uint8_t packed[1024];
        size_t size = 0;
        REQUIRE(prCompressCodecSetEncodeBound(codecs(), format, bytes.size) <= sizeof(packed));
        REQUIRE(prCompressCodecSetEncode(codecs(), format, PrCompressLevel_Default, bytes, packed, &size) ==
            PrStatus_Ok);
        PrCompressDecoder* decoder = nullptr;
        REQUIRE(prCompressDecoderCreate(codecs(), format, &decoder) == PrStatus_Ok);
        for (int round = 0; round < 3; ++round) {
            uint8_t out[1024];
            PrCompressRead read{};
            REQUIRE(prCompressDecoderRead(decoder, PrByteView{packed, size}, true, out, sizeof(out), &read) ==
                PrStatus_Ok);
            CHECK(read.progress == PrCompressProgress_Done);
            CHECK(PrStringView{reinterpret_cast<const char*>(out), read.written} == input.view());
            prCompressDecoderReset(decoder);
        }
        prCompressDecoderDestroy(decoder);
    }
}

TEST_CASE("a window limit refuses a Zstandard frame of a larger window") {
    // Over a megabyte, so that the frame names a window of 512K rather than its size.
    String input;
    uint32_t state = 1;
    while (input.count() < 1100000) {
        state = state * 1103515245u + 12345u;
        input.append(char('a' + (state >> 16) % 26));
    }
    const PrByteView bytes{reinterpret_cast<const uint8_t*>(input.data()), input.count()};
    Array<uint8_t> frame(uint32_t(prCompressCodecSetEncodeBound(codecs(), PrCompressFormat_Zstd, bytes.size)));
    size_t size = 0;
    REQUIRE(prCompressCodecSetEncode(codecs(), PrCompressFormat_Zstd, PrCompressLevel_Fast, bytes, frame.data(),
        &size) == PrStatus_Ok);
    Array<uint8_t> room(uint32_t(bytes.size));
    const size_t limits[] = {size_t(256) << 10, size_t(512) << 10};
    for (size_t limit : limits) {
        CAPTURE(limit);
        PrCompressDecoder* decoder = nullptr;
        REQUIRE(prCompressDecoderCreate(codecs(), PrCompressFormat_Zstd, &decoder) == PrStatus_Ok);
        prCompressDecoderSetWindowLimit(decoder, limit);
        // In pieces, so that the decoder keeps the window itself.
        PrCompressRead read{};
        PrStatus status = prCompressDecoderRead(decoder, PrByteView{frame.data(), size / 2}, false, room.data(),
            room.count(), &read);
        const size_t at = read.consumed;
        const size_t written = read.written;
        if (status == PrStatus_Ok) {
            status = prCompressDecoderRead(decoder, PrByteView{frame.data() + at, size - at}, true,
                room.data() + written, room.count() - written, &read);
        }
        CHECK(status == (limit < (size_t(512) << 10) ? PrStatus_InvalidData : PrStatus_Ok));
        prCompressDecoderDestroy(decoder);
    }
}
