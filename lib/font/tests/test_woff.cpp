#include "pr/compress_builtin.h"
#include "pr/font.h"
#include "pr/path.h"

#include "tools/harness/test.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

std::vector<uint8_t> readFile(const std::string& name) {
    const std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    uint8_t buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
    return bytes;
}

uint32_t be32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

uint16_t be16(const uint8_t* bytes) {
    return uint16_t(bytes[0] << 8 | bytes[1]);
}

// Creates the font of the WOFF or WOFF2 file `name`, at `index`, or returns the status.
PrStatus loadWoff(const std::string& name, uint32_t index, const PrFont** font) {
    std::vector<uint8_t> bytes = readFile(name);
    PrCompressCodec* codec = prCompressBuiltinCreate();
    const PrStatus status = prFontCreateWoff(bytes, index, nullptr, codec, font);
    prCompressCodecDestroy(codec);
    return status;
}

const PrFont* loadFont(const std::string& name) {
    std::vector<uint8_t> bytes = readFile(name);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Returns the unhinted outline of the glyph as its verbs and points.
std::pair<std::vector<PrPathVerb>, std::vector<PrVec2>> outline(const PrFont* font, uint16_t glyph) {
    PrPath* path = prPathCreate();
    prFontGlyphOutline(font, glyph, 0, prAffine2Identity(), path);
    std::pair<std::vector<PrPathVerb>, std::vector<PrVec2>> out;
    if (prPathVerbCount(path) != 0)
        out.first.assign(prPathVerbs(path), prPathVerbs(path) + prPathVerbCount(path));
    for (uint32_t i = 0; i < prPathPointCount(path); ++i)
        out.second.push_back(prPathPoints(path)[i]);
    prPathDestroy(path);
    return out;
}

bool sameOutline(const PrFont* a, uint16_t glyphA, const PrFont* b, uint16_t glyphB) {
    const std::pair<std::vector<PrPathVerb>, std::vector<PrVec2>> first = outline(a, glyphA);
    const std::pair<std::vector<PrPathVerb>, std::vector<PrVec2>> second = outline(b, glyphB);
    if (first.first != second.first || first.second.size() != second.second.size())
        return false;
    for (size_t i = 0; i < first.second.size(); ++i)
        if (first.second[i].x != second.second[i].x || first.second[i].y != second.second[i].y)
            return false;
    return true;
}

// Checks that the two fonts map printable ASCII to glyphs of the same outlines.
void checkSameOutlines(const PrFont* a, const PrFont* b) {
    for (uint32_t c = 0x20; c < 0x7f; ++c) {
        CAPTURE(c);
        const uint16_t glyphA = prFontCodePointGlyph(a, c);
        const uint16_t glyphB = prFontCodePointGlyph(b, c);
        CHECK((glyphA == 0) == (glyphB == 0));
        CHECK(sameOutline(a, glyphA, b, glyphB));
    }
}

// Returns the sum of the big-endian 32-bit words of a font file, which the checksum adjustment of a whole font makes
// 0xb1b0afba.
uint32_t fileSum(PrByteView bytes) {
    uint32_t sum = 0;
    for (size_t at = 0; at + 4 <= bytes.size; at += 4)
        sum += be32(bytes.data + at);
    return sum;
}

// A table record of an sfnt.
struct TableRecord {
    uint32_t tag;
    uint32_t checksum;
    uint32_t offset;
    uint32_t length;
};

std::vector<TableRecord> tableRecords(PrByteView font) {
    std::vector<TableRecord> records;
    const uint16_t count = be16(font.data + 4);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* record = font.data + 12 + 16 * i;
        records.push_back({be32(record), be32(record + 4), be32(record + 8), be32(record + 12)});
    }
    return records;
}

// Writes the fields of a stream bit by bit, first bit lowest.
struct Bits {
    std::vector<uint8_t> bytes;
    uint32_t bit = 0;

    void put(uint32_t value, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i, ++bit) {
            if (bit % 8 == 0)
                bytes.push_back(0);
            bytes.back() |= uint8_t(((value >> i) & 1) << (bit % 8));
        }
    }

    void align() { bit = (bit + 7) / 8 * 8; }
};

