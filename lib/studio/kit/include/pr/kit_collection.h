// Studio Kit's virtualized lists, their selection, trees and tables.
//
// A list view is a scroll container over rows of one height, and declares only the window of rows that the scrollport
// shows. The caller asks the view for its window (prKitListWindow) and creates those rows under the view, and spacers
// stand in for the rows before and after the window, so that the scroll range covers every row. Selection membership
// belongs to the application: a view edits a cursor that the application binds, and reports each gesture as an edit
// that the application applies to its own storage (prKitSelectionEdited). A tree view is a list view over the visible
// rows of a tree, which the application flattens in display order, and also reports folds and drops. A table is a list
// view whose rows are cut into columns under a header of their headings.

#pragma once

#include "pr/color.h"
#include "pr/kit.h"
#include "pr/kit_icon.h"
#include "pr/kit_image.h"
#include "pr/kit_text.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Selection

// The absent index of a row.
static const uint32_t kPrKitRowNone = 0xffffffffu;

// How an edit's rows join the application's selection.
enum {
    PrKitSelectionOp_None,
    PrKitSelectionOp_Replace,       // the rows become the selection
    PrKitSelectionOp_Add,           // the rows join the selection
    PrKitSelectionOp_Toggle,        // each row leaves the selection if it is in it, and joins it otherwise
};
typedef uint8_t PrKitSelectionOp;

// The rows that one gesture covers, `count` rows from `first`, and how they join the selection.
typedef struct PrKitSelectionEdit {
    PrKitSelectionOp op;
    uint32_t first;
    uint32_t count;
} PrKitSelectionEdit;

// A selection's cursor, which the application keeps and a view edits. A range gesture spans from `anchor` to the row
// that it reaches. `lead` is the row that the keyboard moves from.
typedef struct PrKitSelection {
    uint32_t anchor;
    uint32_t lead;
} PrKitSelection;

// Applies `edit` to `selected`, an array of `count` flags by row.
void prKitSelectionApply(const PrKitSelectionEdit* edit, bool* selected, uint32_t count);

// ---------------------------------------------------------------------------------------------------------------------
// List views

// The content of a list view: `count` rows, and the selection that it edits. A view bound to a selection takes focus,
// and its keys and clicks on its rows edit the selection. The arrows, Home, End and the page keys move the lead, and
// with Shift they extend the range from the anchor; in a grid, Up and Down move by a row of tiles, and Left and Right
// by a tile. Space toggles the lead. Command or Control with A selects every row. An unbound view shows rows and takes
// no keys.
typedef struct PrKitListView {
    uint64_t key;
    uint32_t count;
    PrKitSelection* PR_NULLABLE selection;
} PrKitListView;

// The style of a list, tree or table view: the height of its rows, its placement in its parent, and, for a grid, the
// width of its tiles. If `tileWidth` is above zero, the view is a grid of tiles, as many to a row as fit in the view's
// width at `tileWidth` px each, which share the row's width. Zero rows are a small control tall.
typedef struct PrKitListViewStyle {
    float rowHeight;                // px
    float tileWidth;                // px; above zero for a grid of tiles
    PrKitPlace place;
} PrKitListViewStyle;

// Creates a list view under `parent`, applies the round's keys to its selection, and scrolls the lead into view when a
// key moves it. Returns the view, under which the caller creates the rows of its window.
PrLayoutNode prKitListView(PrKit* kit, PrLayoutNode parent, PrKitListView list, PrKitListViewStyle style);

// The rows that a view shows in a round: `count` rows from `first`.
typedef struct PrKitWindow {
    uint32_t first;
    uint32_t count;
} PrKitWindow;

// Returns the window of a list, tree or table view: the rows that its scrollport showed at the last layout, scrolled to
// where the round's wheel and keys left it, plus a few rows on either side. The caller creates them in order.
PrKitWindow prKitListWindow(const PrKit* kit, PrLayoutNode view);

// Returns true if a key or a click on a row edited the selection of a list, tree or table view in the current round,
// and writes the edit to `edit`. The caller applies the edit once it has created the rows.
bool prKitSelectionEdited(const PrKit* kit, PrLayoutNode view, PrKitSelectionEdit* edit);

// A row of a list or a tree view: row `index` of the view, which shows `item`, and keys the row by it, so that
// scrolling keeps its node.
typedef struct PrKitRow {
    uint32_t index;
    uint64_t item;
    bool selected;
} PrKitRow;

