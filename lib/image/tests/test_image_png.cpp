#include "pr/checksum.h"
#include "pr/compress.h"
#include "pr/image_png.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A text as a test records it.
struct Text {
    std::string keyword;
    std::string text;
    std::string language;
    std::string translatedKeyword;
};

void appendU32(std::vector<uint8_t>* out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out->push_back(uint8_t(value >> shift));
}

// Appends a chunk of `type` and `data`, with its CRC.
void appendChunk(std::vector<uint8_t>* out, const char* type, const std::string& data) {
    appendU32(out, uint32_t(data.size()));
    size_t start = out->size();
    out->insert(out->end(), type, type + 4);
    out->insert(out->end(), data.begin(), data.end());
    appendU32(out, crc32Update(0, PrByteView{out->data() + start, out->size() - start}));
}

// Returns a PNG file of a header, the chunks of `before`, image data, the chunks of `after`, and an end. The chunks are
// well formed, and the pixels are not.
std::vector<uint8_t> pngWith(const std::vector<std::pair<const char*, std::string>>& before,
        const std::vector<std::pair<const char*, std::string>>& after = {}) {
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    appendChunk(&png, "IHDR", std::string(13, '\1'));
    for (const auto& [type, data] : before)
        appendChunk(&png, type, data);
    appendChunk(&png, "IDAT", "pixels");
    for (const auto& [type, data] : after)
        appendChunk(&png, type, data);
    appendChunk(&png, "IEND", "");
    return png;
}

std::string stringOf(PrStringView view) {
    return std::string(view.data, view.size);
}

PrStatus textsOf(const std::vector<uint8_t>& png, PrCompressCodec* compress, std::vector<Text>* out) {
    auto collect = [](void* context, const PrImagePngText* text) {
        static_cast<std::vector<Text>*>(context)->push_back({
            stringOf(text->keyword), stringOf(text->text),
            stringOf(text->language),
            stringOf(text->translatedKeyword),
        });
    };
    return prImagePngEachText(png, compress, collect, out);
}

std::string zlibOf(PrCompressCodec* codec, const std::string& text) {
    std::vector<uint8_t> out(prCompressCodecEncodeBound(codec, PrCompressFormat_Zlib, text.size()));
    size_t written = 0;
    PrByteView input{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    REQUIRE(prCompressCodecEncode(codec, PrCompressFormat_Zlib, PrCompressLevel_Default, input, out.data(), &written) ==
        PrStatus_Ok);
    return std::string(reinterpret_cast<const char*>(out.data()), written);
}

std::vector<uint8_t> withText(const std::vector<uint8_t>& png, PrStringView keyword, PrStringView text) {
    size_t size = 0;
    REQUIRE(prImagePngSetText(png, keyword, text, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> out(size);
    size_t written = 0;
    REQUIRE(prImagePngSetText(png, keyword, text, out.data(), out.size(), &written) == PrStatus_Ok);
    CHECK(written == size);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("each kind of text chunk reads as UTF-8, with Latin-1 converted") {
    PrCompressCodec* codec = prCompressCodecCreate(PrCompressCodecBackend_Default);
    REQUIRE(codec != nullptr);
    std::string itxt = std::string("Title\0\0\0fr\0Titre\0", 17) + "caf\xC3\xA9";
    std::string compressed = std::string("Note\0\1\0\0\0", 9) + zlibOf(codec, "long \xE2\x9C\x93");
    std::vector<uint8_t> png = pngWith({
        {"tEXt", std::string("Author\0Ren\xE9", 11)},
        {"zTXt", std::string("Comment\0\0", 9) + zlibOf(codec, "squeezed")},
        {"iTXt", itxt},
    }, {{"iTXt", compressed}});
    std::vector<Text> texts;
    REQUIRE(textsOf(png, codec, &texts) == PrStatus_Ok);
    REQUIRE(texts.size() == 4);
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
    REQUIRE(texts.size() == 2);
    CHECK(texts[1].keyword == "Title");
    prCompressCodecDestroy(codec);
}

TEST_CASE("a malformed text chunk is skipped, and a file that is not a PNG fails") {
    std::vector<uint8_t> png = pngWith({
        {"tEXt", std::string("NoTerminator")},
        {"iTXt", std::string("Bad\0\0\0\0\0\xFF", 9)},
        {"tEXt", std::string("Good\0yes", 8)},
    });
    // A chunk whose CRC does not match.
    std::vector<uint8_t> corrupt = pngWith({{"tEXt", std::string("Flipped\0bits", 12)}});
    corrupt[8 + 25 + 8 + 9] ^= 1;
    std::vector<Text> texts;
    REQUIRE(textsOf(png, nullptr, &texts) == PrStatus_Ok);
    REQUIRE(texts.size() == 1);
    CHECK(texts[0].keyword == "Good");
    texts.clear();
    REQUIRE(textsOf(corrupt, nullptr, &texts) == PrStatus_Ok);
    CHECK(texts.empty());

    std::vector<uint8_t> truncated(png.begin(), png.end() - 4);
    CHECK(textsOf(truncated, nullptr, &texts) == PrStatus_Truncated);
    std::vector<uint8_t> notPng{'G', 'I', 'F', '8', '9', 'a'};
    CHECK(textsOf(notPng, nullptr, &texts) == PrStatus_InvalidData);
}

TEST_CASE("a set text replaces every chunk of its keyword, where the first was, and keeps the rest") {
    std::vector<uint8_t> png = pngWith({
        {"tEXt", std::string("Other\0kept", 10)},
        {"tEXt", std::string("Record\0old", 10)},
    }, {{"tEXt", std::string("Record\0older", 12)}});
    std::vector<uint8_t> out = withText(png, "Record", "new \xE2\x9C\x93");
    std::vector<Text> texts;
    REQUIRE(textsOf(out, nullptr, &texts) == PrStatus_Ok);
    REQUIRE(texts.size() == 2);
    CHECK(texts[0].keyword == "Other");
    CHECK(texts[1].keyword == "Record");
    CHECK(texts[1].text == "new \xE2\x9C\x93");

    // With no chunk of the keyword, the text goes before the image data.
    std::vector<uint8_t> plain = pngWith({});
    std::vector<uint8_t> added = withText(plain, "Record", "first");
    texts.clear();
    REQUIRE(textsOf(added, nullptr, &texts) == PrStatus_Ok);
    REQUIRE(texts.size() == 1);
    CHECK(texts[0].text == "first");
    std::string bytes(reinterpret_cast<const char*>(added.data()), added.size());
    CHECK(bytes.find("iTXt") < bytes.find("IDAT"));

    // A short buffer is filled as far as it goes, and the size is the whole file's.
    std::vector<uint8_t> partial(10);
    size_t size = 0;
    CHECK(prImagePngSetText(plain, "Record", "first", partial.data(), partial.size(), &size) == PrStatus_Ok);
    CHECK(size == added.size());
    CHECK(std::vector<uint8_t>(added.begin(), added.begin() + 10) == partial);

    std::vector<uint8_t> headerOnly(plain.begin(), plain.begin() + 33);
    CHECK(prImagePngSetText(headerOnly, "Record", "x", nullptr, 0, &size) == PrStatus_InvalidData);
    CHECK(size == 0);
}
