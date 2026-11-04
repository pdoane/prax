// Table layout: a table's grid of rows and columns, sized from its cells, with its captions.

#include "layout/src/layout_table.h"

#include "pr/alloc.h"
#include "pr/float.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_position.h"
#include "layout/src/layout_sizing.h"

namespace pr_impl {

// A cell of a table being laid out: the box of a cell, or an anonymous cell around a run of other content. Sizes are of
// its border box.
struct LayoutTableCell {
    LayoutNodeIndex node;           // its box, or the first node of an anonymous cell's run
    LayoutNodeIndex runLast;        // an anonymous cell: the last node of its run; otherwise kPrLayoutNodeNone
    uint32_t row;                   // its row, among the table's rows in tree order
    uint32_t gridRow;               // its first row in the grid
    uint32_t column;                // its first column
    uint32_t rowSpan;
    uint32_t columnSpan;
    LayoutCellAlign align;
    bool fixedWidth;                // true if its width is a length, which constrains a column that it alone spans
    bool orthogonal;                // true if its inline axis is the table's block axis

    // Its outer min-content and max-content widths, and its percentage contribution as a fraction
    // (css-tables-3/#computing-cell-measures).
    PrLayoutUnit minContent;
    PrLayoutUnit maxContent;
    double percent;
    PrLayoutUnit width;             // the width its style gives, or zero

    // Its used borders if the table collapses its borders, by physical side.
    PrLayoutUnit border[PrRectSide_Count];

    PrLayoutUnit inlineSize;                    // the width of its columns
    PrLayoutUnit measured;                      // the block size of its content at that width
    PrLayoutUnit styledBlock;                   // the block size its style gives, or zero
    double blockPercent;                        // its block size as a fraction of the table's, or zero
    PrLayoutUnit baseline;                      // its first baseline from its block-start edge, or its content box's
                                                // block-end edge
    PrLayoutUnit blockEdgeEnd;                  // its block-end padding and border
    const LayoutFragment* PR_NULLABLE fragment; // a reference that the table holds
};

// A column of a table's grid, and the column and column group boxes that define it, if any.
struct LayoutTableColumn {
    LayoutNodeIndex node;
    LayoutNodeIndex group;
    PrLayoutUnit minContent;
    PrLayoutUnit maxContent;
    PrLayoutUnit spanningMax;       // its max-content width from the cells that span it alone
    double percent;
    bool constrained;               // true if its column box, column group or lone-spanning cell has a length width
    bool origin;                    // true if a cell starts in it
    bool collapsed;                 // true if the visibility of its column box or column group is collapse
    PrLayoutUnit offset;            // the offset of its inline-start edge in the grid's border box
    PrLayoutUnit size;
};

// A row of a table: the box of a row, or the anonymous row around cells that are children of a table or a row group.
struct LayoutTableRow {
    LayoutNodeIndex node;           // its box, or kPrLayoutNodeNone
    uint32_t group;                 // its row group, among the table's row groups in tree order
    uint32_t firstCell;             // its cells, among the table's cells in tree order
    uint32_t cellCount;
    uint32_t grid;                  // its index in the grid
    PrLayoutUnit styledBlock;       // the block size its style gives, or zero
    double percent;                 // its block size as a fraction of the table's, or zero
    PrLayoutUnit blockSize;
    PrLayoutUnit ascent;            // the most any cell aligned by its baseline has above that baseline
    PrLayoutUnit descent;
    bool baselines;                 // true if a cell in it is aligned by its baseline
    bool collapsed;                 // true if the visibility of its row or row group is collapse
    PrLayoutUnit offset;            // the offset of its block-start edge in the grid's border box
};

// A row group of a table: the box of a group, or the anonymous group of rows that are children of the table.
struct LayoutTableGroup {
    LayoutNodeIndex node;           // its box, or kPrLayoutNodeNone
    PrLayoutDisplay display;
    uint32_t firstRow;              // its rows, among the table's rows in tree order
    uint32_t rowCount;
};

// A column or column group box of a table, which has a fragment of its own.
struct LayoutTableColumnBox {
    LayoutNodeIndex node;
    uint32_t group;                 // the column group box that holds it, among the boxes, or kNone
    uint32_t firstColumn;
    uint32_t columnCount;
};

// A caption of a table, or a child out of flow of a part with no fragment, whose place the table holds.
struct LayoutTableAside {
    LayoutNodeIndex node;
    LayoutNodeIndex container;      // an out-of-flow child: the part whose fragment holds its placeholder, or the table
};

// The border that a collapsing table resolves at one segment of a line of its grid (css-tables-3/#border-collapsing).
struct LayoutTableBorder {
    PrLayoutUnit width;
    PrDrawBorderStyle style;
    PrColor16 color;
};

// Scratch memory that table layout keeps between layouts: the parts of the tables being laid out, innermost last.
struct LayoutTableScratch {
    RawArray<LayoutTableCell> cells;
    RawArray<LayoutTableColumn> columns;
    RawArray<LayoutTableRow> rows;
    RawArray<LayoutTableGroup> groups;
    RawArray<LayoutTableColumnBox> boxes;
    RawArray<LayoutTableAside> captions;
    RawArray<LayoutTableAside> outOfFlow;
    RawArray<uint32_t> order;          // the rows in grid order, then the busy columns while cells are placed
    RawArray<LayoutTableBorder> borders;
    RawArray<LayoutGridBorder> segments;
};

namespace {

constexpr uint32_t kNone = 0xffffffffu;

// The largest size a table gives a column, so that sums over many columns do not saturate.
constexpr int32_t kColumnMax = 1 << 28;

// Returns the unit nearest to `raw` units.
PrLayoutUnit unitOf(double raw) {
    return prLayoutUnitClamped(int64_t(prF64Round(raw)));
}

// Returns the box style of the table part `node`, or that of an anonymous cell for kPrLayoutNodeNone.
const PrLayoutBoxStyle& boxOf(const LayoutTree* tree, LayoutNodeIndex node) {
    if (node != kPrLayoutNodeNone)
        return layoutBoxStyle(tree->styles, layoutTreeNode(tree, node).style);
    return layoutAnonymousCellStyle();
}

// ---------------------------------------------------------------------------------------------------------------------
// Tables

// One layout or measurement of a table: the table, its mode and style, and its parts in the scratch memory.
struct Table {
    LayoutState& state;
    LayoutNodeIndex node;           // the table's box, or kPrLayoutNodeNone for an anonymous table
    LayoutNodeIndex first;          // the first and last of its children, or of the run that an anonymous table wraps
    LayoutNodeIndex last;
    const PrLayoutBoxStyle* boxStyle;
    LayoutMode mode;
    PrLayoutTableStyle style;
    bool collapsed;                 // true if its borders collapse
    bool fixed;                     // true if it is laid out in fixed mode (css-tables-3/#table-layout-property)
    PrLayoutUnit inlineSpacing;     // the border spacing between columns, zero if its borders collapse
    PrLayoutUnit blockSpacing;      // the border spacing between rows, zero if its borders collapse
    LayoutEdges edges;              // its grid's padding and borders, collapsed if its borders collapse
    LayoutIntrinsicInput cellInput; // what its cells are measured in

    uint32_t cellMark, cellCount;
    uint32_t columnMark, columnCount;
    uint32_t rowMark, rowCount;
    uint32_t groupMark, groupCount;
    uint32_t boxMark, boxCount;
    uint32_t captionMark, captionCount;
    uint32_t outOfFlowMark, outOfFlowCount;
    uint32_t orderMark;
    uint32_t borderMark;

    LayoutTableScratch& scratch() const { return *state.scratch->table; }
    LayoutTableCell& cell(uint32_t i) const { return scratch().cells.data[cellMark + i]; }
    LayoutTableColumn& column(uint32_t i) const { return scratch().columns.data[columnMark + i]; }
    LayoutTableRow& row(uint32_t i) const { return scratch().rows.data[rowMark + i]; }
    LayoutTableGroup& group(uint32_t i) const { return scratch().groups.data[groupMark + i]; }
    LayoutTableColumnBox& box(uint32_t i) const { return scratch().boxes.data[boxMark + i]; }
    LayoutTableAside& caption(uint32_t i) const { return scratch().captions.data[captionMark + i]; }
    LayoutTableAside& outOfFlow(uint32_t i) const { return scratch().outOfFlow.data[outOfFlowMark + i]; }

    // The row at `grid` in the grid.
    LayoutTableRow& gridRow(uint32_t grid) const { return row(scratch().order.data[orderMark + grid]); }

    // The border at a segment of a line of the grid, if its borders collapse. A line across the inline axis, at the
    // block-start edge of row `line`, has a segment in each column, and a line along the block axis, at the
    // inline-start edge of column `line`, has a segment in each row.
    LayoutTableBorder& acrossBorder(uint32_t line, uint32_t column) const {
        return scratch().borders.data[borderMark + line * columnCount + column];
    }
    LayoutTableBorder& alongBorder(uint32_t line, uint32_t row) const {
        return scratch().borders.data[borderMark + (rowCount + 1) * columnCount + line * rowCount + row];
    }
};

// Starts the layout of the table `node` at the end of the scratch memory, in the table's own mode. If `runLast` is not
// kPrLayoutNodeNone, the table is the anonymous table that wraps the run of siblings from `node` through `runLast`. An
// anonymous table has the initial style, except for the border spacing and the border model, which it inherits from its
// parent (css-tables-3/#fixup-boxes).
Table beginTable(LayoutState& state, LayoutNodeIndex node, LayoutNodeIndex runLast) {
    if (state.scratch->table == nullptr)
        state.scratch->table = layoutTableScratchCreate();
    LayoutTableScratch& s = *state.scratch->table;
    const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
    bool anonymous = runLast != kPrLayoutNodeNone;
    LayoutNodeIndex parent = anonymous ? n.parent : node;
    PrLayoutTableStyle style = layoutTableStyle(state.styles, layoutTreeNode(state.tree, parent).style);
    if (anonymous) {
        PrLayoutTableStyle inherited = prLayoutTableStyleDefault();
        inherited.borderSpacing[0] = style.borderSpacing[0];
        inherited.borderSpacing[1] = style.borderSpacing[1];
        inherited.borderCollapse = style.borderCollapse;
        style = inherited;
    }
    Table t{
        .state = state,
        .node = anonymous ? kPrLayoutNodeNone : node,
        .first = anonymous ? node : n.first,
        .last = anonymous ? runLast : n.last,
        .boxStyle = anonymous ? &layoutAnonymousTableStyle() : &layoutBoxStyle(state.styles, n.style),
        .mode = layoutOwnMode(state.tree, parent),
        .style = style,
        .collapsed = false,
        .fixed = false,
        .inlineSpacing = kLayoutUnitZero,
        .blockSpacing = kLayoutUnitZero,
        .edges = {},
        .cellInput = {},
        .cellMark = s.cells.count,
        .cellCount = 0,
        .columnMark = s.columns.count,
        .columnCount = 0,
        .rowMark = s.rows.count,
        .rowCount = 0,
        .groupMark = s.groups.count,
        .groupCount = 0,
        .boxMark = s.boxes.count,
        .boxCount = 0,
        .captionMark = s.captions.count,
        .captionCount = 0,
        .outOfFlowMark = s.outOfFlow.count,
        .outOfFlowCount = 0,
        .orderMark = s.order.count,
        .borderMark = s.borders.count,
    };
    t.collapsed = t.style.borderCollapse == PrLayoutBorderCollapse_Collapse;
    // Border spacing is physical: its first length is horizontal (css-tables-3/#border-spacing-property).
    if (!t.collapsed) {
        bool horizontal = layoutIsHorizontal(t.mode);
        t.inlineSpacing = t.style.borderSpacing[horizontal ? 0 : 1];
        t.blockSpacing = t.style.borderSpacing[horizontal ? 1 : 0];
    }
    // A table is laid out in fixed mode if its table-layout is fixed and its width is neither auto nor max-content.
    PrLayoutLength width = layoutLogicalBox(*t.boxStyle, t.mode).inlineSize;
    t.fixed = t.style.tableLayout == PrLayoutTableLayout_Fixed && !layoutLengthIsAuto(width) &&
        width.bits != prLayoutLengthMaxContent().bits;
    return t;
}

// Releases the fragments that the cells hold, and frees the table's scratch memory.
void endTable(Table& t) {
    LayoutTableScratch& s = t.scratch();
    for (uint32_t i = 0; i < t.cellCount; ++i)
        layoutFragmentRelease(t.cell(i).fragment);
    s.cells.count = t.cellMark;
    s.columns.count = t.columnMark;
    s.rows.count = t.rowMark;
    s.groups.count = t.groupMark;
    s.boxes.count = t.boxMark;
    s.captions.count = t.captionMark;
    s.outOfFlow.count = t.outOfFlowMark;
    s.order.count = t.orderMark;
    s.borders.count = t.borderMark;
}

// Returns the inline spacing across `count` visible columns of the table, or the block spacing across as many rows: one
// more gap than tracks, or none if there are no tracks. A collapsed track takes no room, and no spacing
// (css-tables-3/#visible-track).
PrLayoutUnit spacingAcross(PrLayoutUnit spacing, uint32_t count) {
    return count != 0 ? spacing * int32_t(count + 1) : kLayoutUnitZero;
}

// Returns the number of the table's columns from `first`, `count` of them, that are not collapsed.
uint32_t visibleColumns(const Table& t, uint32_t first, uint32_t count) {
    uint32_t visible = 0;
    for (uint32_t c = first; c < first + count; ++c)
        visible += t.column(c).collapsed ? 0 : 1;
    return visible;
}

// Returns the number of the rows of the grid from `first`, `count` of them, that are not collapsed.
uint32_t visibleRows(const Table& t, uint32_t first, uint32_t count) {
    uint32_t visible = 0;
    for (uint32_t r = first; r < first + count; ++r)
        visible += t.gridRow(r).collapsed ? 0 : 1;
    return visible;
}

// ---------------------------------------------------------------------------------------------------------------------
// The grid

// Collects the parts of a table: its captions, column boxes, row groups, rows, cells and children out of flow, in tree
// order, with the anonymous parts that fix up its structure (css-tables-3/#fixup-algorithm). Clears the marks of the
// parts that the table lays out itself, which no cache of their own clears.
struct Collector {
    Table& t;
    const LayoutTree* tree;
    uint32_t group = kNone;         // the open row group, or kNone
    uint32_t row = kNone;           // the open row, or kNone
    bool implicitGroup = false;     // true if the open row group is anonymous
    bool implicitRow = false;       // true if the open row is anonymous

