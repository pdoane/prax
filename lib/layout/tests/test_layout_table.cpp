// Table layout: the grid of a table's rows and columns, sized from its cells, with its captions, how its parts paint,
// and what each layout of it costs. Cells hold inline-blocks of the sizes given, in a font of size 0, so that a cell is
// only as tall and wide as its blocks, and its first baseline is the bottom of its first line of blocks.

#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

#include <vector>

namespace pr_impl {

namespace {

// The styles of a box that table cases set: its box group, and its table, decoration and font groups if given.
struct Styled {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    PrLayoutTableStyle table = prLayoutTableStyleDefault();
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    PrLayoutColorStyle color = prLayoutColorStyleDefault();
};

// A tree whose root is in a font of size 0, with helpers for the boxes that table cases are made of.
struct Stage {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    PrLayoutNode root = kPrLayoutNodeNone;

    explicit Stage(PrLayoutWritingMode mode = PrLayoutWritingMode_HorizontalTb,
            PrTextDirection direction = PrTextDirection_Ltr) {
        font.font = textFontOf(0);
        font.writingMode = mode;
        font.direction = direction;
        Styled styled;
        styled.box = block();
        root = add(kPrLayoutNodeNone, styled);
    }

    uint32_t style(const Styled& styled) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &styled.box);
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(f.tree, &font);
        record.groups[PrLayoutGroup_Table] = prLayoutTableStyleIntern(f.tree, &styled.table);
        record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(f.tree, &styled.decoration);
        record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(f.tree, &styled.color);
        return prLayoutStyleIntern(f.tree, &record);
    }

    PrLayoutNode add(PrLayoutNode parent, const Styled& styled) {
        PrLayoutNode node = prLayoutNodeCreateElement(f.tree, parent, style(styled));
        if (parent == kPrLayoutNodeNone)
            f.root = node;
        return node;
    }

    // Creates a part of `display` under `parent`.
    PrLayoutNode part(PrLayoutNode parent, PrLayoutDisplay display) { return add(parent, of(display)); }

    // Creates an inline-block of `width` by `height` under `parent`.
    PrLayoutNode word(PrLayoutNode parent, float width, float height) {
        Styled styled;
        styled.box = block(height);
        styled.box.display = PrLayoutDisplay_InlineBlock;
        styled.box.width = prLayoutLengthPx(width);
        return add(parent, styled);
    }

    // Creates a cell under `parent` that holds a word of `width` by `height`.
    PrLayoutNode cell(PrLayoutNode parent, float width, float height) {
        PrLayoutNode node = part(parent, PrLayoutDisplay_TableCell);
        word(node, width, height);
        return node;
    }

    void layout(float width = 600, float height = 400) {
        f.layout(px(width, height));
        CHECK(verified(f.tree));
    }

    PrLayoutTreeStats stats() { return prLayoutTreeStats(f.tree); }

    // Returns a box's border box, or one of its edges or sizes, relative to the root's top left.
    PrRect at(PrLayoutNode node) { return f.bounds(node); }
    float left(PrLayoutNode node) { return at(node).left; }
    float top(PrLayoutNode node) { return at(node).top; }
    float width(PrLayoutNode node) { return at(node).right - at(node).left; }
    float height(PrLayoutNode node) { return at(node).bottom - at(node).top; }

    // Returns the styles of a box of `display`.
    static Styled of(PrLayoutDisplay display) {
        Styled styled;
        styled.box = block();
        styled.box.display = display;
        return styled;
    }
};

// Returns the styles of a table with `spacing` px of border spacing in both axes.
Styled spacedTable(float spacing) {
    Styled styled = Stage::of(PrLayoutDisplay_Table);
    styled.table.borderSpacing[0] = prLayoutUnitFromPx(spacing);
    styled.table.borderSpacing[1] = prLayoutUnitFromPx(spacing);
    return styled;
}

// Returns the styles of a cell that vertical-align aligns by `keyword`, Top or Bottom, or by its alignment baseline.
Styled alignedCell(PrLayoutKeyword keyword) {
    Styled styled = Stage::of(PrLayoutDisplay_TableCell);
    if (keyword == PrLayoutKeyword_Center)
        styled.box.alignmentBaseline = PrLayoutAlignmentBaseline_Middle;
    else
        styled.box.baselineShift = prLayoutBaselineShiftKeyword(keyword);
    return styled;
}

// Sets every side of a box's border to `width` px of `color`, solid.
void setBorder(Styled& styled, float width, PrColor color) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        styled.box.border[side] = prLayoutUnitFromPx(width);
        styled.decoration.borderStyle[side] = PrDrawBorderStyle_Solid;
        styled.decoration.borderColor[side] = prColor16FromColor(color);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// The grid

