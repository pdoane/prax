// Font subsets: a font's file cut down to the glyphs that a document draws, for the document to embed.
//
// OpenType - https://learn.microsoft.com/typography/opentype/spec/
//
// CFF - Adobe Technical Note #5176, The Compact Font Format Specification
//
// Type 2 charstrings - Adobe Technical Note #5177, The Type 2 Charstring Format

#include "pr/font_subset.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/path.h"
#include "pr/raw_array.h"

#include "font/src/sfnt.h"

namespace pr_impl {

namespace {

constexpr uint32_t kTagGlyf = PR_FONT_TAG('g', 'l', 'y', 'f');
constexpr uint32_t kTagLoca = PR_FONT_TAG('l', 'o', 'c', 'a');
constexpr uint32_t kTagHead = PR_FONT_TAG('h', 'e', 'a', 'd');
constexpr uint32_t kTagHhea = PR_FONT_TAG('h', 'h', 'e', 'a');
constexpr uint32_t kTagHmtx = PR_FONT_TAG('h', 'm', 't', 'x');
constexpr uint32_t kTagMaxp = PR_FONT_TAG('m', 'a', 'x', 'p');
constexpr uint32_t kTagPost = PR_FONT_TAG('p', 'o', 's', 't');
constexpr uint32_t kTagCff = PR_FONT_TAG('C', 'F', 'F', ' ');
constexpr uint32_t kTagCff2 = PR_FONT_TAG('C', 'F', 'F', '2');

// The tables that a TrueType subset copies as they are: the hinting programs and their data, and the tables that
// describe the font as a whole.
constexpr uint32_t kCopied[] = {
    PR_FONT_TAG('O', 'S', '/', '2'), PR_FONT_TAG('c', 'v', 't', ' '), PR_FONT_TAG('f', 'p', 'g', 'm'),
    PR_FONT_TAG('n', 'a', 'm', 'e'), PR_FONT_TAG('p', 'r', 'e', 'p'),
};

// The flags of a component of a composite glyph (glyf).
constexpr uint16_t kArgsAreWords = 0x0001;
constexpr uint16_t kHaveScale = 0x0008;
constexpr uint16_t kMoreComponents = 0x0020;
constexpr uint16_t kHaveXYScale = 0x0040;
constexpr uint16_t kHaveTwoByTwo = 0x0080;

void put16(RawArray<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value >> 8));
    out.push(uint8_t(value));
}

void put32(RawArray<uint8_t>& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value & 0xffff);
}

void set16(uint8_t* at, uint32_t value) {
    at[0] = uint8_t(value >> 8);
    at[1] = uint8_t(value);
}

void set32(uint8_t* at, uint32_t value) {
    set16(at, value >> 16);
    set16(at + 2, value & 0xffff);
}

uint32_t checksum(const uint8_t* data, uint32_t size) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t word = 0;
        for (uint32_t k = 0; k < 4; ++k)
            word = (word << 8) | (i + k < size ? data[i + k] : 0);
        sum += word;
    }
    return sum;
}

// A table of the subset, by its tag, as bytes of the subset's own.
struct SubsetTable {
    uint32_t tag;
    RawArray<uint8_t> bytes;
};

// The glyph's bytes in glyf, as loca locates them.
SfntTable glyphData(SfntTable glyf, SfntTable loca, bool longOffsets, uint32_t glyph) {
    uint32_t start = longOffsets ? loca.uint32At(glyph * 4) : uint32_t(loca.uint16At(glyph * 2)) * 2;
    uint32_t end = longOffsets ? loca.uint32At(glyph * 4 + 4) : uint32_t(loca.uint16At(glyph * 2 + 2)) * 2;
    if (end <= start || end > glyf.size)
        return SfntTable{};
    return SfntTable{glyf.data + start, end - start};
}

// Marks the glyph, and the glyphs that it is built from, as kept.
void keep(SfntTable glyf, SfntTable loca, bool longOffsets, uint32_t glyphCount, uint32_t glyph,
        RawArray<uint8_t>& kept, uint32_t depth) {
    if (glyph >= glyphCount || kept.data[glyph] != 0 || depth > 32)
        return;
    kept.data[glyph] = 1;
    SfntTable data = glyphData(glyf, loca, longOffsets, glyph);
    if (data.size < 10 || data.int16At(0) >= 0)
        return;
    uint32_t at = 10;
    for (;;) {
        uint16_t flags = data.uint16At(at);
        uint16_t component = data.uint16At(at + 2);
        keep(glyf, loca, longOffsets, glyphCount, component, kept, depth + 1);
        at += 4 + ((flags & kArgsAreWords) != 0 ? 4 : 2);
        if ((flags & kHaveScale) != 0)
            at += 2;
        else if ((flags & kHaveXYScale) != 0)
            at += 4;
        else if ((flags & kHaveTwoByTwo) != 0)
            at += 8;
        if ((flags & kMoreComponents) == 0 || at >= data.size)
            break;
    }
}

void copyTable(RawArray<uint8_t>& out, SfntTable table) {
    out.append(table.data, table.size);
}

