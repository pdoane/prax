// The operations on a terminal's screens.

#include "vterm/src/vterm_screen.h"

#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/unicode.h"
#include "pr/unicode_grapheme.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

constexpr uint32_t kEmojiPresentation = 0xfe0f;

// The interval between the default tab stops, in columns.
constexpr uint32_t kTabInterval = 8;

// The characters of the DEC Special Graphics set, for the codes from 0x5f to 0x7e.
constexpr uint16_t kDecSpecial[32] = {
    0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c,
    0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265,
    0x03c0, 0x2260, 0x00a3, 0x00b7,
};

// Returns the character that `codePoint` stands for in `charset`.
uint32_t charsetMap(VtermCharset charset, uint32_t codePoint) {
    if (charset == VtermCharset_DecSpecial && codePoint >= 0x5f && codePoint <= 0x7e)
        return kDecSpecial[codePoint - 0x5f];
    if (charset == VtermCharset_British && codePoint == '#')
        return 0x00a3;
    return codePoint;
}

// The 8-bit code that DEC's checksum sums for a cell's character. ASCII and Latin-1 count as themselves, and a
// character of the DEC Special Graphics set counts as the code that selects it. DEL counts as zero, and anything else
// as ESC.
uint32_t checksumCode(uint32_t codePoint) {
    if (codePoint == 0x7f)
        return 0;
    if (codePoint >= 0x20 && codePoint <= 0xff)
        return codePoint;
    for (uint32_t i = 0; i < 32; ++i) {
        if (kDecSpecial[i] == codePoint)
            return 0x5f + i;
    }
    return 0x1b;
}

// What a cell's attributes add to DEC's checksum.
uint32_t checksumAttributes(const PrVtermCell& cell) {
    uint32_t sum = 0;
    if ((cell.flags & PrVtermCellFlag_Invisible) != 0)
        sum += 0x08;
    if (cell.underline != PrVtermUnderline_None)
        sum += 0x10;
    if ((cell.flags & PrVtermCellFlag_Inverse) != 0)
        sum += 0x20;
    if ((cell.flags & PrVtermCellFlag_Blink) != 0)
        sum += 0x40;
    if ((cell.flags & PrVtermCellFlag_Bold) != 0)
        sum += 0x80;
    return sum;
}

// The ring slot that holds row `row`.
uint32_t rowSlotOf(const VtermRows* rows, uint32_t row) {
    uint32_t slot = rows->start + row;
    return slot >= rows->slots.count ? slot - rows->slots.count : slot;
}

// The ring slot that holds the arena's block `index`, counted from the oldest.
uint32_t arenaSlotOf(const VtermArena* arena, uint32_t index) {
    uint32_t slot = arena->start + index;
    return slot >= arena->slots.count ? slot - arena->slots.count : slot;
}

VtermArenaBlock& arenaBlockAt(VtermArena* arena, uint32_t index) {
    return arena->slots.data[arenaSlotOf(arena, index)];
}

// Frees a block of the arena. If `keep` is true, the block is a standard block and the arena has no spare, the block's
// storage becomes the spare.
void blockFree(VtermArena* arena, VtermArenaBlock* block, bool keep) {
    if (keep && block->size == kVtermArenaBlockBytes && arena->spare == nullptr)
        arena->spare = block->data;
    else
        prMemoryFree(block->data);
    block->pens.reset();
    *block = VtermArenaBlock{};
}

// The size in bytes of a packed line's marks: one byte each for the marks and the command's status, and four each for
// the input column and the exit code.
constexpr uint32_t kPackedMarksBytes = 10;

// The most bytes that an unsigned LEB128 of a 32-bit value takes.
constexpr uint32_t kCountMaxBytes = 5;

// The bits of a cell's first word that hold its code point. The bits above hold its cluster's count and its underline.
constexpr uint32_t kCodePointMask = 0x1fffff;

// The shape bits of a packed cell: the count of its cluster's code points after the first, then its placement flags.
constexpr uint8_t kShapeClusterMask = 0x1f;
constexpr uint8_t kShapeWide = 1u << 5;
constexpr uint8_t kShapeWideTail = 1u << 6;
constexpr uint8_t kShapeWideSpacer = 1u << 7;

// Writes `value` at `at` as an unsigned LEB128, and returns the byte after it.
PR_FORCE_INLINE uint8_t* countWrite(uint8_t* at, uint32_t value) {
    while (value >= 0x80) {
        *at++ = uint8_t(value | 0x80);
        value >>= 7;
    }
    *at++ = uint8_t(value);
    return at;
}

// The number of bytes that `value` takes as an unsigned LEB128.
uint32_t countBytes(uint32_t value) {
    uint32_t bytes = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++bytes;
    }
    return bytes;
}

// Writes `codePoint`, which is below 2^21, at `at` in UTF-8's encoding form. The form extends to every such value,
// surrogates included, so a cell's code point unpacks unchanged. Returns the byte after it.
PR_FORCE_INLINE uint8_t* codePointWrite(uint8_t* at, uint32_t codePoint) {
    if (codePoint < 0x80) {
        *at++ = uint8_t(codePoint);
    } else if (codePoint < 0x800) {
        *at++ = uint8_t(0xc0 | (codePoint >> 6));
        *at++ = uint8_t(0x80 | (codePoint & 0x3f));
    } else if (codePoint < 0x10000) {
        *at++ = uint8_t(0xe0 | (codePoint >> 12));
        *at++ = uint8_t(0x80 | ((codePoint >> 6) & 0x3f));
        *at++ = uint8_t(0x80 | (codePoint & 0x3f));
    } else {
        *at++ = uint8_t(0xf0 | (codePoint >> 18));
        *at++ = uint8_t(0x80 | ((codePoint >> 12) & 0x3f));
        *at++ = uint8_t(0x80 | ((codePoint >> 6) & 0x3f));
        *at++ = uint8_t(0x80 | (codePoint & 0x3f));
    }
    return at;
}

// Reads a code point that codePointWrite wrote at `*at`, and moves `*at` past it.
PR_FORCE_INLINE uint32_t codePointRead(const uint8_t** at) {
    const uint8_t* bytes = *at;
    uint32_t lead = bytes[0];
    if (lead < 0x80) {
        *at = bytes + 1;
        return lead;
    }
    if (lead < 0xe0) {
        *at = bytes + 2;
        return (lead & 0x1f) << 6 | (bytes[1] & 0x3fu);
    }
    if (lead < 0xf0) {
        *at = bytes + 3;
        return (lead & 0x0f) << 12 | (bytes[1] & 0x3fu) << 6 | (bytes[2] & 0x3fu);
    }
    *at = bytes + 4;
    return (lead & 0x07) << 18 | (bytes[1] & 0x3fu) << 12 | (bytes[2] & 0x3fu) << 6 | (bytes[3] & 0x3fu);
}

// The pen of a cell: its attributes and colors, without its link or its place in a wide cluster.
PR_FORCE_INLINE VtermPackedPen penOf(const PrVtermCell& cell) {
    VtermPackedPen pen;
    pen.flags = PrVtermCellFlags(cell.flags & ~PrVtermCellFlag_WidePlacement);
    pen.underline = PrVtermUnderline(cell.underline);
    pen._pad = 0;
    pen.foreground = cell.foreground;
    pen.background = cell.background;
    pen.underlineColor = cell.underlineColor;
    return pen;
}

// The hash of a pen. It mixes every bit of the pen's two words into the high bits of the hash.
uint32_t penHash(const VtermPackedPen& pen) {
    uint64_t words[2];
    prMemCopy(words, &pen, sizeof(words));
    uint64_t mixed = (words[0] * 0x9e3779b97f4a7c15ull) ^ (words[1] * 0xc2b2ae3d27d4eb4full);
    mixed ^= mixed >> 29;
    return uint32_t((mixed * 0x9e3779b97f4a7c15ull) >> 32);
}

// A cell as three words, which writes and packing copy and compare. The first word holds the code point, the cluster
// count, the underline and the cluster's start. The others hold the rest of the style and the link.
struct CellWords {
    uint64_t words[3];
};
static_assert(sizeof(PrVtermCell) == sizeof(CellWords));

// The bits of a cell's second word that hold its link and its place in a wide cluster. They are not part of its style.
constexpr uint64_t kLinkPlacementBits1 = 0xffff0000u | uint64_t(PrVtermCellFlag_WidePlacement);

// The bits of a cell's first word that are part of its style: its underline.
constexpr uint64_t kStyleBits0 = uint64_t(7) << 26;

PR_FORCE_INLINE CellWords cellWordsOf(const PrVtermCell& cell) {
    CellWords words;
    prMemCopy(&words, &cell, sizeof(CellWords));
    return words;
}

bool isSamePen(const VtermPackedPen& a, const VtermPackedPen& b) {
    return prMemEquals(&a, &b, sizeof(VtermPackedPen));
}

// Returns true if the cell of `words` holds ASCII or nothing, with no cluster, link or placement, in the style `style`.
// `style` is a cell's words without its text, link or placement. Such a cell's words differ from the style's only in
// the low 7 bits of the code point and in the cluster's start.
PR_FORCE_INLINE bool isAsciiInStyle(const CellWords& words, const CellWords& style) {
    uint64_t differs = ((words.words[0] ^ style.words[0]) & 0xffffff80u) | (words.words[1] ^ style.words[1]) |
        (words.words[2] ^ style.words[2]);
    return differs == 0;
}

// Returns true if the cell holds nothing and has no style, link or placement, so that it unpacks as a cell of all
// zeros.
bool isPackedBlank(const PrVtermCell& cell) {
    // Most blanks are all zeros except for a stale cluster start, which one test of the words finds.
    CellWords words = cellWordsOf(cell);
    if (((words.words[0] & 0xffffffffu) | words.words[1] | words.words[2]) == 0)
        return true;
    bool empty = cell.codePoint == 0 && cell.clusterCount == 0 && cell.underline == PrVtermUnderline_None;
    bool unstyled = cell.flags == 0 && cell.link == kPrVtermLinkNone;
    bool uncolored = cell.foreground.kind == PrVtermColorKind_Default &&
        cell.background.kind == PrVtermColorKind_Default && cell.underlineColor.kind == PrVtermColorKind_Default;
    return empty && unstyled && uncolored;
}

// The number of cells of `line` that packing keeps: up to the last cell that does not unpack as all zeros.
uint32_t packedEnd(const VtermStoredLine& line) {
    uint32_t end = line.written < line.cells.count ? line.written : line.cells.count;
    while (end > 0 && isPackedBlank(line.cells.data[end - 1]))
        --end;
    return end;
}

// Writes the marks of `line` at `at`, and returns the byte after them.
uint8_t* marksWrite(uint8_t* at, const VtermStoredLine& line) {
    at[0] = line.marks;
    at[1] = line.commandStatus;
    prMemCopy(at + 2, &line.inputColumn, sizeof(uint32_t));
    prMemCopy(at + 6, &line.exitCode, sizeof(int32_t));
    return at + kPackedMarksBytes;
}

// The parts of a line to pack, other than its runs: its cells, its text, its shapes and the line whose marks it keeps.
struct PackedParts {
    uint32_t cellCount;
    const uint8_t* text;
    uint32_t textBytes;
    const uint8_t* PR_NULLABLE shapes;              // a byte for each cell, or null for a line without shapes
    const VtermStoredLine* PR_NULLABLE marked;      // the line that holds the marks, or null for a line without marks
};

// Packs a line from `parts`, `runs` and `linkRuns` into the arena, and writes the start of its bytes to `*position`.
// Each run of pens ends where the next starts, or at the line's last cell. Interning the runs' pens may merge runs, and
// empty runs are dropped. Returns the line's packing flags.
VtermPackedFlags packWrite(VtermScrollback* scrollback, const PackedParts& parts, RawArray<VtermPackedRun>* runs,
        const RawArray<VtermPackedRun>& linkRuns, VtermArenaPosition* position) {
    bool marked = parts.marked != nullptr;
    bool linked = linkRuns.count != 0;
    uint32_t shapeBytes = parts.shapes != nullptr ? parts.cellCount : 0;
    uint32_t bound = (marked ? kPackedMarksBytes : 0) + 4 * kCountMaxBytes + parts.textBytes + shapeBytes +
        2 * kCountMaxBytes * runs->count + 3 * kCountMaxBytes * linkRuns.count;
    uint8_t* start = vtermArenaReserve(&scrollback->arena, bound, position);
    // Runs whose styles differ only in bytes that their colors do not use get one pen, and merge.
    uint32_t merged = 0;
    for (uint32_t k = 0; k < runs->count; ++k) {
        VtermPackedRun run = runs->data[k];
        run.count = (k + 1 < runs->count ? runs->data[k + 1].start : parts.cellCount) - run.start;
        if (run.count == 0)
            continue;
        run.index = vtermArenaPen(&scrollback->arena, run.pen);
        if (merged != 0 && runs->data[merged - 1].index == run.index)
            runs->data[merged - 1].count += run.count;
        else
            runs->data[merged++] = run;
    }
    runs->count = merged;
    uint32_t runBytes = 0;
    for (const VtermPackedRun& run : runs->span())
        runBytes += countBytes(run.count) + countBytes(run.index);
    if (runs->count == 1 && runs->data[0].index == 0)
        runBytes = 0;
    uint32_t linkBytes = 0;
    uint32_t linkEnd = 0;
    for (const VtermPackedRun& run : linkRuns.span()) {
        linkBytes += countBytes(run.start - linkEnd) + countBytes(run.count) + countBytes(run.link);
        linkEnd = run.start + run.count;
    }
    uint8_t* at = marked ? marksWrite(start, *parts.marked) : start;
    at = countWrite(at, parts.cellCount);
    at = countWrite(at, parts.textBytes);
    at = countWrite(at, runBytes);
    if (linked)
        at = countWrite(at, linkBytes);
    prMemCopy(at, parts.text, parts.textBytes);
    at += parts.textBytes;
    if (parts.shapes != nullptr) {
        prMemCopy(at, parts.shapes, parts.cellCount);
        at += parts.cellCount;
    }
    if (runBytes != 0) {
        for (const VtermPackedRun& run : runs->span()) {
            at = countWrite(at, run.count);
            at = countWrite(at, run.index);
        }
    }
    linkEnd = 0;
    for (const VtermPackedRun& run : linkRuns.span()) {
        at = countWrite(at, run.start - linkEnd);
        at = countWrite(at, run.count);
        at = countWrite(at, run.link);
        linkEnd = run.start + run.count;
    }
    vtermArenaCommit(&scrollback->arena, uint32_t(at - start));
    VtermPackedFlags flags = 0;
    if (marked)
        flags |= VtermPackedFlag_Marks;
    if (linked)
        flags |= VtermPackedFlag_Links;
    if (parts.shapes != nullptr)
        flags |= VtermPackedFlag_Shaped;
    return flags;
}