TEST_CASE("a table's columns take their cells' widths, and its rows their heights") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(2));
    PrLayoutNode body = s.part(table, PrLayoutDisplay_TableRowGroup);
    PrLayoutNode row1 = s.part(body, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row1, 50, 10);
    PrLayoutNode b = s.cell(row1, 30, 20);
    PrLayoutNode row2 = s.part(body, PrLayoutDisplay_TableRow);
    PrLayoutNode c = s.cell(row2, 10, 5);
    PrLayoutNode d = s.cell(row2, 70, 5);
    s.layout();
    // Two columns of 50 and 70, and rows of 20 and 5, with 2px between and around them.
    checkRect(s.at(table), 0, 0, 2 + 50 + 2 + 70 + 2, 2 + 20 + 2 + 5 + 2);
    checkRect(s.at(a), 2, 2, 52, 22);
    checkRect(s.at(b), 54, 2, 124, 22);
    checkRect(s.at(c), 2, 24, 52, 29);
    checkRect(s.at(d), 54, 24, 124, 29);
    checkRect(s.at(row1), 2, 2, 124, 22);
    checkRect(s.at(body), 2, 2, 124, 29);
}

TEST_CASE("rows and cells need no boxes of their own: missing ones are anonymous") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    // A row in no row group, and cells in no row.
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row, 40, 10);
    PrLayoutNode b = s.cell(table, 20, 30);
    PrLayoutNode c = s.cell(table, 10, 30);
    s.layout();
    checkRect(s.at(a), 0, 0, 40, 10);
    checkRect(s.at(b), 0, 10, 40, 40);
    checkRect(s.at(c), 40, 10, 50, 40);
    checkRect(s.at(table), 0, 0, 50, 40);
}

TEST_CASE("content that is not a part of a table is wrapped in an anonymous row and cell") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(2));
    Styled div;
    div.box = block(10);
    PrLayoutNode child = s.add(table, div);
    s.word(child, 40, 10);
    s.layout();
    checkRect(s.at(child), 2, 2, 42, 12);
    checkRect(s.at(table), 0, 0, 44, 14);
}

TEST_CASE("an inline-block in an anonymous cell beside a block is placed, and stays placed as it changes") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    // The anonymous cell holds a block and an inline-block, which an anonymous block around it wraps.
    Styled div;
    div.box = block(10);
    s.add(table, div);
    PrLayoutNode word = s.word(table, 40, 20);
    s.layout();
    checkRect(s.at(word), 0, 10, 40, 30);
    Styled wider;
    wider.box = block(20);
    wider.box.display = PrLayoutDisplay_InlineBlock;
    wider.box.width = prLayoutLengthPx(60);
    prLayoutNodeSetStyle(s.f.tree, word, s.style(wider));
    s.layout();
    checkRect(s.at(word), 0, 10, 60, 30);
}

TEST_CASE("table parts outside a table are wrapped in an anonymous table") {
    Stage s;
    PrLayoutNode a = s.cell(s.root, 30, 10);
    PrLayoutNode b = s.cell(s.root, 20, 15);
    Styled after;
    after.box = block(5);
    PrLayoutNode next = s.add(s.root, after);
    s.layout();
    // The two cells are one row of one table, which shrinks to fit them, and the block after it is below it.
    checkRect(s.at(a), 0, 0, 30, 15);
    checkRect(s.at(b), 30, 0, 50, 15);
    checkRect(s.at(next), 0, 15, 600, 20);
}

TEST_CASE("table parts in an inline box are wrapped in an anonymous inline table on the line") {
    Stage s;
    PrLayoutNode x = s.word(s.root, 10, 10);
    PrLayoutNode span = s.part(s.root, PrLayoutDisplay_Inline);
    PrLayoutNode a = s.part(span, PrLayoutDisplay_TableCell);
    PrLayoutNode word = s.word(a, 30, 10);
    PrLayoutNode b = s.cell(span, 20, 10);
    PrLayoutNode y = s.word(s.root, 10, 10);
    s.layout();
    // One line: x, then a table of one row, A and B, on its baseline, then y.
    checkRect(s.at(x), 0, 0, 10, 10);
    checkRect(s.at(a), 10, 0, 40, 10);
    checkRect(s.at(b), 40, 0, 60, 10);
    checkRect(s.at(y), 60, 0, 70, 10);
    // A wider cell lays the table out again, and moves y along the line.
    Styled wider;
    wider.box = block(10);
    wider.box.display = PrLayoutDisplay_InlineBlock;
    wider.box.width = prLayoutLengthPx(50);
    prLayoutNodeSetStyle(s.f.tree, word, s.style(wider));
    s.layout();
    checkRect(s.at(a), 10, 0, 60, 10);
    checkRect(s.at(b), 60, 0, 80, 10);
    checkRect(s.at(y), 80, 0, 90, 10);
}

TEST_CASE("the first header group goes first and the first footer group last") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode foot = s.part(table, PrLayoutDisplay_TableFooterGroup);
    PrLayoutNode f = s.cell(s.part(foot, PrLayoutDisplay_TableRow), 10, 10);
    PrLayoutNode body = s.part(table, PrLayoutDisplay_TableRowGroup);
    PrLayoutNode b = s.cell(s.part(body, PrLayoutDisplay_TableRow), 10, 20);
    PrLayoutNode head = s.part(table, PrLayoutDisplay_TableHeaderGroup);
    PrLayoutNode h = s.cell(s.part(head, PrLayoutDisplay_TableRow), 10, 30);
    s.layout();
    CHECK(s.top(h) == 0.0f);
    CHECK(s.top(b) == 30.0f);
    CHECK(s.top(f) == 50.0f);
}

