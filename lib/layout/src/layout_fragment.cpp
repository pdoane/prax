// Fragments: the output of layout for boxes, immutable once made.

#include "layout/src/layout_fragment.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/layout_tree.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// Returns the start of the trailing array of an allocation, past `offset` bytes and aligned for T.
template <typename T>
size_t alignedFor(size_t offset) {
    return (offset + alignof(T) - 1) & ~(alignof(T) - 1);
}

// Returns the start of the list of boxes out of flow in the allocation of a fragment, past its children. The list is
// its count, then its entries.
size_t pendingOffset(uint32_t childCount) {
    return alignedFor<uint32_t>(alignedFor<LayoutChild>(sizeof(LayoutFragment)) + sizeof(LayoutChild) * childCount);
}

static_assert(sizeof(uint32_t) % alignof(LayoutPending) == 0);

// Returns the size of the allocation of a box, line or row fragment: the fragment, its children and its list.
size_t boxBytes(const LayoutFragment* fragment) {
    if ((fragment->flags & LayoutFragmentFlag_Pending) != 0)
        return pendingOffset(fragment->childCount) + sizeof(uint32_t)
               + sizeof(LayoutPending) * layoutFragmentPending(fragment).count;
    return alignedFor<LayoutChild>(sizeof(LayoutFragment)) + sizeof(LayoutChild) * fragment->childCount;
}

// Returns the offset in a table fragment's allocation at which the grid's data starts: the grid's rectangle, the number
// of its collapsed borders, and the borders.
size_t gridOffset(const LayoutFragment* fragment) {
    return alignedFor<PrLayoutRect>(boxBytes(fragment));
}

// The header of each axis that a grid container's fragment holds after its children and its list, before its sizes.
struct GridAxisHeader {
    uint32_t count;
    int32_t explicitStart;
    uint32_t repetitions;
};

static_assert(sizeof(PrLayoutRect) % alignof(uint32_t) == 0);
static_assert(sizeof(GridAxisHeader) % alignof(PrLayoutUnit) == 0);
static_assert(sizeof(uint32_t) % alignof(LayoutGridBorder) == 0);

// Returns true if the fragment `child` adds a placeholder to its parent's subtree.
bool addsPlaceholders(const LayoutFragment* child) {
    return (child->flags & (LayoutFragmentFlag_Placeholders | LayoutFragmentFlag_OutOfFlow))
           == LayoutFragmentFlag_Placeholders;
}

} // namespace

void layoutBaselinesKeepClamps(LayoutBaselines& into, const LayoutBaseline& first, const LayoutBaseline& last) {
    PR_ASSERT(into.clamps == nullptr);
    const LayoutBaseline* edges[] = {&first, &last};
    LayoutBaselineClamps* clamps = allocZeroed<LayoutBaselineClamps>();
    for (uint32_t edge = 0; edge < LayoutBaselineEdge_Count; ++edge) {
        const LayoutBaseline& baseline = *edges[edge];
        clamps->clamped[edge] = baseline.present && baseline.clamped;
        clamps->unclamped[edge] = baseline.unclamped;
        clamps->lo[edge] = baseline.lo;
        clamps->hi[edge] = baseline.hi;
    }
    into.clamps = clamps;
}

LayoutBaseline layoutBaselinesClamped(const LayoutBaselines& kept, LayoutBaselineEdge edge, LayoutBaseline baseline) {
    const LayoutBaselineClamps* clamps = kept.clamps;
    if (clamps != nullptr && clamps->clamped[edge]) {
        baseline.clamped = true;
        baseline.unclamped = clamps->unclamped[edge];
        baseline.lo = clamps->lo[edge];
        baseline.hi = clamps->hi[edge];
    }
    return baseline;
}

void layoutBaselinesReset(LayoutBaselines& kept) {
    prMemoryFree(kept.clamps);
    kept.clamps = nullptr;
}

