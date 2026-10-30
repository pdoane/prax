#include "dom/src/dom_range.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

bool isCharacterData(const PrDomNode* node) {
    return node->type == PrDomNodeType_Text || node->type == PrDomNodeType_CdataSection ||
        node->type == PrDomNodeType_Comment || node->type == PrDomNodeType_ProcessingInstruction;
}

// Removes the range from its document's list.
void unlink(PrDomRange* range) {
    PrDomDocument* document = range->document;
    if (document == nullptr)
        return;
    if (range->previous != nullptr)
        range->previous->next = range->next;
    else
        document->ranges = range->next;
    if (range->next != nullptr)
        range->next->previous = range->previous;
    range->previous = nullptr;
    range->next = nullptr;
    range->document = nullptr;
}

// Adds the range to the live ranges of `document`.
void link(PrDomRange* range, PrDomDocument* document) {
    range->document = document;
    range->previous = nullptr;
    range->next = document->ranges;
    if (range->next != nullptr)
        range->next->previous = range;
    document->ranges = range;
}

// Moves a boundary point of `range` to `value`. A live range holds a reference to the node of each of its boundary
// points. The new reference is taken before the old one is released, because both nodes may be in one tree.
void setPoint(PrDomRange* range, DomBoundary* point, DomBoundary value) {
    PrDomNode* previous = point->node;
    if (range->document != nullptr)
        domNodeRetain(value.node);
    *point = value;
    if (range->document != nullptr)
        domNodeRelease(previous);
}

// Moves a live range, and its reference, to the live ranges of `document`.
void track(PrDomRange* range, PrDomDocument* document) {
    PrDomDocument* from = range->document;
    if (from == document || from == nullptr)
        return;
    unlink(range);
    link(range, document);
    domDocumentRetain(document);
    domDocumentRelease(from);
}

// Sets the start boundary point if `start` is set, and the end otherwise. These are the steps of
// dom/#concept-range-bp-set that follow its checks.
void setBoundary(PrDomRange* range, DomBoundary point, bool start) {
    bool sameRoot = domNodeRoot(point.node) == domRangeRoot(range);
    if (start) {
        if (!sameRoot || domBoundaryCompare(point, range->end) > 0)
            setPoint(range, &range->end, point);
        setPoint(range, &range->start, point);
    } else {
        if (!sameRoot || domBoundaryCompare(point, range->start) < 0)
            setPoint(range, &range->start, point);
        setPoint(range, &range->end, point);
    }
    track(range, point.node->document);
}

// Returns the substring of a character data node's data, in UTF-16 code units.
PrString substring(const PrDomNode* node, uint32_t offset, uint32_t count) {
    PrString out = {};
    domSubstringData(static_cast<const PrDomCharacterData*>(node), offset, count, &out);
    return out;
}

// Returns a copy of the node alone, with `data` as its data if it has data.
PrDomNode* cloneWithData(const PrDomNode* node, PrStringView data) {
    PrDomNode* copy = domClone(node, node->document, false);
    domSetData(static_cast<PrDomCharacterData*>(copy), data);
    return copy;
}

// Returns the child of `ancestor` that is `node` or an ancestor of it.
PrDomNode* childOnPath(PrDomNode* ancestor, PrDomNode* node) {
    while (domNodeParent(node) != ancestor)
        node = domNodeParent(node);
    return node;
}

// Returns the boundary point that a range collapses to when its contents are removed. It is the start if the start node
// contains the end. Otherwise it is just after the ancestor of the start node that is a sibling of the end or of one of
// the end's ancestors.
DomBoundary collapsedPoint(PrDomRange* range) {
    if (domIsInclusiveAncestor(range->start.node, range->end.node))
        return range->start;
    PrDomNode* reference = range->start.node;
    while (domNodeParent(reference) != nullptr && !domIsInclusiveAncestor(domNodeParent(reference), range->end.node))
        reference = domNodeParent(reference);
    return DomBoundary{domNodeParent(reference), domNodeIndex(reference) + 1};
}

