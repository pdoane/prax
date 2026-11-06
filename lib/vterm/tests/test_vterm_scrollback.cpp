#include "pr/mem.h"

#include "vterm/src/vterm_screen.h"

#include "tools/harness/test.h"

#include <initializer_list>

namespace pr_impl {

namespace {

// Appends a line of `size` bytes, each set to `byte`, to `arena`. Returns the position where the line starts.
VtermArenaPosition append(VtermArena* arena, uint32_t size, uint8_t byte) {
    VtermArenaPosition position{};
    uint8_t* bytes = vtermArenaReserve(arena, size, &position);
    for (uint32_t i = 0; i < size; ++i)
        bytes[i] = byte;
    vtermArenaCommit(arena, size);
    return position;
}

// A pen that is all zeros except for `flags`, and a palette foreground of `index` if `index` is not zero.
VtermPackedPen penOf(PrVtermCellFlags flags, uint8_t index = 0) {
    VtermPackedPen pen;
    prMemZero(&pen, sizeof(pen));
    pen.flags = flags;
    if (index != 0)
        pen.foreground = prVtermColorPalette(index);
    return pen;
}

// A row of `columns` cells that a test fills and then packs.
struct Row {
    VtermStoredLine line{};

    explicit Row(uint32_t columns) {
        line.cells.resizeZeroed(columns);
        line.revision = 7;
    }
    ~Row() {
        line.cells.reset();
        line.codePoints.reset();
    }
    Row(const Row&) = delete;
    Row& operator=(const Row&) = delete;

    PrVtermCell& at(uint32_t column) {
        if (column + 1 > line.written)
            line.written = column + 1;
        return line.cells.data[column];
    }

    // Writes the ASCII `text` from `column`, in the style of `cell`.
    void text(uint32_t column, const char* text, PrVtermCell cell = PrVtermCell{}) {
        for (uint32_t i = 0; text[i] != 0; ++i) {
            cell.codePoint = uint8_t(text[i]);
            at(column + i) = cell;
        }
    }