LayoutFragment* layoutFragmentCreate(LayoutFragmentKind kind, LayoutNodeIndex node, uint32_t childCount,
                                     uint32_t pendingCount) {
    size_t children = alignedFor<LayoutChild>(sizeof(LayoutFragment));
    size_t bytesNeeded = children + sizeof(LayoutChild) * childCount;
    if (pendingCount != 0)
        bytesNeeded = pendingOffset(childCount) + sizeof(uint32_t) + sizeof(LayoutPending) * pendingCount;
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAllocZeroed(bytesNeeded));
    LayoutFragment* fragment = reinterpret_cast<LayoutFragment*>(bytes);
    fragment->references.storeRelaxed(1);
    fragment->kind = kind;
    fragment->node = node;
    fragment->childCount = childCount;
    fragment->children = childCount != 0 ? reinterpret_cast<LayoutChild*>(bytes + children) : nullptr;
    if (pendingCount != 0) {
        fragment->flags |= LayoutFragmentFlag_Pending;
        *reinterpret_cast<uint32_t*>(bytes + pendingOffset(childCount)) = pendingCount;
    }
    if (kind == LayoutFragmentKind_Box) {
        for (uint32_t edge = 0; edge < LayoutBaselineEdge_Count; ++edge) {
            fragment->box.baselines.offset[edge] = kLayoutBaselineNone;
            fragment->box.baselines.source[edge] = kLayoutBaselineSynthesized;
        }
    }
    return fragment;
}

LayoutPending* layoutFragmentPendingStorage(LayoutFragment* fragment) {
    PR_ASSERT((fragment->flags & LayoutFragmentFlag_Pending) != 0);
    uint8_t* bytes = reinterpret_cast<uint8_t*>(fragment) + pendingOffset(fragment->childCount);
    return reinterpret_cast<LayoutPending*>(bytes + sizeof(uint32_t));
}

LayoutFragment* layoutFragmentCreatePlaceholder(LayoutNodeIndex node, LayoutStaticEdges edges, bool fixed) {
    LayoutFragment* fragment = layoutFragmentCreate(LayoutFragmentKind_Placeholder, node, 0);
    fragment->edges = edges;
    fragment->placeholder.fixed = fixed;
    fragment->flags = LayoutFragmentFlag_Placeholders;
    return fragment;
}

LayoutFragment* layoutFragmentCreateAnchor(LayoutNodeIndex node, LayoutFragmentFlags flags) {
    LayoutFragment* fragment = layoutFragmentCreate(LayoutFragmentKind_FloatAnchor, node, 0);
    fragment->flags = LayoutFragmentFlags(flags & LayoutFragmentFlag_Placeholders);
    fragment->anchor.sibling = kLayoutAnchorNone;
    return fragment;
}

// The fonts follow the fragment with no padding, the ranges follow the fonts, and the glyphs follow the ranges
// (layoutFragmentTextBytes).
static_assert(sizeof(LayoutFragment) % alignof(const PrFont*) == 0);
static_assert(sizeof(const PrFont*) % alignof(LayoutFragmentFontRange) == 0);
static_assert(sizeof(LayoutFragmentFontRange) % alignof(PrFontGlyph) == 0);
static_assert(sizeof(PrFontGlyph) % alignof(LayoutCaretStop) == 0);