// Packs the first `end` cells of `line` into the arena, whatever they hold, and writes the start of its bytes to
// `*position`. Returns the line's packing flags.
VtermPackedFlags packCells(VtermScrollback* scrollback, const VtermStoredLine& line, uint32_t end,
        VtermArenaPosition* position) {
    RawArray<uint8_t>& text = scrollback->text;
    RawArray<uint8_t>& shapes = scrollback->shapes;
    RawArray<VtermPackedRun>& runs = scrollback->runs;
    RawArray<VtermPackedRun>& linkRuns = scrollback->linkRuns;
    runs.clear();
    linkRuns.clear();
    // Each code point takes at most 4 bytes, and the line's code points include every cluster's.
    text.resize(kPrUtf8MaxBytes * (end + line.codePoints.count));
    uint8_t* written = text.data;
    // The shapes, allocated and zeroed once a cell has a shape.
    uint8_t* PR_NULLABLE shaped = nullptr;
    // The style of the run in progress, as a cell's words without its text, link or placement. An ASCII cell in that
    // style (isAsciiInStyle) continues the run with its byte alone. The first run starts in the style of all zeros, and
    // is dropped if it is empty.
    CellWords key{};
    runs.push(VtermPackedRun{});
    const PrVtermCell* cells = line.cells.data;
    const uint32_t* codePoints = line.codePoints.data;
    for (size_t i = 0; i < end; ++i) {
        CellWords words = cellWordsOf(cells[i]);
        if (isAsciiInStyle(words, key)) {
            // A run of such cells is written in a loop of its own.
            uint8_t* out = written;
            do {
                *out++ = uint8_t(words.words[0]);
                if (++i == end)
                    break;
                words = cellWordsOf(cells[i]);
            } while (isAsciiInStyle(words, key));
            written = out;
            if (i == end)
                break;
        }
        // Copy the cell, so that the writes of text, which may alias anything, do not force it to be read again.
        PrVtermCell cell;
        prMemCopy(&cell, &words, sizeof(PrVtermCell));
        uint8_t shape = uint8_t(cell.clusterCount | ((cell.flags & PrVtermCellFlag_WidePlacement) >> 3));
        static_assert(PrVtermCellFlag_Wide >> 3 == kShapeWide && PrVtermCellFlag_WideTail >> 3 == kShapeWideTail);
        static_assert(PrVtermCellFlag_WideSpacer >> 3 == kShapeWideSpacer);
        if (shape != 0) {
            if (shaped == nullptr) {
                shapes.resize(end);
                shaped = shapes.data;
                prMemZero(shaped, end);
            }
            shaped[i] = shape;
        }
        if ((shape & (kShapeWideTail | kShapeWideSpacer)) != 0) {
            PR_ASSERT(cell.codePoint == 0 && cell.clusterCount == 0);
        } else {
            written = codePointWrite(written, cell.codePoint);
            for (uint32_t k = 0; k < cell.clusterCount; ++k)
                written = codePointWrite(written, codePoints[cell.clusterStart + k]);
        }
        words.words[0] &= kStyleBits0;
        words.words[1] &= ~kLinkPlacementBits1;
        if (words.words[0] != key.words[0] || words.words[1] != key.words[1] || words.words[2] != key.words[2]) {
            key = words;
            runs.push(VtermPackedRun{.start = uint32_t(i), .pen = penOf(cell)});
        }
        if (cell.link != kPrVtermLinkNone) {
            bool continues = linkRuns.count != 0 && linkRuns.last().link == cell.link &&
                linkRuns.last().start + linkRuns.last().count == i;
            if (continues)
                ++linkRuns.last().count;
            else
                linkRuns.push(VtermPackedRun{.start = uint32_t(i), .count = 1, .link = cell.link});
        }
    }
    PackedParts parts{
        .cellCount = end,
        .text = text.data,
        .textBytes = uint32_t(written - text.data),
        .shapes = shaped,
        .marked = line.marks != 0 ? &line : nullptr,
    };
    return packWrite(scrollback, parts, &runs, linkRuns, position);
}

// The ring slot that holds the scrollback's line `index`.
uint32_t scrollbackSlotOf(const VtermScrollback* scrollback, uint32_t index) {
    uint32_t slot = scrollback->start + index;
    return slot >= scrollback->slots.count ? slot - scrollback->slots.count : slot;
}

// The number of slots in the scrollback's ring when it first grows.
constexpr uint32_t kScrollbackInitialSlots = 16;

// Adds a record after the newest line and returns it, for the caller to fill. If the scrollback is at its limit, its
// oldest line is dropped first. `*dropped` is set to whether a line was dropped.
VtermPackedLine& scrollbackAppend(VtermScrollback* scrollback, bool* dropped) {
    PR_ASSERT(scrollback->limit > 0);
    *dropped = false;
    if (scrollback->count == scrollback->limit) {
        // Drop the oldest line. Its block goes too, once the new oldest line starts in a newer block.
        scrollback->start = scrollbackSlotOf(scrollback, 1);
        --scrollback->count;
        VtermArena* arena = &scrollback->arena;
        uint32_t block = arena->first + arena->count;
        if (scrollback->count != 0)
            block = scrollback->slots.data[scrollback->start].position.block;
        if (block != arena->first)
            vtermArenaDropBefore(arena, block);
        *dropped = true;
    }
    if (scrollback->count == scrollback->slots.count) {
        // The ring is full but below its limit, so it grows, doubling up to the limit. The lines are copied in order
        // from the oldest.
        uint32_t grown = scrollback->count < scrollback->limit / 2 ? 2 * scrollback->count : scrollback->limit;
        if (grown < kScrollbackInitialSlots)
            grown = kScrollbackInitialSlots < scrollback->limit ? kScrollbackInitialSlots : scrollback->limit;
        RawArray<VtermPackedLine> ordered;
        ordered.reserveExact(grown);
        for (uint32_t i = 0; i < scrollback->count; ++i)
            ordered.push(scrollback->slots.data[scrollbackSlotOf(scrollback, i)]);
        ordered.resize(grown);
        scrollback->slots.reset();
        scrollback->slots = ordered;
        scrollback->start = 0;
    }
    return scrollback->slots.data[scrollbackSlotOf(scrollback, scrollback->count++)];
}

void lineReset(VtermStoredLine* line) {
    line->cells.reset();
    line->codePoints.reset();
    line->links.reset();
}

// Swaps the screen's rows `a` and `b`.
void swapRows(Vterm* vterm, uint32_t a, uint32_t b) {
    VtermStoredLine& first = vtermRow(vterm, a);
    VtermStoredLine& second = vtermRow(vterm, b);
    VtermStoredLine held = first;
    first = second;
    second = held;
}

void reverseRows(Vterm* vterm, uint32_t first, uint32_t last) {
    while (first + 1 < last) {
        swapRows(vterm, first, last - 1);
        ++first;
        --last;
    }
}

// Rotates the rows from `first` to `last`, excluded, so that the row `first + count` becomes the first.
void rotateRows(Vterm* vterm, uint32_t first, uint32_t last, uint32_t count) {
    if (count == 0 || count >= last - first)
        return;
    reverseRows(vterm, first, first + count);
    reverseRows(vterm, first + count, last);
    reverseRows(vterm, first, last);
}

// The width in cells of `codePoint` at the start of a cluster.
uint32_t widthOf(uint32_t codePoint) {
    if (codePoint < 0x300)
        return 1;
    PrUnicodeFlags flags = prUnicodeFlags(codePoint);
    if ((flags & PrUnicodeFlag_EastAsianWide) != 0)
        return 2;
    if ((flags & PrUnicodeFlag_DefaultIgnorable) != 0)
        return 0;
    switch (prUnicodeGeneralCategory(codePoint)) {
        case PrUnicodeGeneralCategory_NonspacingMark:
        case PrUnicodeGeneralCategory_EnclosingMark:
        case PrUnicodeGeneralCategory_Format:
        case PrUnicodeGeneralCategory_Control:
            return 0;
        default:
            return 1;
    }
}

// Rebuilds the line's code points from the clusters of its cells, dropping those that cells no longer use.
void compactCodePoints(VtermStoredLine* line) {
    RawArray<uint32_t> kept;
    for (uint32_t i = 0; i < line->cells.count; ++i) {
        PrVtermCell& cell = line->cells.data[i];
        if (cell.clusterCount == 0)
            continue;
        uint32_t start = kept.count;
        kept.append(line->codePoints.data + cell.clusterStart, cell.clusterCount);
        cell.clusterStart = start;
    }
    line->codePoints.reset();
    line->codePoints = kept;
}

// Compacts the line's code points once the line holds more than four per cell, plus 64. This keeps storage bounded as
// clusters are rewritten.
void codePointsTrim(VtermStoredLine* line) {
    if (line->codePoints.count > 4 * line->cells.count + 64)
        compactCodePoints(line);
}

// Appends `codePoint` to the cluster of `cell`, which is a cell of `line`.
void clusterAppend(VtermStoredLine* line, PrVtermCell* cell, uint32_t codePoint) {
    if (cell->clusterCount != 0 && cell->clusterStart + cell->clusterCount != line->codePoints.count) {
        // The cluster is not at the end of the code points, so it moves there to grow.
        uint32_t start = line->codePoints.count;
        for (uint32_t i = 0; i < cell->clusterCount; ++i)
            line->codePoints.push(line->codePoints.data[cell->clusterStart + i]);
        cell->clusterStart = start;
    }
    if (cell->clusterCount == 0)
        cell->clusterStart = line->codePoints.count;
    line->codePoints.push(codePoint);
    ++cell->clusterCount;
    codePointsTrim(line);
}