    void clearMarks(LayoutNodeIndex node) {
        if (t.state.reuse)
            t.state.tree->nodes.data[node].dirty = 0;
    }

    void openGroup(LayoutNodeIndex node, PrLayoutDisplay display) {
        LayoutTableScratch& s = t.scratch();
        group = t.groupCount++;
        s.groups.push(LayoutTableGroup{.node = node, .display = display, .firstRow = t.rowCount, .rowCount = 0});
        implicitGroup = node == kPrLayoutNodeNone;
    }

    // Returns true if the box `node` is collapsed, or false for kPrLayoutNodeNone.
    bool collapses(LayoutNodeIndex node) const {
        return node != kPrLayoutNodeNone && layoutBoxStyle(tree->styles, layoutTreeNode(tree, node).style).collapsed;
    }

    void openRow(LayoutNodeIndex node) {
        if (group == kNone)
            openGroup(kPrLayoutNodeNone, PrLayoutDisplay_TableRowGroup);
        LayoutTableScratch& s = t.scratch();
        row = t.rowCount++;
        s.rows.push(LayoutTableRow{
            .node = node,
            .group = group,
            .firstCell = t.cellCount,
            .collapsed = collapses(node) || collapses(t.group(group).node),
        });
        t.group(group).rowCount += 1;
        implicitRow = node == kPrLayoutNodeNone;
    }

    void addCell(LayoutNodeIndex node, LayoutNodeIndex runLast) {
        if (row == kNone)
            openRow(kPrLayoutNodeNone);
        LayoutTableScratch& s = t.scratch();
        LayoutTableCell cell{};
        cell.node = node;
        cell.runLast = runLast;
        cell.row = row;
        cell.rowSpan = 1;
        cell.columnSpan = 1;
        if (runLast == kPrLayoutNodeNone) {
            const PrLayoutTableStyle& table = layoutTableStyle(tree->styles, layoutTreeNode(tree, node).style);
            cell.rowSpan = table.rowSpan;
            cell.columnSpan = table.columnSpan;
        }
        cell.align = layoutCellAlignFromBox(boxOf(tree, runLast == kPrLayoutNodeNone ? node : kPrLayoutNodeNone));
        s.cells.push(cell);
        ++t.cellCount;
        t.row(row).cellCount += 1;
    }

    void addOutOfFlow(LayoutNodeIndex node, LayoutNodeIndex container) {
        t.scratch().outOfFlow.push(LayoutTableAside{node, container});
        ++t.outOfFlowCount;
    }

    // Adds a run of other content, from `first` through `last`, as an anonymous cell, unless it is white space alone.
    // The children out of flow of a blank run hold their places at the start of `container`.
    void addRun(LayoutNodeIndex first, LayoutNodeIndex last, LayoutNodeIndex container) {
        if (!layoutTableRunIsBlank(tree, first, last)) {
            addCell(first, last);
            return;
        }
        for (uint32_t c = first;; c = layoutTreeNode(tree, c).next) {
            if (layoutChildKind(tree, c) == LayoutChildKind_OutOfFlow)
                addOutOfFlow(c, container);
            if (c == last)
                break;
        }
    }

    // Collects the children of the table container `node`, whose display is `display`.
    void children(LayoutNodeIndex node, PrLayoutDisplay display) {
        const LayoutTreeNode& n = layoutTreeNode(tree, node);
        if (n.first != kPrLayoutNodeNone)
            childrenIn(node, display, n.first, n.last);
    }

    // Collects the siblings from `first` through `last`. They are children of the table container `node`, whose display
    // is `display`, or of an anonymous table if `node` is kPrLayoutNodeNone.
    void childrenIn(LayoutNodeIndex node, PrLayoutDisplay display, LayoutNodeIndex first, LayoutNodeIndex last) {
        LayoutNodeIndex runFirst = kPrLayoutNodeNone;
        LayoutNodeIndex runLast = kPrLayoutNodeNone;
        LayoutNodeIndex end = layoutTreeNode(tree, last).next;
        for (uint32_t c = first; c != end; c = layoutTreeNode(tree, c).next) {
            LayoutTablePart part = layoutTablePartFromDisplay(tree, display, c);
            bool extends = runFirst != kPrLayoutNodeNone &&
                (part == LayoutTablePart_None || part == LayoutTablePart_OutOfFlow);
            if (part == LayoutTablePart_Other || extends) {
                if (runFirst == kPrLayoutNodeNone)
                    runFirst = c;
                runLast = c;
                continue;
            }
            if (runFirst != kPrLayoutNodeNone) {
                addRun(runFirst, runLast, node);
                runFirst = kPrLayoutNodeNone;
            }
            // A caption or a column ends the anonymous row of the cells and other content before it.
            if (implicitRow && (part == LayoutTablePart_Caption || part == LayoutTablePart_ColumnGroup ||
                    part == LayoutTablePart_Column))
                row = kNone;
            switch (part) {
                case LayoutTablePart_Caption:
                    t.scratch().captions.push(LayoutTableAside{c, node});
                    ++t.captionCount;
                    break;
                case LayoutTablePart_ColumnGroup: columnGroup(c); break;
                case LayoutTablePart_Column: column(c, kNone); break;
                case LayoutTablePart_RowGroup: {
                    clearMarks(c);
                    row = kNone;
                    openGroup(c, layoutBoxStyle(tree->styles, layoutTreeNode(tree, c).style).display);
                    children(c, t.group(group).display);
                    group = kNone;
                    row = kNone;
                    break;
                }
                case LayoutTablePart_Row:
                    // A row that is a child of the table joins the anonymous group of the rows around it.
                    clearMarks(c);
                    openRow(c);
                    children(c, PrLayoutDisplay_TableRow);
                    row = kNone;
                    break;
                case LayoutTablePart_Cell:
                    // A cell that is a child of a table or row group is in the anonymous row around the cells and other
                    // content beside it.
                    if (display != PrLayoutDisplay_TableRow && !implicitRow)
                        row = kNone;
                    addCell(c, kPrLayoutNodeNone);
                    break;
                case LayoutTablePart_OutOfFlow: addOutOfFlow(c, node); break;
                default: break;
            }
        }
        if (runFirst != kPrLayoutNodeNone)
            addRun(runFirst, runLast, node);
    }

    // Adds the columns of the column group `node`: its columns, or, if it has none, as many as it spans. Its other
    // children generate no boxes (css-tables-3/#fixup-algorithm).
    void columnGroup(LayoutNodeIndex node) {
        clearMarks(node);
        LayoutTableScratch& s = t.scratch();
        uint32_t box = t.boxCount++;
        uint32_t first = t.columnCount;
        s.boxes.push(LayoutTableColumnBox{.node = node, .group = kNone, .firstColumn = first});
        bool any = false;
        const LayoutTreeNode& n = layoutTreeNode(tree, node);
        for (uint32_t c = n.first; c != kPrLayoutNodeNone; c = layoutTreeNode(tree, c).next) {
            if (layoutTablePartFromDisplay(tree, PrLayoutDisplay_TableColumnGroup, c) != LayoutTablePart_Column)
                continue;
            column(c, box);
            any = true;
        }
        if (!any) {
            uint32_t span = layoutTableStyle(tree->styles, n.style).columnSpan;
            for (uint32_t i = 0; i < span; ++i)
                addColumn(kPrLayoutNodeNone, node);
        }
        t.box(box).columnCount = t.columnCount - first;
    }

    // Adds the columns of the column box `node`, in the column group box `group` if it is not kNone.
    void column(LayoutNodeIndex node, uint32_t groupBox) {
        clearMarks(node);
        LayoutTableScratch& s = t.scratch();
        uint32_t span = layoutTableStyle(tree->styles, layoutTreeNode(tree, node).style).columnSpan;
        s.boxes.push(LayoutTableColumnBox{
            .node = node,
            .group = groupBox,
            .firstColumn = t.columnCount,
            .columnCount = span,
        });
        ++t.boxCount;
        LayoutNodeIndex groupNode = groupBox != kNone ? t.box(groupBox).node : kPrLayoutNodeNone;
        for (uint32_t i = 0; i < span; ++i)
            addColumn(node, groupNode);
    }

    void addColumn(LayoutNodeIndex node, LayoutNodeIndex groupNode) {
        t.scratch().columns.push(
            LayoutTableColumn{.node = node, .group = groupNode, .collapsed = collapses(node) || collapses(groupNode)});
        ++t.columnCount;
    }
};

// Orders the rows of the table into its grid: the first header group's rows first, the first footer group's rows last,
// and the other groups' rows in tree order between (css-tables-3/#table-structure).
void orderRows(Table& t) {
    uint32_t header = kNone;
    uint32_t footer = kNone;
    for (uint32_t g = 0; g < t.groupCount; ++g) {
        PrLayoutDisplay display = t.group(g).display;
        if (display == PrLayoutDisplay_TableHeaderGroup && header == kNone)
            header = g;
        else if (display == PrLayoutDisplay_TableFooterGroup && footer == kNone)
            footer = g;
    }
    RawArray<uint32_t>& order = t.scratch().order;
    auto add = [&](uint32_t g) {
        const LayoutTableGroup& group = t.group(g);
        for (uint32_t r = group.firstRow; r < group.firstRow + group.rowCount; ++r) {
            t.row(r).grid = order.count - t.orderMark;
            order.push(r);
        }
    };
    if (header != kNone)
        add(header);
    for (uint32_t g = 0; g < t.groupCount; ++g) {
        if (g != header && g != footer)
            add(g);
    }
    if (footer != kNone)
        add(footer);
}

// Places each cell at the next slot of its row that no cell above it spans, by the HTML algorithm
// (css-tables-3/#dimensioning-the-row-column-grid). A row span of zero, or one past its group's last row, ends at that
// row. Then adds the columns that the cells reach past the column boxes.
void placeCells(Table& t) {
    // For each column, the first grid row below the cells placed so far that span it, in the rows' order.
    RawArray<uint32_t>& busy = t.scratch().order;
    uint32_t busyMark = busy.count;
    for (uint32_t grid = 0; grid < t.rowCount; ++grid) {
        const LayoutTableRow& row = t.gridRow(grid);
        const LayoutTableGroup& group = t.group(row.group);
        uint32_t groupEnd = t.row(group.firstRow + group.rowCount - 1).grid + 1;
        uint32_t column = 0;
        for (uint32_t i = row.firstCell; i < row.firstCell + row.cellCount; ++i) {
            LayoutTableCell& cell = t.cell(i);
            while (busyMark + column < busy.count && busy.data[busyMark + column] > grid)
                ++column;
            uint32_t left = groupEnd - grid;
            cell.rowSpan = cell.rowSpan == 0 || cell.rowSpan > left ? left : cell.rowSpan;
            cell.gridRow = grid;
            cell.column = column;
            while (busy.count < busyMark + column + cell.columnSpan)
                busy.push(0);
            for (uint32_t c = column; c < column + cell.columnSpan; ++c)
                busy.data[busyMark + c] = grid + cell.rowSpan;
            column += cell.columnSpan;
        }
    }
    uint32_t reached = busy.count - busyMark;
    busy.count = busyMark;
    while (t.columnCount < reached) {
        t.scratch().columns.push(LayoutTableColumn{.node = kPrLayoutNodeNone, .group = kPrLayoutNodeNone});
        ++t.columnCount;
    }
}

// Merges each column that no column box defines into the column before it if the same cells span both
// (css-tables-3/#dimensioning-the-row-column-grid--step2). Only a table in auto mode merges its columns.
void mergeColumns(Table& t) {
    if (t.fixed || t.columnCount < 2)
        return;
    // starts[c] is set if a cell starts in column c, and ends[c] if a cell ends in it.
    RawArray<uint32_t>& flags = t.scratch().order;
    uint32_t mark = flags.count;
    for (uint32_t c = 0; c < t.columnCount; ++c)
        flags.push(0);
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        const LayoutTableCell& cell = t.cell(i);
        flags.data[mark + cell.column] |= 1;
        flags.data[mark + cell.column + cell.columnSpan - 1] |= 2;
    }
    // Each column's index once merged, in place of its flags. A column that merges takes the index of the one before.
    uint32_t next = 0;
    uint32_t previous = 0;
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        bool merges = c != 0 && t.column(c).node == kPrLayoutNodeNone && t.column(c).group == kPrLayoutNodeNone &&
            (flags.data[mark + c] & 1) == 0 && (previous & 2) == 0;
        previous = flags.data[mark + c];
        if (!merges)
            t.column(next) = t.column(c);
        flags.data[mark + c] = merges ? next - 1 : next++;
    }
    if (next != t.columnCount) {
        for (uint32_t i = 0; i < t.cellCount; ++i) {
            LayoutTableCell& cell = t.cell(i);
            uint32_t first = flags.data[mark + cell.column];
            uint32_t last = flags.data[mark + cell.column + cell.columnSpan - 1];
            cell.column = first;
            cell.columnSpan = last - first + 1;
        }
        t.scratch().columns.count = t.columnMark + next;
        t.columnCount = next;
    }
    flags.count = mark;
}