TEST_CASE("spanning cells take the room of the slots they span") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row1 = s.part(table, PrLayoutDisplay_TableRow);
    Styled wide = Stage::of(PrLayoutDisplay_TableCell);
    wide.table.columnSpan = 2;
    PrLayoutNode a = s.add(row1, wide);
    s.word(a, 200, 10);
    Styled tall = alignedCell(PrLayoutKeyword_Top);
    tall.table.rowSpan = 2;
    PrLayoutNode t = s.add(row1, tall);
    s.word(t, 10, 100);
    PrLayoutNode row2 = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode b = s.cell(row2, 50, 30);
    PrLayoutNode c = s.cell(row2, 50, 30);
    s.layout();
    // The wide cell's 200px goes to its two columns in proportion to their max-content widths. The tall cell's 100px
    // grows the two rows it spans in proportion to their heights, 10 and 30.
    checkRect(s.at(a), 0, 0, 200, 25);
    checkRect(s.at(b), 0, 25, 100, 100);
    checkRect(s.at(c), 100, 25, 200, 100);
    checkRect(s.at(t), 200, 0, 210, 100);
}

TEST_CASE("a row span of zero spans the rest of its row group") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode body = s.part(table, PrLayoutDisplay_TableRowGroup);
    Styled rest = alignedCell(PrLayoutKeyword_Top);
    rest.table.rowSpan = 0;
    PrLayoutNode row1 = s.part(body, PrLayoutDisplay_TableRow);
    PrLayoutNode r = s.add(row1, rest);
    s.word(r, 10, 10);
    s.cell(row1, 10, 10);
    PrLayoutNode b = s.cell(s.part(body, PrLayoutDisplay_TableRow), 10, 10);
    PrLayoutNode c = s.cell(s.part(body, PrLayoutDisplay_TableRow), 10, 10);
    s.layout();
    checkRect(s.at(r), 0, 0, 10, 30);
    checkRect(s.at(b), 10, 10, 20, 20);
    checkRect(s.at(c), 10, 20, 20, 30);
}

// ---------------------------------------------------------------------------------------------------------------------
// Widths

TEST_CASE("a table's extra width goes to its columns in proportion to their max-content widths") {
    Stage s;
    Styled styled = spacedTable(0);
    styled.box.width = prLayoutLengthPx(300);
    PrLayoutNode table = s.add(s.root, styled);
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row, 50, 10);
    PrLayoutNode b = s.cell(row, 150, 10);
    s.layout();
    checkRect(s.at(a), 0, 0, 75, 10);
    checkRect(s.at(b), 75, 0, 300, 10);
}

TEST_CASE("a table narrower than its max-content width interpolates between its sizing guesses") {
    Stage s;
    Styled styled = spacedTable(0);
    styled.box.width = prLayoutLengthPx(150);
    PrLayoutNode table = s.add(s.root, styled);
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.part(row, PrLayoutDisplay_TableCell);
    s.word(a, 40, 20);
    s.word(a, 40, 20);
    PrLayoutNode b = s.part(row, PrLayoutDisplay_TableCell);
    s.word(b, 60, 20);
    s.word(b, 60, 20);
    s.layout();
    // Half way from min-content (40, 60) to max-content (80, 120); each cell's words then wrap.
    checkRect(s.at(a), 0, 0, 60, 40);
    checkRect(s.at(b), 60, 0, 150, 40);
}

TEST_CASE("a table is never narrower than its min-content width, and an auto table fits its content") {
    Stage s;
    Styled narrow = spacedTable(0);
    narrow.box.width = prLayoutLengthPx(10);
    PrLayoutNode n = s.add(s.root, narrow);
    s.cell(s.part(n, PrLayoutDisplay_TableRow), 80, 10);
    PrLayoutNode fit = s.add(s.root, spacedTable(0));
    s.cell(s.part(fit, PrLayoutDisplay_TableRow), 80, 10);
    s.layout();
    CHECK(s.width(n) == 80.0f);
    CHECK(s.width(fit) == 80.0f);
}

TEST_CASE("a percentage column takes its share of the table") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    Styled half = Stage::of(PrLayoutDisplay_TableCell);
    half.box.width = prLayoutLengthPercent(50);
    PrLayoutNode a = s.add(row, half);
    s.word(a, 50, 10);
    PrLayoutNode b = s.cell(row, 100, 10);
    s.layout();
    // The other column's 100px asks for a table of 200px, of which the percentage column takes half.
    checkRect(s.at(a), 0, 0, 100, 10);
    checkRect(s.at(b), 100, 0, 200, 10);
}