// Adds to `fragment` the part of `node` that the range from `from` to `to` spans, and removes it from the node if
// `extract` is set. For character data, the part is its data. Otherwise the part is a clone of the node that holds the
// contents that this range extracts.
PrStatus extractPart(PrDomNode* fragment, PrDomNode* node, DomBoundary from, DomBoundary to, bool extract) {
    if (isCharacterData(node)) {
        PrString data = substring(node, from.offset, to.offset - from.offset);
        domAttach(fragment, cloneWithData(node, prStringView(&data)), nullptr);
        prStringReset(&data);
        if (extract)
            domReplaceData(static_cast<PrDomCharacterData*>(node), from.offset, to.offset - from.offset, "");
        return PrStatus_Ok;
    }
    PrDomNode* clone = domClone(node, node->document, false);
    domAttach(fragment, clone, nullptr);
    PrDomRange sub = {{from, to, nullptr, nullptr, nullptr, 0, {}}};
    PrDomDocumentFragment* inner = nullptr;
    PrStatus status = domRangeExtract(&sub, extract, &inner);
    if (status == PrStatus_Ok)
        domInsert(clone, inner, nullptr);
    domNodeRelease(inner);
    return status;
}

} // namespace

PrDomRange* domRangeCreate(PrDomDocument* document) {
    PrDomRange* range = allocZeroed<PrDomRange>();
    range->start = DomBoundary{document, 0};
    range->end = range->start;
    range->references = 1;
    link(range, document);
    domDocumentRetain(document);
    domNodeRetainBy(document, 2);
    return range;
}

PrDomRange* domStaticRangeCreate(DomBoundary start, DomBoundary end) {
    PrDomRange* range = allocZeroed<PrDomRange>();
    range->start = start;
    range->end = end;
    range->references = 1;
    domNodeRetain(start.node);
    domNodeRetain(end.node);
    return range;
}

void domRangeRetain(PrDomRange* range) {
    ++range->references;
}

void domRangeRelease(PrDomRange* PR_NULLABLE range) {
    if (range == nullptr || --range->references != 0)
        return;
    weakClearAll(&range->weak);
    PrDomDocument* document = range->document;
    unlink(range);
    domNodeRelease(range->start.node);
    domNodeRelease(range->end.node);
    domDocumentRelease(document);
    prMemoryFree(range);
}

uint32_t domNodeLength(const PrDomNode* node) {
    switch (node->type) {
        case PrDomNodeType_DocumentType:
            return 0;
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection:
        case PrDomNodeType_Comment:
        case PrDomNodeType_ProcessingInstruction:
            return domCharacterDataLength(static_cast<const PrDomCharacterData*>(node));
        default: {
            uint32_t count = 0;
            for (const PrDomNode* child = node->type != PrDomNodeType_Attribute ? node->first : nullptr;
                    child != nullptr; child = child->next)
                ++count;
            return count;
        }
    }
}

PrStatus domCheckBoundaryPoint(const PrDomNode* node, uint32_t offset) {
    if (node->type == PrDomNodeType_DocumentType)
        return PrStatus_WrongType;
    return offset > domNodeLength(node) ? PrStatus_OutOfRange : PrStatus_Ok;
}

uint32_t domNodeIndex(const PrDomNode* node) {
    uint32_t index = 0;
    for (const PrDomNode* at = domNodeParent(node) != nullptr ? node->previous : nullptr; at != nullptr; at =
            at->previous)
        ++index;
    return index;
}

bool domIsInclusiveAncestor(const PrDomNode* ancestor, const PrDomNode* node) {
    for (const PrDomNode* at = node; at != nullptr; at = domNodeParent(at)) {
        if (at == ancestor)
            return true;
    }
    return false;
}

int domBoundaryCompare(DomBoundary a, DomBoundary b) {
    if (a.node == b.node)
        return a.offset == b.offset ? 0 : a.offset < b.offset ? -1 : 1;
    if (domNodePrecedes(b.node, a.node))
        return -domBoundaryCompare(b, a);
    if (domIsInclusiveAncestor(a.node, b.node)) {
        PrDomNode* child = childOnPath(a.node, b.node);
        if (domNodeIndex(child) < a.offset)
            return 1;
    }
    return -1;
}

PrDomNode* domRangeRoot(const PrDomRange* range) {
    return domNodeRoot(range->start.node);
}