// Collects the parts of the table and places its cells in its grid.
void buildGrid(Table& t) {
    Collector collector{t, t.state.tree};
    if (t.first != kPrLayoutNodeNone)
        collector.childrenIn(t.node, t.boxStyle->display, t.first, t.last);
    orderRows(t);
    placeCells(t);
    mergeColumns(t);
    for (uint32_t i = 0; i < t.cellCount; ++i)
        t.column(t.cell(i).column).origin = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Collapsed borders

// Returns the precedence of a border style among collapsed borders: hidden wins over everything, and then the style
// that comes first in the order of css-tables-3/#border-specificity.
int32_t stylePrecedence(PrDrawBorderStyle style) {
    switch (style) {
        case PrDrawBorderStyle_Double: return 8;
        case PrDrawBorderStyle_Solid: return 7;
        case PrDrawBorderStyle_Dashed: return 6;
        case PrDrawBorderStyle_Dotted: return 5;
        case PrDrawBorderStyle_Ridge: return 4;
        case PrDrawBorderStyle_Outset: return 3;
        case PrDrawBorderStyle_Groove: return 2;
        case PrDrawBorderStyle_Inset: return 1;
        default: return 0;
    }
}

// A border that takes part in resolving a segment of a collapsing table's grid lines.
struct Candidate {
    PrLayoutUnit width;
    PrDrawBorderStyle style;
    PrColor16 color;
    bool hidden;
};

// Resolves the borders that meet at one segment of a grid line, considered in order of precedence: the more specific
// border wins, and an earlier one wins a tie (css-tables-3/#border-conflict-resolution-algorithm). A hidden border
// hides every other.
struct Resolution {
    Candidate winner{};
    bool any = false;
    bool hidden = false;

    // Considers the border on physical side `side` of the box `node`, if it has one.
    void consider(const LayoutTree* tree, LayoutNodeIndex node, PrRectSide side) {
        if (node == kPrLayoutNodeNone || hidden)
            return;
        PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
        const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(tree->styles, style);
        PrDrawBorderStyle borderStyle = decoration.borderStyle[side];
        if (borderStyle == PrDrawBorderStyle_Hidden) {
            hidden = true;
            winner = Candidate{.hidden = true};
            return;
        }
        if (borderStyle == PrDrawBorderStyle_None)
            return;
        Candidate c{
            .width = layoutBoxStyle(tree->styles, style).border[side],
            .style = borderStyle,
            .color = decoration.borderColor[side],
        };
        if (!any || c.width > winner.width ||
                (c.width == winner.width && stylePrecedence(c.style) > stylePrecedence(winner.style)))
            winner = c;
        any = true;
    }

    LayoutTableBorder result() const {
        if (!any || hidden)
            return LayoutTableBorder{.width = kLayoutUnitZero, .style = PrDrawBorderStyle_None};
        // In a collapsing table, inset is drawn as ridge and outset as groove (css-tables-3/#border-style-overrides).
        PrDrawBorderStyle style = winner.style;
        if (winner.style == PrDrawBorderStyle_Inset)
            style = PrDrawBorderStyle_Ridge;
        else if (winner.style == PrDrawBorderStyle_Outset)
            style = PrDrawBorderStyle_Groove;
        return LayoutTableBorder{.width = winner.width, .style = style, .color = winner.color};
    }
};

// Returns the cell that covers the grid slot at `row` and `column`, or kNone. `slots` holds the cells by slot.
uint32_t slotCell(const Table& t, const RawArray<uint32_t>& slots, uint32_t mark, uint32_t row, uint32_t column) {
    return slots.data[mark + row * t.columnCount + column];
}

// Resolves the borders of each segment of the grid lines of a collapsing table, and from them the used borders of its
// cells and its own (css-tables-3/#border-conflict-resolution-algorithm). A cell's border on a side is half of the
// widest segment along that side, and a line's width is split with its greater half toward the start of its axis, so
// that the two cells that share it sum to its width. The table's border on a side is the outer half of the widest
// segment of its outermost line on that side, and its padding is ignored (css-tables-3/#collapsed-style-overrides).
void resolveBorders(Table& t) {
    const LayoutTree* tree = t.state.tree;
    LayoutTableScratch& s = t.scratch();
    // The cells by slot, in the busy array's place.
    RawArray<uint32_t>& slots = s.order;
    uint32_t mark = slots.count;
    for (uint32_t i = 0; i < t.rowCount * t.columnCount; ++i)
        slots.push(kNone);
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        const LayoutTableCell& cell = t.cell(i);
        for (uint32_t r = cell.gridRow; r < cell.gridRow + cell.rowSpan; ++r) {
            for (uint32_t c = cell.column; c < cell.column + cell.columnSpan; ++c)
                slots.data[mark + r * t.columnCount + c] = i;
        }
    }
    uint32_t count = (t.rowCount + 1) * t.columnCount + (t.columnCount + 1) * t.rowCount;
    for (uint32_t i = 0; i < count; ++i)
        s.borders.push(LayoutTableBorder{});
    PrRectSide blockStart = layoutPhysicalSide(t.mode, LayoutSide_BlockStart);
    PrRectSide blockEnd = layoutPhysicalSide(t.mode, LayoutSide_BlockEnd);
    PrRectSide inlineStart = layoutPhysicalSide(t.mode, LayoutSide_InlineStart);
    PrRectSide inlineEnd = layoutPhysicalSide(t.mode, LayoutSide_InlineEnd);
    auto cellNode = [&](uint32_t i) {
        return i == kNone || t.cell(i).runLast != kPrLayoutNodeNone ? kPrLayoutNodeNone : t.cell(i).node;
    };
    // The lines across the inline axis, at the block-start edge of each row and the block-end edge of the last.
    for (uint32_t line = 0; line <= t.rowCount; ++line) {
        for (uint32_t c = 0; c < t.columnCount; ++c) {
            Resolution r;
            uint32_t before = line != 0 ? slotCell(t, slots, mark, line - 1, c) : kNone;
            uint32_t after = line != t.rowCount ? slotCell(t, slots, mark, line, c) : kNone;
            if (before == after)
                before = kNone;
            if (before != kNone)
                r.consider(tree, cellNode(before), blockEnd);
            if (after != kNone)
                r.consider(tree, cellNode(after), blockStart);
            const LayoutTableRow* rowBefore = line != 0 ? &t.gridRow(line - 1) : nullptr;
            const LayoutTableRow* rowAfter = line != t.rowCount ? &t.gridRow(line) : nullptr;
            if (rowBefore != nullptr)
                r.consider(tree, rowBefore->node, blockEnd);
            if (rowAfter != nullptr)
                r.consider(tree, rowAfter->node, blockStart);
            if (rowBefore != nullptr && (rowAfter == nullptr || rowAfter->group != rowBefore->group))
                r.consider(tree, t.group(rowBefore->group).node, blockEnd);
            if (rowAfter != nullptr && (rowBefore == nullptr || rowAfter->group != rowBefore->group))
                r.consider(tree, t.group(rowAfter->group).node, blockStart);
            if (line == 0 || line == t.rowCount) {
                PrRectSide side = line == 0 ? blockStart : blockEnd;
                r.consider(tree, t.column(c).node, side);
                r.consider(tree, t.column(c).group, side);
                r.consider(tree, t.node, side);
            }
            t.acrossBorder(line, c) = r.result();
        }
    }
    // The lines along the block axis, at the inline-start edge of each column and the inline-end edge of the last.
    for (uint32_t line = 0; line <= t.columnCount; ++line) {
        for (uint32_t row = 0; row < t.rowCount; ++row) {
            Resolution r;
            uint32_t before = line != 0 ? slotCell(t, slots, mark, row, line - 1) : kNone;
            uint32_t after = line != t.columnCount ? slotCell(t, slots, mark, row, line) : kNone;
            if (before == after)
                before = kNone;
            if (before != kNone)
                r.consider(tree, cellNode(before), inlineEnd);
            if (after != kNone)
                r.consider(tree, cellNode(after), inlineStart);
            const LayoutTableRow& gridRow = t.gridRow(row);
            if (line == 0 || line == t.columnCount) {
                PrRectSide side = line == 0 ? inlineStart : inlineEnd;
                r.consider(tree, gridRow.node, side);
                r.consider(tree, t.group(gridRow.group).node, side);
            }
            const LayoutTableColumn* columnBefore = line != 0 ? &t.column(line - 1) : nullptr;
            const LayoutTableColumn* columnAfter = line != t.columnCount ? &t.column(line) : nullptr;
            if (columnBefore != nullptr)
                r.consider(tree, columnBefore->node, inlineEnd);
            if (columnAfter != nullptr)
                r.consider(tree, columnAfter->node, inlineStart);
            if (columnBefore != nullptr && (columnAfter == nullptr || columnAfter->group != columnBefore->group))
                r.consider(tree, columnBefore->group, inlineEnd);
            if (columnAfter != nullptr && (columnBefore == nullptr || columnAfter->group != columnBefore->group))
                r.consider(tree, columnAfter->group, inlineStart);
            if (line == 0 || line == t.columnCount)
                r.consider(tree, t.node, line == 0 ? inlineStart : inlineEnd);
            t.alongBorder(line, row) = r.result();
        }
    }
    slots.count = mark;
    // The halves of a line's width toward its end and toward its start.
    auto endHalf = [](PrLayoutUnit width) { return layoutUnit(width.raw / 2); };
    auto startHalf = [&](PrLayoutUnit width) { return width - endHalf(width); };
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        LayoutTableCell& cell = t.cell(i);
        PrLayoutUnit widths[LayoutSide_Count] = {};
        for (uint32_t c = cell.column; c < cell.column + cell.columnSpan; ++c) {
            widths[LayoutSide_BlockStart] =
                layoutUnitMax(widths[LayoutSide_BlockStart], endHalf(t.acrossBorder(cell.gridRow, c).width));
            widths[LayoutSide_BlockEnd] = layoutUnitMax(
                widths[LayoutSide_BlockEnd], startHalf(t.acrossBorder(cell.gridRow + cell.rowSpan, c).width));
        }
        for (uint32_t row = cell.gridRow; row < cell.gridRow + cell.rowSpan; ++row) {
            widths[LayoutSide_InlineStart] =
                layoutUnitMax(widths[LayoutSide_InlineStart], endHalf(t.alongBorder(cell.column, row).width));
            widths[LayoutSide_InlineEnd] = layoutUnitMax(
                widths[LayoutSide_InlineEnd], startHalf(t.alongBorder(cell.column + cell.columnSpan, row).width));
        }
        for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
            cell.border[layoutPhysicalSide(t.mode, side)] = widths[side];
    }
    PrLayoutUnit outer[LayoutSide_Count] = {};
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        PrLayoutUnit start = startHalf(t.acrossBorder(0, c).width);
        PrLayoutUnit end = endHalf(t.acrossBorder(t.rowCount, c).width);
        outer[LayoutSide_BlockStart] = layoutUnitMax(outer[LayoutSide_BlockStart], start);
        outer[LayoutSide_BlockEnd] = layoutUnitMax(outer[LayoutSide_BlockEnd], end);
    }
    for (uint32_t row = 0; row < t.rowCount; ++row) {
        outer[LayoutSide_InlineStart] =
            layoutUnitMax(outer[LayoutSide_InlineStart], startHalf(t.alongBorder(0, row).width));
        outer[LayoutSide_InlineEnd] =
            layoutUnitMax(outer[LayoutSide_InlineEnd], endHalf(t.alongBorder(t.columnCount, row).width));
    }
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side) {
        t.edges.border[side] = outer[side];
        t.edges.padding[side] = kLayoutUnitZero;
    }
}

// Finds the grid's padding and borders, against a containing block of inline size `containingInline`: the table's own,
// or those that its collapsed borders give it.
void findEdges(Table& t, PrLayoutUnit containingInline) {
    t.edges = layoutEdgesFromBox(t.state.styles, layoutLogicalBox(*t.boxStyle, t.mode), containingInline);
    if (t.collapsed)
        resolveBorders(t);
}

// ---------------------------------------------------------------------------------------------------------------------
// Column measures