    // Gives the cell at `column` the cluster code points `points`, which follow its first code point.
    void cluster(uint32_t column, std::initializer_list<uint32_t> points) {
        PrVtermCell& cell = at(column);
        cell.clusterStart = line.codePoints.count;
        cell.clusterCount = uint32_t(points.size());
        for (uint32_t point : points)
            line.codePoints.push(point);
    }
};

bool isSameColor(PrVtermColor a, PrVtermColor b) {
    return prMemEquals(&a, &b, sizeof(PrVtermColor));
}

// Returns true if unpacking line `index` of `scrollback` gives the line fields and cells of `row`, including the code
// points of each cluster. Cells past the written cells of the row must unpack as zeros.
bool unpacksAs(const VtermScrollback* scrollback, uint32_t index, const Row& row) {
    VtermStoredLine line{};
    vtermScrollbackUnpack(scrollback, index, row.line.cells.count, &line);
    bool same = line.cells.count == row.line.cells.count && line.revision == row.line.revision &&
        line.flags == row.line.flags && line.marks == row.line.marks &&
        line.commandStatus == row.line.commandStatus && line.inputColumn == row.line.inputColumn &&
        line.exitCode == row.line.exitCode;
    for (uint32_t i = 0; same && i < line.cells.count; ++i) {
        PrVtermCell expected = row.line.cells.data[i];
        PrVtermCell actual = line.cells.data[i];
        for (uint32_t k = 0; k < expected.clusterCount && k < actual.clusterCount; ++k) {
            same = same && row.line.codePoints.data[expected.clusterStart + k] ==
                line.codePoints.data[actual.clusterStart + k];
        }
        same = same && expected.codePoint == actual.codePoint && expected.clusterCount == actual.clusterCount &&
            expected.underline == actual.underline && expected.flags == actual.flags && expected.link == actual.link &&
            isSameColor(expected.foreground, actual.foreground) && isSameColor(expected.background, actual.background) &&
            isSameColor(expected.underlineColor, actual.underlineColor);
    }
    line.cells.reset();
    line.codePoints.reset();
    return same;
}

// The number of cells packed for line `index` of `scrollback`.
uint32_t packedCells(const VtermScrollback* scrollback, uint32_t index) {
    return vtermPackedView(scrollback, vtermScrollbackAt(scrollback, index)).cellCount;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm arena: lines append in order, and a line that does not fit starts the next block") {
    VtermArena arena{};
    VtermArenaPosition first = append(&arena, 100, 1);
    VtermArenaPosition second = append(&arena, 200, 2);
    CHECK(first.block == second.block);
    CHECK(first.offset == 0);
    CHECK(second.offset == 100);
    CHECK(vtermArenaAt(&arena, second)[0] == 2);

    VtermArenaPosition last = second;
    uint32_t filled = 300;
    while (filled + 1000 <= kVtermArenaBlockBytes) {
        last = append(&arena, 1000, 3);
        filled += 1000;
    }
    CHECK(last.block == first.block);
    VtermArenaPosition next = append(&arena, 1000, 4);
    CHECK(next.block == first.block + 1);
    CHECK(next.offset == 0);
    CHECK(vtermArenaAt(&arena, first)[99] == 1);
    CHECK(vtermArenaAt(&arena, next)[999] == 4);

    // A line larger than a standard block has a block of its own.
    VtermArenaPosition large = append(&arena, 2 * kVtermArenaBlockBytes, 5);
    CHECK(large.block == next.block + 1);
    VtermArenaPosition after = append(&arena, 10, 6);
    CHECK(after.block == large.block + 1);
    CHECK(vtermArenaAt(&arena, large)[2 * kVtermArenaBlockBytes - 1] == 5);
    vtermArenaReset(&arena);
}

TEST_CASE("vterm arena: blocks are freed oldest first, and a freed block's storage is reused") {
    VtermArena arena{};
    VtermArenaPosition first = append(&arena, kVtermArenaBlockBytes, 1);
    VtermArenaPosition second = append(&arena, kVtermArenaBlockBytes, 2);
    size_t held = vtermArenaMemorySize(&arena);
    vtermArenaDropBefore(&arena, second.block);
    CHECK(vtermArenaAt(&arena, second)[0] == 2);
    CHECK(vtermArenaMemorySize(&arena) == held);
    VtermArenaPosition third = append(&arena, 10, 3);
    CHECK(third.block == second.block + 1);
    CHECK(vtermArenaMemorySize(&arena) == held);
    vtermArenaDropBefore(&arena, first.block);
    CHECK(vtermArenaAt(&arena, second)[0] == 2);
    vtermArenaDropBefore(&arena, third.block + 1);
    CHECK(vtermArenaMemorySize(&arena) < held);

    // After every block is freed, the next block is numbered after them.
    VtermArenaPosition fourth = append(&arena, 10, 4);
    CHECK(fourth.block == third.block + 1);
    vtermArenaReset(&arena);
}

TEST_CASE("vterm arena: a truncation drops the bytes from a line on, and the blocks after it") {
    VtermArena arena{};
    append(&arena, 100, 1);
    VtermArenaPosition second = append(&arena, 100, 2);
    VtermArenaPosition third = append(&arena, kVtermArenaBlockBytes, 3);
    VtermPackedPen pen = penOf(PrVtermCellFlag_Bold);
    CHECK(vtermArenaPen(&arena, pen) == 1);
    vtermArenaTruncate(&arena, second);
    VtermArenaPosition again = append(&arena, 50, 4);
    CHECK(again.block == second.block);
    CHECK(again.offset == second.offset);
    CHECK(third.block == second.block + 1);
    vtermArenaReset(&arena);
}

TEST_CASE("vterm arena: pens are interned in the newest block, and the pen of all zeros is 0") {
    VtermArena arena{};
    append(&arena, 10, 1);
    VtermPackedPen bold = penOf(PrVtermCellFlag_Bold);
    VtermPackedPen red = penOf(0, 1);
    CHECK(vtermArenaPen(&arena, penOf(0)) == 0);
    CHECK(vtermArenaPen(&arena, bold) == 1);
    CHECK(vtermArenaPen(&arena, red) == 2);
    CHECK(vtermArenaPen(&arena, bold) == 1);
    VtermArenaPosition next = append(&arena, kVtermArenaBlockBytes, 2);
    CHECK(vtermArenaPen(&arena, red) == 1);
    CHECK(vtermArenaBlock(&arena, next.block).pens.count == 1);
    CHECK(vtermArenaBlock(&arena, next.block - 1).pens.count == 2);
    vtermArenaReset(&arena);
}

TEST_CASE("vterm scrollback: a line of ASCII packs to its text, and its blanks at the end are not stored") {
    VtermScrollback scrollback{.limit = 10};
    Row row(80);
    row.text(0, "hello world");
    row.at(20) = PrVtermCell{};
    bool dropped = true;
    vtermScrollbackPush(&scrollback, row.line, &dropped);
    CHECK(!dropped);
    CHECK(packedCells(&scrollback, 0) == 11);
    VtermPackedView view = vtermPackedView(&scrollback, vtermScrollbackAt(&scrollback, 0));
    CHECK(view.textBytes == 11);
    CHECK(view.runBytes == 0);
    CHECK(view.shapes == nullptr);
    CHECK(unpacksAs(&scrollback, 0, row));
    CHECK(vtermArenaBlock(&scrollback.arena, 0).used == 3 + 11);
    vtermScrollbackReset(&scrollback);
}

TEST_CASE("vterm scrollback: runs of pens, empty cells and colored blanks unpack as they were") {
    VtermScrollback scrollback{.limit = 10};
    Row row(40);
    PrVtermCell bold{.flags = PrVtermCellFlag_Bold, .foreground = prVtermColorPalette(3)};
    PrVtermCell under{
        .underline = PrVtermUnderline_Curly,
        .background = prVtermColorRgb(1, 2, 3),
        .underlineColor = prVtermColorPalette(9),
    };
    row.text(0, "plain");
    row.text(8, "bold", bold);
    row.text(12, "curly", under);
    row.at(30) = PrVtermCell{.background = prVtermColorPalette(1)};
    bool dropped = false;
    vtermScrollbackPush(&scrollback, row.line, &dropped);
    CHECK(packedCells(&scrollback, 0) == 31);
    CHECK(unpacksAs(&scrollback, 0, row));

    // A line of one pen packs as ASCII with a single run.
    Row same(10);
    same.text(0, "abcdef", bold);
    vtermScrollbackPush(&scrollback, same.line, &dropped);
    CHECK(unpacksAs(&scrollback, 1, same));
    CHECK(vtermPackedView(&scrollback, vtermScrollbackAt(&scrollback, 1)).runBytes == 2);
    vtermScrollbackReset(&scrollback);
}

TEST_CASE("vterm scrollback: wide clusters, spacers, clusters and every code point unpack as they were") {
    VtermScrollback scrollback{.limit = 10};
    Row row(20);
    row.at(0) = PrVtermCell{.codePoint = 0x4e2d, .flags = PrVtermCellFlag_Wide};
    row.at(1) = PrVtermCell{.flags = PrVtermCellFlag_WideTail};
    row.at(2) = PrVtermCell{.codePoint = 'e'};
    row.cluster(2, {0x301, 0x302});
    row.at(3) = PrVtermCell{.codePoint = 0x1f600, .flags = PrVtermCellFlag_Wide};
    row.cluster(3, {0xfe0f});
    row.at(4) = PrVtermCell{.flags = PrVtermCellFlag_WideTail};
    row.at(5) = PrVtermCell{.codePoint = 0xd800};
    row.at(6) = PrVtermCell{.codePoint = 0x1fffff};
    row.at(7) = PrVtermCell{.codePoint = 0xe9};
    row.at(19) = PrVtermCell{.flags = PrVtermCellFlag_WideSpacer};
    bool dropped = false;
    vtermScrollbackPush(&scrollback, row.line, &dropped);
    CHECK((vtermScrollbackAt(&scrollback, 0).packed & VtermPackedFlag_Shaped) != 0);
    CHECK(unpacksAs(&scrollback, 0, row));

    // A line of non-ASCII code points, each in its own cell, has no shapes.
    Row latin(10);
    latin.at(0) = PrVtermCell{.codePoint = 0xe9};
    latin.at(1) = PrVtermCell{.codePoint = 0x2500};
    vtermScrollbackPush(&scrollback, latin.line, &dropped);
    CHECK(vtermScrollbackAt(&scrollback, 1).packed == 0);
    CHECK(unpacksAs(&scrollback, 1, latin));
    vtermScrollbackReset(&scrollback);
}

TEST_CASE("vterm scrollback: links, marks and the line's flags unpack as they were") {
    VtermScrollback scrollback{.limit = 10};
    Row row(30);
    PrVtermCell linked{.link = 4};
    row.text(0, "see ");
    row.text(4, "here", linked);
    row.text(8, " and ");
    linked.link = 5;
    row.text(13, "there", linked);
    row.line.flags = PrVtermLineFlag_Wrapped;
    row.line.marks = PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input;
    row.line.commandStatus = PrVtermCommandStatus_Failed;
    row.line.inputColumn = 4;
    row.line.exitCode = -2;
    bool dropped = false;
    vtermScrollbackPush(&scrollback, row.line, &dropped);
    VtermPackedLine packed = vtermScrollbackAt(&scrollback, 0);
    CHECK(packed.packed == (VtermPackedFlag_Links | VtermPackedFlag_Marks));
    CHECK(packed.flags == PrVtermLineFlag_Wrapped);
    CHECK(unpacksAs(&scrollback, 0, row));

    // Marks pack with a line of ASCII too.
    Row plain(10);
    plain.text(0, "$ ls");
    plain.line.marks = PrVtermLineMarkFlag_Prompt;
    vtermScrollbackPush(&scrollback, plain.line, &dropped);
    CHECK(vtermScrollbackAt(&scrollback, 1).packed == VtermPackedFlag_Marks);
    CHECK(unpacksAs(&scrollback, 1, plain));
    vtermScrollbackReset(&scrollback);
}

TEST_CASE("vterm scrollback: the oldest lines drop past the limit, and their blocks are freed") {
    VtermScrollback scrollback{.limit = 1000};
    Row row(120);
    for (uint32_t i = 0; i < 120; ++i)
        row.at(i) = PrVtermCell{.codePoint = 'a' + i % 26};
    bool dropped = false;
    for (uint32_t i = 0; i < 1000; ++i) {
        row.line.revision = i + 1;
        vtermScrollbackPush(&scrollback, row.line, &dropped);
        CHECK(!dropped);
    }
    uint32_t blocks = scrollback.arena.count;
    CHECK(blocks >= 2);
    size_t held = vtermScrollbackMemorySize(&scrollback);
    for (uint32_t i = 0; i < 3000; ++i) {
        row.line.revision = 1000 + i + 1;
        vtermScrollbackPush(&scrollback, row.line, &dropped);
        CHECK(dropped);
    }
    CHECK(scrollback.count == 1000);
    CHECK(vtermScrollbackAt(&scrollback, 0).revision == 3001);
    CHECK(scrollback.arena.count <= blocks + 1);
    CHECK(vtermScrollbackMemorySize(&scrollback) <= held + kVtermArenaBlockBytes);
    CHECK(unpacksAs(&scrollback, 999, row));

    // A pop reclaims the bytes of the newest line.
    VtermArenaPosition newest = vtermScrollbackAt(&scrollback, 999).position;
    vtermScrollbackPop(&scrollback);
    vtermScrollbackPush(&scrollback, row.line, &dropped);
    CHECK(vtermScrollbackAt(&scrollback, 999).position.block == newest.block);
    CHECK(vtermScrollbackAt(&scrollback, 999).position.offset == newest.offset);
    vtermScrollbackDrop(&scrollback, 1000);
    CHECK(scrollback.arena.count == 0);
    vtermScrollbackReset(&scrollback);
}