TEST_CASE("a fixed table sizes its columns from its column boxes and its first row alone") {
    Stage s;
    Styled styled = spacedTable(0);
    styled.box.width = prLayoutLengthPx(300);
    styled.table.tableLayout = PrLayoutTableLayout_Fixed;
    PrLayoutNode table = s.add(s.root, styled);
    Styled column = Stage::of(PrLayoutDisplay_TableColumn);
    column.box.width = prLayoutLengthPx(60);
    s.add(table, column);
    PrLayoutNode row1 = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row1, 10, 10);
    Styled sized = Stage::of(PrLayoutDisplay_TableCell);
    sized.box.width = prLayoutLengthPx(100);
    PrLayoutNode b = s.add(row1, sized);
    s.word(b, 10, 10);
    PrLayoutNode c = s.cell(row1, 10, 10);
    // A later row's width and content do not count.
    PrLayoutNode row2 = s.part(table, PrLayoutDisplay_TableRow);
    s.cell(row2, 10, 10);
    s.cell(row2, 10, 10);
    Styled wide = Stage::of(PrLayoutDisplay_TableCell);
    wide.box.width = prLayoutLengthPx(250);
    PrLayoutNode d = s.add(row2, wide);
    s.word(d, 250, 10);
    s.layout();
    checkRect(s.at(a), 0, 0, 60, 10);
    checkRect(s.at(b), 60, 0, 160, 10);
    checkRect(s.at(c), 160, 0, 300, 10);
    CHECK(s.width(table) == 300.0f);
}

TEST_CASE("a cell's width is clamped by its max-width, then by its min-width") {
    Stage s;
    Styled styled = spacedTable(0);
    styled.box.width = prLayoutLengthPx(400);
    PrLayoutNode table = s.add(s.root, styled);
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    s.cell(row, 10, 10);
    Styled clamped = Stage::of(PrLayoutDisplay_TableCell);
    clamped.box.width = prLayoutLengthPx(200);
    clamped.box.minWidth = prLayoutLengthPx(150);
    clamped.box.maxWidth = prLayoutLengthPx(100);
    PrLayoutNode b = s.add(row, clamped);
    Styled narrow = Stage::of(PrLayoutDisplay_TableCell);
    narrow.box.width = prLayoutLengthPx(50);
    narrow.box.maxWidth = prLayoutLengthPx(1);
    PrLayoutNode c = s.add(row, narrow);
    s.word(c, 30, 10);
    s.layout();
    // The min-width wins over the max-width, and the max-width limits even the content's min-content width.
    CHECK(s.width(b) == 150.0f);
    CHECK(s.width(c) == 1.0f);
}

TEST_CASE("percentage columns that leave nothing fill the container, and contribute only their content") {
    Stage s;
    // A float shrinks to fit the container, which the table's unbounded max-content width fills.
    Styled floated = spacedTable(0);
    floated.box.floating = PrLayoutFloat_Left;
    PrLayoutNode fills = s.add(s.root, floated);
    PrLayoutNode row = s.part(fills, PrLayoutDisplay_TableRow);
    Styled whole = Stage::of(PrLayoutDisplay_TableCell);
    whole.box.width = prLayoutLengthPercent(100);
    s.word(s.add(row, whole), 10, 10);
    s.cell(row, 10, 10);
    // A flex item's base size is the table's max-content width without the percentage ask.
    Styled flex;
    flex.box = block();
    flex.box.display = PrLayoutDisplay_Flex;
    flex.box.clear = PrLayoutClear_Both;
    PrLayoutNode container = s.add(s.root, flex);
    PrLayoutNode item = s.add(container, spacedTable(0));
    PrLayoutNode itemRow = s.part(item, PrLayoutDisplay_TableRow);
    s.word(s.add(itemRow, whole), 10, 10);
    s.cell(itemRow, 10, 10);
    s.layout();
    CHECK(s.width(fills) == 600.0f);
    CHECK(s.width(item) == 20.0f);
}

TEST_CASE("a fixed table whose width is a percentage has an unbounded max-content width") {
    Stage s;
    Styled floated;
    floated.box = block();
    floated.box.floating = PrLayoutFloat_Left;
    PrLayoutNode container = s.add(s.root, floated);
    Styled styled = spacedTable(0);
    styled.box.width = prLayoutLengthPercent(50);
    styled.table.tableLayout = PrLayoutTableLayout_Fixed;
    PrLayoutNode table = s.add(container, styled);
    s.cell(s.part(table, PrLayoutDisplay_TableRow), 10, 10);
    s.layout();
    CHECK(s.width(container) == 600.0f);
    CHECK(s.width(table) == 300.0f);
}