// Writes the tables as an sfnt of TrueType outlines, with their checksums and the font's checksum adjustment.
void writeSfnt(RawArray<SubsetTable>& tables, PrFontWriteFn write, void* PR_NULLABLE context) {
    // The table records are sorted by tag.
    for (uint32_t i = 1; i < tables.count; ++i) {
        for (uint32_t k = i; k > 0 && tables.data[k - 1].tag > tables.data[k].tag; --k) {
            SubsetTable swap = tables.data[k];
            tables.data[k] = tables.data[k - 1];
            tables.data[k - 1] = swap;
        }
    }
    uint32_t count = tables.count;
    uint32_t power = 1;
    uint32_t log = 0;
    while (power * 2 <= count) {
        power *= 2;
        ++log;
    }
    RawArray<uint8_t> file;
    put32(file, 0x00010000);
    put16(file, count);
    put16(file, power * 16);
    put16(file, log);
    put16(file, count * 16 - power * 16);
    uint32_t offset = 12 + count * 16;
    uint32_t head = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const SubsetTable& t = tables.data[i];
        put32(file, t.tag);
        put32(file, checksum(t.bytes.data, t.bytes.count));
        put32(file, offset);
        put32(file, t.bytes.count);
        if (t.tag == kTagHead)
            head = offset;
        offset += (t.bytes.count + 3) & ~3u;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const SubsetTable& t = tables.data[i];
        file.append(t.bytes.data, t.bytes.count);
        while ((file.count & 3) != 0)
            file.push(0);
    }
    set32(file.data + head + 8, 0xB1B0AFBAu - checksum(file.data, file.count));
    write(context, PrByteView{file.data, file.count});
    file.reset();
}

