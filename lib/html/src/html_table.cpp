// Tables: the elements of a table, and the operations that build one from script (html/#tables)

#include "pr/html_table.h"

#include "dom/src/dom_collection.h"
#include "dom/src/dom_node.h"
#include "html/src/html_interface.h"

namespace pr_impl {

namespace {

PrDomTag partTag(PrHtmlTablePart part) {
    switch (part) {
        case PrHtmlTablePart_Caption: return PrDomTag_Caption;
        case PrHtmlTablePart_THead: return PrDomTag_Thead;
        default: return PrDomTag_Tfoot;
    }
}

// Returns the first child of `parent` that is the HTML element `tag`, or null.
PrDomElement* PR_NULLABLE firstChildOf(PrDomNode* parent, PrDomTag tag) {
    for (PrDomNode* child = parent->first; child != nullptr; child = child->next) {
        PrDomElement* element = domElementFromNode(child);
        if (domIsHtml(element, tag))
            return element;
    }
    return nullptr;
}

PrDomElement* createElement(PrDomNode* parent, PrDomTag tag, PrStringView name) {
    return domElementCreate(parent->document, PrDomNamespace_Html, tag, name);
}

// Returns the child that a new part goes before. A caption is the first child. A thead goes before the first element
// child that is neither a caption nor a colgroup. A tfoot is the last child.
PrDomNode* PR_NULLABLE partPosition(PrDomNode* table, PrHtmlTablePart part) {
    if (part == PrHtmlTablePart_Caption)
        return table->first;
    if (part == PrHtmlTablePart_TFoot)
        return nullptr;
    for (PrDomNode* child = table->first; child != nullptr; child = child->next) {
        PrDomElement* element = domElementFromNode(child);
        if (element != nullptr && !domIsHtml(element, PrDomTag_Caption) && !domIsHtml(element, PrDomTag_Colgroup))
            return child;
    }
    return nullptr;
}

// Returns true if `index` names a place to insert among `length` items.
bool insertable(int32_t index, uint32_t length) {
    return index >= -1 && (index < 0 || uint32_t(index) <= length);
}

// Removes the item that `index` names in `collection`, which is the last for -1.
PrStatus deleteItem(PrDomHtmlCollection* collection, int32_t index) {
    uint32_t length = domHtmlCollectionLength(collection);
    if (index < -1 || (index >= 0 && uint32_t(index) >= length))
        return PrStatus_OutOfRange;
    if (length != 0)
        domRemoveFromParent(domHtmlCollectionItem(collection, index < 0 ? length - 1 : uint32_t(index)));
    return PrStatus_Ok;
}

// Inserts a new element `tag` at `index` of the items of `collection`, which are the children of `parent` that are like
// it. An index of -1 or the length appends it.
PrStatus insertChild(PrDomNode* parent, PrDomHtmlCollection* collection, int32_t index, PrDomTag tag, PrStringView name,
        PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    uint32_t length = domHtmlCollectionLength(collection);
    if (!insertable(index, length))
        return PrStatus_OutOfRange;
    PrDomNode* before =
        index < 0 || uint32_t(index) == length ? nullptr : domHtmlCollectionItem(collection, uint32_t(index));
    *out = createElement(parent, tag, name);
    domInsert(parent, *out, before);
    return PrStatus_Ok;
}

// Returns the position of `element` in `collection`, or -1.
int32_t positionIn(PrDomHtmlCollection* collection, const PrDomElement* element) {
    uint32_t length = domHtmlCollectionLength(collection);
    for (uint32_t i = 0; i < length; ++i) {
        if (domHtmlCollectionItem(collection, i) == element)
            return int32_t(i);
    }
    return -1;
}

// Calls `use` with a collection of `filter` under `root`, and returns its answer.
template <typename Answer, typename Use>
Answer withCollection(PrDomNode* root, DomCollectionFilter filter, Use use) {
    PrDomHtmlCollection* collection = domHtmlCollectionCreate(root, filter, PrStringView{});
    Answer answer = use(collection);
    domHtmlCollectionDestroy(collection);
    return answer;
}

bool isSection(const PrDomElement* element) {
    return domIsHtml(element, PrDomTag_Thead) || domIsHtml(element, PrDomTag_Tbody) ||
        domIsHtml(element, PrDomTag_Tfoot);
}

uint32_t clampedSpan(const PrDomElement* cell, PrStringView name, uint32_t least, uint32_t most) {
    int64_t value = prHtmlElementReflectedInteger(static_cast<const PrHtmlElement*>(cell), name,
        PrHtmlReflectedInteger_Unsigned, 1);
    return value < least ? least : value > most ? most : uint32_t(value);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrHtmlTableElement* PR_NULLABLE prHtmlTableElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlTableElement>(object, PrDomTag_Table);
}

PrHtmlTableSectionElement* PR_NULLABLE prHtmlTableSectionElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(object));
    return isSection(element) ? static_cast<PrHtmlTableSectionElement*>(element) : nullptr;
}