TEST_CASE("captions take the table's width, outside its grid, at the side they name") {
    Stage s;
    Styled styled = spacedTable(0);
    setBorder(styled, 1, PrColor{0, 0, 0, 1});
    PrLayoutNode table = s.add(s.root, styled);
    PrLayoutNode top = s.part(table, PrLayoutDisplay_TableCaption);
    s.word(top, 30, 10);
    PrLayoutNode cell = s.cell(s.part(table, PrLayoutDisplay_TableRow), 50, 20);
    Styled below = Stage::of(PrLayoutDisplay_TableCaption);
    below.table.captionSide = PrLayoutCaptionSide_Bottom;
    PrLayoutNode bottom = s.add(table, below);
    s.word(bottom, 70, 5);
    s.layout();
    // The table is as wide as its widest caption's min-content width, and its borders go around its grid alone.
    checkRect(s.at(table), 0, 0, 70, 10 + 1 + 20 + 1 + 5);
    checkRect(s.at(top), 0, 0, 70, 10);
    checkRect(s.at(cell), 1, 11, 69, 31);
    checkRect(s.at(bottom), 0, 32, 70, 37);
}

TEST_CASE("a collapsed row or column takes no room and no spacing") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(2));
    Styled collapsedColumn = Stage::of(PrLayoutDisplay_TableColumn);
    collapsedColumn.box.collapsed = 1;
    s.part(table, PrLayoutDisplay_TableColumn);
    s.add(table, collapsedColumn);
    Styled collapsedRow = Stage::of(PrLayoutDisplay_TableRow);
    collapsedRow.box.collapsed = 1;
    PrLayoutNode row1 = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row1, 10, 10);
    s.cell(row1, 30, 10);
    PrLayoutNode row2 = s.add(table, collapsedRow);
    s.cell(row2, 10, 10);
    s.cell(row2, 30, 10);
    PrLayoutNode row3 = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode c = s.cell(row3, 10, 10);
    s.cell(row3, 30, 10);
    s.layout();
    checkRect(s.at(table), 0, 0, 2 + 10 + 2, 2 + 10 + 2 + 10 + 2);
    checkRect(s.at(a), 2, 2, 12, 12);
    checkRect(s.at(c), 2, 14, 12, 24);
    CHECK(s.height(row2) == 0.0f);
}

TEST_CASE("rows are sized as if none were collapsed, and a cell spanning a collapsed row is shorter") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row1 = s.part(table, PrLayoutDisplay_TableRow);
    // A top-aligned cell, so that its ascent does not size the first row alone.
    Styled spanning = alignedCell(PrLayoutKeyword_Top);
    spanning.table.rowSpan = 3;
    PrLayoutNode tall = s.add(row1, spanning);
    s.word(tall, 10, 60);
    s.cell(row1, 10, 10);
    Styled collapsedRow = Stage::of(PrLayoutDisplay_TableRow);
    collapsedRow.box.collapsed = 1;
    s.cell(s.add(table, collapsedRow), 10, 10);
    PrLayoutNode c = s.cell(s.part(table, PrLayoutDisplay_TableRow), 10, 10);
    s.layout();
    // The spanning cell's 60px grows each of the three rows to 20px, and the collapsed row's 20px is then removed.
    CHECK(s.top(c) == 20.0f);
    CHECK(s.height(tall) == 40.0f);
    CHECK(s.height(table) == 40.0f);
}

TEST_CASE("a table's height is within its max-height, and the rows fill it before a collapsed row is removed") {
    Stage s;
    Styled limited = spacedTable(0);
    limited.box.height = prLayoutLengthPx(300);
    limited.box.maxHeight = prLayoutLengthPx(100);
    PrLayoutNode a = s.add(s.root, limited);
    s.cell(s.part(a, PrLayoutDisplay_TableRow), 10, 10);
    Styled styled = spacedTable(0);
    styled.box.height = prLayoutLengthPx(150);
    PrLayoutNode b = s.add(s.root, styled);
    s.cell(s.part(b, PrLayoutDisplay_TableRow), 10, 10);
    Styled collapsedRow = Stage::of(PrLayoutDisplay_TableRow);
    collapsedRow.box.collapsed = 1;
    s.cell(s.add(b, collapsedRow), 10, 10);
    s.cell(s.part(b, PrLayoutDisplay_TableRow), 10, 10);
    s.layout();
    CHECK(s.height(a) == 100.0f);
    CHECK(s.height(b) == 100.0f);
}

TEST_CASE("the margins between captions add up") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    Styled spaced = Stage::of(PrLayoutDisplay_TableCaption);
    setMargins(spaced.box, 5, 5);
    PrLayoutNode first = s.add(table, spaced);
    s.word(first, 10, 10);
    PrLayoutNode second = s.add(table, spaced);
    s.word(second, 10, 10);
    PrLayoutNode cell = s.cell(s.part(table, PrLayoutDisplay_TableRow), 10, 10);
    s.layout();
    CHECK(s.top(first) == 5.0f);
    CHECK(s.top(second) == float(5 + 10 + 5 + 5));
    CHECK(s.top(cell) == float(5 + 10 + 5 + 5 + 10 + 5));
}

// ---------------------------------------------------------------------------------------------------------------------
// Heights and alignment

