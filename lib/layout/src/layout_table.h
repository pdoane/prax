// Table layout: a table's grid of rows and columns, sized from its cells, with its captions (css-tables-3/#layout).
//
// CSS2 - https://drafts.csswg.org/css2/
// css-tables-3 - https://drafts.csswg.org/css-tables-3/
//
// A table is a block-level or inline-level box. Block layout lays it out as it lays out any box that establishes an
// independent formatting context, and this file lays out its content. The table's fragment is its wrapper box
// (LayoutFragmentFlag_Table). The fragment's children are the table's captions and the parts of its grid: its column
// groups, columns, row groups, rows and cells. The fragment also holds the rectangle of the grid, which the table's
// decoration paints over, and the grid's collapsed borders.
//
// The grid is found again from the tree at each layout, with the anonymous boxes that fix up the table's structure
// (css-tables-3/#fixup-algorithm). Rows that are children of the table are in a row group with no box. Cells that are
// children of a table or a row group are in a row with no box. Each run of other content is in an anonymous cell. A
// part with no box has no fragment, and the fragments of its children are children of the fragment around it. An
// anonymous cell is the exception: it is an anonymous block box, with a fragment.
//
// A table lays out each cell and caption through the cell's or caption's own cache, as flex layout does with its items.
// A cell is first measured at the width of its columns, for the block size and baselines of its content. It is then
// laid out at the height of its rows, with its content aligned by its vertical-align (LayoutSpace::cellAligned). A cell
// whose final size is the size it was measured at takes the layout of its measurement (layoutFinal). An anonymous cell
// is laid out again at each layout of its table.

#pragma once

#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutTableScratch;

// Creates empty scratch memory, and frees it.
LayoutTableScratch* layoutTableScratchCreate(void);
void layoutTableScratchDestroy(LayoutTableScratch* PR_NULLABLE scratch);

// Frees the tables that the running layout sized for its fragmentation contexts. A table across many fragmentainers is
// sized once, by its first, and the others take its rows from the memo, so they lay out only the rows that each holds.
// Nothing changes during a layout, so the memo holds for the whole layout, and its end frees it.
void layoutTableEndLayout(LayoutTableScratch* PR_NULLABLE scratch);

// The inputs to the layout of a table's parts. `space` is the space that the table is laid out in. `inlineSize` is the
// inline size of the table's border box, which its captions also take.
//
// `blockKind` says what sets the table's block size, which the rows fill if they are shorter. With
// LayoutRequestKind_Own, the table's style sets it. With LayoutRequestKind_Fixed, the container decides it as
// `blockSize`. With LayoutRequestKind_Content, nothing sets it, and the table is as tall as its rows and captions.
struct LayoutTableInput {
    const LayoutSpace* space;
    PrLayoutUnit inlineSize;
    LayoutRequestKind blockKind;
    PrLayoutUnit blockSize;     // the border-box size if `blockKind` is LayoutRequestKind_Fixed
};

// The result of laying out a table's parts. `blockSize` is the block size of the table's border box, which holds its
// captions and its grid. `first` and `last` are the baselines of its first and last rows, from the block-start edge of
// its border box. `grid` is the rectangle of its grid, from the top left of its border box. The grid's `borderCount`
// collapsed borders are in the table's scratch memory from `borderMark`, until layoutTableFinish.
struct LayoutTableOutcome {
    PrLayoutUnit blockSize;
    LayoutBaseline first;
    LayoutBaseline last;
    PrLayoutRect grid;
    uint32_t borderMark;
    uint32_t borderCount;

    // In a fragmentation context, where the table resumes in the next fragmentainer, a reference that the caller takes,
    // or null if its rows end in this one.
    const LayoutBreakToken* PR_NULLABLE token;
};

// Lays out the parts of the table `node` in its own writing mode. Appends its captions and the parts of its grid to the
// scratch children, at their logical offsets from the start corner of its border box. A child out of flow of the table,
// or of a part with no fragment, is held at the start of the grid's content box. If `runLast` is not kPrLayoutNodeNone,
// the table is the anonymous table that wraps the run of table parts from `node` through `runLast`
// (layoutTablePartRunLast), in the writing mode of their parent.
LayoutTableOutcome layoutTableContent(LayoutState& state, LayoutNodeIndex node, LayoutNodeIndex runLast,
    const LayoutTableInput& input);