PrStatus domRangeSetStart(PrDomRange* range, PrDomNode* node, uint32_t offset) {
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status == PrStatus_Ok)
        setBoundary(range, DomBoundary{node, offset}, true);
    return status;
}

PrStatus domRangeSetEnd(PrDomRange* range, PrDomNode* node, uint32_t offset) {
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status == PrStatus_Ok)
        setBoundary(range, DomBoundary{node, offset}, false);
    return status;
}

PrStatus domRangeSelect(PrDomRange* range, PrDomNode* node) {
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return PrStatus_WrongType;
    uint32_t index = domNodeIndex(node);
    setPoint(range, &range->start, DomBoundary{parent, index});
    setPoint(range, &range->end, DomBoundary{parent, index + 1});
    track(range, parent->document);
    return PrStatus_Ok;
}

PrStatus domRangeSelectContents(PrDomRange* range, PrDomNode* node) {
    if (node->type == PrDomNodeType_DocumentType)
        return PrStatus_WrongType;
    setPoint(range, &range->start, DomBoundary{node, 0});
    setPoint(range, &range->end, DomBoundary{node, domNodeLength(node)});
    track(range, node->document);
    return PrStatus_Ok;
}

void domRangeCollapse(PrDomRange* range, bool toStart) {
    if (toStart)
        setPoint(range, &range->end, range->start);
    else
        setPoint(range, &range->start, range->end);
}

bool domRangeContains(const PrDomRange* range, const PrDomNode* node) {
    PrDomNode* held = const_cast<PrDomNode*>(node);
    return domNodeRoot(node) == domRangeRoot(range) && domBoundaryCompare(DomBoundary{held, 0}, range->start) > 0 &&
        domBoundaryCompare(DomBoundary{held, domNodeLength(node)}, range->end) < 0;
}

bool domRangePartiallyContains(const PrDomRange* range, const PrDomNode* node) {
    bool start = domIsInclusiveAncestor(node, range->start.node);
    bool end = domIsInclusiveAncestor(node, range->end.node);
    return start != end;
}

PrStatus domRangeExtract(PrDomRange* range, bool extract, PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out) {
    PrDomDocumentFragment* fragment = domDocumentFragmentCreate(range->start.node->document);
    domNodeRetain(fragment);
    *out = fragment;
    if (domRangeCollapsed(range))
        return PrStatus_Ok;
    DomBoundary start = range->start;
    DomBoundary end = range->end;

    // A range within one character data node takes a copy of the data that it spans.
    if (start.node == end.node && isCharacterData(start.node))
        return extractPart(fragment, start.node, start, end, extract);

    PrDomNode* common = start.node;
    while (!domIsInclusiveAncestor(common, end.node))
        common = domNodeParent(common);
    PrDomNode* firstPartial = nullptr;
    PrDomNode* lastPartial = nullptr;
    Array<PrDomNode*> contained;
    for (PrDomNode* child = common->first; child != nullptr; child = child->next) {
        if (domRangeContains(range, child)) {
            if (child->type == PrDomNodeType_DocumentType) {
                contained.reset();
                return PrStatus_NotAllowed;
            }
            contained.push(child);
        } else if (domRangePartiallyContains(range, child)) {
            // The first partially contained child holds the start, and the last holds the end.
            if (firstPartial == nullptr && domIsInclusiveAncestor(child, start.node))
                firstPartial = child;
            if (domIsInclusiveAncestor(child, end.node))
                lastPartial = child;
        }
    }
    DomBoundary collapsed = collapsedPoint(range);

    if (firstPartial != nullptr) {
        PrStatus status = extractPart(fragment, firstPartial, start,
            DomBoundary{firstPartial, domNodeLength(firstPartial)}, extract);
        if (status != PrStatus_Ok) {
            contained.reset();
            return status;
        }
    }

    for (uint32_t i = 0; i < contained.count; ++i) {
        if (extract)
            domInsert(fragment, contained.data[i], nullptr);
        else
            domAttach(fragment, domClone(contained.data[i], contained.data[i]->document, true), nullptr);
    }
    contained.reset();

    if (lastPartial != nullptr) {
        PrStatus status = extractPart(fragment, lastPartial, DomBoundary{lastPartial, 0}, end, extract);
        if (status != PrStatus_Ok)
            return status;
    }

    if (extract) {
        setPoint(range, &range->start, collapsed);
        setPoint(range, &range->end, collapsed);
    }
    return PrStatus_Ok;
}