// Returns a Brotli stream of `data` in uncompressed meta-blocks (rfc 7932 9.2), which needs no encoder.
std::vector<uint8_t> storedBrotli(const std::vector<uint8_t>& data) {
    Bits w;
    w.put(0, 1); // WBITS 16
    for (size_t at = 0; at < data.size(); at += 65536) {
        const size_t size = std::min<size_t>(65536, data.size() - at);
        w.put(0, 1);              // ISLAST
        w.put(0, 2);              // four nibbles
        w.put(uint32_t(size - 1), 16);
        w.put(1, 1);              // ISUNCOMPRESSED
        w.align();
        w.bytes.insert(w.bytes.end(), data.begin() + long(at), data.begin() + long(at + size));
        w.bit += uint32_t(size * 8);
    }
    w.put(1, 1); // ISLAST
    w.put(1, 1); // ISLASTEMPTY
    return w.bytes;
}

void putBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 24));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

void putBase128(std::vector<uint8_t>& out, uint32_t value) {
    uint8_t digits[5];
    int count = 0;
    do {
        digits[count++] = uint8_t(value & 0x7f);
        value >>= 7;
    } while (value != 0);
    while (count-- > 0)
        out.push_back(uint8_t(digits[count] | (count != 0 ? 0x80 : 0)));
}

// Returns a WOFF2 file of the font file `font`, with no table transformed and the table data stored. The glyf and loca
// tables are named by their known tags and the null transform, and every other table by its own tag. The file ends with
// the compressed data, padded to a four-byte boundary if `padded` is set.
std::vector<uint8_t> woff2Of(PrByteView font, bool padded = true) {
    const std::vector<TableRecord> records = tableRecords(font);
    std::vector<uint8_t> directory;
    std::vector<uint8_t> block;
    for (const TableRecord& record : records) {
        if (record.tag == 0x676c7966)
            directory.push_back(10 | 0xc0);
        else if (record.tag == 0x6c6f6361)
            directory.push_back(11 | 0xc0);
        else {
            directory.push_back(63);
            putBe32(directory, record.tag);
        }
        putBase128(directory, record.length);
        block.insert(block.end(), font.data + record.offset, font.data + record.offset + record.length);
    }
    const std::vector<uint8_t> compressed = storedBrotli(block);
    std::vector<uint8_t> file;
    putBe32(file, 0x774f4632);
    putBe32(file, be32(font.data));
    putBe32(file, 0); // length, below
    file.push_back(uint8_t(records.size() >> 8));
    file.push_back(uint8_t(records.size()));
    file.push_back(0);
    file.push_back(0);
    putBe32(file, uint32_t(font.size));
    putBe32(file, uint32_t(compressed.size()));
    putBe32(file, 0x00010000);
    for (int i = 0; i < 5; ++i)
        putBe32(file, 0);
    file.insert(file.end(), directory.begin(), directory.end());
    file.insert(file.end(), compressed.begin(), compressed.end());
    while (padded && file.size() % 4 != 0)
        file.push_back(0);
    const uint32_t length = uint32_t(file.size());
    file[8] = uint8_t(length >> 24);
    file[9] = uint8_t(length >> 16);
    file[10] = uint8_t(length >> 8);
    file[11] = uint8_t(length);
    return file;
}

// A codec that decodes nothing.
PrStatus refuseDecoder(PrCompressCodec*, PrCompressFormat, PrCompressDecoder* PR_NULLABLE* PR_NONNULL) {
    return PrStatus_NotSupported;
}
void destroyNothing(PrCompressCodec*, PrCompressDecoder*) {}
PrStatus readNothing(PrCompressCodec*, PrCompressDecoder*, PrByteView, bool, uint8_t* PR_NULLABLE, size_t,
        PrCompressRead*) {
    return PrStatus_NotSupported;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a WOFF file's font has the tables it wraps, compressed or stored") {
    const PrFont* font = nullptr;
    REQUIRE(loadWoff("AD.woff", 0, &font) == PrStatus_Ok);
    // glyf, loca and hmtx are compressed; gasp and maxp are stored.
    CHECK(prFontGlyphCount(font) > 2);
    CHECK(prFontCodePointGlyph(font, 'A') != 0);
    CHECK(prFontCodePointGlyph(font, 'D') != 0);
    CHECK(!prRectIsEmpty(prFontGlyphBounds(font, prFontCodePointGlyph(font, 'A'))));
    prFontRelease(font);
    // Anything else is not a font.
    PrCompressCodec* codec = prCompressBuiltinCreate();
    const uint8_t junk[] = {'w', 'O', 'F', 'F', 0, 1, 0, 0};
    CHECK(prFontCreateWoff(junk, 0, nullptr, codec, &font) == PrStatus_InvalidData);
    const uint8_t woff2[] = {'w', 'O', 'F', '2'};
    CHECK(prFontCreateWoff(PrByteView{woff2, 4}, 0, nullptr, codec, &font) == PrStatus_InvalidData);
    CHECK(font == nullptr);
    prCompressCodecDestroy(codec);
}

