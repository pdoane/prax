#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/compress.h"
#include "pr/image_png.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A text as a test records it.
struct Text {
    String keyword;
    String text;
    String language;
    String translatedKeyword;
};

// A chunk that a test writes: its type and its data.
struct ChunkData {
    const char* type;
    String data;
};

void appendU32(Array<uint8_t>* out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out->push(uint8_t(value >> shift));
}

// Appends a chunk of `type` and `data`, with its CRC.
void appendChunk(Array<uint8_t>* out, const char* type, PrStringView data) {
    appendU32(out, uint32_t(data.size));
    uint32_t start = out->count();
    out->append(reinterpret_cast<const uint8_t*>(type), 4);
    out->append(prStringBytes(data));
    appendU32(out, prCrc32Update(0, PrByteView{out->data() + start, out->count() - start}));
}

// Returns a PNG file of a header, the chunks of `before`, image data, the chunks of `after`, and an end. The chunks are
// well formed, and the pixels are not.
Array<uint8_t> pngWith(Span<const ChunkData> before, Span<const ChunkData> after = {}) {
    Array<uint8_t> png({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    appendChunk(&png, "IHDR", "\1\1\1\1\1\1\1\1\1\1\1\1\1");
    for (const ChunkData& chunk : before)
        appendChunk(&png, chunk.type, chunk.data);
    appendChunk(&png, "IDAT", "pixels");
    for (const ChunkData& chunk : after)
        appendChunk(&png, chunk.type, chunk.data);
    appendChunk(&png, "IEND", "");
    return png;
}

PrStatus textsOf(PrByteView png, PrCompressCodec* compress, Array<Text>* out) {
    auto collect = [](void* context, const PrImagePngText* text) {
        static_cast<Array<Text>*>(context)->push(Text{
            .keyword = text->keyword,
            .text = text->text,
            .language = text->language,
            .translatedKeyword = text->translatedKeyword,
        });
    };
    return prImagePngEachText(png, compress, collect, out);
}

String zlibOf(PrCompressCodec* codec, PrStringView text) {
    Array<uint8_t> out(prCompressCodecEncodeBound(codec, PrCompressFormat_Zlib, text.size));
    size_t written = 0;
    REQUIRE(prCompressCodecEncode(codec, PrCompressFormat_Zlib, PrCompressLevel_Default, prStringBytes(text),
        out.data(), &written) == PrStatus_Ok);
    return String(PrStringView(reinterpret_cast<const char*>(out.data()), written));
}

// Returns `head`, which may hold U+0000, followed by `tail`.
String joined(PrStringView head, PrStringView tail = "") {
    String out(head);
    out.append(tail);
    return out;
}

Array<uint8_t> withText(PrByteView png, PrStringView keyword, PrStringView text) {
    size_t size = 0;
    REQUIRE(prImagePngSetText(png, keyword, text, nullptr, 0, &size) == PrStatus_Ok);
    Array<uint8_t> out(size);
    size_t written = 0;
    REQUIRE(prImagePngSetText(png, keyword, text, out.data(), out.count(), &written) == PrStatus_Ok);
    CHECK(written == size);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("each kind of text chunk reads as UTF-8, with Latin-1 converted") {
    PrCompressCodec* codec = prCompressCodecCreate(PrCompressCodecBackend_Default);
    REQUIRE(codec != nullptr);
    String itxt = joined(PrStringView("Title\0\0\0fr\0Titre\0", 17), "caf\xC3\xA9");
    String compressed = joined(PrStringView("Note\0\1\0\0\0", 9), zlibOf(codec, "long \xE2\x9C\x93"));
    const ChunkData before[] = {
        {"tEXt", joined(PrStringView("Author\0Ren\xE9", 11))},
        {"zTXt", joined(PrStringView("Comment\0\0", 9), zlibOf(codec, "squeezed"))},
        {"iTXt", itxt},
    };
    const ChunkData after[] = {{"iTXt", compressed}};
    Array<uint8_t> png = pngWith(Span(before).asConst(), Span(after).asConst());
    Array<Text> texts;
    REQUIRE(textsOf(png, codec, &texts) == PrStatus_Ok);
    REQUIRE(texts.count() == 4);
    CHECK(texts[0].keyword == "Author");
    CHECK(texts[0].text == "Ren\xC3\xA9");
    CHECK(texts[1].keyword == "Comment");
    CHECK(texts[1].text == "squeezed");
    CHECK(texts[2].keyword == "Title");
    CHECK(texts[2].text == "caf\xC3\xA9");
    CHECK(texts[2].language == "fr");
    CHECK(texts[2].translatedKeyword == "Titre");
    CHECK(texts[3].text == "long \xE2\x9C\x93");

    // Without a codec, the compressed texts are skipped.
    texts.clear();
    REQUIRE(textsOf(png, nullptr, &texts) == PrStatus_Ok);
    REQUIRE(texts.count() == 2);
    CHECK(texts[1].keyword == "Title");
    prCompressCodecDestroy(codec);
}

TEST_CASE("a malformed text chunk is skipped, and a file that is not a PNG fails") {
    const ChunkData chunks[] = {
        {"tEXt", "NoTerminator"},
        {"iTXt", joined(PrStringView("Bad\0\0\0\0\0\xFF", 9))},
        {"tEXt", joined(PrStringView("Good\0yes", 8))},
    };
    Array<uint8_t> png = pngWith(Span(chunks).asConst());
    // A chunk whose CRC does not match.
    const ChunkData flipped[] = {{"tEXt", joined(PrStringView("Flipped\0bits", 12))}};
    Array<uint8_t> corrupt = pngWith(Span(flipped).asConst());
    corrupt[8 + 25 + 8 + 9] ^= 1;
    Array<Text> texts;
    REQUIRE(textsOf(png, nullptr, &texts) == PrStatus_Ok);
    REQUIRE(texts.count() == 1);
    CHECK(texts[0].keyword == "Good");
    texts.clear();
    REQUIRE(textsOf(corrupt, nullptr, &texts) == PrStatus_Ok);
    CHECK(texts.isEmpty());

    Array<uint8_t> truncated;
    truncated.append(png.data(), png.count() - 4);
    CHECK(textsOf(truncated, nullptr, &texts) == PrStatus_Truncated);
    Array<uint8_t> notPng({'G', 'I', 'F', '8', '9', 'a'});
    CHECK(textsOf(notPng, nullptr, &texts) == PrStatus_InvalidData);
}

TEST_CASE("a set text replaces every chunk of its keyword, where the first was, and keeps the rest") {
    const ChunkData before[] = {
        {"tEXt", joined(PrStringView("Other\0kept", 10))},
        {"tEXt", joined(PrStringView("Record\0old", 10))},
    };
    const ChunkData after[] = {{"tEXt", joined(PrStringView("Record\0older", 12))}};
    Array<uint8_t> png = pngWith(Span(before).asConst(), Span(after).asConst());
    Array<uint8_t> out = withText(png, "Record", "new \xE2\x9C\x93");
    Array<Text> texts;
    REQUIRE(textsOf(out, nullptr, &texts) == PrStatus_Ok);
    REQUIRE(texts.count() == 2);
    CHECK(texts[0].keyword == "Other");
    CHECK(texts[1].keyword == "Record");
    CHECK(texts[1].text == "new \xE2\x9C\x93");

    // With no chunk of the keyword, the text goes before the image data.
    Array<uint8_t> plain = pngWith({});
    Array<uint8_t> added = withText(plain, "Record", "first");
    texts.clear();
    REQUIRE(textsOf(added, nullptr, &texts) == PrStatus_Ok);
    REQUIRE(texts.count() == 1);
    CHECK(texts[0].text == "first");
    PrStringView bytes(reinterpret_cast<const char*>(added.data()), added.count());
    CHECK(prStringFind(bytes, "iTXt") < prStringFind(bytes, "IDAT"));

    // A short buffer is filled as far as it goes, and the size is the whole file's.
    Array<uint8_t> partial(10);
    size_t size = 0;
    CHECK(prImagePngSetText(plain, "Record", "first", partial.data(), partial.count(), &size) == PrStatus_Ok);
    CHECK(size == added.count());
    CHECK(PrByteView(added.data(), 10) == partial);

    Array<uint8_t> headerOnly;
    headerOnly.append(plain.data(), 33);
    CHECK(prImagePngSetText(headerOnly, "Record", "x", nullptr, 0, &size) == PrStatus_InvalidData);
    CHECK(size == 0);
}