// The widths that a box's style gives in the table's inline axis (css-tables-3/#computing-cell-measures). A cell's
// widths are border-box widths, and a column's are as its style gives them. A width that is not a length is zero.
// `percent` is the box's percentage contribution.
struct Widths {
    PrLayoutUnit width;
    PrLayoutUnit minWidth;
    PrLayoutUnit maxWidth;          // negative if none
    double percent;                 // min(percentage width, percentage max-width), or 0
    bool fixedWidth;                // true if its width is a length
};

// Returns true if `length` is a size that a table part takes: anything but a math expression that mixes a percentage
// with a non-zero length, which counts as auto on a table part (css-values-4/#calc-computed-value).
bool partSize(const LayoutStyles* styles, PrLayoutLength length) {
    if (!layoutLengthIsFixed(length))
        return false;
    return !layoutLengthReadsBase(length) || layoutResolve(styles, length, kLayoutUnitZero) == kLayoutUnitZero;
}

// Returns the fraction that `length` takes of its base, if it reads one.
double percentOf(const LayoutStyles* styles, PrLayoutLength length) {
    constexpr int32_t kBase = 1 << 20;
    double whole = double(layoutResolve(styles, length, layoutUnit(kBase)).raw);
    double none = double(layoutResolve(styles, length, kLayoutUnitZero).raw);
    return prF64Max(0, (whole - none) / double(kBase));
}

// Returns the widths that the style `box` gives a cell or a column. `edges` is the box's inline padding and borders. A
// cell's widths include `edges`, and a column's do not.
//
// A cell's width is clamped by its max-width and then by its min-width, so that min-width wins, as it does for other
// boxes (css-sizing-3/#min-size-properties). css-tables-3 does not clamp the width, but a width outside the cell's own
// limits would otherwise size its column.
Widths widthsOf(const LayoutStyles* styles, const PrLayoutBoxStyle& box, const LayoutLogicalBox& logical,
        PrLayoutUnit edges, bool cell) {
    Widths w{.maxWidth = layoutUnit(-1)};
    auto border = [&](PrLayoutLength length) {
        PrLayoutUnit size = layoutResolve(styles, length, kLayoutUnitZero);
        return cell ? layoutBorderSize(box, size, edges) : size;
    };
    double percentMax = 1e9;
    if (partSize(styles, logical.inlineSize)) {
        if (layoutLengthReadsBase(logical.inlineSize)) {
            w.percent = percentOf(styles, logical.inlineSize);
        } else {
            w.width = border(logical.inlineSize);
            w.fixedWidth = true;
        }
    }
    if (partSize(styles, logical.minInlineSize) && !layoutLengthReadsBase(logical.minInlineSize))
        w.minWidth = border(logical.minInlineSize);
    if (partSize(styles, logical.maxInlineSize)) {
        if (layoutLengthReadsBase(logical.maxInlineSize))
            percentMax = percentOf(styles, logical.maxInlineSize);
        else
            w.maxWidth = border(logical.maxInlineSize);
    }
    w.percent = prF64Min(w.percent, percentMax);
    if (cell && w.fixedWidth) {
        if (layoutIsDefinite(w.maxWidth))
            w.width = layoutUnitMin(w.width, w.maxWidth);
        w.width = layoutUnitMax(w.width, w.minWidth);
    }
    return w;
}

// Returns the cell's padding and borders as its table resolves them, against a containing block of inline size
// `containingInline`. They are the cell's own, with its collapsed borders in place of its borders if the table
// collapses them. An anonymous cell has none.
LayoutEdges cellEdgesOf(const Table& t, const LayoutTableCell& cell, PrLayoutUnit containingInline) {
    if (cell.runLast != kPrLayoutNodeNone)
        return LayoutEdges{};
    LayoutEdges e = layoutEdgesFromBox(t.state.styles, layoutLogicalBox(boxOf(t.state.tree, cell.node), t.mode),
        containingInline);
    if (t.collapsed) {
        for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
            e.border[side] = cell.border[layoutPhysicalSide(t.mode, side)];
    }
    return e;
}

// Finds the outer min-content width, the outer max-content width and the percentage contribution of each cell, in auto
// mode (css-tables-3/#computing-cell-measures). In a constrained column, a cell's max-content width is its width.
//
// A cell's max-width limits its min-content width as well as its max-content width. css-tables-3 limits only the
// max-content width, but the engine treats max-width on a cell as a limit on the cell's whole width, as the width clamp
// in widthsOf does.
//
// An orthogonal cell's min-content and max-content widths are both its block size, as for any orthogonal flow root
// (css-writing-modes-4/#orthogonal-flows).
void measureCells(Table& t) {
    const LayoutStyles* styles = t.state.styles;
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        LayoutTableCell cell = t.cell(i);
        LayoutIntrinsicSizes content{};
        Widths w{.maxWidth = layoutUnit(-1)};
        cell.orthogonal = cell.runLast == kPrLayoutNodeNone && layoutIsOrthogonalRoot(t.state.tree, cell.node, t.mode);
        if (cell.runLast != kPrLayoutNodeNone) {
            content = layoutAnonymousIntrinsic(t.state, cell.node, cell.runLast, t.cellInput);
        } else {
            const PrLayoutBoxStyle& box = boxOf(t.state.tree, cell.node);
            LayoutLogicalBox logical = layoutLogicalBox(box, t.mode);
            PrLayoutUnit styleEdges = layoutEdgesInline(layoutEdgesFromBox(styles, logical, layoutUnit(-1)));
            PrLayoutUnit edges = layoutEdgesInline(cellEdgesOf(t, cell, layoutUnit(-1)));
            if (cell.orthogonal)
                content = layoutContribution(t.state, cell.node, t.cellInput);
            else
                content = layoutIntrinsicSizes(t.state, cell.node, t.cellInput);
            content = layoutWithEdges(content, styleEdges, edges);
            w = widthsOf(styles, box, logical, edges, true);
        }
        PrLayoutUnit min = content.minContent;
        PrLayoutUnit max = content.maxContent;
        if (layoutIsDefinite(w.maxWidth)) {
            min = layoutUnitMin(min, w.maxWidth);
            max = layoutUnitMin(max, w.maxWidth);
        }
        cell.minContent = layoutUnitMax(w.minWidth, min);
        cell.maxContent = layoutUnitMax(layoutUnitMax(w.minWidth, w.width), layoutUnitMax(cell.minContent, max));
        cell.width = w.width;
        cell.percent = w.percent;
        cell.fixedWidth = w.fixedWidth;
        t.cell(i) = cell;
    }
}

// Applies the widths of the column and column group boxes to the columns they define. In auto mode, a box's width
// counts toward the max-content width of its columns and constrains them, but not toward their min-content width, which
// css-tables-3/#computing-column-measures makes max(min-width, width).
void measureColumnBoxes(Table& t) {
    const LayoutStyles* styles = t.state.styles;
    for (uint32_t b = 0; b < t.boxCount; ++b) {
        const LayoutTableColumnBox box = t.box(b);
        const PrLayoutBoxStyle& style = boxOf(t.state.tree, box.node);
        Widths w = widthsOf(styles, style, layoutLogicalBox(style, t.mode), kLayoutUnitZero, false);
        PrLayoutUnit min = t.fixed ? layoutUnitMax(w.minWidth, w.width) : w.minWidth;
        PrLayoutUnit max = w.width;
        if (layoutIsDefinite(w.maxWidth))
            max = layoutUnitMin(max, w.maxWidth);
        max = layoutUnitMax(w.minWidth, max);
        for (uint32_t c = box.firstColumn; c < box.firstColumn + box.columnCount && c < t.columnCount; ++c) {
            LayoutTableColumn& column = t.column(c);
            column.minContent = layoutUnitMax(column.minContent, min);
            column.maxContent = layoutUnitMax(column.maxContent, max);
            column.percent = prF64Max(column.percent, w.percent);
            column.constrained = column.constrained || w.fixedWidth;
        }
    }
}

// Distributes the width `excess` of a cell over its columns from `first`, `count` of them, in proportion to `weights`,
// or equally if they sum to zero, into `out`, so that the shares sum to `excess`.
template <typename Weight>
void shareOut(double excess, uint32_t first, uint32_t count, Weight weight, double* out) {
    double total = 0;
    for (uint32_t c = 0; c < count; ++c)
        total += weight(first + c);
    for (uint32_t c = 0; c < count; ++c)
        out[c] = total > 0 ? excess * weight(first + c) / total : excess / count;
}

// Finds the min-content and max-content widths and the intrinsic percentage of each column, from its boxes and from its
// cells, span by span (css-tables-3/#computing-column-measures). In fixed mode only the first row's cells count, and
// their content does not.
void measureColumns(Table& t) {
    measureColumnBoxes(t);
    uint32_t maxSpan = 1;
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        const LayoutTableCell& cell = t.cell(i);
        if (t.fixed && cell.gridRow != 0)
            continue;
        maxSpan = cell.columnSpan > maxSpan ? cell.columnSpan : maxSpan;
        if (cell.columnSpan != 1)
            continue;
        LayoutTableColumn& column = t.column(cell.column);
        column.constrained = column.constrained || cell.fixedWidth;
    }
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        const LayoutTableCell& cell = t.cell(i);
        if ((t.fixed && cell.gridRow != 0) || cell.columnSpan != 1)
            continue;
        LayoutTableColumn& column = t.column(cell.column);
        PrLayoutUnit min = t.fixed ? cell.width : cell.minContent;
        PrLayoutUnit max = t.fixed ? cell.width : cell.maxContent;
        // In a constrained column, a cell's max-content width is its width, if it is larger than its min-content width.
        if (column.constrained && !t.fixed)
            max = layoutUnitMax(cell.minContent, cell.width);
        column.minContent = layoutUnitMax(column.minContent, min);
        column.maxContent = layoutUnitMax(column.maxContent, max);
        column.percent = prF64Max(column.percent, cell.percent);
    }
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        LayoutTableColumn& column = t.column(c);
        column.maxContent = layoutUnitMax(column.maxContent, column.minContent);
        column.spanningMax = column.maxContent;
    }
    // Cells that span more columns, by increasing span. Each span's contributions are found from the columns as the
    // spans before it left them, and then applied.
    LayoutTableScratch& s = t.scratch();
    for (uint32_t span = 2; span <= maxSpan; ++span) {
        // The copy is made in the same storage as the columns, which growth moves, so the storage grows before any
        // column is read.
        uint32_t mark = s.columns.count;
        s.columns.resize(mark + t.columnCount);
        for (uint32_t c = 0; c < t.columnCount; ++c)
            s.columns.data[mark + c] = t.column(c);
        auto before = [&](uint32_t c) -> const LayoutTableColumn& { return s.columns.data[mark + c]; };
        double shares[kPrLayoutTableMaxColumnSpan];
        for (uint32_t i = 0; i < t.cellCount; ++i) {
            const LayoutTableCell& cell = t.cell(i);
            if (cell.columnSpan != span || (t.fixed && cell.gridRow != 0))
                continue;
            uint32_t first = cell.column;
            double baseMin = 0;
            double baseMax = 0;
            double percents = 0;
            double freeMax = 0;
            uint32_t free = 0;
            for (uint32_t c = first; c < first + span; ++c) {
                baseMin += double(before(c).minContent.raw);
                baseMax += double(before(c).maxContent.raw);
                percents += before(c).percent;
                if (before(c).percent == 0) {
                    freeMax += double(before(c).spanningMax.raw);
                    ++free;
                }
            }
            double spacing = double(t.inlineSpacing.raw) * double(span - 1);
            double cellMin = double((t.fixed ? cell.width : cell.minContent).raw) - spacing;
            double cellMax = double((t.fixed ? cell.width : cell.maxContent).raw) - spacing;
            // The min-content contribution: what lies between the columns' min-content and max-content widths goes in
            // proportion to the room between them, and what lies past their max-content widths in proportion to those.
            double between = prF64Min(prF64Max(cellMin - baseMin, 0), baseMax - baseMin);
            double past = prF64Max(cellMin - baseMax, 0);
            shareOut(past, first, span, [&](uint32_t c) { return double(before(c).maxContent.raw); }, shares);
            for (uint32_t c = first; c < first + span; ++c) {
                double room = double((before(c).maxContent - before(c).minContent).raw);
                double share = baseMax > baseMin ? between * room / (baseMax - baseMin) : 0;
                PrLayoutUnit contribution = unitOf(double(before(c).minContent.raw) + share + shares[c - first]);
                t.column(c).minContent = layoutUnitMax(t.column(c).minContent, contribution);
            }
            shareOut(prF64Max(cellMax - baseMax, 0), first, span,
                [&](uint32_t c) { return double(before(c).maxContent.raw); }, shares);
            for (uint32_t c = first; c < first + span; ++c) {
                PrLayoutUnit contribution = unitOf(double(before(c).maxContent.raw) + shares[c - first]);
                t.column(c).maxContent = layoutUnitMax(t.column(c).maxContent, contribution);
            }
            // The percentage contribution goes to the columns that have none, in proportion to their max-content widths
            // from the cells that span them alone, or equally.
            double percent = prF64Max(cell.percent - percents, 0);
            if (percent > 0 && free != 0) {
                for (uint32_t c = first; c < first + span; ++c) {
                    if (before(c).percent != 0)
                        continue;
                    double share =
                        freeMax > 0 ? percent * double(before(c).spanningMax.raw) / freeMax : percent / double(free);
                    t.column(c).percent = prF64Max(t.column(c).percent, share);
                }
            }
        }
        s.columns.count = mark;
        for (uint32_t c = 0; c < t.columnCount; ++c)
            t.column(c).maxContent = layoutUnitMax(t.column(c).maxContent, t.column(c).minContent);
    }
    // In auto mode, the percentages of the columns sum to at most 100%, taken in order. In fixed mode, the width
    // distribution scales them instead (css-tables-3/#width-distribution-in-fixed-mode). A collapsed column has no
    // size.
    double sum = 0;
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        LayoutTableColumn& column = t.column(c);
        if (column.collapsed) {
            column.minContent = kLayoutUnitZero;
            column.maxContent = kLayoutUnitZero;
            column.percent = 0;
        }
        if (!t.fixed)
            column.percent = prF64Min(column.percent, prF64Max(1 - sum, 0));
        sum += column.percent;
        if (t.fixed && column.percent > 0)
            column.minContent = kLayoutUnitZero;
        column.minContent = layoutUnit(column.minContent.raw < kColumnMax ? column.minContent.raw : kColumnMax);
        column.maxContent = layoutUnit(column.maxContent.raw < kColumnMax ? column.maxContent.raw : kColumnMax);
    }
}