TEST_CASE("cells align in their row by vertical-align") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode top = s.add(row, alignedCell(PrLayoutKeyword_Top));
    s.word(top, 10, 60);
    PrLayoutNode middle = s.add(row, alignedCell(PrLayoutKeyword_Center));
    PrLayoutNode m = s.word(middle, 10, 20);
    PrLayoutNode bottom = s.add(row, alignedCell(PrLayoutKeyword_Bottom));
    PrLayoutNode b = s.word(bottom, 10, 20);
    PrLayoutNode tall = s.word(s.part(row, PrLayoutDisplay_TableCell), 10, 30);
    PrLayoutNode shortWord = s.word(s.part(row, PrLayoutDisplay_TableCell), 10, 10);
    s.layout();
    // Every cell is as tall as the row; the content moves within it.
    CHECK(s.height(middle) == 60.0f);
    CHECK(s.top(m) == 20.0f);
    CHECK(s.top(b) == 40.0f);
    // The baseline cells share the baseline of the tallest above it: 30px down.
    CHECK(s.top(tall) == 0.0f);
    CHECK(s.top(shortWord) == 20.0f);
}

TEST_CASE("a table's block size grows its rows, and an inline table sits on its first row's baseline") {
    Stage s;
    Styled styled = spacedTable(0);
    styled.box.height = prLayoutLengthPx(100);
    PrLayoutNode table = s.add(s.root, styled);
    PrLayoutNode a = s.cell(s.part(table, PrLayoutDisplay_TableRow), 10, 20);
    PrLayoutNode b = s.cell(s.part(table, PrLayoutDisplay_TableRow), 10, 20);
    Styled line;
    line.box = block();
    PrLayoutNode container = s.add(s.root, line);
    s.word(container, 20, 40);
    PrLayoutNode inlineTable = s.add(container, Stage::of(PrLayoutDisplay_InlineTable));
    s.cell(s.part(inlineTable, PrLayoutDisplay_TableRow), 10, 10);
    s.layout();
    checkRect(s.at(a), 0, 0, 10, 50);
    checkRect(s.at(b), 0, 50, 10, 100);
    // The word's baseline is its bottom, 40px down the line, and the table's first baseline is 10px down.
    CHECK(s.top(inlineTable) - s.top(container) == 30.0f);
}

TEST_CASE("a cell with no baseline stays at the top of its row") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    Styled empty = Stage::of(PrLayoutDisplay_TableCell);
    empty.box.padding[PrRectSide_Top] = prLayoutLengthPx(30);
    s.add(row, empty);
    PrLayoutNode word = s.word(s.part(row, PrLayoutDisplay_TableCell), 10, 10);
    s.layout();
    // The empty cell's content-box end, 30px down, would push the other cell's baseline down if it counted.
    CHECK(s.top(word) == 0.0f);
}

TEST_CASE("a cell with content but no baseline aligns the end of its content on its row's baseline") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    // A table with no rows has no baseline, so the cell that holds it has none either.
    PrLayoutNode holder = s.part(row, PrLayoutDisplay_TableCell);
    Styled inner = spacedTable(0);
    inner.box.height = prLayoutLengthPx(30);
    s.add(holder, inner);
    PrLayoutNode word = s.word(s.part(row, PrLayoutDisplay_TableCell), 10, 10);
    s.layout();
    // The holder's content ends 30px down, which is the row's baseline: the word's bottom lies on it.
    CHECK(s.top(word) == 20.0f);
}

TEST_CASE("a block container takes no last baseline from a table") {
    Stage s;
    Styled line;
    line.box = block();
    PrLayoutNode container = s.add(s.root, line);
    s.word(container, 20, 40);
    Styled inlineBlock = Stage::of(PrLayoutDisplay_InlineBlock);
    PrLayoutNode holder = s.add(container, inlineBlock);
    PrLayoutNode table = s.add(holder, spacedTable(0));
    PrLayoutNode cell = s.part(s.part(table, PrLayoutDisplay_TableRow), PrLayoutDisplay_TableCell);
    s.word(cell, 10, 10);
    Styled below;
    below.box = block(20);
    s.add(cell, below);
    s.layout();
    // The inline-block has no line box of its own, so it sits on its bottom edge: 30px tall on a baseline 40px down.
    CHECK(s.height(holder) == 30.0f);
    CHECK(s.top(holder) - s.top(container) == 10.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Collapsed borders

TEST_CASE("collapsing borders share each line between the boxes on either side of it") {
    Stage s;
    Styled styled = spacedTable(5);
    styled.table.borderCollapse = PrLayoutBorderCollapse_Collapse;
    setBorder(styled, 4, PrColor{0, 0, 1, 1});
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        styled.box.padding[side] = prLayoutLengthPx(7);
    PrLayoutNode table = s.add(s.root, styled);
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    Styled bordered = Stage::of(PrLayoutDisplay_TableCell);
    setBorder(bordered, 2, PrColor{1, 0, 0, 1});
    PrLayoutNode a = s.add(row, bordered);
    PrLayoutNode wa = s.word(a, 20, 10);
    PrLayoutNode b = s.add(row, bordered);
    s.word(b, 20, 10);
    s.layout();
    // The table's 4px wins at its edges, half inside it and half in each cell. The cells' 2px between them is shared.
    // The table's padding and border spacing are ignored.
    checkRect(s.at(table), 0, 0, 2 + 23 + 23 + 2, 2 + 14 + 2);
    checkRect(s.at(a), 2, 2, 25, 16);
    checkRect(s.at(b), 25, 2, 48, 16);
    checkRect(s.at(wa), 4, 4, 24, 14);
}

// ---------------------------------------------------------------------------------------------------------------------
// Writing modes

TEST_CASE("a right-to-left table runs its columns from the right") {
    Stage s(PrLayoutWritingMode_HorizontalTb, PrTextDirection_Rtl);
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row, 10, 10);
    PrLayoutNode b = s.cell(row, 20, 10);
    s.layout();
    checkRect(s.at(table), 570, 0, 600, 10);
    checkRect(s.at(a), 590, 0, 600, 10);
    checkRect(s.at(b), 570, 0, 590, 10);
}

TEST_CASE("a vertical table stacks its rows across its lines and its columns along them") {
    Stage s(PrLayoutWritingMode_VerticalRl);
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row1 = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row1, 10, 20);
    PrLayoutNode b = s.cell(row1, 10, 30);
    PrLayoutNode row2 = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode c = s.cell(row2, 15, 20);
    s.layout(600, 400);
    // The first row is the rightmost, and its cells run down it.
    CHECK(s.left(a) == s.left(b));
    CHECK(s.top(b) == s.at(a).bottom);
    CHECK(s.at(c).right == s.left(a));
    CHECK(s.top(c) == s.top(a));
}