LayoutFragment* layoutFragmentCreateText(LayoutNodeIndex node, float size, const PrFont* const* PR_NULLABLE fonts,
                                         uint32_t fontCount, const LayoutFragmentFontRange* PR_NULLABLE ranges,
                                         uint32_t rangeCount, uint32_t glyphCount, uint32_t stopCount) {
    PR_ASSERT(glyphCount == 0 ? fontCount == 0 && rangeCount == 0
                              : fontCount != 0 && rangeCount != 0 && ranges[rangeCount - 1].glyphEnd == glyphCount);
    // The maker writes every glyph and stop, so only the fragment is zeroed.
    uint8_t* bytes = static_cast<uint8_t*>(
        prMemoryAlloc(layoutFragmentTextBytes(fontCount, rangeCount, glyphCount, stopCount)));
    memZero(bytes, sizeof(LayoutFragment));
    LayoutFragment* fragment = reinterpret_cast<LayoutFragment*>(bytes);
    fragment->references.storeRelaxed(1);
    fragment->kind = LayoutFragmentKind_Text;
    fragment->node = node;
    fragment->text.size = size;
    fragment->text.glyphCount = glyphCount;
    fragment->text.fontCount = fontCount;
    fragment->text.rangeCount = rangeCount;
    fragment->text.stopCount = stopCount;
    const PrFont** ownFonts = const_cast<const PrFont**>(layoutFragmentTextFonts(fragment));
    for (uint32_t i = 0; i < fontCount; ++i) {
        PR_ASSERT(i == 0 || fonts[i] != fonts[i - 1]);
        prFontRetain(fonts[i]);
        ownFonts[i] = fonts[i];
    }
    LayoutFragmentFontRange* ownRanges = const_cast<LayoutFragmentFontRange*>(layoutFragmentTextRanges(fragment));
    for (uint32_t i = 0; i < rangeCount; ++i) {
        PR_ASSERT(ranges[i].font < fontCount && ranges[i].glyphEnd > (i != 0 ? ranges[i - 1].glyphEnd : 0));
        PR_ASSERT(i == 0 || ranges[i].font != ranges[i - 1].font);
        ownRanges[i] = ranges[i];
    }
    fragment->text.glyphs = glyphCount != 0 ? reinterpret_cast<PrFontGlyph*>(ownRanges + rangeCount) : nullptr;
    return fragment;
}

void layoutFragmentSetBaselines(LayoutFragment* fragment, const LayoutBaseline& first, const LayoutBaseline& last) {
    PR_ASSERT(fragment->kind == LayoutFragmentKind_Box);
    layoutBaselinesKeep(fragment->box.baselines, first, last);
}

LayoutBaseline layoutFragmentBaseline(const LayoutFragment* fragment, LayoutBaselineEdge edge) {
    if (fragment->kind != LayoutFragmentKind_Box)
        return LayoutBaseline{.source = kLayoutBaselineSynthesized};
    return layoutBaselinesAt(fragment->box.baselines, edge);
}

LayoutBaseline layoutBaselineClamped(LayoutBaseline baseline, PrLayoutUnit lo, PrLayoutUnit hi) {
    auto clamp = [&](PrLayoutUnit value) { return layoutUnitMin(layoutUnitMax(value, lo), hi); };
    if (baseline.clamped) {
        baseline.lo = clamp(baseline.lo);
        baseline.hi = clamp(baseline.hi);
    } else {
        baseline.lo = lo;
        baseline.hi = hi;
        baseline.clamped = true;
    }
    baseline.offset = clamp(baseline.offset);
    return baseline;
}

LayoutPendingList layoutFragmentPending(const LayoutFragment* fragment) {
    if ((fragment->flags & LayoutFragmentFlag_Pending) == 0)
        return LayoutPendingList{0, nullptr};
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(fragment) + pendingOffset(fragment->childCount);
    return LayoutPendingList{*reinterpret_cast<const uint32_t*>(bytes),
                             reinterpret_cast<const LayoutPending*>(bytes + sizeof(uint32_t))};
}

void layoutFragmentRetain(const LayoutFragment* fragment) {
    fragment->references.fetchAddRelaxed(1);
}

void layoutFragmentRelease(const LayoutFragment* PR_NULLABLE fragment) {
    if (fragment == nullptr || fragment->references.fetchSub(1) != 1)
        return;
    // A stack of fragments to free. The children of each are released as it is freed. The explicit stack lets a deep
    // tree be freed in bounded call-stack space.
    Array<LayoutFragment*> freeing;
    freeing.push(const_cast<LayoutFragment*>(fragment));
    while (freeing.count != 0) {
        LayoutFragment* f = freeing.last();
        freeing.pop();
        for (uint32_t i = 0; i < f->childCount; ++i) {
            LayoutFragment* child = const_cast<LayoutFragment*>(f->children[i].fragment);
            if (child->references.fetchSub(1) == 1)
                freeing.push(child);
        }
        if (f->kind == LayoutFragmentKind_Text) {
            const PrFont* const* fonts = layoutFragmentTextFonts(f);
            for (uint32_t i = 0; i < f->text.fontCount; ++i)
                prFontRelease(fonts[i]);
        }
        else if (f->kind == LayoutFragmentKind_Box)
            layoutBaselinesReset(f->box.baselines);
        prMemoryFree(f);
    }
    freeing.reset();
}