// The widths of a table's grid, from its columns' measures, its spacing and its edges
// (css-tables-3/#computing-the-table-width).
struct GridWidths {
    PrLayoutUnit minContent;        // GRIDMIN
    PrLayoutUnit maxContent;        // GRIDMAX, raised as its percentage columns ask
    bool unbounded;                 // true if the percentage columns ask for a table of unbounded width
};

// Returns the min-content and max-content widths of the table's grid. A percentage column asks for a table at least as
// wide as its max-content width divided by its percentage. The other columns ask for a table at least as wide as their
// max-content widths divided by the fraction that the percentages leave. If the percentages leave nothing and the other
// columns have content, the ask is unbounded, and the max-content width stays what the columns alone give.
GridWidths gridWidths(const Table& t) {
    double min = 0;
    double max = 0;
    double percents = 0;
    double others = 0;
    double asked = 0;
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        const LayoutTableColumn& column = t.column(c);
        min += double(column.minContent.raw);
        max += double(column.maxContent.raw);
        percents += column.percent;
        if (column.percent > 0)
            asked = prF64Max(asked, double(column.maxContent.raw) / column.percent);
        else
            others += double(column.maxContent.raw);
    }
    bool unbounded = percents >= 1 && others > 0;
    if (percents > 0) {
        if (percents < 1)
            asked = prF64Max(asked, others / (1 - percents));
        max = prF64Max(max, asked);
    }
    PrLayoutUnit spacing = spacingAcross(t.inlineSpacing, visibleColumns(t, 0, t.columnCount));
    double fixed = double((spacing + layoutEdgesInline(t.edges)).raw);
    return GridWidths{
        .minContent = unitOf(min + fixed),
        .maxContent = unitOf(prF64Min(max, double(kColumnMax)) + fixed),
        .unbounded = unbounded,
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// Width distribution

// Distributes `width`, the assignable width of the table, to its columns (css-tables-3/#width-distribution). Writes
// each column's size.
void distributeAuto(Table& t, double width) {
    // The four sizing guesses, each a column's width under it, in nondecreasing order of their sums
    // (css-tables-3/#width-distribution-algorithm).
    auto guess = [&](uint32_t which, const LayoutTableColumn& column) -> double {
        double min = double(column.minContent.raw);
        if (which == 0 || column.collapsed)
            return min;
        if (column.percent > 0)
            return prF64Max(min, column.percent * width);
        if (which == 1 || (which == 2 && !column.constrained))
            return min;
        return double(column.maxContent.raw);
    };
    double sums[4] = {};
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        for (uint32_t which = 0; which < 4; ++which)
            sums[which] += guess(which, t.column(c));
    }
    // Each size is rounded where the sizes before it end, so that the sizes sum to their total within one unit.
    double sum = 0;
    PrLayoutUnit placed = kLayoutUnitZero;
    auto assign = [&](uint32_t c, double size) {
        sum += size;
        PrLayoutUnit end = unitOf(sum);
        t.column(c).size = layoutUnitMax(kLayoutUnitZero, end - placed);
        placed = end;
    };
    if (width <= sums[3]) {
        // The used widths are a linear combination of the two guesses whose sums bound the width.
        uint32_t lower = 0;
        while (lower < 2 && sums[lower + 1] < width)
            ++lower;
        double span = sums[lower + 1] - sums[lower];
        double weight = span > 0 ? prF64Min(prF64Max((width - sums[lower]) / span, 0), 1) : 0;
        for (uint32_t c = 0; c < t.columnCount; ++c) {
            const LayoutTableColumn& column = t.column(c);
            double from = guess(lower, column);
            assign(c, from + weight * (guess(lower + 1, column) - from));
        }
        return;
    }
    // Otherwise the columns take the max-content guess and the excess, by the first rule whose columns there are
    // (css-tables-3/#distributing-width-to-columns).
    double excess = width - sums[3];
    auto grows = [&](uint32_t rule, const LayoutTableColumn& column) {
        if (column.collapsed)
            return false;
        switch (rule) {
            case 0: return !column.constrained && column.origin && column.percent == 0 && column.maxContent.raw > 0;
            case 1: return !column.constrained && column.origin && column.percent == 0;
            case 2: return column.constrained && column.percent == 0 && column.maxContent.raw > 0;
            case 3: return column.percent > 0;
            case 4: return column.origin;
            default: return true;
        }
    };
    uint32_t rule = 0;
    for (; rule < 5; ++rule) {
        bool any = false;
        for (uint32_t c = 0; c < t.columnCount && !any; ++c)
            any = grows(rule, t.column(c));
        if (any)
            break;
    }
    double total = 0;
    uint32_t count = 0;
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        const LayoutTableColumn& column = t.column(c);
        if (!grows(rule, column))
            continue;
        total += rule == 3 ? column.percent : double(column.maxContent.raw);
        ++count;
    }
    bool proportional = rule == 0 || rule == 2 || rule == 3;
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        const LayoutTableColumn& column = t.column(c);
        double size = guess(3, column);
        if (grows(rule, column)) {
            double weight = rule == 3 ? column.percent : double(column.maxContent.raw);
            size += proportional && total > 0 ? excess * weight / total : excess / double(count);
        }
        assign(c, size);
    }
}

// Distributes `width` to the columns of a table in fixed mode (css-tables-3/#width-distribution-in-fixed-mode). A
// column with a length takes that length, and a column with a percentage takes that share of the width. Both are scaled
// down if together they exceed the width. The columns with neither share what is left. If there are no such columns,
// what is left goes to the other columns in proportion to their lengths, or else to their percentages, or else equally.
void distributeFixed(Table& t, double width) {
    double lengths = 0;
    double percents = 0;
    uint32_t autos = 0;
    uint32_t visible = 0;
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        const LayoutTableColumn& column = t.column(c);
        if (column.collapsed)
            continue;
        ++visible;
        if (column.percent > 0)
            percents += column.percent;
        else if (column.maxContent.raw > 0 || column.constrained)
            lengths += double(column.maxContent.raw);
        else
            ++autos;
    }
    bool overflows = percents > 0 && lengths + percents * width > width;
    double scale = overflows ? prF64Max(0, width - lengths) / (percents * width) : 1;
    double left = width - lengths - percents * width * scale;
    double sum = 0;
    PrLayoutUnit placed = kLayoutUnitZero;
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        const LayoutTableColumn& column = t.column(c);
        bool percent = column.percent > 0;
        bool length = !percent && (column.maxContent.raw > 0 || column.constrained);
        double size = percent ? column.percent * width * scale : length ? double(column.maxContent.raw) : 0;
        if (left > 0 && !column.collapsed) {
            if (autos != 0)
                size += !percent && !length ? left / double(autos) : 0;
            else if (lengths > 0)
                size += length ? left * double(column.maxContent.raw) / lengths : 0;
            else if (percents > 0)
                size += percent ? left * column.percent / percents : 0;
            else
                size += left / double(visible);
        }
        sum += size;
        PrLayoutUnit end = unitOf(sum);
        t.column(c).size = layoutUnitMax(kLayoutUnitZero, end - placed);
        placed = end;
    }
}

// Sizes and places the columns of the table, whose border box is `inlineSize` wide, in its grid.
void placeColumns(Table& t, PrLayoutUnit inlineSize) {
    PrLayoutUnit spacing = spacingAcross(t.inlineSpacing, visibleColumns(t, 0, t.columnCount));
    PrLayoutUnit assignable = inlineSize - layoutEdgesInline(t.edges) - spacing;
    double width = double(layoutUnitMax(kLayoutUnitZero, assignable).raw);
    if (t.fixed)
        distributeFixed(t, width);
    else
        distributeAuto(t, width);
    PrLayoutUnit at = layoutEdgesContent(t.edges, LayoutSide_InlineStart) + t.inlineSpacing;
    for (uint32_t c = 0; c < t.columnCount; ++c) {
        LayoutTableColumn& column = t.column(c);
        if (column.collapsed)
            column.size = kLayoutUnitZero;
        column.offset = at;
        if (!column.collapsed)
            at = at + column.size + t.inlineSpacing;
    }
}

// Returns the width of the columns that the cell spans, with the spacing between them.
PrLayoutUnit spannedWidth(const Table& t, const LayoutTableCell& cell) {
    uint32_t visible = visibleColumns(t, cell.column, cell.columnSpan);
    PrLayoutUnit width = visible > 1 ? t.inlineSpacing * int32_t(visible - 1) : kLayoutUnitZero;
    for (uint32_t c = cell.column; c < cell.column + cell.columnSpan; ++c)
        width = width + t.column(c).size;
    return width;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rows

// Returns the space that the table's cells are laid out in: its grid's content box, of inline size `contentInline` and
// of block size `contentBlock` if that is definite.
LayoutSpace cellSpace(const Table& t, const LayoutSpace& space, PrLayoutUnit contentInline, PrLayoutUnit contentBlock) {
    return LayoutSpace{
        .mode = t.mode,
        .available = contentInline,
        .percentInline = contentInline,
        .percentBlock = contentBlock,
        .containing = LayoutLogicalSize{contentInline, contentBlock},
        .blockLimit = layoutUnit(-1),
        .fallback = space.fallback,
    };
}

// Returns the inputs that the table gives a cell beyond its size requests: its collapsed borders, if the table
// collapses its borders. The cell is not aligned by a baseline until its row's baseline is known.
LayoutCellSpace cellInputs(const Table& t, const LayoutTableCell& cell) {
    LayoutCellSpace inputs{.collapsed = t.collapsed};
    for (uint32_t side = 0; t.collapsed && side < PrRectSide_Count; ++side)
        inputs.borders[side] = cell.border[side];
    return inputs;
}

// Returns the requests that measure a cell of `inlineSize` for the block size of its content.
LayoutRequests contentRequests(PrLayoutUnit inlineSize) {
    return LayoutRequests{
        .inlineAxis = LayoutRequest{.size = inlineSize, .kind = LayoutRequestKind_Fixed, .definite = true},
        .blockAxis = LayoutRequest{.kind = LayoutRequestKind_Content},
    };
}

// Returns the requests that lay out an orthogonal cell whose width in the table's inline axis is `inlineSize`. The
// requests are in the cell's own axes, so that width is the cell's block size. The cell sizes its own inline axis, as
// an orthogonal box in flow does (css-writing-modes-4/#orthogonal-auto).
LayoutRequests orthogonalRequests(PrLayoutUnit inlineSize) {
    return LayoutRequests{
        .inlineAxis = LayoutRequest{.kind = LayoutRequestKind_Own},
        .blockAxis = LayoutRequest{.size = inlineSize, .kind = LayoutRequestKind_Fixed, .definite = true},
    };
}

// Returns the block size that the style of the cell or row `node` gives its border box, with `edges` as its block-axis
// padding and borders, or zero for an anonymous part. A percentage counts as zero (css-tables-3/#row-layout). If
// `percent` is given, a percentage sets `*percent` to its fraction.
PrLayoutUnit styledBlockOf(const Table& t, LayoutNodeIndex node, PrLayoutUnit edges, double* PR_NULLABLE percent) {
    if (node == kPrLayoutNodeNone)
        return kLayoutUnitZero;
    const PrLayoutBoxStyle& box = boxOf(t.state.tree, node);
    LayoutLogicalBox logical = layoutLogicalBox(box, t.mode);
    PrLayoutUnit size = kLayoutUnitZero;
    if (partSize(t.state.styles, logical.blockSize)) {
        if (!layoutLengthReadsBase(logical.blockSize))
            size = layoutBorderSize(box, layoutResolve(t.state.styles, logical.blockSize, kLayoutUnitZero), edges);
        else if (percent != nullptr)
            *percent = percentOf(t.state.styles, logical.blockSize);
    }
    if (partSize(t.state.styles, logical.minBlockSize) && !layoutLengthReadsBase(logical.minBlockSize)) {
        PrLayoutUnit min = layoutResolve(t.state.styles, logical.minBlockSize, kLayoutUnitZero);
        size = layoutUnitMax(size, layoutBorderSize(box, min, edges));
    }
    return size;
}

// Measures each cell at the width of its columns, for the block size of its content and its first baseline.
//
// A cell with content but no baseline takes the block-end edge of its content box as its baseline
// (css-tables-3/#row-layout). An empty cell is aligned to the top of its row instead, and does not contribute to the
// row's baseline. Its content-box edge still sets the row's baseline if no cell in the row has a baseline
// (rowBaseline).
//
// An orthogonal cell is laid out whole, because its height in the table is its inline size, which a measurement does
// not give. It has no baseline in the table's axes.
void measureRows(Table& t, const LayoutSpace& cells) {
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        LayoutTableCell cell = t.cell(i);
        cell.inlineSize = spannedWidth(t, cell);
        LayoutCellSpace inputs = cellInputs(t, cell);
        LayoutSpace space = cells;
        space.cell = inputs.collapsed ? &inputs : nullptr;
        LayoutMeasurement m{};
        if (cell.orthogonal) {
            LayoutResult result = layoutFinal(t.state, cell.node, space, orthogonalRequests(cell.inlineSize));
            m.blockSize = layoutLogicalSize(t.mode, result.fragment->extent).blockSize;
            layoutFragmentRelease(result.fragment);
        } else if (cell.runLast == kPrLayoutNodeNone) {
            m = layoutMeasure(t.state, cell.node, space, contentRequests(cell.inlineSize), LayoutMeasureKind_Baselines);
        } else {
            // An anonymous cell has no cache, and keeps the layout that it is measured with.
            LayoutResult result =
                layoutAnonymousBlock(t.state, cell.node, cell.runLast, space, contentRequests(cell.inlineSize));
            m.blockSize = layoutLogicalSize(t.mode, result.fragment->extent).blockSize;
            m.first = layoutFragmentBaseline(result.fragment, LayoutBaselineEdge_First);
            layoutFragmentRelease(cell.fragment);
            cell.fragment = result.fragment;
        }
        LayoutEdges e = cellEdgesOf(t, cell, cells.containing.inlineSize);
        cell.measured = m.blockSize;
        cell.blockEdgeEnd = layoutEdgesContent(e, LayoutSide_BlockEnd);
        cell.blockPercent = 0;
        cell.styledBlock = styledBlockOf(t, cell.runLast == kPrLayoutNodeNone ? cell.node : kPrLayoutNodeNone,
            layoutEdgesBlock(e), &cell.blockPercent);
        cell.baseline = m.first.present ? m.first.offset : m.blockSize - cell.blockEdgeEnd;
        if (!m.first.present && cell.align == LayoutCellAlign_Baseline && cell.runLast == kPrLayoutNodeNone) {
            const LayoutTreeNode& n = layoutTreeNode(t.state.tree, cell.node);
            if (n.first == kPrLayoutNodeNone || layoutTableRunIsBlank(t.state.tree, n.first, n.last))
                cell.align = LayoutCellAlign_Top;
        }
        t.cell(i) = cell;
    }
}