TEST_CASE("an orthogonal cell is as wide as its block size, and as tall as its content") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    s.font.writingMode = PrLayoutWritingMode_VerticalRl;
    PrLayoutNode cell = s.part(row, PrLayoutDisplay_TableCell);
    s.word(cell, 10, 30);
    s.font.writingMode = PrLayoutWritingMode_HorizontalTb;
    PrLayoutNode other = s.cell(row, 20, 10);
    s.layout();
    CHECK(s.width(cell) == 10.0f);
    CHECK(s.height(cell) == 30.0f);
    CHECK(s.height(other) == 30.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

TEST_CASE("a row's background paints behind its cells, and an empty cell can hide") {
    Stage s;
    Styled styled = spacedTable(10);
    PrLayoutNode table = s.add(s.root, styled);
    Styled red = Stage::of(PrLayoutDisplay_TableRow);
    red.decoration.background = prColor16FromColor(PrColor{1, 0, 0, 1});
    PrLayoutNode row = s.add(table, red);
    s.cell(row, 30, 30);
    Styled hidden = Stage::of(PrLayoutDisplay_TableCell);
    hidden.color.emptyCells = PrLayoutEmptyCells_Hide;
    hidden.box.width = prLayoutLengthPx(30);
    s.add(row, hidden);
    s.layout(100, 60);
    std::vector<uint8_t> pixels = render(s.f.tree, 100, 60);
    // In the first cell, the row's background shows; in the spacing and in the hidden cell, nothing does.
    CHECK(pixel(pixels, 100, 20, 20) == "255 0 0 255");
    CHECK(pixel(pixels, 100, 45, 20) == "0 0 0 0");
    CHECK(pixel(pixels, 100, 60, 20) == "0 0 0 0");
}

TEST_CASE("a table paints its border around its grid, and collapsed borders over its cells") {
    Stage s;
    Styled styled = spacedTable(0);
    styled.table.borderCollapse = PrLayoutBorderCollapse_Collapse;
    setBorder(styled, 4, PrColor{0, 0, 1, 1});
    PrLayoutNode table = s.add(s.root, styled);
    PrLayoutNode caption = s.part(table, PrLayoutDisplay_TableCaption);
    s.word(caption, 40, 10);
    Styled cell = Stage::of(PrLayoutDisplay_TableCell);
    cell.decoration.background = prColor16FromColor(PrColor{0, 1, 0, 1});
    PrLayoutNode c = s.add(s.part(table, PrLayoutDisplay_TableRow), cell);
    s.word(c, 40, 20);
    s.layout(100, 60);
    std::vector<uint8_t> pixels = render(s.f.tree, 100, 60);
    // The caption has no border; the grid's 4px border straddles its edge over the cell's background.
    CHECK(pixel(pixels, 100, 1, 5) == "0 0 0 0");
    CHECK(pixel(pixels, 100, 1, 12) == "0 0 255 255");
    CHECK(pixel(pixels, 100, 3, 12) == "0 0 255 255");
    CHECK(pixel(pixels, 100, 20, 25) == "0 255 0 255");
}

TEST_CASE("each column of a column group paints its background behind its own cells") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    PrLayoutNode group = s.part(table, PrLayoutDisplay_TableColumnGroup);
    Styled red = Stage::of(PrLayoutDisplay_TableColumn);
    red.decoration.background = prColor16FromColor(PrColor{1, 0, 0, 1});
    s.add(group, red);
    Styled green = Stage::of(PrLayoutDisplay_TableColumn);
    green.decoration.background = prColor16FromColor(PrColor{0, 1, 0, 1});
    s.add(group, green);
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    s.cell(row, 30, 30);
    s.cell(row, 30, 30);
    s.layout(100, 60);
    std::vector<uint8_t> pixels = render(s.f.tree, 100, 60);
    CHECK(pixel(pixels, 100, 15, 15) == "255 0 0 255");
    CHECK(pixel(pixels, 100, 45, 15) == "0 255 0 255");
}