// Sets the `count` cells at `cells` to `cell`. A cell of all zeros, such as a blank of the default background, is
// filled by zeroing memory.
void cellsFill(PrVtermCell* cells, const PrVtermCell& cell, uint32_t count) {
    constexpr PrVtermCell kZero{};
    if (prMemEquals(&cell, &kZero, sizeof(PrVtermCell))) {
        prMemZero(cells, sizeof(PrVtermCell) * count);
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
        cells[i] = cell;
}

// Clears a cell of what it holds, keeping its colors and attributes.
void cellEmpty(PrVtermCell* cell) {
    cell->codePoint = 0;
    cell->clusterCount = 0;
    cell->flags &= PrVtermCellFlags(~PrVtermCellFlag_WidePlacement);
}

// If column `column` holds half of a wide cluster, empties the other half, so that a write to the column leaves no half
// cluster.
void splitWide(VtermStoredLine* line, uint32_t column) {
    if (column >= line->cells.count)
        return;
    PrVtermCell& cell = line->cells.data[column];
    if ((cell.flags & PrVtermCellFlag_WideTail) != 0 && column > 0)
        cellEmpty(&line->cells.data[column - 1]);
    if ((cell.flags & PrVtermCellFlag_Wide) != 0 && column + 1 < line->cells.count)
        cellEmpty(&line->cells.data[column + 1]);
}

// Empties both halves of a wide cluster whose tail is at `column`, so that no cluster crosses the edge just before the
// column. A range of cells that moves begins or ends at such an edge.
void splitEdge(VtermStoredLine* line, uint32_t column) {
    if (column == 0 || column >= line->cells.count || (line->cells.data[column].flags & PrVtermCellFlag_WideTail) == 0)
        return;
    cellEmpty(&line->cells.data[column - 1]);
    cellEmpty(&line->cells.data[column]);
}

// Copies the cells of `source` from column `start` to `end`, excluded, with their clusters, to the same columns of
// `target`, another line.
void cellsCopy(VtermStoredLine* target, const VtermStoredLine& source, uint32_t start, uint32_t end) {
    for (uint32_t i = start; i < end; ++i) {
        PrVtermCell cell = source.cells.data[i];
        if (cell.clusterCount != 0) {
            uint32_t clusterStart = target->codePoints.count;
            target->codePoints.append(source.codePoints.data + cell.clusterStart, cell.clusterCount);
            cell.clusterStart = clusterStart;
        }
        target->cells.data[i] = cell;
    }
    vtermLineWrote(target, source.written < end ? source.written : end);
    codePointsTrim(target);
}

// Returns true if every byte of the cell is zero, as in a blank of the default background.
bool isZeroCell(const PrVtermCell& cell) {
    constexpr PrVtermCell kZero{};
    return prMemEquals(&cell, &kZero, sizeof(PrVtermCell));
}

// Returns true if every cell of `line` from its `written` bound on is all zeros.
bool isZeroPastWritten(const VtermStoredLine& line) {
    for (uint32_t i = line.written; i < line.cells.count; ++i) {
        if (!isZeroCell(line.cells.data[i]))
            return false;
    }
    return true;
}

// The number of cells of `line` that may not be all zeros: those before its `written` bound.
uint32_t writtenEnd(const VtermStoredLine& line) {
    PR_ASSERT(isZeroPastWritten(line));
    return line.written < line.cells.count ? line.written : line.cells.count;
}

// Returns true if the cell shows nothing that a rewrap keeps at the end of a line.
bool isTrailingBlank(const PrVtermCell& cell) {
    // Most blank cells are all zeros, which one comparison finds.
    if (isZeroCell(cell))
        return true;
    bool unstyled = (cell.flags & ~PrVtermCellFlag_WidePlacement) == 0 && cell.underline == PrVtermUnderline_None;
    return cell.codePoint == 0 && unstyled && cell.background.kind == PrVtermColorKind_Default;
}

// Returns true if no cell of the line shows anything.
bool isBlankLine(const VtermStoredLine& line) {
    uint32_t end = writtenEnd(line);
    for (uint32_t i = 0; i < end; ++i) {
        if (!isTrailingBlank(line.cells.data[i]))
            return false;
    }
    return true;
}

// The number of cells of a line that a rewrap keeps. The cells past the last one that shows something are dropped.
uint32_t contentEnd(const VtermStoredLine& line) {
    uint32_t end = writtenEnd(line);
    while (end > 0 && isTrailingBlank(line.cells.data[end - 1]))
        --end;
    return end;
}

// The index that stands for no line in a rewrap.
constexpr uint32_t kRewrapLineNone = UINT32_MAX;

// The lines that a resize of the primary screen reads: the scrollback's lines, then the screen rows that it keeps.
struct ResizeSource {
    const VtermScrollback* scrollback;
    const RawArray<VtermStoredLine>* rows;      // by row, from the top
    uint32_t count;                             // the number of scrollback lines and rows
    uint32_t columns;                           // the width of the lines

    // A line of the scrollback, unpacked to `columns` cells for a rewrap to read.
    VtermStoredLine unpacked;
};

PrVtermLineFlags sourceFlags(const ResizeSource* source, uint32_t index) {
    uint32_t packed = source->scrollback->count;
    if (index < packed)
        return vtermScrollbackAt(source->scrollback, index).flags;
    return source->rows->data[index - packed].flags;
}

// Line `index` of the source. It is valid until the next call.
const VtermStoredLine& sourceLine(ResizeSource* source, uint32_t index) {
    uint32_t packed = source->scrollback->count;
    if (index >= packed)
        return source->rows->data[index - packed];
    vtermScrollbackUnpack(source->scrollback, index, source->columns, &source->unpacked);
    return source->unpacked;
}

// The lines that a rewrap builds, packed into `out` as each is finished, and the line and column where the tracked
// position ends.
struct Rewrap {
    VtermScrollback* out;
    uint32_t columns;

    // The line in progress, of `columns` cells, and the column that the next cell goes to. While `open`, the line is
    // the last that the rewrap built. `count` is the number of lines built.
    VtermStoredLine line;
    uint32_t count;
    uint32_t column;
    bool open;

    // For a rewrap of packed lines: the text and the runs of the packed line in progress, and the runs of the line
    // being read.
    RawArray<uint8_t> text;
    RawArray<VtermPackedRun> runs;
    RawArray<VtermPackedRun> sourceRuns;

    bool tracked;
    uint32_t trackedLine;
    uint32_t trackedColumn;
    uint32_t topLine;           // the built line where the line of the old top row begins

    // The line of the current command's prompt among the lines that the rewrap reads, and among those that it builds,
    // or kRewrapLineNone.
    uint32_t promptSource;
    uint32_t promptLine;

    // For each line that the rewrap reads, the built line that its first cell goes to. Null if the caller needs no map.
    RawArray<uint32_t>* PR_NULLABLE lineMap;
};

// Packs the line in progress. Its cells past those that the rewrap placed hold nothing.
void rewrapFinish(Rewrap* rewrap) {
    if (!rewrap->open)
        return;
    rewrap->open = false;
    VtermStoredLine& line = rewrap->line;
    line.written = rewrap->column;
    bool dropped = false;
    vtermScrollbackPush(rewrap->out, line, &dropped);
    PR_ASSERT(!dropped);
}

// Finishes the line in progress and begins another.
void rewrapBegin(Vterm* vterm, Rewrap* rewrap) {
    rewrapFinish(rewrap);
    VtermStoredLine& line = rewrap->line;
    uint32_t written = line.written < rewrap->columns ? line.written : rewrap->columns;
    prMemZero(line.cells.data, sizeof(PrVtermCell) * written);
    line.written = 0;
    line.codePoints.clear();
    line.flags = 0;
    vtermLineClearMarks(&line);
    line.revision = vtermRevise(vterm);
    ++rewrap->count;
    rewrap->column = 0;
    rewrap->open = true;
}

// Ends the line in progress as wrapped and begins another.
void rewrapBreak(Vterm* vterm, Rewrap* rewrap) {
    rewrap->line.flags |= PrVtermLineFlag_Wrapped;
    rewrapBegin(vterm, rewrap);
}

// Appends a cell of `source` to the line in progress, with its wide tail if it has one.
void rewrapCell(Vterm* vterm, Rewrap* rewrap, const VtermStoredLine& source, uint32_t index) {
    const PrVtermCell& cell = source.cells.data[index];
    uint32_t width = (cell.flags & PrVtermCellFlag_Wide) != 0 && rewrap->columns > 1 ? 2 : 1;
    if (rewrap->column + width > rewrap->columns) {
        if (rewrap->column < rewrap->columns)
            rewrap->line.cells.data[rewrap->column++] = PrVtermCell{.flags = PrVtermCellFlag_WideSpacer};
        rewrapBreak(vterm, rewrap);
    }
    VtermStoredLine& line = rewrap->line;
    PrVtermCell placed = cell;
    if (width == 1)
        placed.flags &= PrVtermCellFlags(~PrVtermCellFlag_WidePlacement);
    if (placed.clusterCount != 0) {
        placed.clusterStart = line.codePoints.count;
        line.codePoints.append(source.codePoints.data + cell.clusterStart, cell.clusterCount);
    }
    line.cells.data[rewrap->column] = placed;
    if (width == 2) {
        bool tailed = index + 1 < source.cells.count;
        line.cells.data[rewrap->column + 1] = tailed ? source.cells.data[index + 1] : PrVtermCell{};
    }
    rewrap->column += width;
}

// Appends the cells of `source` from column `start` to `end`, excluded, with their clusters, to the line in progress.
// None of the cells is part of a wide cluster, and the line has room for them.
void rewrapRun(Rewrap* rewrap, const VtermStoredLine& source, uint32_t start, uint32_t end) {
    VtermStoredLine& line = rewrap->line;
    PrVtermCell* placed = line.cells.data + rewrap->column;
    uint32_t count = end - start;
    PR_ASSERT(rewrap->column + count <= rewrap->columns);
    prMemCopy(placed, source.cells.data + start, sizeof(PrVtermCell) * count);
    // A line without clusters needs no pass over its cells.
    for (uint32_t i = 0; i < count && source.codePoints.count != 0; ++i) {
        PrVtermCell& cell = placed[i];
        if (cell.clusterCount == 0)
            continue;
        uint32_t clusterStart = line.codePoints.count;
        line.codePoints.append(source.codePoints.data + cell.clusterStart, cell.clusterCount);
        cell.clusterStart = clusterStart;
    }
    rewrap->column += count;
}

// Places the tracked position `offset` cells past the end of the line in progress, clamped to the line's last column.
void rewrapTrack(Rewrap* rewrap, uint32_t offset) {
    rewrap->tracked = true;
    rewrap->trackedLine = rewrap->count - 1;
    uint32_t column = rewrap->column + offset;
    if (column >= rewrap->columns) {
        // A position at the very end of a full line is the start of the next line, which a later write creates.
        column = rewrap->columns - 1;
    }
    rewrap->trackedColumn = column;
}

// Gives the line in progress the marks of `source`, except its input mark. If `source` marks a prompt, the line also
// takes the status and exit code of its command. `sourceIndex` is the index of `source` among the lines that the rewrap
// reads, so the rewrap can find where the current command's prompt goes.
void rewrapMarks(Rewrap* rewrap, const VtermStoredLine& source, uint32_t sourceIndex) {
    VtermStoredLine& line = rewrap->line;
    line.marks |= PrVtermLineMarkFlags(source.marks & ~PrVtermLineMarkFlag_Input);
    if ((source.marks & PrVtermLineMarkFlag_Prompt) != 0) {
        line.commandStatus = source.commandStatus;
        line.exitCode = source.exitCode;
    }
    if (sourceIndex == rewrap->promptSource)
        rewrap->promptLine = rewrap->count - 1;
}

// Marks a command's input as starting at `column` of the line in progress, clamped to the line's last column.
void rewrapInput(Rewrap* rewrap, uint32_t column) {
    VtermStoredLine& line = rewrap->line;
    line.marks |= PrVtermLineMarkFlag_Input;
    line.inputColumn = column < rewrap->columns ? column : rewrap->columns - 1;
}

// Packs the line in progress of a rewrap of packed lines, as wrapped if `wrapped`. The cells of zeros at the line's end
// are dropped.
void rewrapPackedFinish(Vterm* vterm, Rewrap* rewrap, bool wrapped) {
    RawArray<VtermPackedRun>& runs = rewrap->runs;
    uint32_t cells = rewrap->text.count;
    while (cells > 0 && rewrap->text.data[cells - 1] == 0 && runs.last().index == 0) {
        --cells;
        if (--runs.last().count == 0)
            runs.pop();
    }
    bool dropped = false;
    VtermPackedLine& packed = scrollbackAppend(rewrap->out, &dropped);
    PR_ASSERT(!dropped);
    packed = VtermPackedLine{
        .revision = vtermRevise(vterm),
        .flags = wrapped ? PrVtermLineFlags(PrVtermLineFlag_Wrapped) : PrVtermLineFlags(0),
    };
    PackedParts parts{.cellCount = cells, .text = rewrap->text.data, .textBytes = cells};
    packWrite(rewrap->out, parts, &runs, RawArray<VtermPackedRun>{}, &packed.position);
    ++rewrap->count;
    rewrap->text.clear();
    runs.clear();
}

// Appends `count` ASCII cells from `text`, in `pen`, to the logical line that a rewrap of packed lines builds, and
// breaks the logical line into lines of the rewrap's width. A null `text` gives cells of zeros. `zero` is true if `pen`
// is the pen of all zeros.
void rewrapPackedText(Vterm* vterm, Rewrap* rewrap, const uint8_t* PR_NULLABLE text, uint32_t count,
        const VtermPackedPen& pen, bool zero) {
    while (count > 0) {
        if (rewrap->text.count == rewrap->columns)
            rewrapPackedFinish(vterm, rewrap, true);
        uint32_t room = rewrap->columns - rewrap->text.count;
        uint32_t taken = count < room ? count : room;
        uint32_t at = rewrap->text.count;
        rewrap->text.resize(at + taken);
        if (text != nullptr) {
            prMemCopy(rewrap->text.data + at, text, taken);
            text += taken;
        } else {
            prMemZero(rewrap->text.data + at, taken);
        }
        RawArray<VtermPackedRun>& runs = rewrap->runs;
        // Until the run is packed, its `index` is zero for the pen of all zeros and one for any other pen.
        uint32_t index = zero ? 0 : 1;
        if (runs.count != 0 && runs.last().index == index && (zero || isSamePen(runs.last().pen, pen)))
            runs.last().count += taken;
        else
            runs.push(VtermPackedRun{.start = at, .count = taken, .index = index, .pen = pen});
        count -= taken;
    }
}

// Rewraps the logical line made of the lines of `source` from `first` to `last`, included, from their packed text, and
// returns true. This works only if each line is a packed line of ASCII with no links, marks or shapes. Returns false
// for any other line, which the rewrap then reads as cells. A line's width matters only where its text breaks, so the
// text and the runs of pens are copied in slices of the new width.
bool rewrapPacked(Vterm* vterm, ResizeSource* source, uint32_t first, uint32_t last, Rewrap* rewrap) {
    const VtermScrollback* scrollback = source->scrollback;
    if (last >= scrollback->count)
        return false;
    for (uint32_t i = first; i <= last; ++i) {
        const VtermPackedLine& line = vtermScrollbackAt(scrollback, i);
        if (line.packed != 0)
            return false;
        VtermPackedView view = vtermPackedView(scrollback, line);
        if (view.textBytes != view.cellCount)
            return false;
    }
    rewrapFinish(rewrap);
    static constexpr VtermPackedPen kZero{};
    for (uint32_t i = first; i <= last; ++i) {
        uint32_t starts = rewrap->count + (rewrap->text.count >= rewrap->columns ? 1 : 0);
        if (rewrap->lineMap != nullptr)
            rewrap->lineMap->push(starts);
        VtermPackedView view = vtermPackedView(scrollback, vtermScrollbackAt(scrollback, i));
        RawArray<VtermPackedRun>& runs = rewrap->sourceRuns;
        runs.clear();
        const uint8_t* at = view.runs;
        uint32_t column = 0;
        while (at < view.runs + view.runBytes) {
            uint32_t count = vtermPackedReadCount(&at);
            uint32_t pen = vtermPackedReadCount(&at);
            runs.push(VtermPackedRun{.start = column, .count = count, .index = pen});
            column += count;
        }
        if (runs.count == 0)
            runs.push(VtermPackedRun{.count = view.cellCount});
        uint32_t end = view.cellCount;
        if (i == last) {
            // The last line keeps its cells up to the last one that shows something, as contentEnd decides for cells.
            while (end > 0) {
                VtermPackedRun* run = &runs.last();
                while (run->start >= end)
                    run = &runs.data[--runs.count - 1];
                const VtermPackedPen& pen = run->index != 0 ? view.pens[run->index - 1] : kZero;
                bool blank = pen.flags == 0 && pen.underline == PrVtermUnderline_None &&
                    pen.background.kind == PrVtermColorKind_Default;
                if (!blank || view.text[end - 1] != 0)
                    break;
                --end;
            }
        }
        for (const VtermPackedRun& run : runs.span()) {
            if (run.start >= end)
                break;
            uint32_t count = run.start + run.count < end ? run.count : end - run.start;
            const VtermPackedPen& pen = run.index != 0 ? view.pens[run.index - 1] : kZero;
            rewrapPackedText(vterm, rewrap, view.text + run.start, count, pen, run.index == 0);
        }
        if (i != last)
            rewrapPackedText(vterm, rewrap, nullptr, source->columns - view.cellCount, kZero, true);
    }
    rewrapPackedFinish(vterm, rewrap, false);
    rewrap->column = 0;
    return true;
}

// Rewraps the lines of `source` to `rewrap->columns`. Tracks the position at `trackLine` and `trackColumn`, and the
// built line where line `topLine` begins. A line's marks go to the line that its first cell goes to, and the input mark
// goes with the cell where the input starts.
void rewrapLines(Vterm* vterm, ResizeSource* source, uint32_t trackLine, uint32_t trackColumn, uint32_t topLine, Rewrap* rewrap) {
    uint32_t at = 0;
    while (at < source->count) {
        // A logical line: physical lines that each wrap into the next, and the line that ends them.
        uint32_t end = at;
        while (end + 1 < source->count && (sourceFlags(source, end) & PrVtermLineFlag_Wrapped) != 0)
            ++end;
        if (rewrapPacked(vterm, source, at, end, rewrap)) {
            at = end + 1;
            continue;
        }
        rewrapBegin(vterm, rewrap);
        for (uint32_t i = at; i <= end; ++i) {
            const VtermStoredLine& line = sourceLine(source, i);
            bool last = i == end;
            uint32_t starts = rewrap->count - 1 + (rewrap->column >= rewrap->columns ? 1 : 0);
            if (i == topLine)
                rewrap->topLine = starts;
            if (rewrap->lineMap != nullptr)
                rewrap->lineMap->push(starts);
            uint32_t count = last ? contentEnd(line) : line.cells.count;
            if (i == trackLine && trackColumn > count)
                count = trackColumn < line.cells.count ? trackColumn : line.cells.count;
            bool input = (line.marks & PrVtermLineMarkFlag_Input) != 0;
            if (input && line.inputColumn > count)
                count = line.inputColumn < line.cells.count ? line.inputColumn : line.cells.count;
            bool marked = line.marks == 0;
            for (uint32_t column = 0; column < count; ++column) {
                const PrVtermCell& cell = line.cells.data[column];
                if (i == trackLine && column == trackColumn)
                    rewrapTrack(rewrap, 0);
                if ((cell.flags & (PrVtermCellFlag_WideTail | PrVtermCellFlag_WideSpacer)) != 0)
                    continue;
                if (marked && !(input && column == line.inputColumn) && (cell.flags & PrVtermCellFlag_Wide) == 0) {
                    // Copy a run of cells outside wide clusters at once. The run ends at the end of the line in
                    // progress, the tracked position or the input's start, whichever comes first.
                    if (rewrap->column >= rewrap->columns)
                        rewrapBreak(vterm, rewrap);
                    uint32_t room = rewrap->columns - rewrap->column;
                    uint32_t limit = count - column < room ? count : column + room;
                    if (i == trackLine && trackColumn > column && trackColumn < limit)
                        limit = trackColumn;
                    if (input && line.inputColumn > column && line.inputColumn < limit)
                        limit = line.inputColumn;
                    uint32_t runEnd = column + 1;
                    while (runEnd < limit && (line.cells.data[runEnd].flags & PrVtermCellFlag_WidePlacement) == 0)
                        ++runEnd;
                    rewrapRun(rewrap, line, column, runEnd);
                    column = runEnd - 1;
                    continue;
                }
                uint32_t before = rewrap->count;
                uint32_t placed = rewrap->column;
                rewrapCell(vterm, rewrap, line, column);
                if (rewrap->count != before)
                    placed = 0;
                if (!marked)
                    rewrapMarks(rewrap, line, i);
                marked = true;
                if (input && column == line.inputColumn) {
                    rewrapInput(rewrap, placed);
                    input = false;
                }
            }
            if (!marked)
                rewrapMarks(rewrap, line, i);
            if (input) {
                // The input starts past the cells that the line keeps, or in the tail of a wide cluster.
                uint32_t past = line.inputColumn > count ? line.inputColumn - count : 0;
                rewrapInput(rewrap, rewrap->column + past);
            }
            if (i == trackLine && trackColumn >= count)
                rewrapTrack(rewrap, trackColumn - count);
        }
        at = end + 1;
    }
    rewrapFinish(rewrap);
}

// Moves the rows out of the ring to `out`, from the top, and empties the ring.
void rowsTake(VtermRows* rows, RawArray<VtermStoredLine>* out) {
    for (uint32_t i = 0; i < rows->slots.count; ++i)
        out->push(vtermRowsAt(rows, i));
    rows->slots.reset();
    rows->start = 0;
}

// Makes `lines` the ring's rows, from the top, and empties `lines`.
void rowsAssign(VtermRows* rows, RawArray<VtermStoredLine>* lines) {
    rows->slots.reset();
    rows->slots = *lines;
    rows->slots.reserveExact(rows->slots.count);
    *lines = RawArray<VtermStoredLine>{};
    rows->start = 0;
}

// Shrinks the scrollback's ring to hold only its lines. The ring grows again as lines are added.
void scrollbackFit(VtermScrollback* scrollback) {
    if (scrollback->slots.count == scrollback->count)
        return;
    RawArray<VtermPackedLine> ordered;
    ordered.reserveExact(scrollback->count);
    for (uint32_t i = 0; i < scrollback->count; ++i)
        ordered.push(vtermScrollbackAt(scrollback, i));
    scrollback->slots.reset();
    scrollback->slots = ordered;
    scrollback->start = 0;
}

// Takes a row's storage from `spare`, or a new row if it has none.
VtermStoredLine rowFromSpare(RawArray<VtermStoredLine>* spare) {
    if (spare->count == 0)
        return VtermStoredLine{};
    VtermStoredLine line = spare->last();
    spare->pop();
    return line;
}

// Resizes the primary screen, and rewraps it if `columns` changes. `column` and `row` hold the screen's cursor, which
// the resize moves.
void primaryResize(Vterm* vterm, uint32_t columns, uint32_t rows, uint32_t* column, uint32_t* row) {
    VtermScrollback* scrollback = &vterm->scrollback;
    uint32_t oldRows = vterm->rows;
    RawArray<VtermStoredLine> taken;
    rowsTake(&vterm->screens[VtermScreenKind_Primary].rows, &taken);
    uint32_t screenStart = scrollback->count;
    uint32_t oldCount = screenStart + oldRows;

    // Drop the rows below both the cursor and the last row that shows something, so that a smaller height does not push
    // them into the scrollback. Their storage becomes spare, for the rows that the resize makes.
    RawArray<VtermStoredLine> spare;
    uint32_t used = oldRows;
    while (used > *row + 1 && isBlankLine(taken.data[used - 1]))
        --used;
    for (uint32_t i = used; i < taken.count; ++i)
        spare.push(taken.data[i]);
    taken.count = used;

    uint32_t cursorLine = screenStart + *row;
    uint32_t cursorColumn = *column;
    uint32_t top = screenStart;
    // The index among the lines of the current command's prompt, which the resize moves.
    uint32_t prompt = kRewrapLineNone;
    if (vterm->hasPrompt && vterm->promptLine - vterm->scrollbackDropped < screenStart + used)
        prompt = uint32_t(vterm->promptLine - vterm->scrollbackDropped);
    if (columns != vterm->columns) {
        // The rewrapped lines are new lines. The old lines leave the scrollback, and no new line reuses their numbers.
        // Image placements follow the first cells of their lines to the new lines. The rewrap packs every line that it
        // builds, and the lines that become rows are unpacked again below.
        uint64_t firstLine = vterm->scrollbackDropped;
        vterm->scrollbackDropped += oldCount;
        RawArray<uint32_t> lineMap;
        bool placed = vterm->graphics.stores[VtermScreenKind_Primary].placements.count != 0;
        VtermScrollback built{.limit = UINT32_MAX};
        Rewrap rewrap{
            .out = &built,
            .columns = columns,
            .promptSource = prompt,
            .promptLine = kRewrapLineNone,
            .lineMap = placed ? &lineMap : nullptr,
        };
        rewrap.line.cells.resizeZeroed(columns);
        ResizeSource source{
            .scrollback = scrollback,
            .rows = &taken,
            .count = screenStart + used,
            .columns = vterm->columns,
        };
        rewrapLines(vterm, &source, cursorLine, cursorColumn, screenStart, &rewrap);
        vtermGraphicsRewrap(vterm, firstLine, lineMap, vterm->scrollbackDropped);
        lineMap.reset();
        lineReset(&rewrap.line);
        rewrap.text.reset();
        rewrap.runs.reset();
        rewrap.sourceRuns.reset();
        lineReset(&source.unpacked);
        prompt = rewrap.promptLine;
        PR_ASSERT(rewrap.tracked);
        cursorLine = rewrap.trackedLine;
        cursorColumn = rewrap.trackedColumn;
        top = rewrap.topLine;
        built.limit = scrollback->limit;
        vtermScrollbackReset(scrollback);
        *scrollback = built;
        spare.append(taken.data, taken.count);
        taken.count = 0;
    } else if (cursorColumn >= columns) {
        cursorColumn = columns - 1;
    }

    // The lines are the scrollback's, then the rows of `taken`. The screen's top row stays on the line that it showed,
    // and moves only as far as needed to keep the cursor on the screen. A cursor on the bottom row stays there instead,
    // so that a greater height brings back lines from the scrollback.
    uint32_t packed = scrollback->count;
    uint32_t count = packed + taken.count;
    if (*row == oldRows - 1)
        top = cursorLine > rows - 1 ? cursorLine - (rows - 1) : 0;
    if (top > cursorLine)
        top = cursorLine;
    if (cursorLine >= top + rows)
        top = cursorLine - (rows - 1);
    uint32_t end = count < top + rows ? count : top + rows;
    if (prompt >= end)
        prompt = kRewrapLineNone;

    // The lines from `top` to `end` become the rows. The scrollback lines among them are unpacked and leave the
    // scrollback. The rows before `top` enter the scrollback, which drops its oldest lines past its limit.
    uint64_t firstLine = vterm->scrollbackDropped;
    RawArray<VtermStoredLine> fresh;
    fresh.reserveExact(rows);
    for (uint32_t i = top; i < end && i < packed; ++i) {
        VtermStoredLine line = rowFromSpare(&spare);
        vtermScrollbackUnpack(scrollback, i, columns, &line);
        fresh.push(line);
    }
    while (scrollback->count > top)
        vtermScrollbackPop(scrollback);
    for (uint32_t i = packed; i < count; ++i) {
        VtermStoredLine& line = taken.data[i - packed];
        if (i < top) {
            bool dropped = true;
            if (scrollback->limit != 0)
                vtermScrollbackPush(scrollback, line, &dropped);
            vterm->scrollbackDropped += dropped ? 1 : 0;
            spare.push(line);
        } else if (i < end) {
            fresh.push(line);
        } else {
            spare.push(line);
        }
    }
    if (scrollback->count > scrollback->limit) {
        uint32_t dropped = scrollback->count - scrollback->limit;
        vtermScrollbackDrop(scrollback, dropped);
        vterm->scrollbackDropped += dropped;
    }
    while (fresh.count < rows) {
        VtermStoredLine line = rowFromSpare(&spare);
        line.cells.resize(columns);
        prMemZero(line.cells.data, sizeof(PrVtermCell) * columns);
        line.written = 0;
        line.codePoints.clear();
        line.flags = 0;
        vtermLineClearMarks(&line);
        line.revision = vtermRevise(vterm);
        fresh.push(line);
    }
    for (VtermStoredLine& line : spare.span())
        lineReset(&line);
    spare.reset();
    taken.reset();
    scrollbackFit(scrollback);
    rowsAssign(&vterm->screens[VtermScreenKind_Primary].rows, &fresh);

    // The lines that left the front of the scrollback changed the prompt's index, but not its line number.
    uint64_t promptLine = firstLine + prompt;
    vterm->hasPrompt = prompt != kRewrapLineNone && promptLine >= vterm->scrollbackDropped;
    vterm->promptLine = promptLine;
    *row = cursorLine - top;
    *column = cursorColumn;
}

// Resizes the alternate screen, cutting or filling its rows and their cells. `row` holds the cursor's row, and the
// resize keeps the cursor on the screen.
void alternateResize(Vterm* vterm, uint32_t columns, uint32_t rows, uint32_t* row) {
    RawArray<VtermStoredLine> lines;
    rowsTake(&vterm->screens[VtermScreenKind_Alternate].rows, &lines);
    if (lines.count > rows) {
        uint32_t fromTop = *row + 1 > rows ? *row + 1 - rows : 0;
        for (uint32_t i = 0; i < fromTop; ++i)
            lineReset(&lines.data[i]);
        lines.erase(0, fromTop);
        *row -= fromTop;
        vtermGraphicsShiftAlternate(vterm, fromTop);
        for (uint32_t i = rows; i < lines.count; ++i)
            lineReset(&lines.data[i]);
        lines.count = rows;
    }
    while (lines.count < rows) {
        VtermStoredLine& line = lines.emplace();
        line = VtermStoredLine{};
    }
    for (uint32_t i = 0; i < lines.count; ++i) {
        VtermStoredLine& line = lines.data[i];
        uint32_t old = line.cells.count;
        if (old > columns && (line.cells.data[columns - 1].flags & PrVtermCellFlag_Wide) != 0)
            cellEmpty(&line.cells.data[columns - 1]);
        line.cells.resize(columns);
        if (columns > old)
            cellsFill(line.cells.data + old, PrVtermCell{}, columns - old);
        if (line.written > columns)
            line.written = columns;
        line.flags = 0;
        line.revision = vtermRevise(vterm);
    }
    rowsAssign(&vterm->screens[VtermScreenKind_Alternate].rows, &lines);
}

// Clamps a saved cursor to the screen.
void clampCursor(VtermCursorState* cursor, uint32_t columns, uint32_t rows) {
    if (cursor->column >= columns)
        cursor->column = columns - 1;
    if (cursor->row >= rows)
        cursor->row = rows - 1;
    cursor->wrapPending = false;
}

void tabStopsReset(Vterm* vterm, uint32_t from) {
    vterm->tabStops.resize(vterm->columns);
    for (uint32_t i = from; i < vterm->columns; ++i)
        vterm->tabStops.data[i] = i % kTabInterval == 0 && i != 0 ? 1 : 0;
}

// Records that blanks of the pen's background were written to the cells of `line` before column `end`. Blanks of the
// default background are all zeros and need no record.
void lineWroteBlanks(const Vterm* vterm, VtermStoredLine* line, uint32_t end) {
    if (!isZeroCell(vtermBlankCell(vterm)))
        vtermLineWrote(line, end);
}

// Sets the cells of `line` from column `start` to `end`, excluded, to blank cells. No wide cluster crosses either end.
void cellsBlank(const Vterm* vterm, VtermStoredLine* line, uint32_t start, uint32_t end) {
    cellsFill(line->cells.data + start, vtermBlankCell(vterm), end - start);
    lineWroteBlanks(vterm, line, end);
}

// Inserts `count` blank cells at `column` of `line`, moving the cells after it right. Cells pushed past `end` are
// dropped.
void lineInsertCells(Vterm* vterm, VtermStoredLine* line, uint32_t column, uint32_t end, uint32_t count) {
    if (count > end - column)
        count = end - column;
    splitEdge(line, column);
    splitEdge(line, end);
    splitEdge(line, end - count);
    prMemMove(line->cells.data + column + count, line->cells.data + column, sizeof(PrVtermCell) * (end - column - count));
    vtermLineWrote(line, line->written + count < end ? line->written + count : end);
    cellsBlank(vterm, line, column, column + count);
    vtermTouch(vterm, line);
}

// Deletes `count` cells at `column` of `line`, moving the cells after them left, up to `end`. Blanks the cells that the
// move leaves empty.
void lineDeleteCells(Vterm* vterm, VtermStoredLine* line, uint32_t column, uint32_t end, uint32_t count) {
    if (count > end - column)
        count = end - column;
    splitEdge(line, column);
    splitEdge(line, column + count);
    splitEdge(line, end);
    prMemMove(line->cells.data + column, line->cells.data + column + count, sizeof(PrVtermCell) * (end - column - count));
    cellsBlank(vterm, line, end - count, end);
    vtermTouch(vterm, line);
}

// Returns true if the scroll region's columns span the screen, so that a scroll moves whole lines.
bool marginsSpanWidth(const Vterm* vterm) {
    return vterm->left == 0 && vterm->right == vterm->columns;
}

// Returns true if the cursor is in the scroll region's rows and columns.
bool cursorInRegion(const Vterm* vterm) {
    const VtermCursorState& cursor = vterm->cursor;
    return cursor.row >= vterm->top && cursor.row < vterm->bottom && vtermColumnInMargins(vterm, cursor.column);
}

// The column, excluded, at which text written at `column` wraps: the right margin, or the screen's edge for text right
// of the margin.
uint32_t textLimit(const Vterm* vterm, uint32_t column) {
    return column < vterm->right ? vterm->right : vterm->columns;
}

// Moves the cursor from the end of its line's text to the left margin of the next line. Only a wrap across the whole
// width marks the line wrapped.
void wrapLine(Vterm* vterm) {
    if (marginsSpanWidth(vterm))
        vtermRow(vterm, vterm->cursor.row).flags |= PrVtermLineFlag_Wrapped;
    vtermIndex(vterm);
    vterm->cursor.column = vterm->left;
}

// What becomes of the rows that scroll off the top of the scroll region.
enum {
    ScrolledOff_Dropped,
    ScrolledOff_Kept,           // sent to the scrollback if the region starts at the primary screen's top
};
typedef uint8_t ScrolledOff;

// Prepares the rows from `top` to `bottom`, excluded, for a move of the cells between the margins, and marks them
// changed. Splits the wide clusters that cross the margins. A row whose cells move no longer continues one text, so it
// loses its wrapped flag.
void marginRowsPrepare(Vterm* vterm, uint32_t top, uint32_t bottom) {
    for (uint32_t row = top; row < bottom; ++row) {
        VtermStoredLine& line = vtermRow(vterm, row);
        splitEdge(&line, vterm->left);
        splitEdge(&line, vterm->right);
        line.flags &= PrVtermLineFlags(~PrVtermLineFlag_Wrapped);
        vtermTouch(vterm, &line);
    }
}

// Moves the cells between the margins, in the rows from `top` to the region's bottom, up by `count` rows, and blanks
// the rows that they leave. If the region spans the width, whole lines move and keep their revisions.
void regionUp(Vterm* vterm, uint32_t top, uint32_t count, ScrolledOff scrolledOff) {
    uint32_t bottom = vterm->bottom;
    if (count > bottom - top)
        count = bottom - top;
    vtermRevise(vterm);
    vterm->hasLastPrinted = false;
    bool kept = scrolledOff == ScrolledOff_Kept && vterm->active == VtermScreenKind_Primary && top == 0 &&
        vterm->scrollback.limit > 0;
    vtermGraphicsScroll(vterm, top, bottom, -int32_t(count), kept && marginsSpanWidth(vterm));
    if (!marginsSpanWidth(vterm)) {
        marginRowsPrepare(vterm, top, bottom);
        for (uint32_t row = top; row + count < bottom; ++row)
            cellsCopy(&vtermRow(vterm, row), vtermRow(vterm, row + count), vterm->left, vterm->right);
        for (uint32_t row = bottom - count; row < bottom; ++row)
            cellsBlank(vterm, &vtermRow(vterm, row), vterm->left, vterm->right);
        return;
    }
    if (kept) {
        // Pack the top row into the scrollback, and reuse its storage as the row at the bottom margin. For a scroll of
        // the whole screen, rotating the ring of rows does this.
        VtermRows* rows = &vterm->screens[VtermScreenKind_Primary].rows;
        for (uint32_t i = 0; i < count; ++i) {
            bool dropped = false;
            vtermScrollbackPush(&vterm->scrollback, vtermRow(vterm, 0), &dropped);
            if (dropped)
                ++vterm->scrollbackDropped;
            if (bottom == vterm->rows)
                rows->start = rowSlotOf(rows, 1);
            else
                rotateRows(vterm, 0, bottom, 1);
            vtermLineClear(vterm, &vtermRow(vterm, bottom - 1), vterm->columns);
        }
        return;
    }
    rotateRows(vterm, top, bottom, count);
    for (uint32_t row = bottom - count; row < bottom; ++row)
        vtermLineClear(vterm, &vtermRow(vterm, row), vterm->columns);
}

// Moves the cells between the margins, in the rows from `top` to the region's bottom, down by `count` rows, and blanks
// the rows that they leave.
void regionDown(Vterm* vterm, uint32_t top, uint32_t count) {
    uint32_t bottom = vterm->bottom;
    if (count > bottom - top)
        count = bottom - top;
    vtermRevise(vterm);
    vterm->hasLastPrinted = false;
    vtermGraphicsScroll(vterm, top, bottom, int32_t(count), false);
    if (!marginsSpanWidth(vterm)) {
        marginRowsPrepare(vterm, top, bottom);
        for (uint32_t row = bottom; row-- > top + count;)
            cellsCopy(&vtermRow(vterm, row), vtermRow(vterm, row - count), vterm->left, vterm->right);
        for (uint32_t row = top; row < top + count; ++row)
            cellsBlank(vterm, &vtermRow(vterm, row), vterm->left, vterm->right);
        return;
    }
    rotateRows(vterm, top, bottom, bottom - top - count);
    for (uint32_t row = top; row < top + count; ++row)
        vtermLineClear(vterm, &vtermRow(vterm, row), vterm->columns);
}

} // namespace