LayoutFragment* layoutFragmentTable(LayoutFragment* built, PrLayoutRect rect,
                                    const LayoutGridBorder* PR_NULLABLE borders, uint32_t count) {
    size_t base = boxBytes(built);
    size_t tail = alignedFor<PrLayoutRect>(base);
    size_t bytesNeeded = tail + sizeof(PrLayoutRect) + sizeof(uint32_t) + sizeof(LayoutGridBorder) * count;
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAlloc(bytesNeeded));
    memCopy(bytes, built, base);
    LayoutFragment* fragment = reinterpret_cast<LayoutFragment*>(bytes);
    if (fragment->childCount != 0)
        fragment->children = reinterpret_cast<LayoutChild*>(bytes + alignedFor<LayoutChild>(sizeof(LayoutFragment)));
    fragment->flags |= LayoutFragmentFlag_Table;
    memCopy(bytes + tail, &rect, sizeof(PrLayoutRect));
    memCopy(bytes + tail + sizeof(PrLayoutRect), &count, sizeof(uint32_t));
    if (count != 0)
        memCopy(bytes + tail + sizeof(PrLayoutRect) + sizeof(uint32_t), borders, sizeof(LayoutGridBorder) * count);
    // The children of the built fragment now belong to the new one, and it holds nothing else.
    prMemoryFree(built);
    return fragment;
}

LayoutTableGrid layoutFragmentTableGrid(const LayoutFragment* table) {
    PR_ASSERT((table->flags & LayoutFragmentFlag_Table) != 0);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(table) + gridOffset(table);
    LayoutTableGrid grid{};
    memCopy(&grid.rect, bytes, sizeof(PrLayoutRect));
    grid.borderCount = *reinterpret_cast<const uint32_t*>(bytes + sizeof(PrLayoutRect));
    if (grid.borderCount != 0)
        grid.borders = reinterpret_cast<const LayoutGridBorder*>(bytes + sizeof(PrLayoutRect) + sizeof(uint32_t));
    return grid;
}

LayoutFragment* layoutFragmentGrid(LayoutFragment* built, const LayoutGridAxisTracks* axes) {
    size_t base = boxBytes(built);
    size_t tail = alignedFor<GridAxisHeader>(base);
    size_t bytesNeeded = tail;
    for (uint32_t axis = 0; axis < PrLayoutGridAxis_Count; ++axis)
        bytesNeeded += sizeof(GridAxisHeader) + sizeof(PrLayoutUnit) * axes[axis].count;
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAlloc(bytesNeeded));
    memCopy(bytes, built, base);
    LayoutFragment* fragment = reinterpret_cast<LayoutFragment*>(bytes);
    if (fragment->childCount != 0)
        fragment->children = reinterpret_cast<LayoutChild*>(bytes + alignedFor<LayoutChild>(sizeof(LayoutFragment)));
    fragment->flags |= LayoutFragmentFlag_Grid;
    size_t at = tail;
    for (uint32_t axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
        GridAxisHeader header{axes[axis].count, axes[axis].explicitStart, axes[axis].repetitions};
        memCopy(bytes + at, &header, sizeof header);
        at += sizeof header;
        if (header.count != 0)
            memCopy(bytes + at, axes[axis].sizes, sizeof(PrLayoutUnit) * header.count);
        at += sizeof(PrLayoutUnit) * header.count;
    }
    // The children of the built fragment now belong to the new one, and it holds nothing else.
    prMemoryFree(built);
    return fragment;
}

LayoutGridAxisTracks layoutFragmentGridTracks(const LayoutFragment* grid, PrLayoutGridAxis axis) {
    PR_ASSERT((grid->flags & LayoutFragmentFlag_Grid) != 0);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(grid) + alignedFor<GridAxisHeader>(boxBytes(grid));
    for (uint32_t a = 0;; ++a) {
        GridAxisHeader header{};
        memCopy(&header, bytes, sizeof header);
        bytes += sizeof header;
        if (a == axis)
            return LayoutGridAxisTracks{header.count, header.explicitStart, header.repetitions,
                                        header.count != 0 ? reinterpret_cast<const PrLayoutUnit*>(bytes) : nullptr};
        bytes += sizeof(PrLayoutUnit) * header.count;
    }
}