PrHtmlTableRowElement* PR_NULLABLE prHtmlTableRowElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlTableRowElement>(object, PrDomTag_Tr);
}

PrHtmlTableCellElement* PR_NULLABLE prHtmlTableCellElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(object));
    bool cell = domIsHtml(element, PrDomTag_Td) || domIsHtml(element, PrDomTag_Th);
    return cell ? static_cast<PrHtmlTableCellElement*>(element) : nullptr;
}

PrDomElement* PR_NULLABLE prHtmlTableElementPart(PrHtmlTableElement* table, PrHtmlTablePart part) {
    return firstChildOf(table, partTag(part));
}

PrStatus prHtmlTableElementSetPart(PrHtmlTableElement* table, PrHtmlTablePart part, PrDomElement* PR_NULLABLE element) {
    if (element != nullptr && !domIsHtml(element, partTag(part))) {
        if (part == PrHtmlTablePart_Caption || !isSection(element))
            return PrStatus_WrongType;
        return PrStatus_NotAllowed;
    }
    prHtmlTableElementDeletePart(table, part);
    if (element != nullptr)
        return domPreInsert(table, element, partPosition(table, part));
    return PrStatus_Ok;
}

PrDomElement* prHtmlTableElementEnsurePart(PrHtmlTableElement* table, PrHtmlTablePart part) {
    if (PrDomElement* existing = firstChildOf(table, partTag(part)))
        return existing;
    constexpr PrStringView kNames[] = {"caption", "thead", "tfoot"};
    PrDomElement* element = createElement(table, partTag(part), kNames[part]);
    domInsert(table, element, partPosition(table, part));
    return element;
}

void prHtmlTableElementDeletePart(PrHtmlTableElement* table, PrHtmlTablePart part) {
    if (PrDomElement* existing = firstChildOf(table, partTag(part)))
        domRemoveFromParent(existing);
}

PrDomHtmlCollection* prHtmlTableElementTBodies(PrHtmlTableElement* table) {
    return domHtmlCollectionCreate(table, DomCollectionFilter_TableBodies, PrStringView{});
}

PrDomElement* prHtmlTableElementAddTBody(PrHtmlTableElement* table) {
    PrDomNode* before = nullptr;
    for (PrDomNode* child = table->last; child != nullptr; child = child->previous) {
        if (domIsHtml(domElementFromNode(child), PrDomTag_Tbody))
            break;
        before = child;
    }
    // Without a tbody child, the new one is the last child.
    if (firstChildOf(table, PrDomTag_Tbody) == nullptr)
        before = nullptr;
    PrDomElement* body = createElement(table, PrDomTag_Tbody, "tbody");
    domInsert(table, body, before);
    return body;
}

PrDomHtmlCollection* prHtmlTableElementRows(PrHtmlTableElement* table) {
    return domHtmlCollectionCreate(table, DomCollectionFilter_TableRows, PrStringView{});
}

PrStatus prHtmlTableElementInsertRow(PrHtmlTableElement* table, int32_t index, PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return withCollection<PrStatus>(table, DomCollectionFilter_TableRows, [&](PrDomHtmlCollection* rows) -> PrStatus {
        uint32_t length = domHtmlCollectionLength(rows);
        if (!insertable(index, length))
            return PrStatus_OutOfRange;
        PrDomNode* parent = nullptr;
        PrDomNode* before = nullptr;
        if (length == 0) {
            // A table without rows gets its row in its last tbody, which is made if there is none.
            for (PrDomNode* child = table->last; child != nullptr && parent == nullptr; child = child->previous) {
                if (domIsHtml(domElementFromNode(child), PrDomTag_Tbody))
                    parent = child;
            }
            if (parent == nullptr) {
                parent = createElement(table, PrDomTag_Tbody, "tbody");
                domInsert(table, parent, nullptr);
            }
        } else if (index < 0 || uint32_t(index) == length) {
            parent = domHtmlCollectionItem(rows, length - 1)->parent;
        } else {
            before = domHtmlCollectionItem(rows, uint32_t(index));
            parent = before->parent;
        }
        *out = createElement(table, PrDomTag_Tr, "tr");
        domInsert(parent, *out, before);
        return PrStatus_Ok;
    });
}