void vtermRowsReset(VtermRows* rows) {
    for (uint32_t i = 0; i < rows->slots.count; ++i)
        lineReset(&rows->slots.data[i]);
    rows->slots.reset();
    rows->start = 0;
}

VtermStoredLine& vtermRowsAt(VtermRows* rows, uint32_t row) {
    PR_ASSERT(row < rows->slots.count);
    return rows->slots.data[rowSlotOf(rows, row)];
}

const VtermStoredLine& vtermRowsAt(const VtermRows* rows, uint32_t row) {
    PR_ASSERT(row < rows->slots.count);
    return rows->slots.data[rowSlotOf(rows, row)];
}

void vtermArenaReset(VtermArena* arena) {
    for (uint32_t i = 0; i < arena->count; ++i)
        blockFree(arena, &arenaBlockAt(arena, i), false);
    arena->slots.reset();
    arena->start = 0;
    arena->count = 0;
    arena->first = 0;
    prMemoryFree(arena->spare);
    arena->spare = nullptr;
}

uint8_t* vtermArenaReserve(VtermArena* arena, uint32_t size, VtermArenaPosition* position) {
    if (arena->count != 0) {
        VtermArenaBlock& newest = arenaBlockAt(arena, arena->count - 1);
        if (newest.size - newest.used >= size) {
            *position = VtermArenaPosition{.block = arena->first + arena->count - 1, .offset = uint16_t(newest.used)};
            return newest.data + newest.used;
        }
    }
    if (arena->count == arena->slots.count) {
        // The ring is full, so it grows. The blocks are copied in order from the oldest.
        RawArray<VtermArenaBlock> ordered;
        ordered.reserve(arena->count + 1);
        for (uint32_t i = 0; i < arena->count; ++i)
            ordered.push(arenaBlockAt(arena, i));
        ordered.resize(ordered.capacity);
        arena->slots.reset();
        arena->slots = ordered;
        arena->start = 0;
    }
    VtermArenaBlock& block = arena->slots.data[arenaSlotOf(arena, arena->count++)];
    block = VtermArenaBlock{.size = size > kVtermArenaBlockBytes ? size : kVtermArenaBlockBytes};
    if (block.size == kVtermArenaBlockBytes && arena->spare != nullptr) {
        block.data = arena->spare;
        arena->spare = nullptr;
    } else {
        block.data = static_cast<uint8_t*>(prMemoryAlloc(block.size));
    }
    *position = VtermArenaPosition{.block = arena->first + arena->count - 1};
    return block.data;
}