bool layoutPiecesNext(LayoutPieces& pieces, LayoutPiece* out) {
    LayoutPieceAt& at = pieces.at;
    while (!pieces.done && at.child < pieces.container->childCount) {
        const LayoutChild& child = pieces.container->children[at.child];
        const LayoutFragment* f = child.fragment;
        PrLayoutVec offset = pieces.base + child.offset;
        const LayoutFragment* line = nullptr;
        LayoutPieceAt here = at;
        if (f->kind == LayoutFragmentKind_Line) {
            line = f;
            ++at.child;
        } else if (f->kind == LayoutFragmentKind_Box && f->node == kPrLayoutNodeNone) {
            uint32_t k = at.line == kLayoutPieceNone ? 0 : at.line;
            while (k < f->childCount && f->children[k].fragment->kind != LayoutFragmentKind_Line)
                ++k;
            if (k == f->childCount) {
                ++at.child;
                at.line = kLayoutPieceNone;
                continue;
            }
            here.line = k;
            line = f->children[k].fragment;
            offset = offset + f->children[k].offset;
            at.line = k + 1;
        } else if (f->kind == LayoutFragmentKind_Around) {
            ++at.child;
            // The rows of the boxes around this one hold its row. Each row holds the next row inward.
            uint32_t depth = 0;
            while (f->kind == LayoutFragmentKind_Around && f->node != pieces.node && f->childCount == 1) {
                offset = offset + f->children[0].offset;
                f = f->children[0].fragment;
                ++depth;
            }
            if (f->kind != LayoutFragmentKind_Around || f->node != pieces.node)
                continue;
            *out = LayoutPiece{.kind = LayoutPieceKind_Row, .fragment = f, .offset = offset, .at = here,
                               .depth = depth};
            return true;
        } else {
            ++at.child;
            continue;
        }
        for (uint32_t k = 0; k < line->childCount; ++k) {
            const LayoutFragment* part = line->children[k].fragment;
            if (part->kind != LayoutFragmentKind_Inline || part->node != pieces.node)
                continue;
            uint32_t parts = 0;
            uint32_t end = layoutGroupEnd(line, k, &parts);
            pieces.done = line->children[parts - 1].fragment->part.closes;
            *out = LayoutPiece{.kind = LayoutPieceKind_Group, .fragment = line, .offset = offset, .at = here,
                               .first = k, .parts = parts, .end = end};
            return true;
        }
    }
    return false;
}

uint32_t layoutGroupEnd(const LayoutFragment* line, uint32_t first, uint32_t* parts) {
    LayoutNodeIndex node = line->children[first].fragment->node;
    uint32_t end = first + 1;
    while (end < line->childCount && line->children[end].fragment->kind == LayoutFragmentKind_Inline
           && line->children[end].fragment->node == node)
        ++end;
    *parts = end;
    return end + line->children[end - 1].fragment->part.enclosed;
}

