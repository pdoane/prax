// Tables: the elements of a table, and the operations that build one from script
//
// html - https://html.spec.whatwg.org/multipage/tables.html
//
// A table's structure is its element's children, so each call reads and changes the tree at once. A collection that a
// call creates is live. Its caller destroys it. An element that a call returns stays alive while it is in a tree.

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The interfaces that this header declares, each named for its web interface.
typedef struct PrHtmlTableElement PrHtmlTableElement;
typedef struct PrHtmlTableSectionElement PrHtmlTableSectionElement;
typedef struct PrHtmlTableRowElement PrHtmlTableRowElement;
typedef struct PrHtmlTableCellElement PrHtmlTableCellElement;

// Return `object` as each interface, or null if it is not one. A section is a thead, tbody or tfoot element, and a cell
// is a td or th element.
PrHtmlTableElement* PR_NULLABLE prHtmlTableElementQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlTableSectionElement* PR_NULLABLE prHtmlTableSectionElementQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlTableRowElement* PR_NULLABLE prHtmlTableRowElementQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlTableCellElement* PR_NULLABLE prHtmlTableCellElementQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTableElement : HTMLElement (html)

// The children of a table of which it has one in its object model.
enum {
    PrHtmlTablePart_Caption,
    PrHtmlTablePart_THead,
    PrHtmlTablePart_TFoot,
};
typedef uint8_t PrHtmlTablePart;

// Returns the first child of the table that is the part, or null if there is none (html/#dom-table-caption).
PrDomElement* PR_NULLABLE prHtmlTableElementPart(PrHtmlTableElement* table, PrHtmlTablePart part);

// Removes the first child that is the part, then inserts `element` where the part belongs, unless it is null. Returns
// WrongType for an element whose interface is not the part's, and NotAllowed for a section of another kind than the
// part. Either changes nothing.
PrStatus prHtmlTableElementSetPart(PrHtmlTableElement* table, PrHtmlTablePart part, PrDomElement* PR_NULLABLE element);

// Returns the first child that is the part, after creating it where it belongs if there is none.
PrDomElement* prHtmlTableElementEnsurePart(PrHtmlTableElement* table, PrHtmlTablePart part);

// Removes the first child that is the part, if there is one.
void prHtmlTableElementDeletePart(PrHtmlTableElement* table, PrHtmlTablePart part);

// Creates the live collection of the tbody children (html/#dom-table-tbodies).
PrDomHtmlCollection* prHtmlTableElementTBodies(PrHtmlTableElement* table);

// Inserts a new tbody after the last tbody child, or at the end of the table if there is none, and returns it.
PrDomElement* prHtmlTableElementAddTBody(PrHtmlTableElement* table);

// Creates the live collection of the table's rows: those of its thead children, then those that are its children or are
// in its tbody children, then those of its tfoot children (html/#dom-table-rows).
PrDomHtmlCollection* prHtmlTableElementRows(PrHtmlTableElement* table);

// Inserts a new row at `index` of the table's rows, or after the last row for -1, and writes it to `out`
// (html/#dom-table-insertrow). Returns OutOfRange for any other index outside the rows.
PrStatus prHtmlTableElementInsertRow(PrHtmlTableElement* table, int32_t index, PrDomElement* PR_NULLABLE* PR_NONNULL out);

// Removes the row at `index`, or the last row if there is one for -1 (html/#dom-table-deleterow). Returns OutOfRange
// for any other index outside the rows.
PrStatus prHtmlTableElementDeleteRow(PrHtmlTableElement* table, int32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTableSectionElement : HTMLElement (html)

// Creates the live collection of the section's tr children (html/#dom-tbody-rows).
PrDomHtmlCollection* prHtmlTableSectionElementRows(PrHtmlTableSectionElement* section);

// Insert and remove a row, with the indices of prHtmlTableElementInsertRow and prHtmlTableElementDeleteRow.
PrStatus prHtmlTableSectionElementInsertRow(PrHtmlTableSectionElement* section, int32_t index,
    PrDomElement* PR_NULLABLE* PR_NONNULL out);
PrStatus prHtmlTableSectionElementDeleteRow(PrHtmlTableSectionElement* section, int32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTableRowElement : HTMLElement (html)

// Return the row's position in its table's rows and in its parent's rows, or -1 if it has no such table or parent
// (html/#dom-tr-rowindex, html/#dom-tr-sectionrowindex).
int32_t prHtmlTableRowElementRowIndex(const PrHtmlTableRowElement* row);
int32_t prHtmlTableRowElementSectionRowIndex(const PrHtmlTableRowElement* row);

// Creates the live collection of the row's td and th children (html/#dom-tr-cells).
PrDomHtmlCollection* prHtmlTableRowElementCells(PrHtmlTableRowElement* row);

// Insert a new td and remove a cell, with the indices of prHtmlTableElementInsertRow and prHtmlTableElementDeleteRow
// (html/#dom-tr-insertcell).
PrStatus prHtmlTableRowElementInsertCell(PrHtmlTableRowElement* row, int32_t index,
    PrDomElement* PR_NULLABLE* PR_NONNULL out);
PrStatus prHtmlTableRowElementDeleteCell(PrHtmlTableRowElement* row, int32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTableCellElement and HTMLTableColElement : HTMLElement (html)

// Returns the cell's position in its parent row's cells, or -1 if its parent is not a row (html/#dom-tdth-cellindex).
int32_t prHtmlTableCellElementCellIndex(const PrHtmlTableCellElement* cell);

// Return the column and row span, as a table's layout clamps them (html/#dom-tdth-colspan).
uint32_t prHtmlTableCellElementColSpan(const PrHtmlTableCellElement* cell);
uint32_t prHtmlTableCellElementRowSpan(const PrHtmlTableCellElement* cell);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