TEST_CASE("a WOFF2 file holds its font's tables, with their checksums and the font's") {
    // Tinos as WOFF2, each table stored as it is, so that the font decodes to the same tables.
    std::vector<uint8_t> ttf = readFile("Tinos.ttf");
    const std::vector<uint8_t> woff2 = woff2Of(ttf);
    PrCompressCodec* codec = prCompressBuiltinCreate();
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateWoff(woff2, 0, nullptr, codec, &font) == PrStatus_Ok);
    prCompressCodecDestroy(codec);

    const PrByteView decoded = prFontData(font);
    CHECK(fileSum(decoded) == 0xb1b0afbau);
    const std::vector<TableRecord> original = tableRecords(ttf);
    const std::vector<TableRecord> records = tableRecords(decoded);
    REQUIRE(records.size() == original.size());
    for (size_t i = 0; i < records.size(); ++i) {
        const TableRecord& record = records[i];
        CAPTURE(record.tag);
        CHECK(record.tag == original[i].tag);
        CHECK(record.length == original[i].length);
        CHECK(record.offset % 4 == 0);
        std::vector<uint8_t> table(decoded.data + record.offset, decoded.data + record.offset + record.length);
        std::vector<uint8_t> expected(ttf.data() + original[i].offset,
            ttf.data() + original[i].offset + original[i].length);
        if (record.tag == 0x68656164) {
            // The checksum adjustment of head counts as zero in the checksum of head.
            for (int k = 8; k < 12; ++k)
                table[size_t(k)] = expected[size_t(k)] = 0;
        }
        CHECK(table == expected);
        uint32_t sum = 0;
        for (size_t at = 0; at < table.size(); at += 4) {
            uint8_t word[4] = {};
            for (size_t k = 0; k < 4 && at + k < table.size(); ++k)
                word[k] = table[at + k];
            sum += be32(word);
        }
        CHECK(record.checksum == sum);
    }

    const PrFont* reference = loadFont("Tinos.ttf");
    CHECK(prFontGlyphCount(font) == prFontGlyphCount(reference));
    for (uint16_t glyph = 0; glyph < prFontGlyphCount(reference); glyph += 7) {
        CAPTURE(glyph);
        CHECK(sameOutline(font, glyph, reference, glyph));
        CHECK(prFontGlyphAdvance(font, glyph, 0) == prFontGlyphAdvance(reference, glyph, 0));
    }
    prFontRelease(reference);
    prFontRelease(font);
}

TEST_CASE("a WOFF2 file's transformed glyf, loca and hmtx tables reconstruct its glyphs") {
    // The WOFF2 fonts of WPT draw the glyphs of the reference fonts: TrueType with glyf and loca transformed, TrueType
    // with hmtx transformed too, and CFF.
    const PrFont* truetype = loadFont("woff2/SFNT-TTF-Reference.ttf");
    for (const char* name : {"woff2/valid-005.woff2", "woff2/tabledata-transform-hmtx-001.woff2"}) {
        CAPTURE(name);
        const PrFont* font = nullptr;
        REQUIRE(loadWoff(name, 0, &font) == PrStatus_Ok);
        CHECK(fileSum(prFontData(font)) == 0xb1b0afbau);
        checkSameOutlines(font, truetype);
        prFontRelease(font);
    }
    prFontRelease(truetype);
    const PrFont* cff = loadFont("woff2/SFNT-CFF-Reference.otf");
    const PrFont* font = nullptr;
    REQUIRE(loadWoff("woff2/valid-001.woff2", 0, &font) == PrStatus_Ok);
    checkSameOutlines(font, cff);
    prFontRelease(font);
    prFontRelease(cff);
}