void vtermArenaCommit(VtermArena* arena, uint32_t size) {
    VtermArenaBlock& newest = arenaBlockAt(arena, arena->count - 1);
    PR_ASSERT(size <= newest.size - newest.used);
    newest.used += size;
}

uint8_t* vtermArenaAt(const VtermArena* arena, VtermArenaPosition position) {
    const VtermArenaBlock& block = vtermArenaBlock(arena, position.block);
    PR_ASSERT(position.offset < block.used);
    return block.data + position.offset;
}

const VtermArenaBlock& vtermArenaBlock(const VtermArena* arena, uint32_t block) {
    uint32_t index = block - arena->first;
    PR_ASSERT(index < arena->count);
    return arena->slots.data[arenaSlotOf(arena, index)];
}

uint32_t vtermArenaPen(VtermArena* arena, const VtermPackedPen& pen) {
    constexpr VtermPackedPen kZero{};
    if (prMemEquals(&pen, &kZero, sizeof(VtermPackedPen)))
        return 0;
    VtermArenaBlock& newest = arenaBlockAt(arena, arena->count - 1);
    uint32_t last = arena->lastPen;
    if (last != 0 && last <= newest.pens.count && prMemEquals(&newest.pens.data[last - 1], &pen, sizeof(pen)))
        return last;
    uint32_t* ways = arena->recentPens[penHash(pen) % kVtermArenaPenSets];
    for (uint32_t way = 0; way < kVtermArenaPenWays; ++way) {
        uint32_t index = ways[way];
        if (index != 0 && index <= newest.pens.count && prMemEquals(&newest.pens.data[index - 1], &pen, sizeof(pen))) {
            arena->lastPen = index;
            return index;
        }
    }
    newest.pens.push(pen);
    for (uint32_t way = kVtermArenaPenWays - 1; way > 0; --way)
        ways[way] = ways[way - 1];
    ways[0] = newest.pens.count;
    arena->lastPen = newest.pens.count;
    return newest.pens.count;
}

