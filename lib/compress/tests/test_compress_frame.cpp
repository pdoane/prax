#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/compress.h"
#include "pr/compress_select.h"
#include "pr/environment.h"
#include "pr/min_max.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace pr;

// "hello hello hello!\n" framed as zlib and as gzip.
constexpr uint8_t kZlib[] = {
    0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8,
    0x40, 0x90, 0x8a, 0x5c, 0x00, 0x47, 0x74, 0x06, 0xa8,
};
constexpr uint8_t kGzip[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff,
    0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90, 0x8a,
    0x5c, 0x00, 0xa2, 0x98, 0x73, 0xf5, 0x13, 0x00, 0x00, 0x00,
};

constexpr PrStringView kHello = "hello hello hello!\n";

// A set of one codec of the bare format `format` with `backend`, or null if the build does not include it.
struct OneCodec {
    PrCompressCodecSet* PR_NULLABLE set = nullptr;
    PrCompressCodecBackend backend;

    OneCodec(PrCompressCodecBackend chosen, PrCompressFormat format) : backend(chosen) {
        PrCompressCodec* codec = prCompressCodecCreate(chosen, format);
        if (codec != nullptr)
            set = prCompressCodecSetCreate(&codec, 1);
    }
    ~OneCodec() { prCompressCodecSetDestroy(set); }
    OneCodec(const OneCodec&) = delete;
    OneCodec& operator=(const OneCodec&) = delete;

    // True for a codec whose Deflate decoder may read past the end of its stream.
    bool readsPastEnd() const { return backend == PrCompressCodecBackend_Apple; }
};

// The backends that a build may have, each of which the tests try.
constexpr PrCompressCodecBackend kBackends[] = {PrCompressCodecBackend_Builtin, PrCompressCodecBackend_Apple};

// The outcome of decoding a whole stream a piece at a time.
struct Decoded {
    PrStatus status = PrStatus_Ok;
    String text;
    size_t consumed = 0;
};

// Decodes `input` in `format` through `set`, `inputChunk` bytes of input and `outputChunk` bytes of room at a time.
Decoded decode(const PrCompressCodecSet* set, PrCompressFormat format, PrByteView input, size_t inputChunk,
        size_t outputChunk) {
    Decoded out;
    PrCompressDecoder* decoder = nullptr;
    out.status = prCompressDecoderCreate(set, format, &decoder);
    if (out.status != PrStatus_Ok)
        return out;
    Array<uint8_t> room(outputChunk);
    for (;;) {
        const size_t size = min(inputChunk, input.size - out.consumed);
        PrCompressRead read{};
        out.status = prCompressDecoderRead(decoder, PrByteView{input.data + out.consumed, size},
            out.consumed + size == input.size, room.data(), room.count(), &read);
        out.text.append(PrStringView{reinterpret_cast<const char*>(room.data()), read.written});
        out.consumed += read.consumed;
        if (out.status != PrStatus_Ok || read.progress == PrCompressProgress_Done)
            break;
        REQUIRE((read.consumed != 0 || read.written != 0 || out.consumed == input.size));
    }
    prCompressDecoderDestroy(decoder);
    return out;
}

Decoded decodeWhole(const PrCompressCodecSet* set, PrCompressFormat format, PrByteView input) {
    return decode(set, format, input, input.size + 1, 4096);
}

