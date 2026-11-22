// Editing the tables of a single font file, as tests and suites make fonts whose tables are malformed.

#pragma once

#include "pr/align.h"
#include "pr/array.h"
#include "pr/sort.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// The big-endian values at `bytes`, and appended to or written into `out`.
inline uint32_t be32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}
inline uint16_t be16(const uint8_t* bytes) {
    return uint16_t(bytes[0] << 8 | bytes[1]);
}
inline void put16(Array<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value >> 8));
    out.push(uint8_t(value));
}
inline void put32(Array<uint8_t>& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value);
}
inline void set16(Array<uint8_t>& out, uint32_t at, uint32_t value) {
    out[at] = uint8_t(value >> 8);
    out[at + 1] = uint8_t(value);
}

// Returns the bytes of table `tag` of a single font file, or empty if it has none.
inline Array<uint8_t> tableOf(const Array<uint8_t>& font, uint32_t tag) {
    uint16_t count = be16(font.data() + 4);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* record = font.data() + 12 + i * 16;
        if (be32(record) == tag) {
            Array<uint8_t> table;
            table.append(font.data() + be32(record + 8), be32(record + 12));
            return table;
        }
    }
    return {};
}

// Returns the single font file `font` with table `tag` replaced by `table`, or added if it has none.
inline Array<uint8_t> withTable(const Array<uint8_t>& font, uint32_t tag, const Array<uint8_t>& table) {
    struct Table {
        uint32_t tag;
        Array<uint8_t> bytes;
    };
    Array<Table> tables;
    uint16_t count = be16(font.data() + 4);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* record = font.data() + 12 + i * 16;
        if (be32(record) != tag)
            tables.push(Table{be32(record), tableOf(font, be32(record))});
    }
    tables.push(Table{tag, table});
    // The tags are distinct, so that the order is the same whether or not the sort is stable.
    sortItems(tables.span(), [](const Table& a, const Table& b) { return a.tag < b.tag; });
    Array<uint8_t> out;
    out.append(font.data(), 4);
    put16(out, tables.count());
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    uint32_t offset = 12 + tables.count() * 16;
    for (const Table& t : tables) {
        put32(out, t.tag);
        put32(out, 0);
        put32(out, offset);
        put32(out, t.bytes.count());
        offset += alignUp(t.bytes.count(), 4);
    }
    for (const Table& t : tables) {
        out.append(t.bytes.span());
        while (out.count() % 4 != 0)
            out.push(0);
    }
    return out;
}

} // namespace harness

PR_ASSUME_NONNULL_END