void vtermArenaDropBefore(VtermArena* arena, uint32_t block) {
    while (arena->count != 0 && int32_t(block - arena->first) > 0) {
        blockFree(arena, &arena->slots.data[arena->start], true);
        arena->start = arenaSlotOf(arena, 1);
        --arena->count;
        ++arena->first;
    }
}

void vtermArenaTruncate(VtermArena* arena, VtermArenaPosition position) {
    uint32_t index = position.block - arena->first;
    PR_ASSERT(index < arena->count);
    while (arena->count > index + 1)
        blockFree(arena, &arenaBlockAt(arena, --arena->count), true);
    arenaBlockAt(arena, index).used = position.offset;
}

size_t vtermArenaMemorySize(const VtermArena* arena) {
    size_t size = sizeof(VtermArenaBlock) * arena->slots.capacity;
    for (uint32_t i = 0; i < arena->count; ++i) {
        const VtermArenaBlock& block = arena->slots.data[arenaSlotOf(arena, i)];
        size += block.size + sizeof(VtermPackedPen) * block.pens.capacity;
    }
    if (arena->spare != nullptr)
        size += kVtermArenaBlockBytes;
    return size;
}

void vtermScrollbackReset(VtermScrollback* scrollback) {
    scrollback->slots.reset();
    scrollback->start = 0;
    scrollback->count = 0;
    vtermArenaReset(&scrollback->arena);
    scrollback->text.reset();
    scrollback->shapes.reset();
    scrollback->runs.reset();
    scrollback->linkRuns.reset();
}

VtermPackedLine& vtermScrollbackAt(VtermScrollback* scrollback, uint32_t index) {
    PR_ASSERT(index < scrollback->count);
    return scrollback->slots.data[scrollbackSlotOf(scrollback, index)];
}

const VtermPackedLine& vtermScrollbackAt(const VtermScrollback* scrollback, uint32_t index) {
    PR_ASSERT(index < scrollback->count);
    return scrollback->slots.data[scrollbackSlotOf(scrollback, index)];
}

void vtermScrollbackPush(VtermScrollback* scrollback, const VtermStoredLine& line, bool* dropped) {
    VtermPackedLine& packed = scrollbackAppend(scrollback, dropped);
    uint32_t end = packedEnd(line);
    packed = VtermPackedLine{.revision = line.revision, .flags = line.flags};
    packed.packed = packCells(scrollback, line, end, &packed.position);
}

void vtermScrollbackPop(VtermScrollback* scrollback) {
    PR_ASSERT(scrollback->count > 0);
    VtermArenaPosition position = vtermScrollbackAt(scrollback, scrollback->count - 1).position;
    --scrollback->count;
    vtermArenaTruncate(&scrollback->arena, position);
}

void vtermScrollbackDrop(VtermScrollback* scrollback, uint32_t count) {
    PR_ASSERT(count <= scrollback->count);
    if (count == 0)
        return;
    scrollback->start = scrollbackSlotOf(scrollback, count);
    scrollback->count -= count;
    VtermArena* arena = &scrollback->arena;
    if (scrollback->count == 0) {
        scrollback->start = 0;
        vtermArenaDropBefore(arena, arena->first + arena->count);
        return;
    }
    vtermArenaDropBefore(arena, vtermScrollbackAt(scrollback, 0).position.block);
}

size_t vtermScrollbackMemorySize(const VtermScrollback* scrollback) {
    return sizeof(VtermPackedLine) * scrollback->slots.capacity + vtermArenaMemorySize(&scrollback->arena) +
        scrollback->text.capacity + scrollback->shapes.capacity +
        sizeof(VtermPackedRun) * (scrollback->runs.capacity + scrollback->linkRuns.capacity);
}

VtermPackedView vtermPackedView(const VtermScrollback* scrollback, const VtermPackedLine& line) {
    uint8_t* at = vtermArenaAt(&scrollback->arena, line.position);
    VtermPackedView view{};
    if ((line.packed & VtermPackedFlag_Marks) != 0) {
        view.marks = at;
        at += kPackedMarksBytes;
    }
    const uint8_t* read = at;
    view.cellCount = vtermPackedReadCount(&read);
    view.textBytes = vtermPackedReadCount(&read);
    view.runBytes = vtermPackedReadCount(&read);
    if ((line.packed & VtermPackedFlag_Links) != 0)
        view.linkBytes = vtermPackedReadCount(&read);
    view.text = read;
    read += view.textBytes;
    if ((line.packed & VtermPackedFlag_Shaped) != 0) {
        view.shapes = read;
        read += view.cellCount;
    }
    view.runs = read;
    view.links = read + view.runBytes;
    view.pens = vtermArenaBlock(&scrollback->arena, line.position.block).pens.data;
    return view;
}

uint32_t vtermPackedReadCount(const uint8_t* PR_NONNULL* PR_NONNULL at) {
    const uint8_t* bytes = *at;
    uint32_t value = 0;
    uint32_t shift = 0;
    while ((*bytes & 0x80) != 0) {
        value |= uint32_t(*bytes++ & 0x7f) << shift;
        shift += 7;
    }
    value |= uint32_t(*bytes++) << shift;
    *at = bytes;
    return value;
}

void vtermPackedCells(const VtermPackedView& view, uint32_t count, RawArray<PrVtermCell>* cells,
        RawArray<uint32_t>* codePoints) {
    PR_ASSERT(count >= view.cellCount);
    cells->resize(count);
    codePoints->clear();
    PrVtermCell* out = cells->data;
    uint32_t cellCount = view.cellCount;
    if (view.shapes == nullptr && view.textBytes == cellCount && view.linkBytes == 0) {
        // A line of ASCII without links unpacks run by run. Each cell is written once, as the words of its run's cell
        // with its own byte.
        const uint8_t* runs = view.runs;
        const uint8_t* runsEnd = view.runs + view.runBytes;
        uint32_t column = 0;
        while (column < cellCount) {
            uint32_t runCount = cellCount - column;
            uint32_t pen = 0;
            if (runs < runsEnd) {
                runCount = vtermPackedReadCount(&runs);
                pen = vtermPackedReadCount(&runs);
            }
            PrVtermCell style{};
            if (pen != 0) {
                const VtermPackedPen& at = view.pens[pen - 1];
                style.underline = at.underline;
                style.flags = at.flags;
                style.foreground = at.foreground;
                style.background = at.background;
                style.underlineColor = at.underlineColor;
            }
            CellWords words = cellWordsOf(style);
            for (uint32_t i = column; i < column + runCount; ++i) {
                CellWords cell = words;
                cell.words[0] |= view.text[i];
                prMemCopy(out + i, &cell, sizeof(CellWords));
            }
            column += runCount;
        }
        if (count > cellCount)
            prMemZero(out + cellCount, sizeof(PrVtermCell) * (count - cellCount));
        return;
    }
    if (view.shapes == nullptr && view.textBytes == cellCount) {
        for (uint32_t i = 0; i < cellCount; ++i)
            out[i] = PrVtermCell{.codePoint = view.text[i]};
    } else {
        const uint8_t* text = view.text;
        for (uint32_t i = 0; i < cellCount; ++i) {
            uint8_t shape = view.shapes != nullptr ? view.shapes[i] : 0;
            PrVtermCell cell{};
            if ((shape & kShapeWide) != 0)
                cell.flags |= PrVtermCellFlag_Wide;
            if ((shape & kShapeWideTail) != 0)
                cell.flags |= PrVtermCellFlag_WideTail;
            if ((shape & kShapeWideSpacer) != 0)
                cell.flags |= PrVtermCellFlag_WideSpacer;
            if ((shape & (kShapeWideTail | kShapeWideSpacer)) == 0) {
                cell.codePoint = codePointRead(&text) & kCodePointMask;
                uint32_t clusterCount = shape & kShapeClusterMask;
                if (clusterCount != 0) {
                    cell.clusterCount = clusterCount;
                    cell.clusterStart = codePoints->count;
                    for (uint32_t k = 0; k < clusterCount; ++k)
                        codePoints->push(codePointRead(&text));
                }
            }
            out[i] = cell;
        }
    }
    if (count > cellCount)
        prMemZero(out + cellCount, sizeof(PrVtermCell) * (count - cellCount));

    // The runs of pens. Cells outside them keep the pen of all zeros.
    const uint8_t* runs = view.runs;
    const uint8_t* runsEnd = view.runs + view.runBytes;
    uint32_t column = 0;
    while (runs < runsEnd) {
        uint32_t runCount = vtermPackedReadCount(&runs);
        uint32_t pen = vtermPackedReadCount(&runs);
        if (pen != 0) {
            const VtermPackedPen& style = view.pens[pen - 1];
            for (uint32_t i = column; i < column + runCount; ++i) {
                PrVtermCell& cell = out[i];
                cell.underline = style.underline;
                cell.flags |= style.flags;
                cell.foreground = style.foreground;
                cell.background = style.background;
                cell.underlineColor = style.underlineColor;
            }
        }
        column += runCount;
    }
    const uint8_t* links = view.links;
    const uint8_t* linksEnd = view.links + view.linkBytes;
    column = 0;
    while (links < linksEnd) {
        column += vtermPackedReadCount(&links);
        uint32_t linkCount = vtermPackedReadCount(&links);
        PrVtermLinkIndex link = PrVtermLinkIndex(vtermPackedReadCount(&links));
        for (uint32_t i = column; i < column + linkCount; ++i)
            out[i].link = link;
        column += linkCount;
    }
}

void vtermPackedAppendText(const VtermPackedView& view, uint32_t start, uint32_t end, PrRawString* out) {
    if (end > view.cellCount)
        end = view.cellCount;
    if (start >= end)
        return;
    if (view.shapes == nullptr && view.textBytes == view.cellCount) {
        // For a line of ASCII, the text is the packed text, with spaces for the cells that hold nothing. It is appended
        // in chunks.
        char chunk[256];
        for (uint32_t at = start; at < end;) {
            uint32_t size = end - at < sizeof(chunk) ? end - at : uint32_t(sizeof(chunk));
            for (uint32_t i = 0; i < size; ++i)
                chunk[i] = view.text[at + i] != 0 ? char(view.text[at + i]) : ' ';
            prRawStringAppend(out, PrStringView{chunk, size});
            at += size;
        }
        return;
    }
    const uint8_t* text = view.text;
    for (uint32_t i = 0; i < end; ++i) {
        uint8_t shape = view.shapes != nullptr ? view.shapes[i] : 0;
        if ((shape & (kShapeWideTail | kShapeWideSpacer)) != 0)
            continue;
        uint32_t points = 1 + (shape & kShapeClusterMask);
        for (uint32_t k = 0; k < points; ++k) {
            const uint8_t* first = text;
            uint32_t codePoint = codePointRead(&text);
            if (i < start)
                continue;
            if (k == 0 && codePoint == 0) {
                prRawStringAppend(out, " ");
                continue;
            }
            // A code point that UTF-8 cannot hold, such as a surrogate, is written as prUtf8Encode writes it.
            bool scalar = codePoint <= kPrUtf8MaxCodePoint && (codePoint < 0xd800 || codePoint > 0xdfff);
            if (scalar) {
                prRawStringAppend(out, PrStringView{reinterpret_cast<const char*>(first), size_t(text - first)});
            } else {
                char encoded[kPrUtf8MaxBytes];
                prRawStringAppend(out, PrStringView{encoded, prUtf8Encode(codePoint, encoded)});
            }
        }
    }
}