// Returns words of a small vocabulary in an order that repeats only now and then, as text does.
Array<uint8_t> text(uint32_t size) {
    static const char* const kWords[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog. "};
    Array<uint8_t> out;
    uint32_t x = 7;
    while (out.count() < size) {
        x = x * 1103515245u + 12345u;
        PrStringView word = kWords[(x >> 16) % 8];
        out.append(reinterpret_cast<const uint8_t*>(word.data), uint32_t(word.size));
    }
    out.resize(size);
    return out;
}

Array<uint8_t> encode(const PrCompressCodecSet* set, PrCompressFormat format, const Array<uint8_t>& input) {
    Array<uint8_t> out(prCompressCodecSetEncodeBound(set, format, input.count()));
    size_t written = 0;
    REQUIRE(prCompressCodecSetEncode(set, format, PrCompressLevel_Default, input, out.data(), &written) == PrStatus_Ok);
    out.resize(uint32_t(written));
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("zlib and gzip resume their headers, data and checksums across pieces, around each Deflate codec") {
    for (PrCompressCodecBackend backend : kBackends) {
        OneCodec deflate(backend, PrCompressFormat_Deflate);
        if (deflate.set == nullptr)
            continue;
        CAPTURE(int(backend));
        const size_t inputChunks[] = {1, 2, 7, 4096};
        const size_t outputChunks[] = {1, 3, 32};
        for (size_t inputChunk : inputChunks) {
            for (size_t outputChunk : outputChunks) {
                CAPTURE(inputChunk);
                CAPTURE(outputChunk);
                CHECK(decode(deflate.set, PrCompressFormat_Zlib, kZlib, inputChunk, outputChunk).text == kHello);
                CHECK(decode(deflate.set, PrCompressFormat_Gzip, kGzip, inputChunk, outputChunk).text == kHello);
            }
        }
    }
}

TEST_CASE("gzip skips its optional fields and checks its header's CRC") {
    // A member with an extra field, a name, a header CRC, and data that spans several Deflate blocks.
    Array<uint8_t> input;
    input.append(kGzip, 10);
    input[3] = 0x1f & ~0x10;
    const uint8_t fields[] = {3, 0, 'a', 0, 'b', 'f', 'i', 'l', 'e', 0};
    input.append(Span(fields));
    const uint32_t crc = prCrc32Update(0, input);
    input.push(uint8_t(crc));
    input.push(uint8_t(crc >> 8));
    input.append(Span(kGzip).from(10));
    for (PrCompressCodecBackend backend : kBackends) {
        OneCodec deflate(backend, PrCompressFormat_Deflate);
        if (deflate.set == nullptr)
            continue;
        CAPTURE(int(backend));
        CHECK(decode(deflate.set, PrCompressFormat_Gzip, input, 1, 3).text == kHello);
        Array<uint8_t> bad = input;
        bad[10] ^= 1;
        CHECK(decodeWhole(deflate.set, PrCompressFormat_Gzip, bad).status == PrStatus_InvalidData);
    }
}

TEST_CASE("gzip reads consecutive members with a codec that finds its stream's end, and fails with one that does not") {
    Array<uint8_t> two(kGzip);
    two.append(Span(kGzip));
    for (PrCompressCodecBackend backend : kBackends) {
        OneCodec deflate(backend, PrCompressFormat_Deflate);
        if (deflate.set == nullptr)
            continue;
        CAPTURE(int(backend));
        const Decoded decoded = decode(deflate.set, PrCompressFormat_Gzip, two, 1, 3);
        if (deflate.readsPastEnd()) {
            CHECK(decoded.status == PrStatus_NotSupported);
        } else {
            CHECK(decoded.status == PrStatus_Ok);
            CHECK(decoded.text == "hello hello hello!\nhello hello hello!\n");
        }
    }
}

TEST_CASE("a frame cut short is truncated, and a corrupt one fails") {
    const PrCompressFormat formats[] = {PrCompressFormat_Zlib, PrCompressFormat_Gzip};
    for (PrCompressCodecBackend backend : kBackends) {
        OneCodec deflate(backend, PrCompressFormat_Deflate);
        if (deflate.set == nullptr)
            continue;
        CAPTURE(int(backend));
        for (PrCompressFormat format : formats) {
            CAPTURE(int(format));
            const PrByteView bytes = format == PrCompressFormat_Zlib ? PrByteView(kZlib) : PrByteView(kGzip);
            for (size_t size = 0; size < bytes.size; ++size) {
                CAPTURE(size);
                CHECK(decodeWhole(deflate.set, format, PrByteView{bytes.data, size}).status == PrStatus_Truncated);
            }
            const size_t indexes[] = {0, bytes.size - 1, bytes.size - 4};
            for (size_t index : indexes) {
                Array<uint8_t> bad;
                bad.append(bytes.data, uint32_t(bytes.size));
                bad[uint32_t(index)] ^= 1;
                CHECK(decodeWhole(deflate.set, format, bad).status == PrStatus_InvalidData);
            }
        }
    }
}

TEST_CASE("a zlib stream leaves the bytes after it unread, unless its codec reads past its end") {
    Array<uint8_t> input(kZlib);
    input.push(0xaa);
    for (PrCompressCodecBackend backend : kBackends) {
        OneCodec deflate(backend, PrCompressFormat_Deflate);
        if (deflate.set == nullptr)
            continue;
        CAPTURE(int(backend));
        const Decoded decoded = decodeWhole(deflate.set, PrCompressFormat_Zlib, input);
        if (deflate.readsPastEnd()) {
            CHECK(decoded.status == PrStatus_NotSupported);
        } else {
            CHECK(decoded.status == PrStatus_Ok);
            CHECK(decoded.consumed == sizeof(kZlib));
        }
    }
}

TEST_CASE("a gzip stream waits for the final input before it ends") {
    for (PrCompressCodecBackend backend : kBackends) {
        OneCodec deflate(backend, PrCompressFormat_Deflate);
        if (deflate.set == nullptr)
            continue;
        CAPTURE(int(backend));
        PrCompressDecoder* decoder = nullptr;
        REQUIRE(prCompressDecoderCreate(deflate.set, PrCompressFormat_Gzip, &decoder) == PrStatus_Ok);
        uint8_t room[32];
        PrCompressRead read{};
        CHECK(prCompressDecoderRead(decoder, kGzip, false, room, sizeof(room), &read) == PrStatus_Ok);
        CHECK(read.progress == PrCompressProgress_Input);
        CHECK(read.consumed == sizeof(kGzip));
        // A codec that reads past its end is given the last bytes only once the input is final.
        const size_t first = read.written;
        CHECK(prCompressDecoderRead(decoder, PrByteView{}, true, room + first, sizeof(room) - first, &read) ==
            PrStatus_Ok);
        CHECK(read.progress == PrCompressProgress_Done);
        CHECK(PrStringView{reinterpret_cast<const char*>(room), first + read.written} == kHello);
        prCompressDecoderDestroy(decoder);
    }
}

TEST_CASE("each codec decodes what each other codec of its format encodes") {
    const Array<uint8_t> input = text(200000);
    const PrCompressFormat formats[] = {
        PrCompressFormat_Deflate, PrCompressFormat_Zlib, PrCompressFormat_Gzip, PrCompressFormat_Brotli,
        PrCompressFormat_Lzfse, PrCompressFormat_Zstd,
    };
    for (PrCompressFormat format : formats) {
        PrCompressFormat bare = format;
        if (format == PrCompressFormat_Zlib || format == PrCompressFormat_Gzip)
            bare = PrCompressFormat_Deflate;
        for (PrCompressCodecBackend encoding : kBackends) {
            OneCodec encoder(encoding, bare);
            if (encoder.set == nullptr || !prCompressCodecSetEncodes(encoder.set, format))
                continue;
            const Array<uint8_t> encoded = encode(encoder.set, format, input);
            for (PrCompressCodecBackend decoding : kBackends) {
                OneCodec decoder(decoding, bare);
                if (decoder.set == nullptr)
                    continue;
                CAPTURE(int(format));
                CAPTURE(int(encoding));
                CAPTURE(int(decoding));
                const Decoded decoded = decode(decoder.set, format, encoded, 4096, 5000);
                CHECK(decoded.status == PrStatus_Ok);
                CHECK(decoded.consumed == encoded.count());
                CHECK(decoded.text == PrStringView{reinterpret_cast<const char*>(input.data()), input.count()});
            }
        }
    }
}

TEST_CASE("the environment chooses the backend of each bare format") {
    const PrBackendInterface* deflate = prCompressCodecSelectInterface(PrCompressFormat_Deflate);
    CHECK(deflate->name == "compress_deflate");
    CHECK(prBackendIsBuilt(deflate, PrCompressCodecBackend_Builtin));
    CHECK(prCompressCodecSelectInterface(PrCompressFormat_Lzfse)->name == "compress_lzfse");
    REQUIRE(prEnvironmentSetVariable("PR_BACKEND_COMPRESS_BROTLI", "nonesuch"));
    CHECK(prCompressCodecCreate(PrCompressCodecBackend_Default, PrCompressFormat_Brotli) == nullptr);
    CHECK(prCompressCodecSetCreateDefault() == nullptr);
    REQUIRE(prEnvironmentSetVariable("PR_BACKEND_COMPRESS_BROTLI", "builtin"));
    PrCompressCodecSet* set = prCompressCodecSetCreateDefault();
    CHECK(set != nullptr);
    prCompressCodecSetDestroy(set);
    REQUIRE(prEnvironmentRemoveVariable("PR_BACKEND_COMPRESS_BROTLI"));
}