LayoutFragment* layoutFragmentBuild(Array<LayoutLogicalChild>& children, uint32_t mark, LayoutFragmentKind kind,
                                    LayoutNodeIndex node, LayoutMode mode, LayoutLogicalSize size) {
    uint32_t count = children.count - mark;
    const LayoutLogicalChild* from = children.data + mark;
    LayoutFragment* fragment = layoutFragmentCreate(kind, node, count);
    LayoutChild* to = fragment->children;
    uint32_t floatsFirst = 0;
    uint32_t floatsEnd = 0;
    bool placeholders = false;
    bool lines = false;               // true if a child is a line
    uint32_t pendingCount = 0;        // length of the list that the children give
    for (uint32_t i = 0; i < count; ++i) {
        const LayoutFragment* child = from[i].fragment;
        LayoutLogicalSize childSize = layoutLogicalSize(mode, child->extent);
        to[i] = LayoutChild{child, layoutPhysicalOffset(mode, from[i].offset, childSize, size)};
        LayoutFragmentFlags flags = child->flags;
        lines = lines || child->kind == LayoutFragmentKind_Line;
        if ((flags & LayoutFragmentFlag_Float) != 0 || layoutFragmentHoldsFloats(child)) {
            floatsFirst = floatsFirst == floatsEnd ? i : floatsFirst;
            floatsEnd = i + 1;
        }
        // Most children are not placeholders and list nothing.
        if ((flags & (LayoutFragmentFlag_Placeholders | LayoutFragmentFlag_Pending)) != 0) {
            placeholders = placeholders || addsPlaceholders(child);
            if (child->kind == LayoutFragmentKind_Placeholder)
                ++pendingCount;
            else
                pendingCount += layoutFragmentPending(child).count;
        }
    }
    // Most fragments list nothing. A fragment that does is made again with room for its list.
    if (pendingCount != 0) {
        LayoutFragment* listing = layoutFragmentCreate(kind, node, count, pendingCount);
        memCopy(listing->children, fragment->children, sizeof(LayoutChild) * count);
        prMemoryFree(fragment);
        fragment = listing;
        LayoutPending* pending = layoutFragmentPendingStorage(fragment);
        uint32_t listed = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const LayoutChild& child = fragment->children[i];
            const LayoutFragment* f = child.fragment;
            if (f->kind == LayoutFragmentKind_Placeholder) {
                pending[listed++] = LayoutPending{.node = f->node,
                                                  .at = child.offset,
                                                  .edges = f->edges,
                                                  .fixed = f->placeholder.fixed};
                continue;
            }
            bool inLines = f->kind == LayoutFragmentKind_Line || f->kind == LayoutFragmentKind_Around
                           || (f->kind == LayoutFragmentKind_Box && f->node == kPrLayoutNodeNone);
            LayoutPendingList list = layoutFragmentPending(f);
            for (uint32_t k = 0; k < list.count; ++k) {
                LayoutPending entry = list.entries[k];
                entry.at = entry.at + child.offset;
                entry.inLines = inLines;
                pending[listed++] = entry;
            }
        }
        PR_ASSERT(listed == pendingCount);
    }
    fragment->floatsFirst = floatsFirst;
    fragment->floatsEnd = floatsEnd;
    if (placeholders)
        fragment->flags |= LayoutFragmentFlag_Placeholders;
    if (lines)
        fragment->flags |= LayoutFragmentFlag_Lines;
    children.count = mark;
    fragment->extent = layoutPhysicalSize(mode, size);
    return fragment;
}

LayoutFragment* layoutFragmentHold(LayoutFragment* built, const LayoutChild* PR_NULLABLE held, uint32_t heldCount,
                                   const LayoutPending* PR_NULLABLE pending, uint32_t pendingCount) {
    uint32_t count = built->childCount;
    LayoutFragment* fragment = layoutFragmentCreate(built->kind, built->node, count + heldCount, pendingCount);
    memCopy(fragment->children, built->children, sizeof(LayoutChild) * count);
    for (uint32_t i = 0; i < heldCount; ++i) {
        fragment->children[count + i] = held[i];
    }
    if (pendingCount != 0)
        memCopy(layoutFragmentPendingStorage(fragment), pending, sizeof(LayoutPending) * pendingCount);
    fragment->floatsFirst = built->floatsFirst;
    fragment->floatsEnd = built->floatsEnd;
    fragment->flags |=
        LayoutFragmentFlags(built->flags & (LayoutFragmentFlag_Placeholders | LayoutFragmentFlag_Lines));
    if (heldCount != 0)
        fragment->flags |= LayoutFragmentFlag_Holds;
    fragment->extent = built->extent;
    // The children of the built fragment now belong to the new one, and it holds nothing else.
    prMemoryFree(built);
    return fragment;
}

void layoutFragmentDiscard(Array<LayoutLogicalChild>& children, uint32_t mark) {
    for (uint32_t i = mark; i < children.count; ++i)
        layoutFragmentRelease(children.data[i].fragment);
    children.count = mark;
}

} // namespace pr_impl