// Creates a row of a list view. A click edits the view's selection: a plain click replaces the selection with the row,
// Shift extends the range from the anchor, and Command or Control toggles the row, or adds the range with Shift.
//
// A row is a line of cells that the caller fills: what it shows goes under it, in order, from the row's cells below or
// any of the kit's widgets, such as a status (prKitStatus) or a visibility toggle (prKitVisibilityToggle).
PrLayoutNode prKitListRow(PrKit* kit, PrLayoutNode view, PrKitRow row);

// Creates a row's icon, in the accent while the row is selected and muted otherwise.
PrLayoutNode prKitRowIcon(PrKit* kit, PrLayoutNode row, PrKitIcon icon);

// Creates a row's label, in `tone`: one line that takes the room that the row's other cells leave, and clips. A label
// before other cells puts them at the row's end.
PrLayoutNode prKitRowLabel(PrKit* kit, PrLayoutNode row, PrKitLabel label, PrKitTone tone);

// Creates a row's meta text, such as a type, a count or a size, faint and on one line.
PrLayoutNode prKitRowMeta(PrKit* kit, PrLayoutNode row, PrKitLabel label);

// A tile of a grid: tile `index` of the view, which shows `item`, and is keyed by it.
typedef struct PrKitTile {
    uint32_t index;
    uint64_t item;
    bool selected;
} PrKitTile;

// Creates a tile of a grid, which selects in the same way as prKitListRow. A tile is a column that the caller fills
// from the tile's parts below or any of the kit's widgets: a thumbnail, then a label.
PrLayoutNode prKitListTile(PrKit* kit, PrLayoutNode view, PrKitTile tile);

// Create a tile's thumbnail: `image` fitted to cover it, or `icon` on a ground of `tint`, or of the theme's raised
// ground if `tint` is fully transparent.
PrLayoutNode prKitTileImage(PrKit* kit, PrLayoutNode tile, PrKitImage image);
PrLayoutNode prKitTileIcon(PrKit* kit, PrLayoutNode tile, PrKitIcon icon, PrColor tint);

// Creates a tile's label: one centered line that clips, brighter while the tile is selected.
PrLayoutNode prKitTileLabel(PrKit* kit, PrLayoutNode tile, PrKitLabel label);

// The content of an empty list's notice: an icon, if any, above a title, above a hint, if any.
typedef struct PrKitEmptyState {
    uint64_t key;
    PrKitIconKind icon;
    PrStringView title;
    PrStringView hint;
} PrKitEmptyState;

// Creates the notice of an empty list, which fills `parent`.
PrLayoutNode prKitEmptyState(PrKit* kit, PrLayoutNode parent, PrKitEmptyState empty);

// ---------------------------------------------------------------------------------------------------------------------
// Tree views

// One visible row of a tree, in display order. The row's parent is the nearest earlier row with a lesser depth.
typedef struct PrKitTreeItem {
    uint32_t depth;
    bool hasChildren;
    bool open;                      // shows its children, which are the following rows with a greater depth
} PrKitTreeItem;

// Where a drop lands, chosen by which third of the row the pointer is over along the view's axis.
enum {
    PrKitDropZone_None,
    PrKitDropZone_Before,           // a sibling before the row
    PrKitDropZone_Inside,           // a child of the row
    PrKitDropZone_After,            // a sibling after the row
};
typedef uint8_t PrKitDropZone;

// A request to open or close row `row`.
typedef struct PrKitTreeFold {
    uint32_t row;
    bool open;
} PrKitTreeFold;

// A row dropped on row `row` in `zone`. `item` is the dragged row's item, which is the payload's value. The application
// checks that the move is legal and applies it. For example, a row dropped into its own subtree is not legal.
typedef struct PrKitTreeDrop {
    uint64_t item;
    uint32_t row;
    PrKitDropZone zone;
} PrKitTreeDrop;

// The content of a tree view: `count` visible rows, described by `items`, which must stay valid through the round, and
// the selection that it edits. The view moves the lead in the same way as a list view. Right and Left walk the tree:
// Right opens a closed row and steps into an open one, and Left closes an open row and climbs to the parent of any
// other row. If `dragKind` is nonzero, rows drag as payloads of that kind that carry their items, and take drops of
// that kind.
typedef struct PrKitTreeView {
    uint64_t key;
    uint32_t count;
    PrKitSelection* PR_NULLABLE selection;
    const PrKitTreeItem* PR_NULLABLE items;
    uint64_t dragKind;
} PrKitTreeView;

// Creates a tree view under `parent`, in the same way that prKitListView creates a list.
PrLayoutNode prKitTreeView(PrKit* kit, PrLayoutNode parent, PrKitTreeView tree, PrKitListViewStyle style);