void vtermScrollbackUnpack(const VtermScrollback* scrollback, uint32_t index, uint32_t columns, VtermStoredLine* line) {
    const VtermPackedLine& packed = vtermScrollbackAt(scrollback, index);
    VtermPackedView view = vtermPackedView(scrollback, packed);
    vtermPackedCells(view, columns > view.cellCount ? columns : view.cellCount, &line->cells, &line->codePoints);
    line->revision = packed.revision;
    line->flags = packed.flags;
    line->written = view.cellCount;
    vtermLineClearMarks(line);
    if (view.marks != nullptr) {
        line->marks = view.marks[0];
        line->commandStatus = view.marks[1];
        prMemCopy(&line->inputColumn, view.marks + 2, sizeof(uint32_t));
        prMemCopy(&line->exitCode, view.marks + 6, sizeof(int32_t));
    }
}

void vtermPut(Vterm* vterm, PrStringView text) {
    vterm->output.append(reinterpret_cast<const uint8_t*>(text.data), uint32_t(text.size));
}

void vtermPutByte(Vterm* vterm, uint8_t byte) {
    vterm->output.push(byte);
}

void vtermPutNumber(Vterm* vterm, uint32_t value) {
    char digits[kPrU64TextMaxBytes];
    vtermPut(vterm, PrStringView{digits, prU64Format(value, digits, 10)});
}

uint32_t vtermLineCount(const Vterm* vterm) {
    return vterm->scrollback.count + vterm->rows;
}

PrVtermLine vtermLineRead(const Vterm* vterm, uint32_t index) {
    const VtermScrollback* scrollback = &vterm->scrollback;
    PR_ASSERT(index < scrollback->count + vterm->rows);
    if (index >= scrollback->count) {
        const VtermStoredLine& line = vtermRowsAt(&vterm->screens[vterm->active].rows, index - scrollback->count);
        return PrVtermLine{
            .cells = line.cells.data,
            .cellCount = line.cells.count,
            .codePoints = line.codePoints.data,
            .flags = line.flags,
            .marks = line.marks,
            .inputColumn = line.inputColumn,
            .commandStatus = line.commandStatus,
            .exitCode = line.exitCode,
            .revision = line.revision,
        };
    }
    const VtermPackedLine& packed = vtermScrollbackAt(scrollback, index);
    VtermPackedView view = vtermPackedView(scrollback, packed);
    RawArray<VtermLineCacheEntry>& entries = vterm->lineCache.entries;
    PR_ASSERT(entries.count != 0);
    VtermLineCacheEntry& entry = entries.data[(vterm->scrollbackDropped + index) % entries.count];
    if (entry.revision != packed.revision) {
        vtermPackedCells(view, view.cellCount, &entry.cells, &entry.codePoints);
        entry.revision = packed.revision;
    }
    PrVtermLine line{
        .cells = entry.cells.data,
        .cellCount = entry.cells.count,
        .codePoints = entry.codePoints.data,
        .flags = packed.flags,
        .revision = packed.revision,
    };
    if (view.marks != nullptr) {
        line.marks = view.marks[0];
        line.commandStatus = view.marks[1];
        prMemCopy(&line.inputColumn, view.marks + 2, sizeof(uint32_t));
        prMemCopy(&line.exitCode, view.marks + 6, sizeof(int32_t));
    }
    return line;
}

void vtermLineCacheReset(Vterm* vterm) {
    RawArray<VtermLineCacheEntry>& entries = vterm->lineCache.entries;
    for (VtermLineCacheEntry& entry : entries.span()) {
        entry.cells.reset();
        entry.codePoints.reset();
    }
    entries.reset();
    entries.resize(vterm->rows);
    prMemZero(entries.data, sizeof(VtermLineCacheEntry) * vterm->rows);
}

uint64_t vtermRevise(Vterm* vterm) {
    return ++vterm->revision;
}

VtermScreen& vtermScreen(Vterm* vterm) {
    return vterm->screens[vterm->active];
}

VtermStoredLine& vtermRow(Vterm* vterm, uint32_t row) {
    PR_ASSERT(row < vterm->rows);
    return vtermRowsAt(&vtermScreen(vterm).rows, row);
}

void vtermTouch(Vterm* vterm, VtermStoredLine* line) {
    line->revision = vtermRevise(vterm);
}

void vtermLineWrote(VtermStoredLine* line, uint32_t end) {
    if (end > line->written)
        line->written = end;
}

PrVtermCell vtermBlankCell(const Vterm* vterm) {
    PrVtermCell cell{};
    cell.background = vterm->cursor.pen.background;
    return cell;
}

void vtermLineClear(Vterm* vterm, VtermStoredLine* line, uint32_t columns) {
    PR_ASSERT(isZeroPastWritten(*line));
    PrVtermCell blank = vtermBlankCell(vterm);
    uint32_t old = line->cells.count;
    line->cells.resize(columns);
    if (isZeroCell(blank)) {
        // The cells past the written bound are already zeros.
        uint32_t written = line->written < old ? line->written : old;
        if (written > columns)
            written = columns;
        prMemZero(line->cells.data, sizeof(PrVtermCell) * written);
        if (columns > old)
            prMemZero(line->cells.data + old, sizeof(PrVtermCell) * (columns - old));
        line->written = 0;
    } else {
        cellsFill(line->cells.data, blank, columns);
        line->written = columns;
    }
    line->codePoints.clear();
    line->flags = 0;
    vtermLineClearMarks(line);
    vtermTouch(vterm, line);
}

void vtermLineClearMarks(VtermStoredLine* line) {
    line->marks = 0;
    line->commandStatus = PrVtermCommandStatus_None;
    line->inputColumn = 0;
    line->exitCode = 0;
}

void vtermPromptSetStatus(Vterm* vterm, PrVtermCommandStatus status, int32_t exitCode) {
    VtermScrollback* scrollback = &vterm->scrollback;
    if (!vterm->hasPrompt || vterm->promptLine < vterm->scrollbackDropped)
        return;
    uint64_t index = vterm->promptLine - vterm->scrollbackDropped;
    if (index >= scrollback->count + vterm->rows)
        return;
    if (index >= scrollback->count) {
        VtermStoredLine& line =
            vtermRowsAt(&vterm->screens[VtermScreenKind_Primary].rows, uint32_t(index - scrollback->count));
        if ((line.marks & PrVtermLineMarkFlag_Prompt) == 0)
            return;
        line.commandStatus = status;
        line.exitCode = exitCode;
        vtermTouch(vterm, &line);
        return;
    }
    // The marks of a packed line have a fixed size, so they can change in place.
    VtermPackedLine& packed = vtermScrollbackAt(scrollback, uint32_t(index));
    VtermPackedView view = vtermPackedView(scrollback, packed);
    if (view.marks == nullptr || (view.marks[0] & PrVtermLineMarkFlag_Prompt) == 0)
        return;
    view.marks[1] = status;
    prMemCopy(view.marks + 6, &exitCode, sizeof(int32_t));
    packed.revision = vtermRevise(vterm);
}

void vtermScreensInit(Vterm* vterm) {
    for (VtermScreen& screen : vterm->screens) {
        screen.rows.slots.resize(vterm->rows);
        prMemZero(screen.rows.slots.data, sizeof(VtermStoredLine) * vterm->rows);
        screen.rows.start = 0;
        for (uint32_t i = 0; i < vterm->rows; ++i)
            vtermLineClear(vterm, &screen.rows.slots.data[i], vterm->columns);
    }
    vtermLineCacheReset(vterm);
    vterm->top = 0;
    vterm->bottom = vterm->rows;
    vtermMarginsReset(vterm);
    tabStopsReset(vterm, 0);
}

void vtermScreensReset(Vterm* vterm) {
    for (VtermScreen& screen : vterm->screens)
        vtermRowsReset(&screen.rows);
    vtermScrollbackReset(&vterm->scrollback);
    for (VtermLineCacheEntry& entry : vterm->lineCache.entries.span()) {
        entry.cells.reset();
        entry.codePoints.reset();
    }
    vterm->lineCache.entries.reset();
    vterm->tabStops.reset();
}

bool vtermColumnInMargins(const Vterm* vterm, uint32_t column) {
    return column >= vterm->left && column < vterm->right;
}

void vtermPrint(Vterm* vterm, uint32_t codePoint) {
    VtermCursorState& cursor = vterm->cursor;
    codePoint = charsetMap(cursor.charsets[cursor.shift & 3], codePoint);

    // The grapheme segmentation state after the code point. The code point either ends the last printed cell's cluster
    // or begins a cluster of its own.
    PrUnicodeGraphemeState cluster{};
    bool segmented = false;
    if (vterm->hasLastPrinted && codePoint >= 0x300) {
        VtermStoredLine& line = vtermRow(vterm, vterm->lastPrintedRow);
        PrVtermCell& cell = line.cells.data[vterm->lastPrintedColumn];
        bool continues = false;
        if (cell.codePoint != 0 && cell.clusterCount < kPrVtermClusterMaxCodePoints) {
            cluster = vterm->lastCluster;
            segmented = true;
            continues = prUnicodeGraphemeStatePush(&cluster, codePoint);
        }
        if (continues) {
            vterm->lastCluster = cluster;
            bool regional = codePoint >= 0x1f1e6 && codePoint <= 0x1f1ff;
            bool widens = (cell.flags & PrVtermCellFlag_Wide) == 0 &&
                ((codePoint == kEmojiPresentation && (prUnicodeFlags(cell.codePoint) & PrUnicodeFlag_Emoji) != 0) ||
                    (regional && cell.clusterCount == 0));
            clusterAppend(&line, &cell, codePoint);
            uint32_t next = vterm->lastPrintedColumn + 1;
            uint32_t limit = textLimit(vterm, vterm->lastPrintedColumn);
            if (widens && next < limit) {
                // An emoji presentation selector or a second regional indicator makes the cluster two cells wide. The
                // cluster then takes the cell after it.
                splitWide(&line, next);
                PrVtermCell tail = line.cells.data[vterm->lastPrintedColumn];
                tail.codePoint = 0;
                tail.clusterCount = 0;
                tail.flags |= PrVtermCellFlag_WideTail;
                line.cells.data[next] = tail;
                line.cells.data[vterm->lastPrintedColumn].flags |= PrVtermCellFlag_Wide;
                vtermLineWrote(&line, next + 1);
                if (cursor.row == vterm->lastPrintedRow && !cursor.wrapPending && cursor.column == next) {
                    if (next + 1 < limit)
                        cursor.column = next + 1;
                    else
                        cursor.wrapPending = vterm->modes.autowrap;
                }
            }
            vtermTouch(vterm, &line);
            return;
        }
    }

    uint32_t width = widthOf(codePoint);
    if (width == 0)
        return;
    if (width == 2 && vterm->columns < 2)
        width = 1;
    if (cursor.wrapPending && vterm->modes.autowrap)
        wrapLine(vterm);
    cursor.wrapPending = false;
    uint32_t limit = textLimit(vterm, cursor.column);
    if (width == 2 && cursor.column + 1 >= limit) {
        if (vterm->modes.autowrap) {
            // The cluster does not fit in the cell, so the cell is left blank. At the line's end the cell is a spacer,
            // which is not text.
            VtermStoredLine& line = vtermRow(vterm, cursor.row);
            splitWide(&line, cursor.column);
            line.cells.data[cursor.column] = vtermBlankCell(vterm);
            if (cursor.column + 1 == vterm->columns)
                line.cells.data[cursor.column].flags = PrVtermCellFlag_WideSpacer;
            vtermLineWrote(&line, cursor.column + 1);
            vtermTouch(vterm, &line);
            wrapLine(vterm);
            limit = textLimit(vterm, cursor.column);
        } else {
            cursor.column = limit - 2;
        }
    }

    if (vterm->modes.insert)
        vtermInsertCells(vterm, width);
    VtermStoredLine& line = vtermRow(vterm, cursor.row);
    splitWide(&line, cursor.column);
    if (width == 2)
        splitWide(&line, cursor.column + 1);
    const VtermPen& pen = cursor.pen;
    PrVtermCell cell{
        .codePoint = codePoint,
        .underline = pen.underline,
        .flags = pen.flags,
        .link = vterm->link,
        .foreground = pen.foreground,
        .background = pen.background,
        .underlineColor = pen.underlineColor,
    };
    if (width == 2) {
        cell.flags |= PrVtermCellFlag_Wide;
        line.cells.data[cursor.column] = cell;
        cell.codePoint = 0;
        cell.flags = PrVtermCellFlags((cell.flags & ~PrVtermCellFlag_Wide) | PrVtermCellFlag_WideTail);
        line.cells.data[cursor.column + 1] = cell;
    } else {
        line.cells.data[cursor.column] = cell;
    }
    vtermLineWrote(&line, cursor.column + width);
    vtermTouch(vterm, &line);
    vterm->hasLastPrinted = true;
    vterm->lastPrintedRow = cursor.row;
    vterm->lastPrintedColumn = cursor.column;
    vterm->lastCodePoint = codePoint;
    vterm->lastCluster = segmented ? cluster : prUnicodeGraphemeStateBegin(codePoint);
    cursor.column += width;
    if (cursor.column >= limit) {
        cursor.column = limit - 1;
        cursor.wrapPending = vterm->modes.autowrap;
    }
}