// Makes the table's fragment from `built`, which the function takes, with the grid that `outcome` describes
// (layoutFragmentTable), and returns it. Frees the scratch memory that holds the collapsed borders. A layout that
// builds no fragment, such as a measurement, passes null, and the function only frees the memory.
LayoutFragment* PR_NULLABLE layoutTableFinish(LayoutState& state, LayoutFragment* PR_NULLABLE built,
    const LayoutTableOutcome& outcome);

// Returns the intrinsic inline sizes of the border box of the table `node` (css-tables-3/#computing-the-table-width).
// If `runLast` is not kPrLayoutNodeNone, the table is the anonymous table that wraps the run from `node` through
// `runLast`. The table's parts are measured in `input`. The sizes are those of the grid, from the sizes of its columns,
// its border spacing and the table's edges, and are at least the min-content contributions of the captions.
LayoutIntrinsicSizes layoutTableIntrinsic(LayoutState& state, LayoutNodeIndex node, LayoutNodeIndex runLast,
    const LayoutIntrinsicInput& input);

// Returns true if a table cell laid out as `fragment` is empty, as empty-cells defines it: the cell has no float, and
// no content in flow other than collapsed white space (css-tables-3/#empty-cell-rendering).
bool layoutTableCellIsEmpty(const LayoutFragment* fragment);

// Returns true if the table cell `node` is in a table that collapses its borders. The table's grid then draws the
// cell's borders. The function finds the table through the cell's ancestors. A cell in an anonymous table reads the
// border model from its own style instead.
bool layoutTableCellCollapses(const LayoutTree* tree, LayoutNodeIndex node);

// What a child of a table container is to its table's structure (css-tables-3/#fixup-algorithm). A part that belongs in
// another kind of container, such as a caption in a row, is other content there.
enum {
    LayoutTablePart_None,           // it generates no box
    LayoutTablePart_Caption,
    LayoutTablePart_ColumnGroup,
    LayoutTablePart_Column,
    LayoutTablePart_RowGroup,
    LayoutTablePart_Row,
    LayoutTablePart_Cell,
    LayoutTablePart_OutOfFlow,      // a box out of flow, whose place a placeholder holds
    LayoutTablePart_Other,          // content that an anonymous cell holds
};
typedef uint8_t LayoutTablePart;

// Returns the part that `child` is in its table's structure. `child` is a child of a table container whose display is
// `display`.
LayoutTablePart layoutTablePartFromDisplay(const LayoutTree* tree, PrLayoutDisplay display, LayoutNodeIndex child);

// Returns true if the run of siblings from `first` through `last` holds no content in flow other than white space.
// Fixup discards such a run (css-tables-3/#fixup-algorithm).
bool layoutTableRunIsBlank(const LayoutTree* tree, LayoutNodeIndex first, LayoutNodeIndex last);

// Visits the parts of the table container `node` (a table, a row group or a row) that are laid out apart from the
// table, in tree order. Calls `box` with each caption and cell, and `run` with the first and last node of each run of
// other content, which an anonymous cell holds. A run holds the children out of flow among its content. Enters row
// groups and rows, and skips columns and column groups.
template <typename Box, typename Run>
void layoutEachTableBox(const LayoutTree* tree, LayoutNodeIndex node, Box box, Run run) {
    const LayoutTreeNode& container = layoutTreeNode(tree, node);
    PrLayoutDisplay display = layoutBoxStyle(tree->styles, container.style).display;
    LayoutNodeIndex runFirst = kPrLayoutNodeNone;
    LayoutNodeIndex runLast = kPrLayoutNodeNone;
    auto endRun = [&] {
        if (runFirst != kPrLayoutNodeNone && !layoutTableRunIsBlank(tree, runFirst, runLast))
            run(runFirst, runLast);
        runFirst = kPrLayoutNodeNone;
    };
    for (uint32_t c = container.first; c != kPrLayoutNodeNone; c = layoutTreeNode(tree, c).next) {
        LayoutTablePart part = layoutTablePartFromDisplay(tree, display, c);
        bool extends = runFirst != kPrLayoutNodeNone &&
            (part == LayoutTablePart_None || part == LayoutTablePart_OutOfFlow);
        if (part == LayoutTablePart_Other || extends) {
            if (runFirst == kPrLayoutNodeNone)
                runFirst = c;
            runLast = c;
            continue;
        }
        endRun();
        switch (part) {
            case LayoutTablePart_Caption:
            case LayoutTablePart_Cell: box(c); break;
            case LayoutTablePart_RowGroup:
            case LayoutTablePart_Row: layoutEachTableBox(tree, c, box, run); break;
            default: break;
        }
    }
    endRun();
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