TEST_CASE("a table that is not visible draws no collapsed borders") {
    Stage s;
    Styled styled = spacedTable(0);
    styled.table.borderCollapse = PrLayoutBorderCollapse_Collapse;
    styled.color.visibility = PrLayoutVisibility_Hidden;
    setBorder(styled, 10, PrColor{1, 0, 0, 1});
    PrLayoutNode table = s.add(s.root, styled);
    Styled cell = Stage::of(PrLayoutDisplay_TableCell);
    cell.color.visibility = PrLayoutVisibility_Hidden;
    s.word(s.add(s.part(table, PrLayoutDisplay_TableRow), cell), 30, 30);
    s.layout(100, 60);
    std::vector<uint8_t> pixels = render(s.f.tree, 100, 60);
    CHECK(pixel(pixels, 100, 2, 2) == "0 0 0 0");
}

TEST_CASE("a column is never hit, and its cells are") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    s.part(table, PrLayoutDisplay_TableColumn);
    PrLayoutNode a = s.cell(s.part(table, PrLayoutDisplay_TableRow), 30, 30);
    s.layout();
    CHECK(s.f.hit(PrVec2{15, 15}) != kPrLayoutNodeNone);
    CHECK(s.f.hit(PrVec2{15, 15}) != prLayoutNodeFirstChild(s.f.tree, table));
    (void)a;
}

// ---------------------------------------------------------------------------------------------------------------------
// What a layout costs

TEST_CASE("a change in one cell lays out that cell and the table, and the other cells are reused") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    std::vector<PrLayoutNode> words;
    for (uint32_t r = 0; r < 3; ++r) {
        PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
        for (uint32_t c = 0; c < 2; ++c)
            words.push_back(s.word(s.part(row, PrLayoutDisplay_TableCell), 20, 10));
    }
    s.layout();
    CHECK(s.stats().tablesLaidOut == 1);
    s.layout();
    CHECK(s.stats().tablesLaidOut == 0);
    // A taller word in one cell changes its row's height, not its column's width.
    Styled taller;
    taller.box = block(30);
    taller.box.display = PrLayoutDisplay_InlineBlock;
    taller.box.width = prLayoutLengthPx(20);
    prLayoutNodeSetStyle(s.f.tree, words[2], s.style(taller));
    s.layout();
    PrLayoutTreeStats stats = s.stats();
    CHECK(stats.tablesLaidOut == 1);
    CHECK(stats.measuresComputed == 1);
    CHECK(stats.measuresReused == 5);
    CHECK(s.height(prLayoutNodeParent(s.f.tree, words[3])) == 30.0f);
}

TEST_CASE("a change of a column's width or a cell's span lays the table out again") {
    Stage s;
    PrLayoutNode table = s.add(s.root, spacedTable(0));
    Styled column = Stage::of(PrLayoutDisplay_TableColumn);
    PrLayoutNode col = s.add(table, column);
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row, 20, 10);
    PrLayoutNode b = s.cell(row, 20, 10);
    s.layout();
    column.box.width = prLayoutLengthPx(50);
    prLayoutNodeSetStyle(s.f.tree, col, s.style(column));
    s.layout();
    checkRect(s.at(a), 0, 0, 50, 10);
    // The second column the cell spans is merged into the first: no column box and no other cell defines it
    // (css-tables-3/#dimensioning-the-row-column-grid--step2).
    Styled spanning = Stage::of(PrLayoutDisplay_TableCell);
    spanning.table.columnSpan = 2;
    prLayoutNodeSetStyle(s.f.tree, a, s.style(spanning));
    s.layout();
    CHECK(s.width(a) == 50.0f);
    CHECK(s.left(b) == 50.0f);
}

TEST_CASE("a change of a cell's border lays out its neighbours in a collapsing table") {
    Stage s;
    Styled styled = spacedTable(0);
    styled.table.borderCollapse = PrLayoutBorderCollapse_Collapse;
    PrLayoutNode table = s.add(s.root, styled);
    PrLayoutNode row = s.part(table, PrLayoutDisplay_TableRow);
    PrLayoutNode a = s.cell(row, 20, 10);
    PrLayoutNode b = s.cell(row, 20, 10);
    s.layout();
    Styled bordered = Stage::of(PrLayoutDisplay_TableCell);
    setBorder(bordered, 6, PrColor{1, 0, 0, 1});
    prLayoutNodeSetStyle(s.f.tree, a, s.style(bordered));
    s.layout();
    // The cell's 6px border wins at the table's edge, half outside the cell, and between the cells, half in each.
    CHECK(s.left(b) == float(3 + 3 + 20 + 3));
    CHECK(s.width(b) == float(3 + 20));
}