TEST_CASE("a WOFF2 collection holds each of its fonts") {
    const PrFont* fonts[2] = {};
    for (uint32_t index = 0; index < 2; ++index) {
        REQUIRE(loadWoff("woff2/available-002.woff2", index, &fonts[index]) == PrStatus_Ok);
        CHECK(prFontIndex(fonts[index]) == index);
        CHECK(prFontCount(prFontData(fonts[index])) == 2);
        CHECK(prFontCodePointGlyph(fonts[index], 'P') != 0);
    }
    PrRawString first = prFontFamilyName(fonts[0]);
    PrRawString second = prFontFamilyName(fonts[1]);
    CHECK(std::string(prRawStringView(&first).data, prRawStringView(&first).size) == "WOFF Test TTF 0");
    CHECK(std::string(prRawStringView(&second).data, prRawStringView(&second).size) == "WOFF Test TTF 1");
    prRawStringReset(&first);
    prRawStringReset(&second);
    prFontRelease(fonts[0]);
    prFontRelease(fonts[1]);
    const PrFont* font = nullptr;
    CHECK(loadWoff("woff2/available-002.woff2", 2, &font) == PrStatus_InvalidData);
}

TEST_CASE("a WOFF2 file that breaks the format's rules is not a font") {
    // A composite glyph without a bounding box, table data in zlib instead of Brotli, a length field that is not the
    // length of the file, and a collection whose glyf and loca tables are not paired.
    for (const char* name : {"woff2/tabledata-glyf-bbox-002.woff2", "woff2/tabledata-brotli-001.woff2",
        "woff2/header-length-001.woff2", "woff2/directory-mismatched-tables-001.woff2"}) {
        CAPTURE(name);
        const PrFont* font = nullptr;
        CHECK(loadWoff(name, 0, &font) == PrStatus_InvalidData);
        CHECK(font == nullptr);
    }
    // A codec that cannot decode Brotli cannot decode the file.
    std::vector<uint8_t> bytes = readFile("woff2/valid-005.woff2");
    PrCompressCodec refusing{nullptr, refuseDecoder, destroyNothing, readNothing, nullptr, nullptr};
    const PrFont* font = nullptr;
    CHECK(prFontCreateWoff(bytes, 0, nullptr, &refusing, &font) == PrStatus_NotSupported);
}

TEST_CASE("a WOFF2 file ends with its compressed data, or with the padding after it") {
    std::vector<uint8_t> ttf = readFile("Tinos.ttf");
    const std::vector<uint8_t> padded = woff2Of(ttf);
    const std::vector<uint8_t> unpadded = woff2Of(ttf, false);
    REQUIRE(unpadded.size() % 4 != 0);
    PrCompressCodec* codec = prCompressBuiltinCreate();
    for (const std::vector<uint8_t>* file : {&padded, &unpadded}) {
        const PrFont* font = nullptr;
        CHECK(prFontCreateWoff(*file, 0, nullptr, codec, &font) == PrStatus_Ok);
        prFontRelease(font);
    }
    // Four bytes past the padding are part of no block.
    std::vector<uint8_t> extended = padded;
    extended.insert(extended.end(), 4, 0);
    const uint32_t length = uint32_t(extended.size());
    extended[8] = uint8_t(length >> 24);
    extended[9] = uint8_t(length >> 16);
    extended[10] = uint8_t(length >> 8);
    extended[11] = uint8_t(length);
    const PrFont* font = nullptr;
    CHECK(prFontCreateWoff(extended, 0, nullptr, codec, &font) == PrStatus_InvalidData);
    prCompressCodecDestroy(codec);
}

TEST_CASE("a WOFF2 file whose compressed data runs past its end is not a font, whatever its codec") {
    // One cmap table of one byte, and compressed data of 2^32 - 1 bytes, of which the file holds two. The end of that
    // data, aligned to four bytes in 32 bits, would wrap to the length of the file.
    std::vector<uint8_t> bytes(52, 0);
    bytes[0] = 'w';
    bytes[1] = 'O';
    bytes[2] = 'F';
    bytes[3] = '2';
    bytes[5] = 1;
    bytes[11] = 52;
    bytes[13] = 1;
    bytes[20] = bytes[21] = bytes[22] = bytes[23] = 255;
    bytes[49] = 1;
    // The codec would say NotSupported if it were asked to decode anything.
    PrCompressCodec refusing{nullptr, refuseDecoder, destroyNothing, readNothing, nullptr, nullptr};
    const PrFont* font = nullptr;
    CHECK(prFontCreateWoff(bytes, 0, nullptr, &refusing, &font) == PrStatus_InvalidData);
    PrCompressCodec* codec = prCompressBuiltinCreate();
    CHECK(prFontCreateWoff(bytes, 0, nullptr, codec, &font) == PrStatus_InvalidData);
    CHECK(font == nullptr);
    prCompressCodecDestroy(codec);
}