bool subsetTrueType(const Sfnt& sfnt, const uint16_t* PR_NULLABLE glyphs, uint32_t glyphCount, PrFontWriteFn write,
        void* PR_NULLABLE context) {
    SfntTable head = sfntTable(sfnt, kTagHead);
    SfntTable maxp = sfntTable(sfnt, kTagMaxp);
    SfntTable hhea = sfntTable(sfnt, kTagHhea);
    SfntTable hmtx = sfntTable(sfnt, kTagHmtx);
    SfntTable loca = sfntTable(sfnt, kTagLoca);
    SfntTable glyf = sfntTable(sfnt, kTagGlyf);
    if (head.size < 54 || maxp.size < 6 || hhea.size < 36 || loca.size == 0 || glyf.size == 0)
        return false;
    bool longOffsets = head.int16At(50) != 0;
    uint32_t fontGlyphs = maxp.uint16At(4);
    if (loca.size < (fontGlyphs + 1) * (longOffsets ? 4 : 2))
        return false;
    RawArray<uint8_t> kept;
    kept.resizeZeroed(fontGlyphs);
    keep(glyf, loca, longOffsets, fontGlyphs, 0, kept, 0);
    for (uint32_t i = 0; i < glyphCount; ++i)
        keep(glyf, loca, longOffsets, fontGlyphs, glyphs[i], kept, 0);
    uint32_t count = 0;
    for (uint32_t g = 0; g < fontGlyphs; ++g) {
        if (kept.data[g] != 0)
            count = g + 1;
    }
    RawArray<SubsetTable> tables;
    auto add = [&](uint32_t tag) -> RawArray<uint8_t>& {
        SubsetTable& t = tables.emplace();
        t = SubsetTable{};
        t.tag = tag;
        return t.bytes;
    };
    // glyf and loca, with long offsets, and each glyph that is not kept empty.
    RawArray<uint8_t>& newGlyf = add(kTagGlyf);
    RawArray<uint8_t> newLoca;
    for (uint32_t g = 0; g < count; ++g) {
        put32(newLoca, newGlyf.count);
        if (kept.data[g] == 0)
            continue;
        copyTable(newGlyf, glyphData(glyf, loca, longOffsets, g));
        while ((newGlyf.count & 3) != 0)
            newGlyf.push(0);
    }
    put32(newLoca, newGlyf.count);
    RawArray<uint8_t>& locaBytes = add(kTagLoca);
    locaBytes.append(newLoca.data, newLoca.count);
    newLoca.reset();
    RawArray<uint8_t>& headBytes = add(kTagHead);
    copyTable(headBytes, head);
    set32(headBytes.data + 8, 0);
    set16(headBytes.data + 50, 1);
    RawArray<uint8_t>& maxpBytes = add(kTagMaxp);
    copyTable(maxpBytes, maxp);
    set16(maxpBytes.data + 4, count);
    // The metrics of the glyphs kept: an advance and a side bearing for each glyph below the font's count of long
    // metrics, and a side bearing for each glyph past it.
    uint32_t longMetrics = hhea.uint16At(34);
    uint32_t newLong = longMetrics < count ? longMetrics : count;
    if (newLong == 0)
        newLong = 1;
    RawArray<uint8_t>& hheaBytes = add(kTagHhea);
    copyTable(hheaBytes, hhea);
    set16(hheaBytes.data + 34, newLong);
    RawArray<uint8_t>& hmtxBytes = add(kTagHmtx);
    for (uint32_t g = 0; g < newLong; ++g) {
        put16(hmtxBytes, hmtx.uint16At(g * 4));
        put16(hmtxBytes, hmtx.uint16At(g * 4 + 2));
    }
    for (uint32_t g = newLong; g < count; ++g)
        put16(hmtxBytes, hmtx.uint16At(longMetrics * 4 + (g - longMetrics) * 2));
    // A post table of version 3, which names no glyphs.
    SfntTable post = sfntTable(sfnt, kTagPost);
    RawArray<uint8_t>& postBytes = add(kTagPost);
    put32(postBytes, 0x00030000);
    for (uint32_t at = 4; at < 32; ++at)
        postBytes.push(post.uint8At(at));
    for (uint32_t tag : kCopied) {
        SfntTable table = sfntTable(sfnt, tag);
        if (table.size != 0)
            copyTable(add(tag), table);
    }
    writeSfnt(tables, write, context);
    for (uint32_t i = 0; i < tables.count; ++i)
        tables.data[i].bytes.reset();
    tables.reset();
    kept.reset();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// CFF

// An INDEX of CFF, as its count, offsets and data.
struct Index {
    uint32_t start;             // where the INDEX starts in the table
    uint32_t end;               // the byte after it
    uint32_t count;
    uint32_t offsetSize;
};

bool readIndex(SfntTable cff, uint32_t at, Index* out) {
    *out = Index{.start = at};
    uint32_t count = cff.uint16At(at);
    out->count = count;
    if (count == 0) {
        out->end = at + 2;
        return out->end <= cff.size;
    }
    uint32_t offsetSize = cff.uint8At(at + 2);
    if (offsetSize < 1 || offsetSize > 4)
        return false;
    out->offsetSize = offsetSize;
    uint32_t last = 0;
    for (uint32_t k = 0; k < offsetSize; ++k)
        last = (last << 8) | cff.uint8At(at + 3 + count * offsetSize + k);
    uint64_t end = uint64_t(at) + 3 + uint64_t(count + 1) * offsetSize + last - 1;
    if (end > cff.size)
        return false;
    out->end = uint32_t(end);
    return true;
}

// Returns the bounds of item `index` of an INDEX, relative to the table.
void indexItem(SfntTable cff, const Index& index, uint32_t item, uint32_t* start, uint32_t* end) {
    auto offset = [&](uint32_t i) {
        uint32_t value = 0;
        for (uint32_t k = 0; k < index.offsetSize; ++k)
            value = (value << 8) | cff.uint8At(index.start + 3 + i * index.offsetSize + k);
        return value;
    };
    uint32_t data = index.start + 2 + (index.count + 1) * index.offsetSize;
    *start = data + offset(item);
    *end = data + offset(item + 1);
}

// Writes an INDEX whose items are the bytes of `data`, item i ending at `ends[i]`.
void writeIndex(RawArray<uint8_t>& out, const RawArray<uint8_t>& data, const RawArray<uint32_t>& ends) {
    uint32_t count = ends.count;
    put16(out, count);
    if (count == 0)
        return;
    uint32_t last = data.count + 1;
    uint32_t offsetSize = 4;
    if (last <= 0xff)
        offsetSize = 1;
    else if (last <= 0xffff)
        offsetSize = 2;
    else if (last <= 0xffffff)
        offsetSize = 3;
    out.push(uint8_t(offsetSize));
    auto offset = [&](uint32_t value) {
        for (uint32_t k = offsetSize; k-- > 0;)
            out.push(uint8_t(value >> (k * 8)));
    };
    offset(1);
    for (uint32_t i = 0; i < count; ++i)
        offset(ends.data[i] + 1);
    out.append(data.data, data.count);
}

// A DICT's operator and the bytes of its operands, as the table holds them.
struct DictEntry {
    uint32_t op;                // a two-byte operator is 1200 plus its second byte
    uint32_t start;             // the operands' bytes
    uint32_t end;
    int32_t values[2];          // the first two operands, if they are integers
    uint32_t valueCount;
};

// Reads a DICT's entries. Returns false if it does not parse.
bool readDict(SfntTable cff, uint32_t start, uint32_t end, RawArray<DictEntry>& out) {
    out.clear();
    DictEntry entry{.start = start};
    uint32_t at = start;
    while (at < end) {
        uint32_t b = cff.uint8At(at);
        if (b <= 21) {
            entry.op = b == 12 ? 1200 + cff.uint8At(at + 1) : b;
            entry.end = at;
            at += b == 12 ? 2 : 1;
            out.push(entry);
            entry = DictEntry{.start = at};
            continue;
        }
        int32_t value = 0;
        bool integer = true;
        if (b == 28) {
            value = int16_t(cff.uint16At(at + 1));
            at += 3;
        } else if (b == 29) {
            value = int32_t(cff.uint32At(at + 1));
            at += 5;
        } else if (b == 30) {
            integer = false;
            ++at;
            while (at < end) {
                uint32_t nibbles = cff.uint8At(at++);
                if ((nibbles & 0x0f) == 0x0f || (nibbles >> 4) == 0x0f)
                    break;
            }
        } else if (b >= 32 && b <= 246) {
            value = int32_t(b) - 139;
            at += 1;
        } else if (b >= 247 && b <= 250) {
            value = (int32_t(b) - 247) * 256 + int32_t(cff.uint8At(at + 1)) + 108;
            at += 2;
        } else if (b >= 251 && b <= 254) {
            value = -(int32_t(b) - 251) * 256 - int32_t(cff.uint8At(at + 1)) - 108;
            at += 2;
        } else {
            return false;
        }
        if (entry.valueCount < 2 && integer)
            entry.values[entry.valueCount] = value;
        ++entry.valueCount;
    }
    return at == end;
}

// Appends an integer operand in its five-byte form, which keeps the size of a DICT fixed while offsets change.
void putOperand(RawArray<uint8_t>& out, int32_t value) {
    out.push(29);
    put32(out, uint32_t(value));
}

void putOperator(RawArray<uint8_t>& out, uint32_t op) {
    if (op >= 1200) {
        out.push(12);
        out.push(uint8_t(op - 1200));
    } else {
        out.push(uint8_t(op));
    }
}

// The operators of a DICT that name offsets into the table.
constexpr uint32_t kOpCharset = 15;
constexpr uint32_t kOpEncoding = 16;
constexpr uint32_t kOpCharStrings = 17;
constexpr uint32_t kOpPrivate = 18;
constexpr uint32_t kOpSubrs = 19;
constexpr uint32_t kOpFdArray = 1236;
constexpr uint32_t kOpFdSelect = 1237;

// Writes a DICT with the offsets given in place of the operands of the operators that name them. Every other entry is
// copied.
void writeDict(RawArray<uint8_t>& out, SfntTable cff, const RawArray<DictEntry>& entries, uint32_t charset,
        uint32_t encoding, uint32_t charStrings, uint32_t privateSize, uint32_t privateOffset, uint32_t fdArray,
        uint32_t fdSelect) {
    for (uint32_t i = 0; i < entries.count; ++i) {
        const DictEntry& e = entries.data[i];
        switch (e.op) {
            case kOpCharset:
                if (e.values[0] > 2)
                    putOperand(out, int32_t(charset));
                else
                    out.append(cff.data + e.start, e.end - e.start);
                break;
            case kOpEncoding:
                if (e.values[0] > 1)
                    putOperand(out, int32_t(encoding));
                else
                    out.append(cff.data + e.start, e.end - e.start);
                break;
            case kOpCharStrings: putOperand(out, int32_t(charStrings)); break;
            case kOpPrivate:
                putOperand(out, int32_t(privateSize));
                putOperand(out, int32_t(privateOffset));
                break;
            case kOpFdArray: putOperand(out, int32_t(fdArray)); break;
            case kOpFdSelect: putOperand(out, int32_t(fdSelect)); break;
            default: out.append(cff.data + e.start, e.end - e.start); break;
        }
        putOperator(out, e.op);
    }
}

const DictEntry* findEntry(const RawArray<DictEntry>& entries, uint32_t op) {
    for (uint32_t i = 0; i < entries.count; ++i) {
        if (entries.data[i].op == op)
            return &entries.data[i];
    }
    return nullptr;
}

// Returns the size of a charset of `glyphs` glyphs at `at`, or zero if it does not parse.
uint32_t charsetSize(SfntTable cff, uint32_t at, uint32_t glyphs) {
    uint32_t format = cff.uint8At(at);
    if (format == 0)
        return 1 + (glyphs - 1) * 2;
    uint32_t pos = at + 1;
    uint32_t covered = 1;
    while (covered < glyphs && pos < cff.size) {
        uint32_t left = format == 1 ? cff.uint8At(pos + 2) : cff.uint16At(pos + 2);
        covered += left + 1;
        pos += format == 1 ? 3 : 4;
    }
    return covered >= glyphs && (format == 1 || format == 2) ? pos - at : 0;
}

// Returns the size of an FDSelect of `glyphs` glyphs at `at`, or zero if it does not parse.
uint32_t fdSelectSize(SfntTable cff, uint32_t at, uint32_t glyphs) {
    uint32_t format = cff.uint8At(at);
    if (format == 0)
        return 1 + glyphs;
    if (format == 3)
        return 1 + 2 + uint32_t(cff.uint16At(at + 1)) * 3 + 2;
    return 0;
}

// Returns the size of a custom encoding at `at`.
uint32_t encodingSize(SfntTable cff, uint32_t at) {
    uint32_t format = cff.uint8At(at);
    uint32_t size = (format & 0x7f) == 0 ? 2 + cff.uint8At(at + 1) : 2 + cff.uint8At(at + 1) * 2u;
    if ((format & 0x80) != 0)
        size += 1 + cff.uint8At(at + size) * 3u;
    return size;
}

// A Private DICT and the local subroutines after it, copied as one run of bytes so that the Subrs offset, which counts
// from the DICT's start, still holds.
struct PrivateRun {
    uint32_t start;
    uint32_t size;              // the DICT's size
    uint32_t end;               // the end of the run
};

bool privateRun(SfntTable cff, const DictEntry* entry, RawArray<DictEntry>& scratch, PrivateRun* out) {
    *out = PrivateRun{};
    if (entry == nullptr || entry->valueCount < 2)
        return true;
    uint32_t size = uint32_t(entry->values[0]);
    uint32_t start = uint32_t(entry->values[1]);
    if (uint64_t(start) + size > cff.size || !readDict(cff, start, start + size, scratch))
        return false;
    out->start = start;
    out->size = size;
    out->end = start + size;
    if (const DictEntry* subrs = findEntry(scratch, kOpSubrs)) {
        Index local;
        if (!readIndex(cff, start + uint32_t(subrs->values[0]), &local))
            return false;
        if (local.end > out->end)
            out->end = local.end;
    }
    return true;
}

bool subsetCff(const Sfnt& sfnt, const uint16_t* PR_NULLABLE glyphs, uint32_t glyphCount, PrFontWriteFn write,
        void* PR_NULLABLE context) {
    SfntTable cff = sfntTable(sfnt, kTagCff);
    if (cff.size < 4 || cff.uint8At(0) != 1)
        return false;
    uint32_t headerSize = cff.uint8At(2);
    Index names;
    Index tops;
    Index strings;
    Index globals;
    if (!readIndex(cff, headerSize, &names) || !readIndex(cff, names.end, &tops) || tops.count != 1 ||
            !readIndex(cff, tops.end, &strings) || !readIndex(cff, strings.end, &globals))
        return false;
    uint32_t topStart;
    uint32_t topEnd;
    indexItem(cff, tops, 0, &topStart, &topEnd);
    RawArray<DictEntry> top;
    RawArray<DictEntry> scratch;
    bool ok = readDict(cff, topStart, topEnd, top);
    const DictEntry* charStringsEntry = ok ? findEntry(top, kOpCharStrings) : nullptr;
    Index charStrings;
    ok = ok && charStringsEntry != nullptr && readIndex(cff, uint32_t(charStringsEntry->values[0]), &charStrings);
    uint32_t fontGlyphs = ok ? charStrings.count : 0;
    // The parts that the subset copies whole.
    const DictEntry* charsetEntry = ok ? findEntry(top, kOpCharset) : nullptr;
    const DictEntry* encodingEntry = ok ? findEntry(top, kOpEncoding) : nullptr;
    const DictEntry* fdSelectEntry = ok ? findEntry(top, kOpFdSelect) : nullptr;
    const DictEntry* fdArrayEntry = ok ? findEntry(top, kOpFdArray) : nullptr;
    uint32_t charsetAt = charsetEntry != nullptr && charsetEntry->values[0] > 2 ? uint32_t(charsetEntry->values[0]) : 0;
    uint32_t charsetBytes = charsetAt != 0 ? charsetSize(cff, charsetAt, fontGlyphs) : 0;
    uint32_t encodingAt = encodingEntry != nullptr && encodingEntry->values[0] > 1 ? uint32_t(encodingEntry->values[0]) : 0;
    uint32_t encodingBytes = encodingAt != 0 ? encodingSize(cff, encodingAt) : 0;
    uint32_t fdSelectAt = fdSelectEntry != nullptr ? uint32_t(fdSelectEntry->values[0]) : 0;
    uint32_t fdSelectBytes = fdSelectAt != 0 ? fdSelectSize(cff, fdSelectAt, fontGlyphs) : 0;
    ok = ok && (charsetAt == 0 || (charsetBytes != 0 && charsetAt + charsetBytes <= cff.size)) &&
        (encodingAt == 0 || encodingAt + encodingBytes <= cff.size) &&
        (fdSelectAt == 0 || (fdSelectBytes != 0 && fdSelectAt + fdSelectBytes <= cff.size));
    Index fdArray{};
    ok = ok && (fdArrayEntry == nullptr || readIndex(cff, uint32_t(fdArrayEntry->values[0]), &fdArray));
    PrivateRun topPrivate;
    ok = ok && privateRun(cff, findEntry(top, kOpPrivate), scratch, &topPrivate);
    RawArray<DictEntry> fdDict;
    RawArray<PrivateRun> fdPrivates;
    RawArray<uint32_t> fdBounds;     // the start and end of each Font DICT
    for (uint32_t i = 0; ok && i < fdArray.count; ++i) {
        uint32_t start;
        uint32_t end;
        indexItem(cff, fdArray, i, &start, &end);
        ok = end <= cff.size && readDict(cff, start, end, fdDict);
        PrivateRun run;
        ok = ok && privateRun(cff, findEntry(fdDict, kOpPrivate), scratch, &run);
        fdPrivates.push(run);
        fdBounds.push(start);
        fdBounds.push(end);
    }
    if (!ok) {
        top.reset();
        scratch.reset();
        fdDict.reset();
        fdPrivates.reset();
        fdBounds.reset();
        return false;
    }
    // Which glyphs keep their charstrings. A charstring's seac and subroutines are copied with every subroutine, so
    // what a kept glyph calls stays.
    RawArray<uint8_t> kept;
    kept.resizeZeroed(fontGlyphs);
    if (fontGlyphs != 0)
        kept.data[0] = 1;
    for (uint32_t i = 0; i < glyphCount; ++i) {
        if (glyphs[i] < fontGlyphs)
            kept.data[glyphs[i]] = 1;
    }
    RawArray<uint8_t> charData;
    RawArray<uint32_t> charEnds;
    for (uint32_t g = 0; g < fontGlyphs; ++g) {
        if (kept.data[g] != 0) {
            uint32_t start;
            uint32_t end;
            indexItem(cff, charStrings, g, &start, &end);
            charData.append(cff.data + start, end - start);
        } else {
            charData.push(14);  // endchar
        }
        charEnds.push(charData.count);
    }
    // The layout: header, Name INDEX, Top DICT INDEX, String INDEX and Global Subr INDEX as the font has them, with a
    // Top DICT of fixed-size offsets, then the charset, the encoding, the FDSelect, the CharStrings, the Font DICTs and
    // the Private DICTs.
    RawArray<uint8_t> topDict;
    writeDict(topDict, cff, top, 0, 0, 0, 0, 0, 0, 0);
    RawArray<uint8_t> scratchIndex;
    RawArray<uint32_t> topEnds;
    topEnds.push(topDict.count);
    writeIndex(scratchIndex, topDict, topEnds);
    uint32_t topIndexSize = scratchIndex.count;
    uint32_t at = headerSize + (names.end - names.start) + topIndexSize + (strings.end - strings.start) +
        (globals.end - globals.start);
    uint32_t charsetOut = charsetAt != 0 ? at : 0;
    at += charsetBytes;
    uint32_t encodingOut = encodingAt != 0 ? at : 0;
    at += encodingBytes;
    uint32_t fdSelectOut = fdSelectAt != 0 ? at : 0;
    at += fdSelectBytes;
    uint32_t charStringsOut = at;
    RawArray<uint8_t> charIndex;
    writeIndex(charIndex, charData, charEnds);
    at += charIndex.count;
    // The Font DICTs, each with its Private DICT's offset, which follow them.
    RawArray<uint8_t> fdData;
    RawArray<uint32_t> fdEnds;
    uint32_t fdArrayOut = at;
    uint32_t fdIndexSize = 0;
    if (fdArray.count != 0) {
        // The Font DICTs' sizes do not depend on the offsets that they hold, so a first pass measures them.
        for (uint32_t pass = 0; pass < 2; ++pass) {
            fdData.clear();
            fdEnds.clear();
            uint32_t privateAt = fdArrayOut + fdIndexSize + topPrivate.end - topPrivate.start;
            for (uint32_t i = 0; i < fdArray.count; ++i) {
                readDict(cff, fdBounds.data[i * 2], fdBounds.data[i * 2 + 1], scratch);
                const PrivateRun& run = fdPrivates.data[i];
                writeDict(fdData, cff, scratch, 0, 0, 0, run.size, privateAt, 0, 0);
                fdEnds.push(fdData.count);
                privateAt += run.end - run.start;
            }
            RawArray<uint8_t> measured;
            writeIndex(measured, fdData, fdEnds);
            fdIndexSize = measured.count;
            measured.reset();
        }
        at += fdIndexSize;
    }
    uint32_t topPrivateOut = at;
    topDict.clear();
    writeDict(topDict, cff, top, charsetOut, encodingOut, charStringsOut, topPrivate.size, topPrivateOut,
        fdArrayOut, fdSelectOut);
    RawArray<uint8_t> out;
    out.append(cff.data, headerSize);
    out.append(cff.data + names.start, names.end - names.start);
    topEnds.clear();
    topEnds.push(topDict.count);
    writeIndex(out, topDict, topEnds);
    out.append(cff.data + strings.start, strings.end - strings.start);
    out.append(cff.data + globals.start, globals.end - globals.start);
    if (charsetAt != 0)
        out.append(cff.data + charsetAt, charsetBytes);
    if (encodingAt != 0)
        out.append(cff.data + encodingAt, encodingBytes);
    if (fdSelectAt != 0)
        out.append(cff.data + fdSelectAt, fdSelectBytes);
    out.append(charIndex.data, charIndex.count);
    if (fdArray.count != 0)
        writeIndex(out, fdData, fdEnds);
    PR_ASSERT(out.count == topPrivateOut);
    if (topPrivate.end != topPrivate.start)
        out.append(cff.data + topPrivate.start, topPrivate.end - topPrivate.start);
    for (uint32_t i = 0; i < fdPrivates.count; ++i) {
        const PrivateRun& run = fdPrivates.data[i];
        out.append(cff.data + run.start, run.end - run.start);
    }
    write(context, PrByteView{out.data, out.count});
    out.reset();
    topDict.reset();
    scratchIndex.reset();
    topEnds.reset();
    charIndex.reset();
    charData.reset();
    charEnds.reset();
    fdData.reset();
    fdEnds.reset();
    kept.reset();
    top.reset();
    scratch.reset();
    fdDict.reset();
    fdPrivates.reset();
    fdBounds.reset();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Outlines

// Appends a number of a Type 2 charstring (Adobe Technical Note #5177, 3.2), clamped to the range of a short.
void putCharNumber(RawArray<uint8_t>& out, int32_t value) {
    if (value >= -107 && value <= 107) {
        out.push(uint8_t(value + 139));
    } else if (value >= 108 && value <= 1131) {
        uint32_t v = uint32_t(value - 108);
        out.push(uint8_t((v >> 8) + 247));
        out.push(uint8_t(v));
    } else if (value >= -1131 && value <= -108) {
        uint32_t v = uint32_t(-value - 108);
        out.push(uint8_t((v >> 8) + 251));
        out.push(uint8_t(v));
    } else {
        int32_t clamped = value < -32768 ? -32768 : value > 32767 ? 32767 : value;
        out.push(28);
        put16(out, uint32_t(clamped) & 0xffff);
    }
}

// Appends the charstring of an outline in thousandths of an em with y up: each contour as a move, lines and curves,
// with quadratic curves raised to cubic ones, which draw the same curve. Each point is rounded to a whole unit, and
// each operand moves from the rounded point before it, so rounding does not accumulate.
void putCharString(RawArray<uint8_t>& out, const PrPath* path) {
    constexpr uint32_t kRmoveto = 21;
    constexpr uint32_t kRlineto = 5;
    constexpr uint32_t kRrcurveto = 8;
    constexpr uint32_t kEndchar = 14;
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    int32_t x = 0;
    int32_t y = 0;
    PrVec2 last{0, 0};
    auto to = [&](PrVec2 p) {
        int32_t px = int32_t(prF32Round(p.x));
        int32_t py = int32_t(prF32Round(p.y));
        putCharNumber(out, px - x);
        putCharNumber(out, py - y);
        x = px;
        y = py;
    };
    uint32_t at = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move:
                to(points[at]);
                out.push(kRmoveto);
                last = points[at++];
                break;
            case PrPathVerb_Line:
                to(points[at]);
                out.push(kRlineto);
                last = points[at++];
                break;
            case PrPathVerb_Quad: {
                PrVec2 control = points[at];
                PrVec2 end = points[at + 1];
                to(last + (control - last) * (2.0f / 3));
                to(end + (control - end) * (2.0f / 3));
                to(end);
                out.push(kRrcurveto);
                last = end;
                at += 2;
                break;
            }
            case PrPathVerb_Cubic:
                to(points[at]);
                to(points[at + 1]);
                to(points[at + 2]);
                out.push(kRrcurveto);
                last = points[at + 2];
                at += 3;
                break;
            default:
                break;
        }
    }
    out.push(kEndchar);
}