void domRangeDelete(PrDomRange* range) {
    if (domRangeCollapsed(range))
        return;
    DomBoundary start = range->start;
    DomBoundary end = range->end;
    if (start.node == end.node && isCharacterData(start.node)) {
        domReplaceData(static_cast<PrDomCharacterData*>(start.node), start.offset, end.offset - start.offset, "");
        return;
    }
    // The contained nodes whose parents are not contained, in tree order.
    Array<PrDomNode*> removed;
    PrDomNode* root = domRangeRoot(range);
    for (PrDomNode* node = root; node != nullptr;) {
        if (domRangeContains(range, node)) {
            removed.push(node);
            node = const_cast<PrDomNode*>(domNodeNextSkipping(root, node));
        } else {
            node = domNodeNext(root, node);
        }
    }
    DomBoundary collapsed = collapsedPoint(range);
    if (isCharacterData(start.node)) {
        uint32_t length = domNodeLength(start.node);
        domReplaceData(static_cast<PrDomCharacterData*>(start.node), start.offset, length - start.offset, "");
    }
    for (uint32_t i = 0; i < removed.count; ++i) {
        if (domNodeParent(removed.data[i]) != nullptr)
            domRemove(removed.data[i]);
    }
    removed.reset();
    if (isCharacterData(end.node))
        domReplaceData(static_cast<PrDomCharacterData*>(end.node), 0, end.offset, "");
    setPoint(range, &range->start, collapsed);
    setPoint(range, &range->end, collapsed);
}

PrStatus domRangeInsert(PrDomRange* range, PrDomNode* node) {
    PrDomNode* start = range->start.node;
    bool text = domIsText(start);
    if (start->type == PrDomNodeType_ProcessingInstruction || start->type == PrDomNodeType_Comment ||
            (text && domNodeParent(start) == nullptr) || start == node)
        return PrStatus_NotAllowed;
    PrDomNode* reference = nullptr;
    if (text) {
        reference = start;
    } else {
        reference = start->first;
        for (uint32_t i = 0; reference != nullptr && i < range->start.offset; ++i)
            reference = reference->next;
    }
    PrDomNode* parent = reference == nullptr ? start : domNodeParent(reference);
    PrStatus status = domValidateInsertion(parent, node, reference);
    if (status != PrStatus_Ok)
        return status;
    if (text)
        reference = domSplitText(static_cast<PrDomText*>(start), range->start.offset);
    if (node == reference)
        reference = reference->next;
    if (domNodeParent(node) != nullptr)
        domRemove(node);
    uint32_t offset = reference == nullptr ? domNodeLength(parent) : domNodeIndex(reference);
    offset += node->type == PrDomNodeType_DocumentFragment ? domNodeLength(node) : 1;
    status = domPreInsert(parent, node, reference);
    if (status == PrStatus_Ok && domRangeCollapsed(range))
        setPoint(range, &range->end, DomBoundary{parent, offset});
    return status;
}

PrStatus domRangeSurround(PrDomRange* range, PrDomNode* parent) {
    PrDomNode* root = domRangeRoot(range);
    for (PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        if (!domIsText(node) && domRangePartiallyContains(range, node))
            return PrStatus_InvalidState;
    }
    if (parent->type == PrDomNodeType_Document || parent->type == PrDomNodeType_DocumentType ||
            parent->type == PrDomNodeType_DocumentFragment)
        return PrStatus_WrongType;
    PrDomDocumentFragment* fragment = nullptr;
    PrStatus status = domRangeExtract(range, true, &fragment);
    if (status == PrStatus_Ok && parent->first != nullptr)
        domReplaceAll(parent, nullptr);
    if (status == PrStatus_Ok)
        status = domRangeInsert(range, parent);
    if (status == PrStatus_Ok)
        status = domPreInsert(parent, fragment, nullptr);
    domNodeRelease(fragment);
    return status == PrStatus_Ok ? domRangeSelect(range, parent) : status;
}