void vtermPrintAscii(Vterm* vterm, PrStringView text) {
    VtermCursorState& cursor = vterm->cursor;
    VtermCharset charset = cursor.charsets[cursor.shift & 3];
    size_t at = 0;
    if (vterm->modes.insert || (charset != VtermCharset_Ascii && text.size != 0)) {
        // A mapped character may join the last printed cell's cluster, and vtermPrint decides that. Insertion moves the
        // row's cells for each character. Both cases print one character at a time.
        if (vterm->modes.insert) {
            for (size_t i = 0; i < text.size; ++i)
                vtermPrint(vterm, uint8_t(text.data[i]));
            return;
        }
        vtermPrint(vterm, uint8_t(text.data[0]));
        at = 1;
    }
    const VtermPen& pen = cursor.pen;
    PrVtermCell cell{
        .underline = pen.underline,
        .flags = pen.flags,
        .link = vterm->link,
        .foreground = pen.foreground,
        .background = pen.background,
        .underlineColor = pen.underlineColor,
    };
    while (at < text.size) {
        if (cursor.wrapPending && vterm->modes.autowrap)
            wrapLine(vterm);
        cursor.wrapPending = false;
        uint32_t limit = textLimit(vterm, cursor.column);
        uint32_t room = limit - cursor.column;
        size_t left = text.size - at;
        uint32_t count = left < room ? uint32_t(left) : room;
        // Without autowrap, the characters past the room each overwrite the last column, so the run's last character
        // stays there.
        bool overwrites = !vterm->modes.autowrap && left > room;
        VtermStoredLine& line = vtermRow(vterm, cursor.row);
        uint32_t column = cursor.column;
        splitWide(&line, column);
        splitWide(&line, column + count - 1);
        PrVtermCell* cells = line.cells.data + column;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data) + at;
        uint32_t codePoint = 0;
        if (charset == VtermCharset_Ascii) {
            // Write the cells as words: the words of `cell`, with each byte as the code point.
            CellWords words = cellWordsOf(cell);
            words.words[0] &= ~uint64_t(kCodePointMask);
            for (uint32_t i = 0; i < count; ++i) {
                CellWords written = words;
                written.words[0] |= bytes[i];
                prMemCopy(cells + i, &written, sizeof(CellWords));
            }
            codePoint = bytes[count - 1];
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                codePoint = charsetMap(charset, bytes[i]);
                cell.codePoint = codePoint;
                cells[i] = cell;
            }
        }
        at += count;
        if (overwrites) {
            codePoint = charsetMap(charset, uint8_t(text.data[text.size - 1]));
            cell.codePoint = codePoint;
            cells[count - 1] = cell;
            at = text.size;
        }
        vtermLineWrote(&line, column + count);
        vtermTouch(vterm, &line);
        vterm->hasLastPrinted = true;
        vterm->lastPrintedRow = cursor.row;
        vterm->lastPrintedColumn = column + count - 1;
        vterm->lastCodePoint = codePoint;
        // A printable ASCII character has the grapheme break property Other, and is neither pictographic nor an Indic
        // consonant. The segmentation state that it begins is therefore all zeros.
        if (charset == VtermCharset_Ascii)
            vterm->lastCluster = PrUnicodeGraphemeState{};
        else
            vterm->lastCluster = prUnicodeGraphemeStateBegin(codePoint);
        cursor.column = column + count;
        if (cursor.column >= limit) {
            cursor.column = limit - 1;
            cursor.wrapPending = vterm->modes.autowrap;
        }
    }
}

void vtermIndex(Vterm* vterm) {
    VtermCursorState& cursor = vterm->cursor;
    if (cursor.row + 1 == vterm->bottom) {
        if (vtermColumnInMargins(vterm, cursor.column))
            vtermScrollUp(vterm, 1);
    } else if (cursor.row + 1 < vterm->rows) {
        ++cursor.row;
    }
    cursor.wrapPending = false;
}

void vtermReverseIndex(Vterm* vterm) {
    VtermCursorState& cursor = vterm->cursor;
    if (cursor.row == vterm->top) {
        if (vtermColumnInMargins(vterm, cursor.column))
            vtermScrollDown(vterm, 1);
    } else if (cursor.row > 0) {
        --cursor.row;
    }
    cursor.wrapPending = false;
}

void vtermCarriageReturn(Vterm* vterm) {
    VtermCursorState& cursor = vterm->cursor;
    cursor.column = cursor.originMode || cursor.column >= vterm->left ? vterm->left : 0;
    cursor.wrapPending = false;
}

void vtermScrollUp(Vterm* vterm, uint32_t count) {
    regionUp(vterm, vterm->top, count, ScrolledOff_Kept);
}

void vtermScrollDown(Vterm* vterm, uint32_t count) {
    regionDown(vterm, vterm->top, count);
}

void vtermErase(Vterm* vterm, uint32_t row, uint32_t start, uint32_t end) {
    if (end > vterm->columns)
        end = vterm->columns;
    if (start >= end)
        return;
    VtermStoredLine& line = vtermRow(vterm, row);
    splitWide(&line, start);
    splitWide(&line, end - 1);
    cellsFill(line.cells.data + start, vtermBlankCell(vterm), end - start);
    lineWroteBlanks(vterm, &line, end);
    if (start == 0 && end == vterm->columns)
        vtermLineClearMarks(&line);
    vtermTouch(vterm, &line);
}

void vtermInsertCells(Vterm* vterm, uint32_t count) {
    VtermCursorState& cursor = vterm->cursor;
    if (!vtermColumnInMargins(vterm, cursor.column))
        return;
    lineInsertCells(vterm, &vtermRow(vterm, cursor.row), cursor.column, vterm->right, count);
    cursor.wrapPending = false;
}

void vtermDeleteCells(Vterm* vterm, uint32_t count) {
    VtermCursorState& cursor = vterm->cursor;
    if (!vtermColumnInMargins(vterm, cursor.column))
        return;
    lineDeleteCells(vterm, &vtermRow(vterm, cursor.row), cursor.column, vterm->right, count);
    cursor.wrapPending = false;
}

void vtermInsertLines(Vterm* vterm, uint32_t count) {
    VtermCursorState& cursor = vterm->cursor;
    if (!cursorInRegion(vterm))
        return;
    regionDown(vterm, cursor.row, count);
    cursor.column = vterm->left;
    cursor.wrapPending = false;
}

void vtermDeleteLines(Vterm* vterm, uint32_t count) {
    VtermCursorState& cursor = vterm->cursor;
    if (!cursorInRegion(vterm))
        return;
    // Lines that a deletion removes never enter the scrollback, even at the top of the screen.
    regionUp(vterm, cursor.row, count, ScrolledOff_Dropped);
    cursor.column = vterm->left;
    cursor.wrapPending = false;
}

void vtermInsertColumns(Vterm* vterm, uint32_t column, uint32_t count) {
    if (!cursorInRegion(vterm) || !vtermColumnInMargins(vterm, column))
        return;
    vterm->hasLastPrinted = false;
    for (uint32_t row = vterm->top; row < vterm->bottom; ++row)
        lineInsertCells(vterm, &vtermRow(vterm, row), column, vterm->right, count);
}

void vtermDeleteColumns(Vterm* vterm, uint32_t column, uint32_t count) {
    if (!cursorInRegion(vterm) || !vtermColumnInMargins(vterm, column))
        return;
    vterm->hasLastPrinted = false;
    for (uint32_t row = vterm->top; row < vterm->bottom; ++row)
        lineDeleteCells(vterm, &vtermRow(vterm, row), column, vterm->right, count);
}

void vtermMarginsReset(Vterm* vterm) {
    vterm->left = 0;
    vterm->right = vterm->columns;
}

void vtermCursorTo(Vterm* vterm, uint32_t column, uint32_t row) {
    VtermCursorState& cursor = vterm->cursor;
    cursor.column = column < vterm->columns ? column : vterm->columns - 1;
    cursor.row = row < vterm->rows ? row : vterm->rows - 1;
    cursor.wrapPending = false;
}

void vtermScreenShow(Vterm* vterm, VtermScreenKind kind, bool clear) {
    vtermRevise(vterm);
    vterm->hasLastPrinted = false;
    vterm->active = kind;
    if (kind == VtermScreenKind_Alternate && clear) {
        for (uint32_t row = 0; row < vterm->rows; ++row)
            vtermLineClear(vterm, &vtermRow(vterm, row), vterm->columns);
        vtermGraphicsClearAlternate(vterm);
    }
}

void vtermScreensResize(Vterm* vterm, uint32_t columns, uint32_t rows) {
    PR_ASSERT(columns > 0 && rows > 0);
    if (columns == vterm->columns && rows == vterm->rows)
        return;
    vtermRevise(vterm);
    vterm->hasLastPrinted = false;

    // The primary screen's cursor is the terminal's cursor while the primary screen shows. While the alternate screen
    // shows, it is the cursor that showing the alternate screen saved, if it saved one.
    VtermScreen& primary = vterm->screens[VtermScreenKind_Primary];
    bool primaryShows = vterm->active == VtermScreenKind_Primary;
    VtermCursorState* primaryCursor = primaryShows || !primary.hasSaved ? &vterm->cursor : &primary.saved;
    uint32_t column = primaryCursor->column;
    uint32_t row = primaryCursor->row;
    primaryResize(vterm, columns, rows, &column, &row);
    uint32_t alternateRow = vterm->cursor.row;
    alternateResize(vterm, columns, rows, &alternateRow);

    uint32_t oldColumns = vterm->columns;
    vterm->columns = columns;
    vterm->rows = rows;
    vterm->top = 0;
    vterm->bottom = rows;
    vtermMarginsReset(vterm);
    tabStopsReset(vterm, oldColumns < columns ? oldColumns : columns);
    if (primaryShows || primaryCursor != &vterm->cursor) {
        primaryCursor->column = column;
        primaryCursor->row = row;
    }
    if (!primaryShows)
        vterm->cursor.row = alternateRow;
    clampCursor(&vterm->cursor, columns, rows);
    for (VtermScreen& screen : vterm->screens)
        clampCursor(&screen.saved, columns, rows);
    vtermLineCacheReset(vterm);
    vtermGraphicsPrune(vterm);
}

void vtermScrollbackClear(Vterm* vterm) {
    uint32_t scrollback = vterm->scrollback.count;
    if (scrollback == 0)
        return;
    vtermScrollbackDrop(&vterm->scrollback, scrollback);
    scrollbackFit(&vterm->scrollback);
    vterm->scrollbackDropped += scrollback;
    vtermRevise(vterm);
}

uint16_t vtermChecksum(const Vterm* vterm, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right) {
    PrVtermChecksumFlags flags = vterm->checksum;
    bool untrimmed = (flags & PrVtermChecksumFlag_Untrimmed) != 0;
    const VtermRows* rows = &vterm->screens[vterm->active].rows;
    uint32_t sum = 0;
    // Without Untrimmed, a cell that holds nothing and has no attributes counts only if it is the rectangle's first
    // cell, as xterm counts it.
    bool first = true;
    for (uint32_t row = top; row < bottom; ++row) {
        const VtermStoredLine& line = vtermRowsAt(rows, row);
        for (uint32_t column = left; column < right && column < line.cells.count; ++column) {
            const PrVtermCell& cell = line.cells.data[column];
            bool tail = (cell.flags & PrVtermCellFlag_WideTail) != 0;
            bool written = cell.codePoint != 0 || tail;
            uint32_t code = ' ';
            if (!written) {
                if ((flags & (PrVtermChecksumFlag_Untrimmed | PrVtermChecksumFlag_Unwritten)) == 0)
                    continue;
            } else if ((flags & PrVtermChecksumFlag_CodePoints) != 0) {
                if (tail)
                    continue;
                code = cell.codePoint;
            } else {
                code = tail ? 0x1b : checksumCode(cell.codePoint);
            }
            if ((flags & PrVtermChecksumFlag_NoAttributes) == 0)
                code += checksumAttributes(cell);
            if (written || first || untrimmed || code != ' ')
                sum += code;
            if (untrimmed && (flags & PrVtermChecksumFlag_CodePoints) == 0) {
                for (uint32_t i = 0; i < cell.clusterCount; ++i)
                    sum += line.codePoints.data[cell.clusterStart + i];
            }
            first = untrimmed;
        }
        first = untrimmed;
    }
    if ((flags & PrVtermChecksumFlag_Positive) == 0)
        sum = 0u - sum;
    return uint16_t(sum);
}

} // namespace pr_impl