// Creates a row of a tree view, in the same way as prKitListRow. The row is indented by its depth and led by its
// disclosure, a cell whose click asks for a fold and selects nothing. The row marks where a drop of the view's kind
// would land. The caller fills the row after its disclosure, in the same way as a list's row.
PrLayoutNode prKitTreeRow(PrKit* kit, PrLayoutNode view, PrKitRow row);

// Returns true if a key or a disclosure asked to open or close a row of the tree view in the current round, and writes
// the request to `fold`. The caller applies it once it has created the rows.
bool prKitFolded(const PrKit* kit, PrLayoutNode view, PrKitTreeFold* fold);

// Returns true if a row of the view's kind was dropped on a row of the tree view in the current round, and writes the
// drop to `drop`.
bool prKitRowDropped(const PrKit* kit, PrLayoutNode view, PrKitTreeDrop* drop);

// Returns where a drop on `node` lands, from which third of its box the pointer was over at the last layout, along the
// column's axis. Returns None if the pointer is not over the node.
PrKitDropZone prKitDropZone(PrKit* kit, PrLayoutNode node);

// The content of a row's visibility toggle: an eye, which is closed if `hidden` is true. The toggle is shown if the row
// is hidden or `revealed` is true, as it is while the pointer is over the row.
typedef struct PrKitVisibilityToggle {
    uint64_t key;
    bool hidden;
    bool revealed;
} PrKitVisibilityToggle;

// Creates a row's visibility toggle. A click activates it (prKitActivated).
PrLayoutNode prKitVisibilityToggle(PrKit* kit, PrLayoutNode parent, PrKitVisibilityToggle toggle);

// ---------------------------------------------------------------------------------------------------------------------
// Tables

// The absent index of a column.
static const uint32_t kPrKitColumnNone = 0xffffffffu;

// A table's column: its heading, its width, and where its heading and cells sit. A column is `width` px wide, or with
// Fill it is at least `width` px wide and shares the room that the other columns leave by `weight`. Columns wider than
// the table scroll it sideways, and the header scrolls with them.
typedef struct PrKitTableColumn {
    PrStringView label;
    float width;
    PrKitFlex flex;                 // Fill, or zero for a fixed width
    float weight;                   // Fill: the share; 0 means 1
    PrKitTextAlign align;
    bool sortable;                  // a click on the heading sorts by the column
} PrKitTableColumn;

// The column that the application sorts a table's rows by, which a click on a sortable heading edits: a click on the
// sorted column reverses the order, and a click on another column sorts by it in ascending order.
typedef struct PrKitTableSort {
    uint32_t column;                // kPrKitColumnNone for none
    bool descending;
} PrKitTableSort;

// The content of a table: `count` rows in `columnCount` columns, which must stay valid through the round, the selection
// that its rows edit as a list view's rows do, and the sort that its headings edit. A table bound to a sort marks the
// sorted column's heading.
typedef struct PrKitTable {
    uint64_t key;
    uint32_t count;
    PrKitSelection* PR_NULLABLE selection;
    const PrKitTableColumn* columns;
    uint32_t columnCount;
    PrKitTableSort* PR_NULLABLE sort;
} PrKitTable;

// Creates a table under `parent`: its header, and a list view of its rows in the same way that prKitListView creates
// one. Applies a heading's click to the table's sort. Returns the table, under which the caller creates the rows of its
// window (prKitListWindow).
PrLayoutNode prKitTable(PrKit* kit, PrLayoutNode parent, PrKitTable table, PrKitListViewStyle style);

// Returns true if a heading's click changed the table's sort in the current round, so that the application orders its
// rows again before it creates them.
bool prKitSorted(const PrKit* kit, PrLayoutNode table);

// The content of a table's row: row `index` of the table, which shows `item`, and is keyed by it.
typedef struct PrKitTableRow {
    uint32_t index;
    uint64_t item;
    bool selected;
} PrKitTableRow;

// Creates a row of a table, which selects in the same way as prKitListRow. The caller creates the row's cells under it
// in column order.
PrLayoutNode prKitTableRow(PrKit* kit, PrLayoutNode table, PrKitTableRow row);

// The content of a table's cell: the cell of `column`, which shows an icon, if any, and text in `role` and `tone`,
// which the cell clips.
typedef struct PrKitTableCell {
    uint32_t column;
    PrStringView text;
    PrKitIconKind icon;
    PrKitTextRole role;
    PrKitTone tone;
} PrKitTableCell;

// Creates a cell of a table's row, sized and aligned as its column is. Returns the cell, which can take more children
// after its text.
PrLayoutNode prKitTableCell(PrKit* kit, PrLayoutNode row, PrKitTableCell cell);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