// Returns the block size of the rows from `first`, `count` of them, with the spacing between them.
PrLayoutUnit spannedBlock(const Table& t, uint32_t first, uint32_t count) {
    uint32_t visible = visibleRows(t, first, count);
    PrLayoutUnit size = visible > 1 ? t.blockSpacing * int32_t(visible - 1) : kLayoutUnitZero;
    for (uint32_t r = first; r < first + count; ++r)
        size = size + t.gridRow(r).blockSize;
    return size;
}

// Sizes the rows from their cells and their styles (css-tables-3/#row-layout). A row is at least as tall as its own
// block size, as each cell that spans it alone, and as the ascent plus the descent of its cells that are aligned by
// their baselines. Cells that span more rows then grow them, by increasing span, in proportion to the rows' sizes or
// else equally. The rows are sized as if none were collapsed (collapseRows).
void sizeRows(Table& t) {
    // The block size of the rows from `first`, `count` of them, and of the spacing between them, with every row
    // visible.
    auto spanned = [&](uint32_t first, uint32_t count) {
        PrLayoutUnit size = t.blockSpacing * int32_t(count - 1);
        for (uint32_t r = first; r < first + count; ++r)
            size = size + t.gridRow(r).blockSize;
        return size;
    };
    for (uint32_t grid = 0; grid < t.rowCount; ++grid) {
        LayoutTableRow& row = t.gridRow(grid);
        row.percent = 0;
        row.styledBlock = styledBlockOf(t, row.node, kLayoutUnitZero, &row.percent);
        row.blockSize = row.styledBlock;
        row.ascent = kLayoutUnitZero;
        row.descent = kLayoutUnitZero;
        row.baselines = false;
    }
    uint32_t maxSpan = 1;
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        const LayoutTableCell& cell = t.cell(i);
        PrLayoutUnit size = layoutUnitMax(cell.measured, cell.styledBlock);
        LayoutTableRow& row = t.gridRow(cell.gridRow);
        maxSpan = cell.rowSpan > maxSpan ? cell.rowSpan : maxSpan;
        if (cell.align == LayoutCellAlign_Baseline) {
            row.baselines = true;
            row.ascent = layoutUnitMax(row.ascent, cell.baseline);
            if (cell.rowSpan == 1)
                row.descent = layoutUnitMax(row.descent, size - cell.baseline);
        }
        // A cell that spans one row gives the row its percentage, if larger (css-tables-3/#row-layout).
        if (cell.rowSpan == 1) {
            row.blockSize = layoutUnitMax(row.blockSize, size);
            row.percent = prF64Max(row.percent, cell.blockPercent);
        }
    }
    for (uint32_t grid = 0; grid < t.rowCount; ++grid) {
        LayoutTableRow& row = t.gridRow(grid);
        row.blockSize = layoutUnitMax(row.blockSize, row.ascent + row.descent);
    }
    for (uint32_t span = 2; span <= maxSpan; ++span) {
        for (uint32_t i = 0; i < t.cellCount; ++i) {
            const LayoutTableCell& cell = t.cell(i);
            if (cell.rowSpan != span)
                continue;
            // A cell aligned by its baseline is moved down by its row's baseline, and needs that much more room.
            PrLayoutUnit size = layoutUnitMax(cell.measured, cell.styledBlock);
            if (cell.align == LayoutCellAlign_Baseline)
                size = size + layoutUnitMax(kLayoutUnitZero, t.gridRow(cell.gridRow).ascent - cell.baseline);
            PrLayoutUnit excess = size - spanned(cell.gridRow, span);
            if (excess <= kLayoutUnitZero)
                continue;
            // The rows that their content alone sizes take the excess if there are any. Otherwise every row takes it.
            auto sizedByContent = [&](const LayoutTableRow& row) {
                return row.styledBlock == kLayoutUnitZero && row.percent == 0;
            };
            bool autos = false;
            for (uint32_t r = cell.gridRow; r < cell.gridRow + span && !autos; ++r)
                autos = sizedByContent(t.gridRow(r));
            auto takes = [&](const LayoutTableRow& row) { return !autos || sizedByContent(row); };
            double total = 0;
            uint32_t count = 0;
            for (uint32_t r = cell.gridRow; r < cell.gridRow + span; ++r) {
                if (!takes(t.gridRow(r)))
                    continue;
                total += double(t.gridRow(r).blockSize.raw);
                ++count;
            }
            double sum = 0;
            PrLayoutUnit placed = kLayoutUnitZero;
            for (uint32_t r = cell.gridRow; r < cell.gridRow + span && count != 0; ++r) {
                LayoutTableRow& row = t.gridRow(r);
                if (!takes(row))
                    continue;
                if (total > 0)
                    sum += double(excess.raw) * double(row.blockSize.raw) / total;
                else
                    sum += double(excess.raw) / double(count);
                PrLayoutUnit end = unitOf(sum);
                row.blockSize = row.blockSize + end - placed;
                placed = end;
            }
        }
    }
}

// Grows the rows to fill `content`, the block size that the table's style gives its rows and the spacing between them,
// if they are shorter (css-tables-3/#height-distribution-algorithm). A row with a percentage first takes that share of
// the rows' block size. What is left goes equally to the rows whose content alone sizes them, or else to every row.
// Collapsed rows take part as if they were visible (collapseRows).
void distributeHeight(Table& t, PrLayoutUnit content) {
    if (t.rowCount == 0)
        return;
    PrLayoutUnit rows = content - spacingAcross(t.blockSpacing, t.rowCount);
    PrLayoutUnit sum = kLayoutUnitZero;
    for (uint32_t grid = 0; grid < t.rowCount; ++grid)
        sum = sum + t.gridRow(grid).blockSize;
    if (sum >= rows)
        return;
    for (uint32_t grid = 0; grid < t.rowCount && sum < rows; ++grid) {
        LayoutTableRow& row = t.gridRow(grid);
        if (row.percent <= 0)
            continue;
        PrLayoutUnit wanted = layoutUnitMin(unitOf(row.percent * double(rows.raw)), row.blockSize + rows - sum);
        if (wanted > row.blockSize) {
            sum = sum + wanted - row.blockSize;
            row.blockSize = wanted;
        }
    }
    PrLayoutUnit excess = rows - sum;
    if (excess <= kLayoutUnitZero)
        return;
    uint32_t autos = 0;
    for (uint32_t grid = 0; grid < t.rowCount; ++grid) {
        const LayoutTableRow& row = t.gridRow(grid);
        autos += row.styledBlock == kLayoutUnitZero && row.percent == 0 ? 1 : 0;
    }
    uint32_t count = autos != 0 ? autos : t.rowCount;
    uint32_t k = 0;
    PrLayoutUnit placed = kLayoutUnitZero;
    for (uint32_t grid = 0; grid < t.rowCount; ++grid) {
        LayoutTableRow& row = t.gridRow(grid);
        if (autos != 0 && (row.styledBlock != kLayoutUnitZero || row.percent != 0))
            continue;
        ++k;
        PrLayoutUnit end = unitOf(double(excess.raw) * double(k) / double(count));
        row.blockSize = row.blockSize + end - placed;
        placed = end;
    }
}

// Removes the collapsed rows from the grid, once the rows are sized and distributed as if none were collapsed. A
// collapsed row gets no size and no spacing, and the cells that span it become shorter
// (css-tables-3/#visibility-collapse-cell-rendering). Returns the block size that the rows and spacing lose.
PrLayoutUnit collapseRows(Table& t) {
    PrLayoutUnit removed = kLayoutUnitZero;
    for (uint32_t grid = 0; grid < t.rowCount; ++grid) {
        LayoutTableRow& row = t.gridRow(grid);
        if (!row.collapsed)
            continue;
        removed = removed + row.blockSize + t.blockSpacing;
        row.blockSize = kLayoutUnitZero;
    }
    // Without visible rows, the grid loses the spacing after the last row too.
    if (removed != kLayoutUnitZero && visibleRows(t, 0, t.rowCount) == 0)
        removed = removed + t.blockSpacing;
    return removed;
}

// Places the rows of the table in its grid, and returns the block size of the grid's border box.
PrLayoutUnit placeRows(Table& t) {
    PrLayoutUnit at = layoutEdgesContent(t.edges, LayoutSide_BlockStart);
    for (uint32_t grid = 0; grid < t.rowCount; ++grid) {
        LayoutTableRow& row = t.gridRow(grid);
        row.offset = row.collapsed ? at : at + t.blockSpacing;
        at = row.offset + row.blockSize;
    }
    if (visibleRows(t, 0, t.rowCount) != 0)
        at = at + t.blockSpacing;
    return at + layoutEdgesContent(t.edges, LayoutSide_BlockEnd);
}

// Returns the baseline of row `grid`, from its block-start edge: the largest distance to the baseline of a cell aligned
// by its baseline, or else the block-end edge of the content box of its lowest cell (css-tables-3/#row-layout).
PrLayoutUnit rowBaseline(const Table& t, uint32_t grid) {
    const LayoutTableRow& row = t.gridRow(grid);
    if (row.baselines)
        return row.ascent;
    PrLayoutUnit lowest = kLayoutUnitZero;
    bool any = false;
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        const LayoutTableCell& cell = t.cell(i);
        if (cell.gridRow != grid)
            continue;
        PrLayoutUnit end = spannedBlock(t, grid, cell.rowSpan) - cell.blockEdgeEnd;
        lowest = any ? layoutUnitMax(lowest, end) : end;
        any = true;
    }
    return any ? lowest : row.blockSize;
}

// ---------------------------------------------------------------------------------------------------------------------
// Cells