PrStatus prHtmlTableElementDeleteRow(PrHtmlTableElement* table, int32_t index) {
    return withCollection<PrStatus>(table, DomCollectionFilter_TableRows, [&](PrDomHtmlCollection* rows) -> PrStatus {
        return deleteItem(rows, index);
    });
}

PrDomHtmlCollection* prHtmlTableSectionElementRows(PrHtmlTableSectionElement* section) {
    return domHtmlCollectionCreate(section, DomCollectionFilter_ChildRows, PrStringView{});
}

PrStatus prHtmlTableSectionElementInsertRow(PrHtmlTableSectionElement* section, int32_t index,
        PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    return withCollection<PrStatus>(section, DomCollectionFilter_ChildRows, [&](PrDomHtmlCollection* rows) -> PrStatus {
        return insertChild(section, rows, index, PrDomTag_Tr, "tr", out);
    });
}

PrStatus prHtmlTableSectionElementDeleteRow(PrHtmlTableSectionElement* section, int32_t index) {
    return withCollection<PrStatus>(section, DomCollectionFilter_ChildRows, [&](PrDomHtmlCollection* rows) -> PrStatus {
        return deleteItem(rows, index);
    });
}

int32_t prHtmlTableRowElementRowIndex(const PrHtmlTableRowElement* row) {
    PrDomNode* parent = row->parent;
    if (parent == nullptr)
        return -1;
    PrDomElement* element = domElementFromNode(parent);
    PrDomNode* table = isSection(element) ? parent->parent : parent;
    if (table == nullptr || !domIsHtml(domElementFromNode(table), PrDomTag_Table))
        return -1;
    return withCollection<int32_t>(table, DomCollectionFilter_TableRows, [&](PrDomHtmlCollection* rows) -> int32_t {
        return positionIn(rows, row);
    });
}

int32_t prHtmlTableRowElementSectionRowIndex(const PrHtmlTableRowElement* row) {
    PrDomNode* parent = row->parent;
    PrDomElement* element = parent != nullptr ? domElementFromNode(parent) : nullptr;
    if (element == nullptr)
        return -1;
    if (domIsHtml(element, PrDomTag_Table)) {
        return withCollection<int32_t>(parent, DomCollectionFilter_TableRows, [&](PrDomHtmlCollection* rows) -> int32_t {
            return positionIn(rows, row);
        });
    }
    if (!isSection(element))
        return -1;
    return withCollection<int32_t>(parent, DomCollectionFilter_ChildRows, [&](PrDomHtmlCollection* rows) -> int32_t {
        return positionIn(rows, row);
    });
}

PrDomHtmlCollection* prHtmlTableRowElementCells(PrHtmlTableRowElement* row) {
    return domHtmlCollectionCreate(row, DomCollectionFilter_RowCells, PrStringView{});
}

PrStatus prHtmlTableRowElementInsertCell(PrHtmlTableRowElement* row, int32_t index,
        PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    return withCollection<PrStatus>(row, DomCollectionFilter_RowCells, [&](PrDomHtmlCollection* cells) -> PrStatus {
        return insertChild(row, cells, index, PrDomTag_Td, "td", out);
    });
}

PrStatus prHtmlTableRowElementDeleteCell(PrHtmlTableRowElement* row, int32_t index) {
    return withCollection<PrStatus>(row, DomCollectionFilter_RowCells, [&](PrDomHtmlCollection* cells) -> PrStatus {
        return deleteItem(cells, index);
    });
}

int32_t prHtmlTableCellElementCellIndex(const PrHtmlTableCellElement* cell) {
    PrDomNode* parent = cell->parent;
    if (parent == nullptr || !domIsHtml(domElementFromNode(parent), PrDomTag_Tr))
        return -1;
    return withCollection<int32_t>(parent, DomCollectionFilter_RowCells, [&](PrDomHtmlCollection* cells) -> int32_t {
        return positionIn(cells, cell);
    });
}

uint32_t prHtmlTableCellElementColSpan(const PrHtmlTableCellElement* cell) {
    return clampedSpan(cell, "colspan", 1, kPrLayoutTableMaxColumnSpan);
}

uint32_t prHtmlTableCellElementRowSpan(const PrHtmlTableCellElement* cell) {
    return clampedSpan(cell, "rowspan", 0, kPrLayoutTableMaxRowSpan);
}