// Writes the glyphs as the font draws them, as a CID-keyed CFF table with the identity charset, whose CIDs are the
// font's glyph ids. Each kept glyph's charstring is its outline (prFontGlyphOutline) before size-adjust, unhinted, with
// its variations and synthetic bold applied. A document thus embeds the glyphs that the font draws even if its file
// does not hold them. The table has no widths and no hints, because a document gives its glyphs' widths itself.
void subsetOutlines(const PrFont* font, const uint16_t* PR_NULLABLE glyphs, uint32_t glyphCount, PrFontWriteFn write,
        void* PR_NULLABLE context) {
    uint32_t fontGlyphs = prFontGlyphCount(font);
    RawArray<uint8_t> kept;
    kept.resizeZeroed(fontGlyphs);
    uint32_t count = fontGlyphs != 0 ? 1 : 0;
    if (fontGlyphs != 0)
        kept.data[0] = 1;
    for (uint32_t i = 0; i < glyphCount; ++i) {
        if (glyphs[i] < fontGlyphs) {
            kept.data[glyphs[i]] = 1;
            count = glyphs[i] + 1 > count ? glyphs[i] + 1 : count;
        }
    }
    // A CFF has at least one glyph, which is .notdef.
    if (count == 0)
        count = 1;
    float units = 1000 / prFontSizeAdjust(font);
    PrAffine2 toCff = prAffine2Scale(PrVec2{units, -units});
    PrPath* path = prPathCreate();
    RawArray<uint8_t> charData;
    RawArray<uint32_t> charEnds;
    for (uint32_t g = 0; g < count; ++g) {
        prPathClear(path);
        if (g < fontGlyphs && kept.data[g] != 0)
            prFontGlyphOutline(font, uint16_t(g), 0, toCff, path);
        putCharString(charData, path);
        charEnds.push(charData.count);
    }
    prPathDestroy(path);
    kept.reset();

    // The fixed parts: the name, the strings that the ROS names, and no global subroutines.
    RawArray<uint8_t> nameData;
    RawArray<uint32_t> nameEnds;
    const char name[] = "Outlines";
    nameData.append(reinterpret_cast<const uint8_t*>(name), uint32_t(sizeof(name) - 1));
    nameEnds.push(nameData.count);
    RawArray<uint8_t> stringData;
    RawArray<uint32_t> stringEnds;
    const char registry[] = "Adobe";
    const char ordering[] = "Identity";
    stringData.append(reinterpret_cast<const uint8_t*>(registry), uint32_t(sizeof(registry) - 1));
    stringEnds.push(stringData.count);
    stringData.append(reinterpret_cast<const uint8_t*>(ordering), uint32_t(sizeof(ordering) - 1));
    stringEnds.push(stringData.count);
    constexpr int32_t kFirstCustomString = 391;
    constexpr uint32_t kOpRos = 1230;
    constexpr uint32_t kOpCidCount = 1234;

    // The Top DICT's operands are of fixed size, so its size does not depend on the offsets that it holds.
    auto topDict = [&](RawArray<uint8_t>& out, uint32_t charset, uint32_t fdSelect, uint32_t charStrings, uint32_t fdArray) {
        putOperand(out, kFirstCustomString);
        putOperand(out, kFirstCustomString + 1);
        putOperand(out, 0);
        putOperator(out, kOpRos);
        putOperand(out, int32_t(count));
        putOperator(out, kOpCidCount);
        putOperand(out, int32_t(charset));
        putOperator(out, kOpCharset);
        putOperand(out, int32_t(fdSelect));
        putOperator(out, kOpFdSelect);
        putOperand(out, int32_t(charStrings));
        putOperator(out, kOpCharStrings);
        putOperand(out, int32_t(fdArray));
        putOperator(out, kOpFdArray);
    };
    RawArray<uint8_t> top;
    RawArray<uint32_t> topEnds;
    topDict(top, 0, 0, 0, 0);
    topEnds.push(top.count);
    RawArray<uint8_t> out;
    out.push(1);        // major version
    out.push(0);        // minor version
    out.push(4);        // header size
    out.push(4);        // absolute offset size
    writeIndex(out, nameData, nameEnds);
    RawArray<uint8_t> measured;
    writeIndex(measured, top, topEnds);
    uint32_t topAt = out.count;
    uint32_t at = topAt + measured.count;
    RawArray<uint8_t> tail;
    writeIndex(tail, stringData, stringEnds);
    writeIndex(tail, RawArray<uint8_t>{}, RawArray<uint32_t>{});
    // The charset: every glyph after .notdef has the CID of its id, as one range.
    uint32_t charsetAt = at + tail.count;
    if (count > 1) {
        tail.push(2);
        put16(tail, 1);
        put16(tail, count - 2);
    } else {
        tail.push(0);
    }
    // FDSelect: every glyph is in the one Font DICT.
    uint32_t fdSelectAt = at + tail.count;
    tail.push(3);
    put16(tail, 1);
    put16(tail, 0);
    tail.push(0);
    put16(tail, count);
    uint32_t charStringsAt = at + tail.count;
    writeIndex(tail, charData, charEnds);
    // The Font DICT names its Private DICT, which follows the FDArray and holds only the default width.
    uint32_t fdArrayAt = at + tail.count;
    RawArray<uint8_t> privateDict;
    putOperand(privateDict, 0);
    putOperator(privateDict, 20);   // defaultWidthX
    RawArray<uint8_t> fontDict;
    RawArray<uint32_t> fontEnds;
    putOperand(fontDict, int32_t(privateDict.count));
    putOperand(fontDict, 0);
    putOperator(fontDict, kOpPrivate);
    fontEnds.push(fontDict.count);
    measured.clear();
    writeIndex(measured, fontDict, fontEnds);
    uint32_t privateAt = fdArrayAt + measured.count;
    fontDict.clear();
    putOperand(fontDict, int32_t(privateDict.count));
    putOperand(fontDict, int32_t(privateAt));
    putOperator(fontDict, kOpPrivate);
    writeIndex(tail, fontDict, fontEnds);
    tail.append(privateDict.data, privateDict.count);
    top.clear();
    topDict(top, charsetAt, fdSelectAt, charStringsAt, fdArrayAt);
    writeIndex(out, top, topEnds);
    PR_ASSERT(out.count == at);
    out.append(tail.data, tail.count);
    write(context, PrByteView{out.data, out.count});
    out.reset();
    tail.reset();
    measured.reset();
    top.reset();
    topEnds.reset();
    fontDict.reset();
    fontEnds.reset();
    privateDict.reset();
    nameData.reset();
    nameEnds.reset();
    stringData.reset();
    stringEnds.reset();
    charData.reset();
    charEnds.reset();
}