PrString domRangeString(const PrDomRange* range) {
    TextBuffer out;
    auto append = [&](const PrDomNode* node, uint32_t offset, uint32_t count) {
        PrString data = substring(node, offset, count);
        textAppend(&out, prStringView(&data));
        prStringReset(&data);
    };
    const PrDomNode* start = range->start.node;
    const PrDomNode* end = range->end.node;
    if (start == end && domIsText(start)) {
        append(start, range->start.offset, range->end.offset - range->start.offset);
        return textTake(&out);
    }
    if (domIsText(start))
        append(start, range->start.offset, domNodeLength(start) - range->start.offset);
    PrDomNode* root = domRangeRoot(range);
    for (PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        if (domIsText(node) && domRangeContains(range, node))
            append(node, 0, domNodeLength(node));
    }
    if (domIsText(end))
        append(end, 0, range->end.offset);
    return textTake(&out);
}

void domRangesInserted(PrDomNode* node) {
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr || node->next == nullptr)
        return;
    uint32_t index = domNodeIndex(node);
    for (PrDomRange* range = node->document->ranges; range != nullptr; range = range->next) {
        if (range->start.node == parent && range->start.offset > index)
            ++range->start.offset;
        if (range->end.node == parent && range->end.offset > index)
            ++range->end.offset;
    }
}

void domRangesRemoving(PrDomNode* node) {
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return;
    uint32_t index = domNodeIndex(node);
    for (PrDomRange* range = node->document->ranges; range != nullptr; range = range->next) {
        if (domIsInclusiveAncestor(node, range->start.node))
            setPoint(range, &range->start, DomBoundary{parent, index});
        if (domIsInclusiveAncestor(node, range->end.node))
            setPoint(range, &range->end, DomBoundary{parent, index});
        if (range->start.node == parent && range->start.offset > index)
            --range->start.offset;
        if (range->end.node == parent && range->end.offset > index)
            --range->end.offset;
    }
}

void domRangesDataReplaced(PrDomNode* node, uint32_t offset, uint32_t count, uint32_t length) {
    for (PrDomRange* range = node->document->ranges; range != nullptr; range = range->next) {
        DomBoundary* points[] = {&range->start, &range->end};
        for (DomBoundary* point : points) {
            if (point->node != node)
                continue;
            if (point->offset > offset && point->offset <= offset + count)
                point->offset = offset;
            else if (point->offset > offset + count)
                point->offset = point->offset + length - count;
        }
    }
}

void domRangesSplit(PrDomNode* node, uint32_t offset, PrDomNode* created) {
    // dom/#concept-text-split moves the boundary points only for a node with a parent.
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return;
    uint32_t after = domNodeIndex(node) + 1;
    for (PrDomRange* range = node->document->ranges; range != nullptr; range = range->next) {
        DomBoundary* points[] = {&range->start, &range->end};
        for (DomBoundary* point : points) {
            if (point->node == node && point->offset > offset)
                setPoint(range, point, DomBoundary{created, point->offset - offset});
            else if (point->node == parent && point->offset == after)
                ++point->offset;
        }
    }
}

void domRangesMerged(PrDomNode* into, uint32_t offset, PrDomNode* from) {
    PrDomNode* parent = domNodeParent(from);
    uint32_t index = parent != nullptr ? domNodeIndex(from) : 0;
    for (PrDomRange* range = into->document->ranges; range != nullptr; range = range->next) {
        DomBoundary* points[] = {&range->start, &range->end};
        for (DomBoundary* point : points) {
            if (point->node == from)
                setPoint(range, point, DomBoundary{into, point->offset + offset});
            else if (parent != nullptr && point->node == parent && point->offset == index)
                setPoint(range, point, DomBoundary{into, offset});
        }
    }
}

uint32_t domRangesAdopted(PrDomDocument* from, PrDomNode* node) {
    uint32_t moved = 0;
    for (PrDomRange* range = from->ranges; range != nullptr;) {
        PrDomRange* next = range->next;
        if (domIsInclusiveAncestor(node, range->start.node)) {
            unlink(range);
            link(range, node->document);
            ++moved;
        }
        range = next;
    }
    return moved;
}

} // namespace pr_impl
