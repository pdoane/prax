// PNG text read from and written into a PNG file's chunks.

#include "pr/image_png.h"

#include "pr/assert.h"
#include "pr/checksum.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

// A chunk of a PNG file: its type and data, and the whole chunk's bytes, from its length to its CRC.
struct Chunk {
    char type[4];
    PrByteView data;
    PrByteView whole;
    bool crcMatches;
};

uint32_t loadU32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

void storeU32(uint8_t* bytes, uint32_t value) {
    bytes[0] = uint8_t(value >> 24);
    bytes[1] = uint8_t(value >> 16);
    bytes[2] = uint8_t(value >> 8);
    bytes[3] = uint8_t(value);
}

bool isType(const Chunk& chunk, const char* type) {
    return prMemEquals(chunk.type, type, 4);
}

// Reads the chunk at `*at` of `png` and moves `*at` past it. Returns false, and leaves `*at` as it was, if the chunk is
// not whole.
bool readChunk(PrByteView png, size_t* at, Chunk* out) {
    if (png.size - *at < 12)
        return false;
    const uint8_t* start = png.data + *at;
    uint32_t length = loadU32(start);
    // A length past 2^31 - 1 is invalid (png-3/#5Chunk-layout).
    if (length > 0x7fffffffu || png.size - *at - 12 < length)
        return false;
    prMemCopy(out->type, start + 4, 4);
    out->data = PrByteView{start + 8, length};
    out->whole = PrByteView{start, size_t(length) + 12};
    out->crcMatches = prCrc32Update(0, PrByteView{start + 4, size_t(length) + 4}) == loadU32(start + 8 + length);
    *at += size_t(length) + 12;
    return true;
}

// Returns true if `png` starts with the signature and a header chunk.
bool hasHeader(PrByteView png) {
    size_t at = sizeof(kSignature);
    Chunk header;
    return png.size >= sizeof(kSignature) && prMemEquals(png.data, kSignature, sizeof(kSignature)) &&
        readChunk(png, &at, &header) && isType(header, "IHDR");
}

bool isTextType(const Chunk& chunk) {
    return isType(chunk, "tEXt") || isType(chunk, "zTXt") || isType(chunk, "iTXt");
}

// Returns the length of the NUL-terminated field at `from` in `data`, or kArrayMissing if it has no terminator.
uint32_t fieldLength(PrByteView data, size_t from) {
    for (size_t i = from; i < data.size; ++i) {
        if (data.data[i] == 0)
            return uint32_t(i - from);
    }
    return kArrayMissing;
}

// Returns the keyword of a text chunk, or the null view if the chunk has no valid keyword.
PrStringView keywordOf(const Chunk& chunk) {
    uint32_t length = fieldLength(chunk.data, 0);
    if (length == kArrayMissing || length == 0 || length > kPrImagePngKeywordMaxBytes)
        return PrStringView{};
    return PrStringView{reinterpret_cast<const char*>(chunk.data.data), length};
}

// Appends the Latin-1 `bytes` to `out` as UTF-8.
void appendLatin1(PrTextBuffer* out, PrByteView bytes) {
    for (size_t i = 0; i < bytes.size; ++i)
        prTextBufferAppendCodePoint(out, bytes.data[i]);
}

// Decompresses the zlib stream `input` into `out`. Returns false if the stream is corrupt, ends early, or decompresses
// past kPrImagePngTextMaxBytes.
bool inflate(PrCompressCodec* codec, PrByteView input, RawArray<uint8_t>* out) {
    PrCompressDecoder* decoder = nullptr;
    if (prCompressDecoderCreate(codec, PrCompressFormat_Zlib, &decoder) != PrStatus_Ok)
        return false;
    size_t consumed = 0;
    bool ok = false;
    while (true) {
        if (out->capacity - out->count < 4096) {
            if (out->capacity >= kPrImagePngTextMaxBytes)
                break;
            out->reserve(out->count + 4096 > out->capacity * 2 ? out->count + 4096 : out->capacity * 2);
        }
        PrCompressRead read{};
        PrByteView rest{input.data + consumed, input.size - consumed};
        if (prCompressDecoderRead(decoder, rest, true, out->data + out->count, out->capacity - out->count, &read) !=
                PrStatus_Ok)
            break;
        consumed += read.consumed;
        out->count += uint32_t(read.written);
        if (read.progress == PrCompressProgress_Done) {
            ok = out->count <= kPrImagePngTextMaxBytes;
            break;
        }
        if (read.progress == PrCompressProgress_Input)
            break;
    }
    prCompressDecoderDestroy(decoder);
    return ok;
}