// Lays out each cell at its final size: the width of its columns and the height of its rows. A cell that is as tall as
// its content, and that its alignment does not move, asks again for its content's size and takes the layout it was
// measured with. A cell's content is definite in its block axis if the cell's block size is a length, or if the table's
// block size is a length or a percentage of a definite size (css-tables-3/#table-cell-content-relayout). An orthogonal
// cell is laid out with both of its sizes fixed, in its own axes.
void layOutCells(Table& t, const LayoutSpace& cells, bool definiteTable) {
    for (uint32_t i = 0; i < t.cellCount; ++i) {
        LayoutTableCell cell = t.cell(i);
        PrLayoutUnit blockSize = spannedBlock(t, cell.gridRow, cell.rowSpan);
        bool definite = definiteTable;
        if (!definite && cell.runLast == kPrLayoutNodeNone) {
            PrLayoutLength height = layoutLogicalBox(boxOf(t.state.tree, cell.node), t.mode).blockSize;
            definite = layoutLengthIsFixed(height) && !layoutLengthReadsBase(height);
        }
        LayoutCellSpace inputs = cellInputs(t, cell);
        LayoutSpace space = cells;
        PrLayoutUnit rowAscent = t.gridRow(cell.gridRow).ascent;
        bool shifts = cell.align == LayoutCellAlign_Baseline && cell.baseline != rowAscent;
        LayoutRequests requests = contentRequests(cell.inlineSize);
        if (cell.orthogonal) {
            requests = orthogonalRequests(cell.inlineSize);
            requests.inlineAxis = LayoutRequest{.size = blockSize, .kind = LayoutRequestKind_Fixed, .definite = true};
        } else if (definite || blockSize != cell.measured || shifts) {
            requests.blockAxis = LayoutRequest{.size = blockSize, .kind = LayoutRequestKind_Fixed, .definite = definite};
            inputs.aligned = cell.align == LayoutCellAlign_Baseline;
            inputs.baseline = inputs.aligned ? rowAscent : kLayoutUnitZero;
        } else if (cell.runLast != kPrLayoutNodeNone) {
            // The anonymous cell keeps the layout that it was measured with.
            continue;
        }
        space.cell = inputs.collapsed || inputs.aligned ? &inputs : nullptr;
        LayoutResult result;
        if (cell.runLast != kPrLayoutNodeNone)
            result = layoutAnonymousBlock(t.state, cell.node, cell.runLast, space, requests);
        else
            result = layoutFinal(t.state, cell.node, space, requests);
        layoutFragmentRelease(t.cell(i).fragment);
        t.cell(i).fragment = result.fragment;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Fragments

// A logical rectangle in the grid's border box.
struct Area {
    LayoutLogicalOffset offset;
    LayoutLogicalSize size;
};

// Returns the area of the columns from `first`, `count` of them, and of the rows from `row`, `rows` of them, with the
// spacing between them. With no columns or no rows, the area is empty at the start of the grid's content box.
Area areaOf(const Table& t, uint32_t first, uint32_t count, uint32_t row, uint32_t rows) {
    Area area{};
    if (count != 0 && first < t.columnCount) {
        uint32_t last = first + count - 1 < t.columnCount ? first + count - 1 : t.columnCount - 1;
        area.offset.inlineOffset = t.column(first).offset;
        area.size.inlineSize = t.column(last).offset + t.column(last).size - t.column(first).offset;
    } else {
        area.offset.inlineOffset = layoutEdgesContent(t.edges, LayoutSide_InlineStart);
    }
    if (rows != 0 && row < t.rowCount) {
        const LayoutTableRow& last = t.gridRow(row + rows - 1);
        area.offset.blockOffset = t.gridRow(row).offset;
        area.size.blockSize = last.offset + last.blockSize - area.offset.blockOffset;
    } else {
        area.offset.blockOffset = layoutEdgesContent(t.edges, LayoutSide_BlockStart);
    }
    return area;
}

// Appends a placeholder for each child out of flow whose place `container` holds, at `at` in the logical space of the
// fragment being made.
void placeOutOfFlow(Table& t, LayoutNodeIndex container, LayoutLogicalOffset at) {
    for (uint32_t i = 0; i < t.outOfFlowCount; ++i) {
        const LayoutTableAside& aside = t.outOfFlow(i);
        if (aside.container != container)
            continue;
        LayoutFragment* placeholder = layoutFragmentCreatePlaceholder(aside.node, layoutStaticEdges(t.mode),
            layoutIsFixed(t.state.tree, aside.node));
        t.state.scratch->children.push(LayoutLogicalChild{placeholder, at});
    }
}

// Returns the fragment of the table part `node`, of `size`, made from the scratch children from `mark`. The part holds
// the boxes out of flow that it is the containing block of, laid out in `fallback`.
const LayoutFragment* buildPart(Table& t, LayoutNodeIndex node, uint32_t mark, LayoutLogicalSize size,
        PrLayoutVec fallback) {
    ++t.state.stats->boxFragmentsMade;
    LayoutFragment* fragment = layoutHolderBuild(t.state, node, mark, t.mode, size, fallback);
    layoutFragmentSetOverflow(t.state.tree, fragment);
    return fragment;
}

// Appends the fragment of cell `i` at its offset `at` in the logical space of the fragment being made, moved by its
// relative position.
void pushCell(Table& t, uint32_t i, LayoutLogicalOffset at, const LayoutSpace& cells) {
    const LayoutTableCell& cell = t.cell(i);
    LayoutLogicalOffset relative = kLayoutLogicalOffsetZero;
    if (cell.runLast == kPrLayoutNodeNone)
        relative = layoutRelativeOffset(t.state.styles, boxOf(t.state.tree, cell.node), cells);
    layoutFragmentRetain(cell.fragment);
    t.state.scratch->children.push(LayoutLogicalChild{cell.fragment, at + relative});
}

// Appends the fragments of the rows of the group `g`, each holding its cells, at their offsets from `origin`, the start
// corner of the fragment being made in the grid's border box. A row with no box has no fragment, and its cells are
// placed in the fragment around it.
void pushRows(Table& t, uint32_t g, LayoutLogicalOffset origin, const LayoutSpace& cells, PrLayoutVec fallback) {
    const LayoutTableGroup group = t.group(g);
    RawArray<LayoutLogicalChild>& children = t.state.scratch->children;
    for (uint32_t r = group.firstRow; r < group.firstRow + group.rowCount; ++r) {
        LayoutTableRow row = t.row(r);
        Area area = areaOf(t, 0, t.columnCount, row.grid, 1);
        LayoutLogicalOffset base = row.node != kPrLayoutNodeNone ? area.offset : origin;
        uint32_t mark = children.count;
        for (uint32_t i = row.firstCell; i < row.firstCell + row.cellCount; ++i) {
            const LayoutTableCell& cell = t.cell(i);
            Area at = areaOf(t, cell.column, cell.columnSpan, cell.gridRow, cell.rowSpan);
            pushCell(t, i, at.offset - base, cells);
        }
        if (row.node == kPrLayoutNodeNone)
            continue;
        placeOutOfFlow(t, row.node, kLayoutLogicalOffsetZero);
        const LayoutFragment* fragment = buildPart(t, row.node, mark, area.size, fallback);
        LayoutLogicalOffset relative = layoutRelativeOffset(t.state.styles, boxOf(t.state.tree, row.node), cells);
        children.push(LayoutLogicalChild{fragment, area.offset - origin + relative});
    }
}

// Appends the collapsed borders of the table's grid to the scratch segments, in the coordinates of the table's
// fragment, which has the size `size` and its grid at `grid`. Returns how many segments it appended. A segment lies on
// its line as the cells' borders share it: its start half before the line, and its end half after it (resolveBorders).
// A segment across the inline axis also covers the halves of the lines along the block axis that meet it at its ends,
// so that the corners are drawn.
uint32_t collectSegments(Table& t, LayoutLogicalSize size, LayoutLogicalOffset grid) {
    RawArray<LayoutGridBorder>& segments = t.scratch().segments;
    uint32_t mark = segments.count;
    auto endHalf = [](PrLayoutUnit width) { return layoutUnit(width.raw / 2); };
    auto startHalf = [&](PrLayoutUnit width) { return width - endHalf(width); };
    PrRectSide across = layoutPhysicalSide(t.mode, LayoutSide_BlockStart);
    PrRectSide along = layoutPhysicalSide(t.mode, LayoutSide_InlineStart);
    auto push = [&](const LayoutTableBorder& border, LayoutLogicalOffset offset, LayoutLogicalSize extent, PrRectSide side) {
        if (border.width <= kLayoutUnitZero || !prDrawBorderStylePaints(border.style))
            return;
        PrLayoutVec at = layoutPhysicalOffset(t.mode, grid + offset, extent, size);
        segments.push(LayoutGridBorder{
            .rect = layoutRectAt(at, layoutPhysicalSize(t.mode, extent)),
            .color = border.color,
            .style = border.style,
            .side = side,
        });
    };
    // The block offset of each line across the inline axis, and the inline offset of each line along the block axis.
    auto acrossAt = [&](uint32_t line) {
        if (line < t.rowCount)
            return t.gridRow(line).offset;
        const LayoutTableRow& last = t.gridRow(t.rowCount - 1);
        return last.offset + last.blockSize;
    };
    auto alongAt = [&](uint32_t line) {
        if (line < t.columnCount)
            return t.column(line).offset;
        const LayoutTableColumn& last = t.column(t.columnCount - 1);
        return last.offset + last.size;
    };
    // The widest line along the block axis at `line`, in the rows on either side of the line across at `across`.
    auto alongWidth = [&](uint32_t line, uint32_t acrossLine) {
        PrLayoutUnit width = kLayoutUnitZero;
        if (acrossLine != 0)
            width = layoutUnitMax(width, t.alongBorder(line, acrossLine - 1).width);
        if (acrossLine < t.rowCount)
            width = layoutUnitMax(width, t.alongBorder(line, acrossLine).width);
        return width;
    };
    if (t.rowCount == 0 || t.columnCount == 0)
        return 0;
    for (uint32_t line = 0; line <= t.columnCount; ++line) {
        PrLayoutUnit x = alongAt(line);
        for (uint32_t row = 0; row < t.rowCount; ++row) {
            const LayoutTableBorder& border = t.alongBorder(line, row);
            const LayoutTableRow& r = t.gridRow(row);
            push(border, LayoutLogicalOffset{x - startHalf(border.width), r.offset},
                LayoutLogicalSize{border.width, r.blockSize}, along);
        }
    }
    for (uint32_t line = 0; line <= t.rowCount; ++line) {
        PrLayoutUnit y = acrossAt(line);
        for (uint32_t c = 0; c < t.columnCount; ++c) {
            const LayoutTableBorder& border = t.acrossBorder(line, c);
            PrLayoutUnit start = alongAt(c) - startHalf(alongWidth(c, line));
            PrLayoutUnit end = alongAt(c + 1) + endHalf(alongWidth(c + 1, line));
            push(border, LayoutLogicalOffset{start, y - startHalf(border.width)},
                LayoutLogicalSize{end - start, border.width}, across);
        }
    }
    return segments.count - mark;
}

// Appends the fragments of the parts of the table's grid to the scratch children, at their offsets from `at`, the start
// corner of the grid in the table's border box: its column groups, columns, row groups, rows and cells in tree order.
// Each column and column group is as tall as the rows.
void pushParts(Table& t, LayoutLogicalOffset at, const LayoutSpace& cells, PrLayoutVec fallback) {
    RawArray<LayoutLogicalChild>& children = t.state.scratch->children;
    for (uint32_t b = 0; b < t.boxCount; ++b) {
        const LayoutTableColumnBox box = t.box(b);
        if (box.group != kNone)
            continue;
        Area area = areaOf(t, box.firstColumn, box.columnCount, 0, t.rowCount);
        uint32_t boxMark = children.count;
        for (uint32_t k = b + 1; k < t.boxCount && t.box(k).group == b; ++k) {
            const LayoutTableColumnBox column = t.box(k);
            Area columnArea = areaOf(t, column.firstColumn, column.columnCount, 0, t.rowCount);
            const LayoutFragment* fragment = buildPart(t, column.node, children.count, columnArea.size, fallback);
            children.push(LayoutLogicalChild{fragment, columnArea.offset - area.offset});
        }
        const LayoutFragment* fragment = buildPart(t, box.node, boxMark, area.size, fallback);
        children.push(LayoutLogicalChild{fragment, at + area.offset});
    }
    for (uint32_t g = 0; g < t.groupCount; ++g) {
        const LayoutTableGroup group = t.group(g);
        if (group.node == kPrLayoutNodeNone) {
            pushRows(t, g, kLayoutLogicalOffsetZero - at, cells, fallback);
            continue;
        }
        uint32_t first = group.rowCount != 0 ? t.row(group.firstRow).grid : 0;
        Area area = areaOf(t, 0, t.columnCount, first, group.rowCount);
        uint32_t groupMark = children.count;
        pushRows(t, g, area.offset, cells, fallback);
        placeOutOfFlow(t, group.node, kLayoutLogicalOffsetZero);
        const LayoutFragment* fragment = buildPart(t, group.node, groupMark, area.size, fallback);
        LayoutLogicalOffset relative = layoutRelativeOffset(t.state.styles, boxOf(t.state.tree, group.node), cells);
        children.push(LayoutLogicalChild{fragment, at + area.offset + relative});
    }
    placeOutOfFlow(t, t.node, at + LayoutLogicalOffset{
        layoutEdgesContent(t.edges, LayoutSide_InlineStart),
        layoutEdgesContent(t.edges, LayoutSide_BlockStart),
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Captions

// Returns the space that the table's captions are laid out in: the table's border box, of inline size `inlineSize`.
LayoutSpace captionSpace(const Table& t, const LayoutSpace& space, PrLayoutUnit inlineSize) {
    return LayoutSpace{
        .mode = t.mode,
        .available = inlineSize,
        .percentInline = inlineSize,
        .percentBlock = layoutUnit(-1),
        .containing = LayoutLogicalSize{inlineSize, layoutUnit(-1)},
        .blockLimit = layoutUnit(-1),
        .fallback = space.fallback,
    };
}

// Lays out the captions on `side` and appends them to the scratch children, starting at the block offset `at`. Each
// caption follows the one before it, and the margins between them add up rather than collapse. Returns the block offset
// past the last caption and its margin.
PrLayoutUnit pushCaptions(Table& t, const LayoutSpace& captions, PrLayoutCaptionSide side, PrLayoutUnit at) {
    LayoutStrut pending{};
    for (uint32_t i = 0; i < t.captionCount; ++i) {
        LayoutNodeIndex node = t.caption(i).node;
        if (layoutTableStyle(t.state.styles, layoutTreeNode(t.state.tree, node).style).captionSide != side)
            continue;
        LayoutResult result = layoutBlock(t.state, node, captions);
        PrLayoutUnit top = at + layoutStrutSize(pending) + layoutStrutSize(result.start);
        LayoutLogicalOffset relative = layoutRelativeOffset(t.state.styles, boxOf(t.state.tree, node), captions);
        t.state.scratch->children.push(
            LayoutLogicalChild{result.fragment, LayoutLogicalOffset{result.marginInlineStart, top} + relative});
        at = top + layoutLogicalSize(t.mode, result.fragment->extent).blockSize;
        pending = result.end;
    }
    return at + layoutStrutSize(pending);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Tables

LayoutTableScratch* layoutTableScratchCreate(void) {
    return allocZeroed<LayoutTableScratch>();
}

void layoutTableScratchDestroy(LayoutTableScratch* PR_NULLABLE scratch) {
    if (scratch == nullptr)
        return;
    scratch->cells.reset();
    scratch->columns.reset();
    scratch->rows.reset();
    scratch->groups.reset();
    scratch->boxes.reset();
    scratch->captions.reset();
    scratch->outOfFlow.reset();
    scratch->order.reset();
    scratch->borders.reset();
    scratch->segments.reset();
    prMemoryFree(scratch);
}

LayoutTableOutcome layoutTableContent(LayoutState& state, LayoutNodeIndex node, LayoutNodeIndex runLast,
        const LayoutTableInput& input) {
    const LayoutSpace& space = *input.space;
    Table t = beginTable(state, node, runLast);
    ++state.stats->tablesLaidOut;
    buildGrid(t);
    findEdges(t, space.containing.inlineSize);
    const PrLayoutBoxStyle& box = *t.boxStyle;
    LayoutLogicalBox logical = layoutLogicalBox(box, t.mode);
    PrLayoutUnit edgesBlock = layoutEdgesBlock(t.edges);

    // The block size of the grid that the table's style gives it, within its min and max, which its rows fill if they
    // are shorter (css-tables-3/#computing-the-table-height). Its cells' content resolves percentages against the
    // cells' heights if the table's block size is a length, or a percentage of a definite size.
    PrLayoutUnit percentBlock = space.percentBlock;
    PrLayoutUnit styled = kLayoutUnitZero;
    if (layoutResolves(logical.blockSize, percentBlock))
        styled = layoutBorderSize(box, layoutResolve(state.styles, logical.blockSize, percentBlock), edgesBlock);
    if (layoutResolves(logical.maxBlockSize, percentBlock)) {
        PrLayoutUnit max = layoutResolve(state.styles, logical.maxBlockSize, percentBlock);
        styled = layoutUnitMin(styled, layoutBorderSize(box, max, edgesBlock));
    }
    if (layoutResolves(logical.minBlockSize, percentBlock)) {
        PrLayoutUnit min = layoutResolve(state.styles, logical.minBlockSize, percentBlock);
        styled = layoutUnitMax(styled, layoutBorderSize(box, min, edgesBlock));
    }
    bool definiteCells = layoutResolves(logical.blockSize, percentBlock);

    PrLayoutUnit contentInline = layoutUnitMax(kLayoutUnitZero, input.inlineSize - layoutEdgesInline(t.edges));
    t.cellInput = LayoutIntrinsicInput{t.mode, layoutUnit(-1), layoutUnit(-1), space.fallback};
    measureCells(t);
    measureColumns(t);
    placeColumns(t, input.inlineSize);
    LayoutSpace cells = cellSpace(t, space, contentInline, layoutUnit(-1));
    measureRows(t, cells);
    sizeRows(t);

    // The captions come first, so that the block size that a container gives the table leaves the rest to its grid.
    LayoutSpace captions = captionSpace(t, space, input.inlineSize);
    RawArray<LayoutLogicalChild>& children = state.scratch->children;
    PrLayoutUnit gridStart = pushCaptions(t, captions, PrLayoutCaptionSide_Top, kLayoutUnitZero);
    uint32_t bottomMark = children.count;
    PrLayoutUnit bottoms = pushCaptions(t, captions, PrLayoutCaptionSide_Bottom, kLayoutUnitZero);
    // A block size that the container decides replaces the one that the table's style gives, because the container has
    // already applied the style. The size of the content ignores the style. In either case, the rows can make the grid
    // taller.
    if (input.blockKind == LayoutRequestKind_Fixed)
        styled = input.blockSize - gridStart - bottoms;
    else if (input.blockKind == LayoutRequestKind_Content)
        styled = kLayoutUnitZero;
    distributeHeight(t, layoutUnitMax(kLayoutUnitZero, styled - edgesBlock));
    PrLayoutUnit removed = collapseRows(t);
    PrLayoutUnit gridBlock = layoutUnitMax(placeRows(t), styled - removed);
    PrLayoutUnit gridContent = gridBlock - edgesBlock;
    LayoutSpace definite = cellSpace(t, space, contentInline, definiteCells ? gridContent : layoutUnit(-1));
    layOutCells(t, definite, definiteCells);

    // The grid's parts go between the top and bottom captions, and the bottom captions follow them in the scratch
    // children.
    PrLayoutUnit gridEnd = gridStart + gridBlock;
    for (uint32_t i = bottomMark; i < children.count; ++i)
        children.data[i].offset.blockOffset += gridEnd;
    uint32_t bottomCount = children.count - bottomMark;
    LayoutTableScratch& s = t.scratch();
    RawArray<LayoutLogicalChild> bottom;
    bottom.append(children.data + bottomMark, bottomCount);
    children.count = bottomMark;
    LayoutLogicalOffset gridAt{kLayoutUnitZero, gridStart};
    pushParts(t, gridAt, definite, space.fallback);
    children.append(bottom.span());
    bottom.reset();

    LayoutTableOutcome outcome{};
    outcome.blockSize = gridEnd + bottoms;
    LayoutLogicalSize size{input.inlineSize, outcome.blockSize};
    LayoutLogicalSize gridSize{input.inlineSize, gridBlock};
    outcome.grid =
        layoutRectAt(layoutPhysicalOffset(t.mode, gridAt, gridSize, size), layoutPhysicalSize(t.mode, gridSize));
    outcome.borderMark = s.segments.count;
    outcome.borderCount = t.collapsed ? collectSegments(t, size, gridAt) : 0;
    // The table's baselines are those of its first and last rows that cells start in (css-tables-3/#row-layout).
    uint32_t firstRow = 0;
    while (firstRow < t.rowCount && t.gridRow(firstRow).cellCount == 0)
        ++firstRow;
    uint32_t lastRow = t.rowCount;
    while (lastRow > firstRow && t.gridRow(lastRow - 1).cellCount == 0)
        --lastRow;
    if (firstRow < t.rowCount) {
        PrLayoutUnit first = gridStart + t.gridRow(firstRow).offset + rowBaseline(t, firstRow);
        PrLayoutUnit last = gridStart + t.gridRow(lastRow - 1).offset + rowBaseline(t, lastRow - 1);
        outcome.first = layoutBaselineAt(first, kLayoutBaselineSynthesized);
        outcome.last = layoutBaselineAt(last, kLayoutBaselineSynthesized);
    }
    endTable(t);
    return outcome;
}

LayoutFragment* PR_NULLABLE layoutTableFinish(LayoutState& state, LayoutFragment* PR_NULLABLE built,
        const LayoutTableOutcome& outcome) {
    RawArray<LayoutGridBorder>& segments = state.scratch->table->segments;
    LayoutFragment* fragment = built;
    if (built != nullptr)
        fragment = layoutFragmentTable(built, outcome.grid, segments.data + outcome.borderMark, outcome.borderCount);
    segments.count = outcome.borderMark;
    return fragment;
}

LayoutIntrinsicSizes layoutTableIntrinsic(LayoutState& state, LayoutNodeIndex node, LayoutNodeIndex runLast,
        const LayoutIntrinsicInput& input) {
    Table t = beginTable(state, node, runLast);
    buildGrid(t);
    findEdges(t, layoutUnit(-1));
    t.cellInput = LayoutIntrinsicInput{t.mode, layoutUnit(-1), layoutUnit(-1), input.fallback};
    measureCells(t);
    measureColumns(t);
    GridWidths grid = gridWidths(t);
    LayoutIntrinsicSizes sizes{
        .minContent = grid.minContent,
        .maxContent = grid.maxContent,
        .fillsAvailable = grid.unbounded,
    };
    // The captions' min-content contributions floor both sizes (css-tables-3/#computing-the-table-width).
    LayoutIntrinsicInput captions{t.mode, layoutUnit(-1), layoutUnit(-1), input.fallback};
    for (uint32_t i = 0; i < t.captionCount; ++i) {
        LayoutIntrinsicSizes caption = layoutContribution(state, t.caption(i).node, captions);
        sizes.minContent = layoutUnitMax(sizes.minContent, caption.minContent);
        sizes.readsFallback = sizes.readsFallback || caption.readsFallback;
    }
    // A table in fixed mode whose width is a percentage has an unbounded max-content width, so that its container grows
    // to whatever its own container allows. css-tables-3 leaves this undefined.
    PrLayoutLength width = layoutLogicalBox(*t.boxStyle, t.mode).inlineSize;
    if (t.fixed && layoutLengthReadsBase(width))
        sizes.maxContent = layoutUnit(kColumnMax);
    sizes.maxContent = layoutUnitMax(sizes.maxContent, sizes.minContent);
    endTable(t);
    return sizes;
}

bool layoutTableCellIsEmpty(const LayoutFragment* fragment) {
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        if (!layoutFragmentIsPoint(fragment->children[i].fragment))
            return false;
    }
    return true;
}

bool layoutTableCellCollapses(const LayoutTree* tree, LayoutNodeIndex node) {
    for (LayoutNodeIndex at = layoutCachePlacement(tree->cache, node).parent; at != kPrLayoutNodeNone;
            at = layoutCachePlacement(tree->cache, at).parent) {
        PrLayoutStyleIndex style = layoutTreeNode(tree, at).style;
        PrLayoutDisplay display = layoutBoxStyle(tree->styles, style).display;
        if (display == PrLayoutDisplay_Table || display == PrLayoutDisplay_InlineTable)
            return layoutTableStyle(tree->styles, style).borderCollapse == PrLayoutBorderCollapse_Collapse;
        if (!layoutDisplayIsRowGroup(display) && display != PrLayoutDisplay_TableRow)
            break;
    }
    PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
    return layoutTableStyle(tree->styles, style).borderCollapse == PrLayoutBorderCollapse_Collapse;
}

LayoutTablePart layoutTablePartFromDisplay(const LayoutTree* tree, PrLayoutDisplay display, LayoutNodeIndex child) {
    switch (layoutChildKind(tree, child)) {
        case LayoutChildKind_None: return LayoutTablePart_None;
        case LayoutChildKind_OutOfFlow: return LayoutTablePart_OutOfFlow;
        case LayoutChildKind_TablePart: break;
        default: return LayoutTablePart_Other;
    }
    bool root = display == PrLayoutDisplay_Table || display == PrLayoutDisplay_InlineTable;
    switch (layoutBoxStyle(tree->styles, layoutTreeNode(tree, child).style).display) {
        case PrLayoutDisplay_TableCaption: return root ? LayoutTablePart_Caption : LayoutTablePart_Other;
        case PrLayoutDisplay_TableColumnGroup: return root ? LayoutTablePart_ColumnGroup : LayoutTablePart_Other;
        case PrLayoutDisplay_TableColumn:
            return root || display == PrLayoutDisplay_TableColumnGroup ? LayoutTablePart_Column : LayoutTablePart_Other;
        case PrLayoutDisplay_TableRowGroup:
        case PrLayoutDisplay_TableHeaderGroup:
        case PrLayoutDisplay_TableFooterGroup: return root ? LayoutTablePart_RowGroup : LayoutTablePart_Other;
        case PrLayoutDisplay_TableRow:
            return display != PrLayoutDisplay_TableRow ? LayoutTablePart_Row : LayoutTablePart_Other;
        default: return LayoutTablePart_Cell;
    }
}

bool layoutTableRunIsBlank(const LayoutTree* tree, LayoutNodeIndex first, LayoutNodeIndex last) {
    for (uint32_t c = first;; c = layoutTreeNode(tree, c).next) {
        LayoutChildKind kind = layoutChildKind(tree, c);
        bool blank;
        if (layoutTreeNode(tree, c).kind == PrLayoutNodeKind_Text)
            blank = layoutIsBlankText(tree, c);
        else
            blank = kind == LayoutChildKind_None || kind == LayoutChildKind_OutOfFlow;
        if (!blank)
            return false;
        if (c == last)
            return true;
    }
}

} // namespace pr_impl
