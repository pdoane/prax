// WOFF and WOFF2: the font file that a web font file holds.

#include "font/src/woff.h"

#include "pr/assert.h"
#include "pr/font.h"
#include "pr/mem.h"

namespace pr_impl {
namespace {

// The largest font file that a web font file decodes to. A file that claims more is rejected instead of allocated.
constexpr uint64_t kMaxFontSize = uint64_t(1) << 30;

// The size of an sfnt offset table, and of one of its table records.
constexpr uint32_t kOffsetTableSize = 12;
constexpr uint32_t kTableRecordSize = 16;

inline void writeBeU16(uint8_t* bytes, uint16_t value) {
    bytes[0] = uint8_t(value >> 8);
    bytes[1] = uint8_t(value);
}

inline uint64_t align4(uint64_t value) {
    return (value + 3) & ~uint64_t(3);
}

// Returns the checksum of an sfnt table: the sum of its big-endian 32-bit words, with the last word padded with zeros.
uint32_t tableChecksum(const uint8_t* data, size_t size) {
    uint32_t sum = 0;
    size_t at = 0;
    for (; at + 4 <= size; at += 4)
        sum += prMemReadBeU32(data + at);
    if (at < size) {
        uint8_t last[4]{};
        prMemCopy(last, data + at, size - at);
        sum += prMemReadBeU32(last);
    }
    return sum;
}

// Writes an sfnt offset table for `tables` tables of `flavor` (the OpenType "Table Directory").
void writeOffsetTable(uint8_t* out, uint32_t flavor, uint16_t tables) {
    uint16_t power = 1;
    uint16_t selector = 0;
    while (uint32_t(power) * 2 <= tables) {
        power = uint16_t(power * 2);
        ++selector;
    }
    prMemWriteBeU32(out, flavor);
    writeBeU16(out + 4, tables);
    writeBeU16(out + 6, uint16_t(power * 16));
    writeBeU16(out + 8, selector);
    writeBeU16(out + 10, uint16_t(tables * 16 - power * 16));
}

// A bounds-checked read of big-endian data. A read past the end returns zero and marks the reader failed.
struct Reader {
    const uint8_t* PR_NULLABLE data;
    size_t size;
    size_t at;
    bool failed;
};

Reader readerOf(const uint8_t* PR_NULLABLE data, size_t size) {
    return Reader{data, size, 0, false};
}

// Returns the next `count` bytes, or null past the end.
const uint8_t* PR_NULLABLE readBytes(Reader& r, size_t count) {
    if (r.size - r.at < count) {
        r.failed = true;
        r.at = r.size;
        return nullptr;
    }
    const uint8_t* bytes = r.data + r.at;
    r.at += count;
    return bytes;
}

uint8_t readU8(Reader& r) {
    const uint8_t* bytes = readBytes(r, 1);
    return bytes != nullptr ? bytes[0] : 0;
}

uint16_t readU16(Reader& r) {
    const uint8_t* bytes = readBytes(r, 2);
    return bytes != nullptr ? prMemReadBeU16(bytes) : 0;
}

int16_t readS16(Reader& r) {
    return int16_t(readU16(r));
}

uint32_t readU32(Reader& r) {
    const uint8_t* bytes = readBytes(r, 4);
    return bytes != nullptr ? prMemReadBeU32(bytes) : 0;
}

// Reads a 255UInt16 (woff2 3.1). Every encoding of a value is accepted.
uint16_t read255UInt16(Reader& r) {
    const uint8_t code = readU8(r);
    if (code == 253)
        return readU16(r);
    if (code == 255)
        return uint16_t(253 + readU8(r));
    if (code == 254)
        return uint16_t(506 + readU8(r));
    return code;
}

// Reads a UIntBase128 (woff2 3.1). Returns false for an encoding with a leading zero, an encoding of more than five
// bytes, or a value past 2^32 - 1.
bool readBase128(Reader& r, uint32_t* out) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        const uint8_t byte = readU8(r);
        if (r.failed || (i == 0 && byte == 0x80) || (value & 0xfe000000u) != 0)
            return false;
        value = value << 7 | (byte & 0x7f);
        if ((byte & 0x80) == 0) {
            *out = value;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// WOFF

// The size of the WOFF header, and of a table directory entry (woff1 3, 4).
constexpr size_t kWoffHeaderSize = 44;
constexpr size_t kWoffEntrySize = 20;

// Writes the font file that the WOFF file holds: its table directory, then each table, four-byte aligned, decompressed
// if the WOFF file compressed it.
PrStatus decodeWoff(PrByteView woff, PrCompressCodec* codec, Array<uint8_t>* out) {
    const uint8_t* in = woff.data;
    if (woff.size < kWoffHeaderSize)
        return PrStatus_InvalidData;
    const uint16_t tables = prMemReadBeU16(in + 12);
    const uint32_t size = prMemReadBeU32(in + 16);
    if (woff.size < kWoffHeaderSize + size_t(tables) * kWoffEntrySize ||
            size < kOffsetTableSize + uint64_t(tables) * kTableRecordSize || size > kMaxFontSize)
        return PrStatus_InvalidData;
    out->resize(size);
    uint8_t* font = out->data;
    prMemZero(font, size);
    writeOffsetTable(font, prMemReadBeU32(in + 4), tables);
    size_t at = kOffsetTableSize + size_t(tables) * kTableRecordSize;
    for (uint16_t t = 0; t < tables; ++t) {
        const uint8_t* entry = in + kWoffHeaderSize + size_t(t) * kWoffEntrySize;
        const uint32_t offset = prMemReadBeU32(entry + 4);
        const uint32_t compressed = prMemReadBeU32(entry + 8);
        const uint32_t length = prMemReadBeU32(entry + 12);
        if (size_t(offset) + compressed > woff.size || at + length > size || compressed > length)
            return PrStatus_InvalidData;
        uint8_t* record = font + kOffsetTableSize + size_t(t) * kTableRecordSize;
        prMemWriteBeU32(record, prMemReadBeU32(entry));
        prMemWriteBeU32(record + 4, prMemReadBeU32(entry + 16));
        prMemWriteBeU32(record + 8, uint32_t(at));
        prMemWriteBeU32(record + 12, length);
        if (compressed == length) {
            prMemCopy(font + at, in + offset, length);
        } else {
            PrCompressRead read{};
            const PrByteView input{in + offset, compressed};
            const PrStatus status = prCompressCodecDecode(codec, PrCompressFormat_Zlib, input, font + at, length, &read);
            if (status == PrStatus_NotSupported)
                return status;
            if (status != PrStatus_Ok || read.progress != PrCompressProgress_Done || read.written != length)
                return PrStatus_InvalidData;
        }
        at += align4(length);
    }
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// WOFF2

// The size of the WOFF2 header (woff2 3.2).
constexpr size_t kWoff2HeaderSize = 48;

// The tags that the flags of a table directory entry name by index (woff2 4.1).
constexpr uint32_t kKnownTags[63] = {
    PR_FONT_TAG('c', 'm', 'a', 'p'), PR_FONT_TAG('h', 'e', 'a', 'd'), PR_FONT_TAG('h', 'h', 'e', 'a'),
    PR_FONT_TAG('h', 'm', 't', 'x'), PR_FONT_TAG('m', 'a', 'x', 'p'), PR_FONT_TAG('n', 'a', 'm', 'e'),
    PR_FONT_TAG('O', 'S', '/', '2'), PR_FONT_TAG('p', 'o', 's', 't'), PR_FONT_TAG('c', 'v', 't', ' '),
    PR_FONT_TAG('f', 'p', 'g', 'm'), PR_FONT_TAG('g', 'l', 'y', 'f'), PR_FONT_TAG('l', 'o', 'c', 'a'),
    PR_FONT_TAG('p', 'r', 'e', 'p'), PR_FONT_TAG('C', 'F', 'F', ' '), PR_FONT_TAG('V', 'O', 'R', 'G'),
    PR_FONT_TAG('E', 'B', 'D', 'T'), PR_FONT_TAG('E', 'B', 'L', 'C'), PR_FONT_TAG('g', 'a', 's', 'p'),
    PR_FONT_TAG('h', 'd', 'm', 'x'), PR_FONT_TAG('k', 'e', 'r', 'n'), PR_FONT_TAG('L', 'T', 'S', 'H'),
    PR_FONT_TAG('P', 'C', 'L', 'T'), PR_FONT_TAG('V', 'D', 'M', 'X'), PR_FONT_TAG('v', 'h', 'e', 'a'),
    PR_FONT_TAG('v', 'm', 't', 'x'), PR_FONT_TAG('B', 'A', 'S', 'E'), PR_FONT_TAG('G', 'D', 'E', 'F'),
    PR_FONT_TAG('G', 'P', 'O', 'S'), PR_FONT_TAG('G', 'S', 'U', 'B'), PR_FONT_TAG('E', 'B', 'S', 'C'),
    PR_FONT_TAG('J', 'S', 'T', 'F'), PR_FONT_TAG('M', 'A', 'T', 'H'), PR_FONT_TAG('C', 'B', 'D', 'T'),
    PR_FONT_TAG('C', 'B', 'L', 'C'), PR_FONT_TAG('C', 'O', 'L', 'R'), PR_FONT_TAG('C', 'P', 'A', 'L'),
    PR_FONT_TAG('S', 'V', 'G', ' '), PR_FONT_TAG('s', 'b', 'i', 'x'), PR_FONT_TAG('a', 'c', 'n', 't'),
    PR_FONT_TAG('a', 'v', 'a', 'r'), PR_FONT_TAG('b', 'd', 'a', 't'), PR_FONT_TAG('b', 'l', 'o', 'c'),
    PR_FONT_TAG('b', 's', 'l', 'n'), PR_FONT_TAG('c', 'v', 'a', 'r'), PR_FONT_TAG('f', 'd', 's', 'c'),
    PR_FONT_TAG('f', 'e', 'a', 't'), PR_FONT_TAG('f', 'm', 't', 'x'), PR_FONT_TAG('f', 'v', 'a', 'r'),
    PR_FONT_TAG('g', 'v', 'a', 'r'), PR_FONT_TAG('h', 's', 't', 'y'), PR_FONT_TAG('j', 'u', 's', 't'),
    PR_FONT_TAG('l', 'c', 'a', 'r'), PR_FONT_TAG('m', 'o', 'r', 't'), PR_FONT_TAG('m', 'o', 'r', 'x'),
    PR_FONT_TAG('o', 'p', 'b', 'd'), PR_FONT_TAG('p', 'r', 'o', 'p'), PR_FONT_TAG('t', 'r', 'a', 'k'),
    PR_FONT_TAG('Z', 'a', 'p', 'f'), PR_FONT_TAG('S', 'i', 'l', 'f'), PR_FONT_TAG('G', 'l', 'a', 't'),
    PR_FONT_TAG('G', 'l', 'o', 'c'), PR_FONT_TAG('F', 'e', 'a', 't'), PR_FONT_TAG('S', 'i', 'l', 'l'),
};

constexpr uint32_t kTagGlyf = PR_FONT_TAG('g', 'l', 'y', 'f');
constexpr uint32_t kTagLoca = PR_FONT_TAG('l', 'o', 'c', 'a');
constexpr uint32_t kTagHmtx = PR_FONT_TAG('h', 'm', 't', 'x');
constexpr uint32_t kTagHead = PR_FONT_TAG('h', 'e', 'a', 'd');
constexpr uint32_t kTagHhea = PR_FONT_TAG('h', 'h', 'e', 'a');
constexpr uint32_t kTagMaxp = PR_FONT_TAG('m', 'a', 'x', 'p');
constexpr uint32_t kTagCollection = PR_FONT_TAG('t', 't', 'c', 'f');

// The index of a table in the WOFF2 table directory, and the index that names no table.
typedef uint32_t Woff2TableIndex;
constexpr Woff2TableIndex kWoff2TableNone = 0xffffffffu;

// A table of the WOFF2 file, and the table it reconstructs to.
struct Woff2Table {
    uint32_t tag;
    uint32_t origLength;
    uint32_t length;            // the number of its bytes in the table data block
    uint32_t source;            // the offset of those bytes in the block
    bool transformed;

    // The reconstructed table: `size` bytes at `start` of the block, or of the rebuilt tables if `rebuilt` is set.
    bool rebuilt;
    uint32_t start;
    uint32_t size;

    // For a glyf table, the x minima of its glyphs in `Woff2File::xMins`, once read.
    uint32_t xMinsStart;
    uint32_t xMinsCount;

    // The offset and checksum of the table in the font file, once placed.
    uint32_t offset;
    uint32_t checksum;
};

// A font of the file: its flavor and its tables.
struct Woff2Font {
    uint32_t flavor;
    uint32_t first;             // the index of its first table in `Woff2File::fontTables`
    uint32_t count;
};

// A WOFF2 file as it decodes.
struct Woff2File {
    uint32_t flavor;
    bool collection;
    uint32_t collectionVersion;
    Array<Woff2Table> tables;
    Array<Woff2Font> fonts;
    Array<Woff2TableIndex> fontTables;  // by Woff2Font::first plus the index of the table in the font
    Array<uint8_t> block;               // the decompressed table data
    Array<uint8_t> rebuilt;             // the reconstructed glyf, loca and hmtx tables
    Array<int16_t> xMins;               // by glyph, for each glyf table from its Woff2Table::xMinsStart
};

void resetFile(Woff2File& file) {
    file.tables.reset();
    file.fonts.reset();
    file.fontTables.reset();
    file.block.reset();
    file.rebuilt.reset();
    file.xMins.reset();
}

// Returns the reconstructed bytes of the table.
const uint8_t* bytesOf(const Woff2File& file, const Woff2Table& table) {
    return (table.rebuilt ? file.rebuilt.data : file.block.data) + table.start;
}

// Sets whether the transform version in the flags of a table transforms the table (woff2 4.1, 5). Returns false for a
// version that is not defined for the tag.
bool transformOf(uint32_t tag, uint32_t version, bool* transformed) {
    if (tag == kTagGlyf || tag == kTagLoca) {
        *transformed = version == 0;
        return version == 0 || version == 3;
    }
    if (tag == kTagHmtx) {
        *transformed = version == 1;
        return version <= 1;
    }
    *transformed = false;
    return version == 0;
}

// Reads the header and the directories, and checks that the blocks follow one another with no more than the padding
// between them that four-byte alignment allows (woff2 3). Sets `compressed` to the compressed font data.
bool readDirectories(PrByteView bytes, Woff2File& file, PrByteView* compressed) {
    if (bytes.size < kWoff2HeaderSize)
        return false;
    Reader r = readerOf(bytes.data, bytes.size);
    const uint32_t signature = readU32(r);
    file.flavor = readU32(r);
    const uint32_t length = readU32(r);
    const uint16_t tableCount = readU16(r);
    readU16(r); // reserved, which a decoder accepts at any value
    readU32(r); // totalSfntSize, which a decoder does not rely on
    const uint32_t compressedSize = readU32(r);
    readU32(r); // version
    const uint32_t metaOffset = readU32(r);
    const uint32_t metaLength = readU32(r);
    readU32(r); // metaOrigLength
    const uint32_t privateOffset = readU32(r);
    const uint32_t privateLength = readU32(r);
    if (r.failed || signature != PR_FONT_TAG('w', 'O', 'F', '2') || length != bytes.size || tableCount == 0)
        return false;

    uint64_t source = 0;
    for (uint32_t i = 0; i < tableCount; ++i) {
        Woff2Table table{};
        const uint8_t flags = readU8(r);
        table.tag = (flags & 63) == 63 ? readU32(r) : kKnownTags[flags & 63];
        if (!readBase128(r, &table.origLength) || !transformOf(table.tag, flags >> 6, &table.transformed))
            return false;
        table.length = table.origLength;
        if (table.transformed && !readBase128(r, &table.length))
            return false;
        // A transformed loca occupies none of the table data block (woff2 5.3).
        if (table.tag == kTagLoca && table.transformed && table.length != 0)
            return false;
        table.source = uint32_t(source);
        source += table.length;
        if (source > kMaxFontSize)
            return false;
        file.tables.push(table);
    }

    file.collection = file.flavor == kTagCollection;
    if (file.collection) {
        file.collectionVersion = readU32(r);
        const uint16_t fontCount = read255UInt16(r);
        if (fontCount == 0)
            return false;
        for (uint32_t f = 0; f < fontCount && !r.failed; ++f) {
            Woff2Font font{};
            font.count = read255UInt16(r);
            font.flavor = readU32(r);
            font.first = file.fontTables.count;
            if (font.count == 0)
                return false;
            for (uint32_t t = 0; t < font.count; ++t) {
                const uint16_t index = read255UInt16(r);
                if (index >= tableCount)
                    return false;
                file.fontTables.push(index);
            }
            file.fonts.push(font);
        }
    } else {
        file.fonts.push(Woff2Font{file.flavor, 0, tableCount});
        for (uint32_t t = 0; t < tableCount; ++t)
            file.fontTables.push(t);
    }
    if (r.failed)
        return false;

    // The compressed data follows the directories, then the metadata and the private data, each at the next four-byte
    // boundary. The file ends with the last of them, or with the padding to the boundary after it.
    const uint64_t dataOffset = r.at;
    if (compressedSize > bytes.size - dataOffset)
        return false;
    uint64_t end = dataOffset + compressedSize;
    if (metaOffset != 0) {
        if (metaOffset != align4(end) || uint64_t(metaOffset) + metaLength > length)
            return false;
        end = uint64_t(metaOffset) + metaLength;
    }
    if (privateOffset != 0) {
        if (privateOffset != align4(end) || uint64_t(privateOffset) + privateLength > length)
            return false;
        end = uint64_t(privateOffset) + privateLength;
    }
    if (length != end && length != align4(end))
        return false;
    *compressed = PrByteView{bytes.data + dataOffset, compressedSize};
    return true;
}

// Decompresses the table data block, whose length is exactly the sum of the lengths of the tables (woff2 5).
PrStatus decompress(PrByteView compressed, PrCompressCodec* codec, Woff2File& file) {
    const Woff2Table& last = file.tables.data[file.tables.count - 1];
    const uint32_t size = last.source + last.length;
    // Room for one byte past the tables reveals a block that is too long.
    file.block.resize(size + 1);
    PrCompressRead read{};
    const PrStatus status = prCompressCodecDecode(codec, PrCompressFormat_Brotli, compressed, file.block.data, size + 1, &read);
    if (status == PrStatus_NotSupported)
        return status;
    if (status != PrStatus_Ok || read.progress != PrCompressProgress_Done || read.written != size ||
            read.consumed != compressed.size)
        return PrStatus_InvalidData;
    file.block.count = size;
    return PrStatus_Ok;
}

// The flags of a component of a composite glyph (OpenType glyf).
enum {
    ComponentFlag_ArgsAreWords = 0x0001,
    ComponentFlag_HaveScale = 0x0008,
    ComponentFlag_MoreComponents = 0x0020,
    ComponentFlag_HaveXYScale = 0x0040,
    ComponentFlag_HaveTwoByTwo = 0x0080,
    ComponentFlag_HaveInstructions = 0x0100,
};
typedef uint16_t ComponentFlags;

// The flags of a point of a simple glyph (OpenType glyf).
enum {
    PointFlag_OnCurve = 0x01,
    PointFlag_XShort = 0x02,
    PointFlag_YShort = 0x04,
    PointFlag_Repeat = 0x08,
    PointFlag_XSame = 0x10,
    PointFlag_YSame = 0x20,
    PointFlag_OverlapSimple = 0x40,
};
typedef uint8_t PointFlags;

// Reads the point delta that a transformed glyf flag and its coordinate bytes encode (woff2 5.2). The low seven bits of
// the flag index the triplet encoding, whose bit 0 is the sign of x and bit 1 the sign of y.
bool readTriplet(Reader& glyphs, uint8_t flag, int32_t* dx, int32_t* dy) {
    const uint32_t index = flag & 0x7f;
    auto sign = [](uint32_t bits, int32_t value) { return (bits & 1) != 0 ? value : -value; };
    if (index < 10) {
        const uint8_t* b = readBytes(glyphs, 1);
        if (b == nullptr)
            return false;
        *dx = 0;
        *dy = sign(index, int32_t(((index & 14) << 7) + b[0]));
    } else if (index < 20) {
        const uint8_t* b = readBytes(glyphs, 1);
        if (b == nullptr)
            return false;
        *dx = sign(index, int32_t((((index - 10) & 14) << 7) + b[0]));
        *dy = 0;
    } else if (index < 84) {
        const uint8_t* b = readBytes(glyphs, 1);
        if (b == nullptr)
            return false;
        const uint32_t high = index - 20;
        *dx = sign(index, int32_t(1 + (high & 0x30) + (b[0] >> 4)));
        *dy = sign(index >> 1, int32_t(1 + ((high & 0x0c) << 2) + (b[0] & 0x0f)));
    } else if (index < 120) {
        const uint8_t* b = readBytes(glyphs, 2);
        if (b == nullptr)
            return false;
        const uint32_t high = index - 84;
        *dx = sign(index, int32_t(1 + ((high / 12) << 8) + b[0]));
        *dy = sign(index >> 1, int32_t(1 + (((high % 12) >> 2) << 8) + b[1]));
    } else if (index < 124) {
        const uint8_t* b = readBytes(glyphs, 3);
        if (b == nullptr)
            return false;
        *dx = sign(index, int32_t((uint32_t(b[0]) << 4) + (b[1] >> 4)));
        *dy = sign(index >> 1, int32_t((uint32_t(b[1] & 0x0f) << 8) + b[2]));
    } else {
        const uint8_t* b = readBytes(glyphs, 4);
        if (b == nullptr)
            return false;
        *dx = sign(index, int32_t((uint32_t(b[0]) << 8) + b[1]));
        *dy = sign(index >> 1, int32_t((uint32_t(b[2]) << 8) + b[3]));
    }
    return true;
}

// Returns true if bit `index` of a bit array whose first bit is the most significant of its first byte is set.
inline bool bitAt(const uint8_t* bits, uint32_t index) {
    return (bits[index >> 3] & (0x80 >> (index & 7))) != 0;
}

// The substreams of a transformed glyf table (woff2 5.1).
struct GlyfStreams {
    Reader contours;
    Reader points;
    Reader flags;
    Reader glyphs;
    Reader composites;
    Reader boxes;
    Reader instructions;
    const uint8_t* boxBits;
    const uint8_t* PR_NULLABLE overlapBits;
};

// Appends a point coordinate stream of a simple glyph: each delta of `deltas` that is not zero, in one byte if its
// magnitude fits and in two otherwise.
void appendCoordinates(Array<uint8_t>& out, const int32_t* deltas, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t delta = deltas[i];
        if (delta == 0)
            continue;
        if (delta > -256 && delta < 256) {
            out.push(uint8_t(delta < 0 ? -delta : delta));
        } else {
            uint8_t word[2];
            writeBeU16(word, uint16_t(int16_t(delta)));
            out.append(word, 2);
        }
    }
}

// Scratch storage for the points of a simple glyph.
struct GlyphPoints {
    Array<int32_t> dx;          // for each point
    Array<int32_t> dy;          // for each point
    Array<uint8_t> flags;       // the reconstructed flags, run-length encoded
};

void resetPoints(GlyphPoints& points) {
    points.dx.reset();
    points.dy.reset();
    points.flags.reset();
}

// Appends the simple glyph `glyph` of `contours` contours to `glyf` and sets `xMin` to the x of its box.
bool rebuildSimpleGlyph(GlyfStreams& s, uint32_t glyph, int16_t contours, GlyphPoints& points, Array<uint8_t>& glyf,
        int16_t* xMin) {
    const uint32_t header = glyf.count;
    glyf.resize(header + 10 + 2 * uint32_t(contours));
    uint32_t pointCount = 0;
    for (int16_t c = 0; c < contours; ++c) {
        pointCount += read255UInt16(s.points);
        if (s.points.failed || pointCount > 0x10000 || pointCount == 0)
            return false;
        writeBeU16(glyf.data + header + 10 + 2 * uint32_t(c), uint16_t(pointCount - 1));
    }
    const uint8_t* flags = readBytes(s.flags, pointCount);
    if (flags == nullptr)
        return false;

    points.dx.resize(pointCount);
    points.dy.resize(pointCount);
    points.flags.clear();
    int32_t x = 0;
    int32_t y = 0;
    int32_t box[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    uint32_t repeat = 0;
    uint32_t last = 0x100;
    for (uint32_t p = 0; p < pointCount; ++p) {
        int32_t dx = 0;
        int32_t dy = 0;
        if (!readTriplet(s.glyphs, flags[p], &dx, &dy))
            return false;
        points.dx.data[p] = dx;
        points.dy.data[p] = dy;
        x += dx;
        y += dy;
        box[0] = x < box[0] ? x : box[0];
        box[1] = y < box[1] ? y : box[1];
        box[2] = x > box[2] ? x : box[2];
        box[3] = y > box[3] ? y : box[3];

        uint8_t flag = (flags[p] & 0x80) != 0 ? 0 : PointFlag_OnCurve;
        if (p == 0 && s.overlapBits != nullptr && bitAt(s.overlapBits, glyph))
            flag |= PointFlag_OverlapSimple;
        if (dx == 0)
            flag |= PointFlag_XSame;
        else if (dx > -256 && dx < 256)
            flag |= PointFlag_XShort | (dx > 0 ? PointFlag_XSame : 0);
        if (dy == 0)
            flag |= PointFlag_YSame;
        else if (dy > -256 && dy < 256)
            flag |= PointFlag_YShort | (dy > 0 ? PointFlag_YSame : 0);
        // A run of equal flags is one flag with the repeat bit and a count.
        if (flag == last && repeat < 255) {
            points.flags.data[points.flags.count - 1 - (repeat != 0)] |= PointFlag_Repeat;
            if (repeat == 0)
                points.flags.push(0);
            points.flags.data[points.flags.count - 1] = uint8_t(++repeat);
        } else {
            points.flags.push(flag);
            repeat = 0;
        }
        last = flag;
    }

    const uint16_t instructionLength = read255UInt16(s.glyphs);
    const uint8_t* instructions = readBytes(s.instructions, instructionLength);
    if (s.glyphs.failed || instructions == nullptr)
        return false;
    if (bitAt(s.boxBits, glyph)) {
        for (int32_t& edge : box)
            edge = readS16(s.boxes);
        if (s.boxes.failed)
            return false;
    }

    uint8_t* out = glyf.data + header;
    writeBeU16(out, uint16_t(contours));
    for (uint32_t i = 0; i < 4; ++i)
        writeBeU16(out + 2 + 2 * i, uint16_t(int16_t(box[i])));
    *xMin = int16_t(box[0]);
    uint8_t length[2];
    writeBeU16(length, instructionLength);
    glyf.append(length, 2);
    if (instructionLength != 0)
        glyf.append(instructions, instructionLength);
    glyf.append(points.flags.data, points.flags.count);
    appendCoordinates(glyf, points.dx.data, pointCount);
    appendCoordinates(glyf, points.dy.data, pointCount);
    return true;
}

// Appends the composite glyph `glyph` to `glyf`, and sets `xMin` to the x of its box, which a composite glyph gives.
bool rebuildCompositeGlyph(GlyfStreams& s, uint32_t glyph, Array<uint8_t>& glyf, int16_t* xMin) {
    if (!bitAt(s.boxBits, glyph))
        return false;
    uint8_t header[10];
    writeBeU16(header, 0xffff);
    const uint8_t* box = readBytes(s.boxes, 8);
    if (box == nullptr)
        return false;
    prMemCopy(header + 2, box, 8);
    *xMin = int16_t(prMemReadBeU16(box));
    glyf.append(header, 10);
    bool instructions = false;
    ComponentFlags flags = 0;
    do {
        const size_t start = s.composites.at;
        flags = readU16(s.composites);
        uint32_t size = 2 + ((flags & ComponentFlag_ArgsAreWords) != 0 ? 4 : 2);
        if ((flags & ComponentFlag_HaveScale) != 0)
            size += 2;
        else if ((flags & ComponentFlag_HaveXYScale) != 0)
            size += 4;
        else if ((flags & ComponentFlag_HaveTwoByTwo) != 0)
            size += 8;
        if (readBytes(s.composites, size) == nullptr)
            return false;
        glyf.append(s.composites.data + start, uint32_t(s.composites.at - start));
        instructions = instructions || (flags & ComponentFlag_HaveInstructions) != 0;
    } while ((flags & ComponentFlag_MoreComponents) != 0);
    if (instructions) {
        const uint16_t length = read255UInt16(s.glyphs);
        const uint8_t* bytes = readBytes(s.instructions, length);
        if (s.glyphs.failed || bytes == nullptr)
            return false;
        uint8_t field[2];
        writeBeU16(field, length);
        glyf.append(field, 2);
        if (length != 0)
            glyf.append(bytes, length);
    }
    return true;
}

// Reconstructs a glyf table and its loca from the transformed glyf table `data` (woff2 5.1, 5.3), and appends each
// glyph's x minimum to `file.xMins`. `locaLength` is the origLength of the loca table, which the glyph count and the
// loca format determine.
bool rebuildGlyf(Woff2File& file, const uint8_t* data, uint32_t size, uint32_t locaLength, Array<uint8_t>& glyf,
        Array<uint8_t>& loca) {
    Reader header = readerOf(data, size);
    readU16(header); // reserved
    const uint16_t options = readU16(header);
    const uint16_t glyphCount = readU16(header);
    const uint16_t indexFormat = readU16(header);
    uint32_t sizes[7];
    for (uint32_t& streamSize : sizes)
        streamSize = readU32(header);
    if (header.failed || locaLength != (uint32_t(glyphCount) + 1) * (indexFormat != 0 ? 4 : 2))
        return false;

    GlyfStreams s{};
    Reader* streams[7] = {&s.contours, &s.points, &s.flags, &s.glyphs, &s.composites, &s.boxes, &s.instructions};
    uint64_t at = header.at;
    for (uint32_t i = 0; i < 7; ++i) {
        if (at + sizes[i] > size)
            return false;
        *streams[i] = readerOf(data + at, sizes[i]);
        at += sizes[i];
    }
    s.boxBits = readBytes(s.boxes, 4 * ((size_t(glyphCount) + 31) / 32));
    if (s.boxBits == nullptr)
        return false;
    s.overlapBits = nullptr;
    if ((options & 1) != 0) {
        const size_t overlapSize = (size_t(glyphCount) + 7) / 8;
        if (at + overlapSize > size)
            return false;
        s.overlapBits = data + at;
    }

    const uint32_t xMinsStart = file.xMins.count;
    file.xMins.resize(xMinsStart + glyphCount);
    Array<uint32_t> offsets;
    offsets.resize(uint32_t(glyphCount) + 1);
    GlyphPoints points{};
    bool valid = true;
    for (uint32_t glyph = 0; glyph < glyphCount && valid; ++glyph) {
        offsets.data[glyph] = glyf.count;
        int16_t xMin = 0;
        const int16_t contours = readS16(s.contours);
        if (s.contours.failed)
            valid = false;
        else if (contours == 0)
            valid = !bitAt(s.boxBits, glyph);
        else if (contours == -1)
            valid = rebuildCompositeGlyph(s, glyph, glyf, &xMin);
        else if (contours > 0)
            valid = rebuildSimpleGlyph(s, glyph, contours, points, glyf, &xMin);
        else
            valid = false;
        file.xMins.data[xMinsStart + glyph] = xMin;
        // Each glyph starts at a four-byte boundary.
        while (glyf.count % 4 != 0)
            glyf.push(0);
        if (glyf.count > kMaxFontSize)
            valid = false;
    }
    resetPoints(points);
    offsets.data[glyphCount] = glyf.count;

    // A short loca stores half of each offset (OpenType loca).
    if (valid && indexFormat == 0 && glyf.count > 0x1fffe)
        valid = false;
    if (valid) {
        loca.resize(locaLength);
        for (uint32_t glyph = 0; glyph <= glyphCount; ++glyph) {
            if (indexFormat == 0)
                writeBeU16(loca.data + 2 * glyph, uint16_t(offsets.data[glyph] / 2));
            else
                prMemWriteBeU32(loca.data + 4 * glyph, offsets.data[glyph]);
        }
    }
    offsets.reset();
    return valid;
}

// Appends to `file.xMins` the x minimum of each glyph of a glyf table that was not transformed, from its loca.
bool readXMins(Woff2File& file, const Woff2Table& glyf, const Woff2Table& loca, uint16_t glyphCount, bool longLoca) {
    if (loca.size < (uint32_t(glyphCount) + 1) * (longLoca ? 4 : 2))
        return false;
    const uint32_t xMinsStart = file.xMins.count;
    file.xMins.resize(xMinsStart + glyphCount);
    const uint8_t* glyphs = bytesOf(file, glyf);
    const uint8_t* offsets = bytesOf(file, loca);
    auto locaOffset = [&](uint32_t index) {
        return longLoca ? prMemReadBeU32(offsets + 4 * index) : 2u * prMemReadBeU16(offsets + 2 * index);
    };
    for (uint32_t glyph = 0; glyph < glyphCount; ++glyph) {
        const uint32_t start = locaOffset(glyph);
        const uint32_t end = locaOffset(glyph + 1);
        if (end < start || end > glyf.size)
            return false;
        file.xMins.data[xMinsStart + glyph] = end - start >= 10 ? int16_t(prMemReadBeU16(glyphs + start + 2)) : 0;
    }
    return true;
}

// Returns the table of the font with `tag`, or kWoff2TableNone.
Woff2TableIndex findTable(const Woff2File& file, const Woff2Font& font, uint32_t tag) {
    for (uint32_t i = 0; i < font.count; ++i) {
        const Woff2TableIndex index = file.fontTables.data[font.first + i];
        if (file.tables.data[index].tag == tag)
            return index;
    }
    return kWoff2TableNone;
}

// Moves `bytes` to the end of the rebuilt tables as the reconstruction of `table`.
void keepRebuilt(Woff2File& file, Woff2Table& table, const Array<uint8_t>& bytes) {
    table.rebuilt = true;
    table.start = file.rebuilt.count;
    table.size = bytes.count;
    if (bytes.count != 0)
        file.rebuilt.append(bytes.data, bytes.count);
    while (file.rebuilt.count % 4 != 0)
        file.rebuilt.push(0);
}

// Reconstructs a transformed hmtx table from its advances and the bearings it keeps, and from the x minima of the
// glyphs of `glyf` for the bearings it omits (woff2 5.4).
bool rebuildHmtx(Woff2File& file, const Woff2Font& font, const Woff2Table& glyf, Woff2Table& hmtx) {
    const Woff2TableIndex maxp = findTable(file, font, kTagMaxp);
    const Woff2TableIndex hhea = findTable(file, font, kTagHhea);
    if (maxp == kWoff2TableNone || hhea == kWoff2TableNone || file.tables.data[maxp].size < 6 ||
            file.tables.data[hhea].size < 36)
        return false;
    const uint16_t glyphCount = prMemReadBeU16(bytesOf(file, file.tables.data[maxp]) + 4);
    const uint16_t metricCount = prMemReadBeU16(bytesOf(file, file.tables.data[hhea]) + 34);
    if (metricCount == 0 || metricCount > glyphCount || glyf.xMinsCount < glyphCount)
        return false;
    const int16_t* xMins = file.xMins.data + glyf.xMinsStart;

    Reader r = readerOf(file.block.data + hmtx.source, hmtx.length);
    const uint8_t flags = readU8(r);
    if ((flags & 3) == 0 || (flags & 0xfc) != 0)
        return false;
    const uint8_t* advances = readBytes(r, 2 * size_t(metricCount));
    const uint8_t* bearings = (flags & 1) != 0 ? nullptr : readBytes(r, 2 * size_t(metricCount));
    const uint8_t* monospaced = (flags & 2) != 0 ? nullptr : readBytes(r, 2 * (size_t(glyphCount) - metricCount));
    if (r.failed || r.at != r.size)
        return false;

    Array<uint8_t> out;
    out.resize(4 * uint32_t(metricCount) + 2 * (uint32_t(glyphCount) - metricCount));
    for (uint32_t i = 0; i < metricCount; ++i) {
        prMemCopy(out.data + 4 * i, advances + 2 * i, 2);
        if (bearings != nullptr)
            prMemCopy(out.data + 4 * i + 2, bearings + 2 * i, 2);
        else
            writeBeU16(out.data + 4 * i + 2, uint16_t(xMins[i]));
    }
    for (uint32_t i = 0; i < uint32_t(glyphCount) - metricCount; ++i) {
        uint8_t* bearing = out.data + 4 * uint32_t(metricCount) + 2 * i;
        if (monospaced != nullptr)
            prMemCopy(bearing, monospaced + 2 * i, 2);
        else
            writeBeU16(bearing, uint16_t(xMins[metricCount + i]));
    }
    keepRebuilt(file, hmtx, out);
    out.reset();
    return true;
}

// Reads the x minima of a glyf table that was not transformed, for the transformed hmtx of its font.
bool readFontXMins(Woff2File& file, const Woff2Font& font, Woff2Table& glyf, const Woff2Table& loca) {
    const Woff2TableIndex head = findTable(file, font, kTagHead);
    const Woff2TableIndex maxp = findTable(file, font, kTagMaxp);
    if (head == kWoff2TableNone || maxp == kWoff2TableNone || file.tables.data[head].size < 54 ||
            file.tables.data[maxp].size < 6)
        return false;
    // The indexToLocFormat of head, and the numGlyphs of maxp (OpenType head, maxp).
    const bool longLoca = prMemReadBeU16(bytesOf(file, file.tables.data[head]) + 50) != 0;
    const uint16_t glyphCount = prMemReadBeU16(bytesOf(file, file.tables.data[maxp]) + 4);
    glyf.xMinsStart = file.xMins.count;
    const bool valid = readXMins(file, glyf, loca, glyphCount, longLoca);
    glyf.xMinsCount = file.xMins.count - glyf.xMinsStart;
    return valid;
}

// Reconstructs the transformed tables of a font. A transformed glyf and its loca are a pair: both are transformed or
// neither is, and the loca follows the glyf in the directory, immediately in a collection (woff2 4.2, 5.5). A table
// that fonts of a collection share is reconstructed once.
bool rebuildFont(Woff2File& file, const Woff2Font& font, Array<uint8_t>& glyf, Array<uint8_t>& loca) {
    const Woff2TableIndex glyfIndex = findTable(file, font, kTagGlyf);
    const Woff2TableIndex locaIndex = findTable(file, font, kTagLoca);
    const Woff2TableIndex hmtxIndex = findTable(file, font, kTagHmtx);
    if ((glyfIndex == kWoff2TableNone) != (locaIndex == kWoff2TableNone))
        return false;
    if (glyfIndex != kWoff2TableNone) {
        Woff2Table& glyfTable = file.tables.data[glyfIndex];
        Woff2Table& locaTable = file.tables.data[locaIndex];
        if (glyfTable.transformed != locaTable.transformed)
            return false;
        if (glyfTable.transformed && (locaIndex < glyfIndex || (file.collection && locaIndex != glyfIndex + 1)))
            return false;
        if (glyfTable.transformed && !glyfTable.rebuilt) {
            glyf.clear();
            loca.clear();
            glyfTable.xMinsStart = file.xMins.count;
            if (!rebuildGlyf(file, file.block.data + glyfTable.source, glyfTable.length, locaTable.origLength, glyf, loca))
                return false;
            glyfTable.xMinsCount = file.xMins.count - glyfTable.xMinsStart;
            keepRebuilt(file, glyfTable, glyf);
            keepRebuilt(file, locaTable, loca);
        }
    }
    if (hmtxIndex == kWoff2TableNone || !file.tables.data[hmtxIndex].transformed || file.tables.data[hmtxIndex].rebuilt)
        return true;
    // A transformed hmtx takes the bearings that it omits from the boxes of the glyphs, so the font must have a glyf.
    if (glyfIndex == kWoff2TableNone)
        return false;
    Woff2Table& glyfTable = file.tables.data[glyfIndex];
    if (!glyfTable.transformed && glyfTable.xMinsCount == 0 &&
            !readFontXMins(file, font, glyfTable, file.tables.data[locaIndex]))
        return false;
    return rebuildHmtx(file, font, glyfTable, file.tables.data[hmtxIndex]);
}

// Reconstructs the transformed tables of each font.
bool rebuildTables(Woff2File& file) {
    for (uint32_t i = 0; i < file.tables.count; ++i) {
        Woff2Table& table = file.tables.data[i];
        table.start = table.source;
        table.size = table.length;
    }
    Array<uint8_t> glyf;
    Array<uint8_t> loca;
    bool valid = true;
    for (uint32_t f = 0; f < file.fonts.count && valid; ++f)
        valid = rebuildFont(file, file.fonts.data[f], glyf, loca);
    glyf.reset();
    loca.reset();
    return valid;
}

// Writes the font file: the header of a collection, the offset table of each font, then each table once, four-byte
// aligned, with its checksum, and the checksum adjustment of each font in its head (woff2 5).
bool writeFont(Woff2File& file, Array<uint8_t>* out) {
    const uint32_t fontCount = file.fonts.count;
    uint64_t size = file.collection ? 12 + 4 * uint64_t(fontCount) : 0;
    for (uint32_t f = 0; f < fontCount; ++f)
        size += kOffsetTableSize + uint64_t(kTableRecordSize) * file.fonts.data[f].count;
    for (uint32_t i = 0; i < file.tables.count; ++i)
        file.tables.data[i].offset = 0;
    // Writes the tables in directory order, each once however many fonts share it.
    for (uint32_t i = 0; i < file.tables.count; ++i) {
        Woff2Table& table = file.tables.data[i];
        bool used = !file.collection;
        for (uint32_t t = 0; t < file.fontTables.count && !used; ++t)
            used = file.fontTables.data[t] == i;
        if (!used)
            continue;
        table.offset = uint32_t(size);
        size = align4(size + table.size);
        if (size > kMaxFontSize)
            return false;
    }
    out->resize(uint32_t(size));
    uint8_t* font = out->data;
    prMemZero(font, size);
    for (uint32_t i = 0; i < file.tables.count; ++i) {
        Woff2Table& table = file.tables.data[i];
        if (table.offset == 0)
            continue;
        if (table.size != 0)
            prMemCopy(font + table.offset, bytesOf(file, table), table.size);
        // The checksum adjustment of head counts as zero in the checksum of head itself (OpenType head).
        if (table.tag == kTagHead && table.size >= 12)
            prMemWriteBeU32(font + table.offset + 8, 0);
        table.checksum = tableChecksum(font + table.offset, table.size);
    }

    uint32_t at = 0;
    if (file.collection) {
        prMemWriteBeU32(font, kTagCollection);
        // A collection header of version 2 would name a DSIG, which decoding has invalidated, so it is version 1.
        prMemWriteBeU32(font + 4, 0x00010000);
        prMemWriteBeU32(font + 8, fontCount);
        at = 12 + 4 * fontCount;
    }
    for (uint32_t f = 0; f < fontCount; ++f) {
        const Woff2Font& entry = file.fonts.data[f];
        if (file.collection)
            prMemWriteBeU32(font + 12 + 4 * f, at);
        uint8_t* offsetTable = font + at;
        writeOffsetTable(offsetTable, entry.flavor, uint16_t(entry.count));
        // The records, sorted by tag. The tags are unique.
        Array<Woff2TableIndex> sorted;
        sorted.append(file.fontTables.data + entry.first, entry.count);
        for (uint32_t i = 1; i < sorted.count; ++i) {
            const Woff2TableIndex moving = sorted.data[i];
            uint32_t j = i;
            for (; j > 0 && file.tables.data[sorted.data[j - 1]].tag > file.tables.data[moving].tag; --j)
                sorted.data[j] = sorted.data[j - 1];
            sorted.data[j] = moving;
        }
        bool unique = true;
        uint32_t sum = 0;
        Woff2TableIndex head = kWoff2TableNone;
        for (uint32_t i = 0; i < sorted.count; ++i) {
            const Woff2Table& table = file.tables.data[sorted.data[i]];
            unique = unique && (i == 0 || file.tables.data[sorted.data[i - 1]].tag != table.tag);
            uint8_t* record = offsetTable + kOffsetTableSize + kTableRecordSize * i;
            prMemWriteBeU32(record, table.tag);
            prMemWriteBeU32(record + 4, table.checksum);
            prMemWriteBeU32(record + 8, table.offset);
            prMemWriteBeU32(record + 12, table.size);
            sum += table.checksum;
            if (table.tag == kTagHead)
                head = sorted.data[i];
        }
        sorted.reset();
        if (!unique)
            return false;
        const uint32_t directorySize = kOffsetTableSize + kTableRecordSize * entry.count;
        sum += tableChecksum(offsetTable, directorySize);
        if (head != kWoff2TableNone && file.tables.data[head].size >= 12)
            prMemWriteBeU32(font + file.tables.data[head].offset + 8, 0xb1b0afbau - sum);
        at += directorySize;
    }
    return true;
}

PrStatus decodeWoff2(PrByteView bytes, PrCompressCodec* codec, Array<uint8_t>* out) {
    Woff2File file{};
    PrByteView compressed{};
    PrStatus status = PrStatus_InvalidData;
    if (readDirectories(bytes, file, &compressed)) {
        status = decompress(compressed, codec, file);
        if (status == PrStatus_Ok && (!rebuildTables(file) || !writeFont(file, out)))
            status = PrStatus_InvalidData;
    }
    resetFile(file);
    return status;
}

} // namespace

PrStatus woffDecode(PrByteView bytes, PrCompressCodec* codec, Array<uint8_t>* out) {
    PR_ASSERT(out->count == 0);
    if (bytes.size < 4)
        return PrStatus_InvalidData;
    const uint32_t signature = prMemReadBeU32(bytes.data);
    if (signature == PR_FONT_TAG('w', 'O', 'F', '2'))
        return decodeWoff2(bytes, codec, out);
    if (signature == PR_FONT_TAG('w', 'O', 'F', 'F'))
        return decodeWoff(bytes, codec, out);
    return PrStatus_InvalidData;
}

} // namespace pr_impl