// Reads the text of a tEXt, zTXt or iTXt chunk and calls `fn` with it. Returns false if the chunk is malformed, or is
// compressed and `codec` is null.
bool readText(const Chunk& chunk, PrCompressCodec* PR_NULLABLE codec, PrImagePngTextFn fn, void* PR_NULLABLE context) {
    PrStringView keyword = keywordOf(chunk);
    if (keyword.data == nullptr)
        return false;
    PrTextBuffer keywordUtf8{};
    appendLatin1(&keywordUtf8, PrByteView{chunk.data.data, keyword.size});
    PrTextBuffer converted{};
    RawArray<uint8_t> inflated{};
    PrImagePngText text{.keyword = prTextBufferView(&keywordUtf8), .language = "", .translatedKeyword = ""};
    size_t at = keyword.size + 1;
    bool ok = true;
    if (isType(chunk, "tEXt")) {
        appendLatin1(&converted, PrByteView{chunk.data.data + at, chunk.data.size - at});
        text.text = prTextBufferView(&converted);
    } else if (isType(chunk, "zTXt")) {
        // The one compression method is zlib (png-3/#11zTXt).
        ok = codec != nullptr && at < chunk.data.size && chunk.data.data[at] == 0 &&
            inflate(codec, PrByteView{chunk.data.data + at + 1, chunk.data.size - at - 1}, &inflated);
        if (ok) {
            appendLatin1(&converted, PrByteView{inflated.data, inflated.count});
            text.text = prTextBufferView(&converted);
        }
    } else {
        ok = chunk.data.size - at >= 2 && chunk.data.data[at] <= 1 && chunk.data.data[at + 1] == 0;
        bool compressed = ok && chunk.data.data[at] == 1;
        at += 2;
        uint32_t languageLength = ok ? fieldLength(chunk.data, at) : kArrayMissing;
        uint32_t translatedLength =
            languageLength != kArrayMissing ? fieldLength(chunk.data, at + languageLength + 1) : kArrayMissing;
        ok = translatedLength != kArrayMissing;
        if (ok) {
            const char* fields = reinterpret_cast<const char*>(chunk.data.data);
            text.language = PrStringView{fields + at, languageLength};
            text.translatedKeyword = PrStringView{fields + at + languageLength + 1, translatedLength};
            at += size_t(languageLength) + translatedLength + 2;
            PrByteView rest{chunk.data.data + at, chunk.data.size - at};
            if (compressed) {
                ok = codec != nullptr && inflate(codec, rest, &inflated);
                rest = PrByteView{inflated.data, inflated.count};
            }
            text.text = PrStringView{reinterpret_cast<const char*>(rest.data), rest.size};
            ok = ok && prUtf8Validate(text.text) == text.text.size &&
                prUtf8Validate(text.translatedKeyword) == text.translatedKeyword.size;
        }
    }
    if (ok)
        fn(context, &text);
    prTextBufferReset(&keywordUtf8);
    prTextBufferReset(&converted);
    inflated.reset();
    return ok;
}

// Bytes written up to a capacity, and counted past it.
struct Output {
    uint8_t* PR_NULLABLE data;
    size_t capacity;
    size_t size;
};

void write(Output* out, const void* bytes, size_t size) {
    if (out->size < out->capacity) {
        size_t room = out->capacity - out->size;
        prMemCopy(out->data + out->size, bytes, size < room ? size : room);
    }
    out->size += size;
}

// Writes an uncompressed iTXt chunk of `keyword` and `text`, with no language.
void writeText(Output* out, PrStringView keyword, PrStringView text) {
    // The keyword's terminator, the compression flag and method, and the empty language and translated keyword.
    static const uint8_t kFields[] = {0, 0, 0, 0, 0};
    size_t length = keyword.size + sizeof(kFields) + text.size;
    uint8_t head[8];
    storeU32(head, uint32_t(length));
    prMemCopy(head + 4, "iTXt", 4);
    write(out, head, sizeof(head));
    write(out, keyword.data, keyword.size);
    write(out, kFields, sizeof(kFields));
    write(out, text.data, text.size);
    uint32_t crc = prCrc32Update(0, PrByteView{head + 4, 4});
    crc = prCrc32Update(crc, PrByteView{reinterpret_cast<const uint8_t*>(keyword.data), keyword.size});
    crc = prCrc32Update(crc, PrByteView{kFields, sizeof(kFields)});
    crc = prCrc32Update(crc, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    uint8_t tail[4];
    storeU32(tail, crc);
    write(out, tail, sizeof(tail));
}

bool isKeyword(PrStringView keyword) {
    if (keyword.size == 0 || keyword.size > kPrImagePngKeywordMaxBytes || keyword.data[0] == ' ' ||
            keyword.data[keyword.size - 1] == ' ')
        return false;
    for (size_t i = 0; i < keyword.size; ++i) {
        char c = keyword.data[i];
        if (c < ' ' || c > '~' || (c == ' ' && keyword.data[i - 1] == ' '))
            return false;
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prImagePngEachText(PrByteView png, PrCompressCodec* PR_NULLABLE compress, PrImagePngTextFn fn,
        void* PR_NULLABLE context) {
    if (!hasHeader(png))
        return PrStatus_InvalidData;
    size_t at = sizeof(kSignature);
    Chunk chunk;
    while (readChunk(png, &at, &chunk)) {
        if (isType(chunk, "IEND"))
            return PrStatus_Ok;
        if (chunk.crcMatches && isTextType(chunk))
            readText(chunk, compress, fn, context);
    }
    return PrStatus_Truncated;
}

PrStatus prImagePngSetText(PrByteView png, PrStringView keyword, PrStringView text, uint8_t* PR_NULLABLE out,
        size_t capacity, size_t* size) {
    PR_ASSERT(isKeyword(keyword));
    *size = 0;
    if (!hasHeader(png))
        return PrStatus_InvalidData;
    Output output{out, capacity, 0};
    write(&output, kSignature, sizeof(kSignature));
    size_t at = sizeof(kSignature);
    bool written = false;
    bool hasData = false;
    Chunk chunk;
    while (readChunk(png, &at, &chunk)) {
        if (isTextType(chunk) && keywordOf(chunk) == keyword) {
            if (!written)
                writeText(&output, keyword, text);
            written = true;
            continue;
        }
        if (isType(chunk, "IDAT")) {
            hasData = true;
            if (!written)
                writeText(&output, keyword, text);
            written = true;
        }
        write(&output, chunk.whole.data, chunk.whole.size);
    }
    if (!hasData)
        return PrStatus_InvalidData;
    // Bytes past the last whole chunk are copied as they are.
    write(&output, png.data + at, png.size - at);
    *size = output.size;
    return PrStatus_Ok;
}