// Returns true if the font draws the outlines that its file holds: it has no synthetic bold, and its variation axes, if
// any, are at their defaults.
bool ownsOutlines(const PrFont* font) {
    if (prFontEmbolden(font) != 0)
        return false;
    for (uint32_t i = 0; i < prFontAxisCount(font); ++i) {
        if (prFontAxisCoordinate(font, i) != 0)
            return false;
    }
    return true;
}

bool openFace(const PrFont* font, Sfnt* out) {
    PrByteView bytes = prFontData(font);
    return sfntOpen(bytes.data, bytes.size, prFontIndex(font), out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrFontSubsetFormat prFontSubsetFormat(const PrFont* font) {
    Sfnt sfnt;
    if (!openFace(font, &sfnt))
        return PrFontSubsetFormat_None;
    bool glyf = sfntTable(sfnt, kTagGlyf).size != 0 && sfntTable(sfnt, kTagLoca).size != 0;
    bool cff = sfntTable(sfnt, kTagCff).size != 0;
    bool cff2 = sfntTable(sfnt, kTagCff2).size != 0;
    if (glyf && ownsOutlines(font))
        return PrFontSubsetFormat_TrueType;
    if (glyf || cff || cff2)
        return PrFontSubsetFormat_Cff;
    return PrFontSubsetFormat_None;
}

PrFontSubsetFormat prFontSubset(const PrFont* font, const uint16_t* PR_NULLABLE glyphs, uint32_t glyphCount,
        PrFontWriteFn write, void* PR_NULLABLE context) {
    PrFontSubsetFormat format = prFontSubsetFormat(font);
    Sfnt sfnt;
    if (format == PrFontSubsetFormat_None || !openFace(font, &sfnt))
        return PrFontSubsetFormat_None;
    bool written = true;
    if (format == PrFontSubsetFormat_TrueType)
        written = subsetTrueType(sfnt, glyphs, glyphCount, write, context);
    else if (ownsOutlines(font) && sfntTable(sfnt, kTagCff2).size == 0 && sfntTable(sfnt, kTagGlyf).size == 0)
        written = subsetCff(sfnt, glyphs, glyphCount, write, context);
    else
        subsetOutlines(font, glyphs, glyphCount, write, context);
    return written ? format : PrFontSubsetFormat_None;
}
