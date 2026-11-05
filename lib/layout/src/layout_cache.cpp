// The layout cache: the result of the last layout of each box, and the intrinsic sizes from its last measurement. Both
// are reused while nothing in the subtree of the box has changed and the inputs are the same.

#include "layout/src/layout_cache.h"

#include "pr/alloc.h"
#include "pr/mem.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_space.h"

namespace pr_impl {

namespace {

// --- Lines ---

// Releases a record of lines, and frees it.
void destroyLines(LayoutLineRecord* PR_NULLABLE record) {
    if (record == nullptr)
        return;
    layoutLineRecordClear(*record);
    record->lines.reset();
    record->atomics.reset();
    prMemoryFree(record);
}

// --- Offsets ---

// `from` returns `value` relative to `top`, and `at` converts it back. Both return kLayoutOffsetNone unchanged.
PrLayoutUnit from(PrLayoutUnit value, PrLayoutUnit top) {
    return value == kLayoutOffsetNone ? value : value - top;
}
PrLayoutUnit at(PrLayoutUnit value, PrLayoutUnit top) {
    return value == kLayoutOffsetNone ? value : value + top;
}

// Calls `visit` with each float among the first `count` of `exclusions` that meets [top + lo, top + hi). Each float is
// clipped to that range and made relative to `top` and `lineLeft`.
template <typename Visit>
void eachViewed(const LayoutExclusions& exclusions, uint32_t count, PrLayoutUnit top, PrLayoutUnit lineLeft,
        PrLayoutUnit lo, PrLayoutUnit hi, Visit visit) {
    if (lo.raw >= hi.raw)
        return;
    PrLayoutUnit start = top + lo;
    PrLayoutUnit end = top + hi;
    for (uint32_t i = 0; i < count; ++i) {
        const LayoutExclusion& e = exclusions.floats.data[i];
        if (e.top.raw >= e.bottom.raw || e.top.raw >= end.raw || e.bottom.raw <= start.raw)
            continue;
        visit(LayoutExclusion{
            e.left - lineLeft, e.right - lineLeft, layoutUnitMax(e.top, start) - top,
            layoutUnitMin(e.bottom, end) - top, e.side,
        });
    }
}

// --- Context records ---

// Returns the context record of `entry`, and makes one if it has none.
LayoutCacheContext& contextOf(LayoutCache& cache, LayoutCacheEntry& entry) {
    if (entry.context == kLayoutCacheContextNone) {
        if (cache.freeContexts.count != 0) {
            entry.context = cache.freeContexts.last();
            cache.freeContexts.pop();
        } else {
            entry.context = cache.contexts.count;
            cache.contexts.emplace() = LayoutCacheContext{};
        }
    }
    return cache.contexts.data[entry.context];
}

// Frees the context record of `entry`, and keeps its storage for the next record.
void freeContext(LayoutCache& cache, LayoutCacheEntry& entry) {
    if (entry.context == kLayoutCacheContextNone)
        return;
    LayoutCacheContext& context = cache.contexts.data[entry.context];
    context.view.clear();
    context.placed.clear();
    cache.freeContexts.push(entry.context);
    entry.context = kLayoutCacheContextNone;
}

// Records what a box read of its context `bfc` and placed in it, relative to `top`, the block-start edge of its border
// box.
void recordContext(LayoutCache& cache, LayoutCacheEntry& entry, const LayoutBfc& bfc, const LayoutReads& reads,
        PrLayoutUnit top, bool cleared) {
    const LayoutExclusions& exclusions = *bfc.containing.exclusions;
    PrLayoutUnit lineLeft = bfc.containing.lineLeft;
    entry.flags |= LayoutCacheFlag_InContext;
    if (reads.lo.raw < reads.hi.raw) {
        entry.readLo = reads.lo - top;
        entry.readHi = reads.hi - top;
    }
    bool floorRead = reads.clearance[0].read || reads.clearance[1].read || reads.lastTop.read;
    uint32_t prefix = reads.entry.count;
    // If there were no floats in the context when it began or after, it read no floor, and it had no clearance, then it
    // read nothing.
    bool plain = !floorRead && !cleared && prefix == 0 && exclusions.floats.count == 0;
    if (plain) {
        entry.flags |= LayoutCacheFlag_Plain;
        return;
    }
    LayoutCacheContext& context = contextOf(cache, entry);
    context.clearance = cleared;
    for (uint32_t side = 0; side < 2; ++side) {
        context.clearanceRead[side] = reads.clearance[side];
        context.clearanceRead[side].value = from(reads.entry.lowest[side], top);
        context.clearanceRead[side].limit = from(reads.clearance[side].limit, top);
    }
    context.lastTopRead = reads.lastTop;
    context.lastTopRead.value = from(reads.entry.lastTop, top);
    context.lastTopRead.limit = from(reads.lastTop.limit, top);
    context.view.clear();
    context.placed.clear();
    eachViewed(exclusions, prefix, top, lineLeft, entry.readLo, entry.readHi,
        [&](const LayoutExclusion& e) { context.view.push(e); });
    for (uint32_t i = prefix; i < exclusions.floats.count; ++i) {
        const LayoutExclusion& e = exclusions.floats.data[i];
        context.placed.push(LayoutExclusion{
            e.left - lineLeft, e.right - lineLeft, e.top - top, e.bottom - top,
            e.side,
        });
    }
    if (!floorRead && !cleared && context.view.count == 0 && context.placed.count == 0)
        entry.flags |= LayoutCacheFlag_Plain;
}

// --- Reuse in a context ---

// What a cached result does in its context at its current position.
enum {
    Placement_Differs,              // it would read something different there, and is laid out again
    Placement_Stops,                // floats wait for its margins, which resolve at the point its layout would stop
    Placement_Placed,               // it reads the same there, and its floats are placed again
};
typedef uint8_t Placement;

// Returns true if a floor read then and now is the same, or bounded nothing both times.
bool sameFloor(const LayoutFloorRead& cached, PrLayoutUnit now) {
    if (!cached.read)
        return true;
    if (cached.value == now)
        return true;
    return cached.limit != kLayoutOffsetNone && cached.value.raw < cached.limit.raw && now.raw < cached.limit.raw;
}

// Returns true if what a cached result read of its context, relative to `top`, is what it would read from there now.
// Compares the floats in its range and the floors it read.
bool readsSame(const LayoutCacheEntry& entry, const LayoutCacheContext* PR_NULLABLE cached, const LayoutBfc& bfc,
        PrLayoutUnit top) {
    const LayoutExclusions& exclusions = *bfc.containing.exclusions;
    // If the context has no floats and the result saw none, it saw what there is.
    if (exclusions.floats.count != 0 || cached != nullptr) {
        uint32_t viewed = 0;
        bool same = true;
        eachViewed(exclusions, exclusions.floats.count, top, bfc.containing.lineLeft, entry.readLo, entry.readHi,
            [&](const LayoutExclusion& e) {
                same = same && cached != nullptr && viewed < cached->view.count && cached->view.data[viewed] == e;
                ++viewed;
            });
        if (!same || viewed != (cached != nullptr ? cached->view.count : 0))
            return false;
    }
    if (cached == nullptr)
        return true;
    for (uint32_t side = 0; side < 2; ++side) {
        if (!sameFloor(cached->clearanceRead[side], from(exclusions.mark.lowest[side], top)))
            return false;
    }
    return sameFloor(cached->lastTopRead, from(exclusions.mark.lastTop, top));
}

// Adds what a cached result read to the reads of its parent, and places its floats again, relative to `top`.
void replay(const LayoutCacheEntry& entry, const LayoutCacheContext* PR_NULLABLE cached, const LayoutBfc& bfc,
        PrLayoutUnit top) {
    LayoutReads& reads = *bfc.containing.reads;
    if (entry.readLo.raw < entry.readHi.raw)
        layoutReadsRange(reads, entry.readLo + top, entry.readHi + top);
    if (cached == nullptr)
        return;
    for (uint32_t side = 0; side < 2; ++side) {
        if (cached->clearanceRead[side].read)
            layoutReadsFloor(reads.clearance[side], at(cached->clearanceRead[side].limit, top));
    }
    if (cached->lastTopRead.read)
        layoutReadsFloor(reads.lastTop, at(cached->lastTopRead.limit, top));
    for (uint32_t i = 0; i < cached->placed.count; ++i) {
        LayoutExclusion e = cached->placed.data[i];
        e.left = e.left + bfc.containing.lineLeft;
        e.right = e.right + bfc.containing.lineLeft;
        e.top = e.top + top;
        e.bottom = e.bottom + top;
        layoutExclusionsAdd(*bfc.containing.exclusions, e);
    }
}

// Finds where the border box of a cached result is in the context `bfc`, from its cached margins, and whether it reads
// the same values there. Sets its position in `out`, or the point where it stops. Kept out of line from
// layoutCacheFind: most reuse in most layouts is of results that have nothing to read, and this keeps that path cheap.
PR_NOINLINE Placement placeInContext(const LayoutCache& cache, const LayoutCacheEntry& entry,
        const LayoutBfc& bfc, LayoutResult* out) {
    bool collapsedThrough = (entry.flags & LayoutResultFlag_CollapsedThrough) != 0;
    bool plain = (entry.flags & LayoutCacheFlag_Plain) != 0;
    const LayoutCacheContext* cached = plain ? nullptr : &cache.contexts.data[entry.context];
    const LayoutMarginChain& chain = bfc.chain;
    // Its floats were placed where an earlier layout resolved the margins, and this layout has not resolved them.
    if (collapsedThrough && cached != nullptr && cached->placed.count != 0 && chain.resolved == kLayoutOffsetNone)
        return Placement_Differs;
    PrLayoutUnit end = layoutMarginChainEnd(chain, out->start);
    LayoutResolution resolution = layoutMarginChainResolve(chain, end, false);
    if (resolution.cleared != (cached != nullptr && cached->clearance))
        return Placement_Differs;
    // Floats adjoining its margins wait for them, unless the box collapses through without clearance, in which case its
    // margins do not resolve in it.
    if (chain.adjoining != 0 && (!collapsedThrough || resolution.cleared)) {
        resolution = layoutMarginChainResolve(chain, end, true);
        if (resolution.waits) {
            out->resolution = resolution.at;
            return Placement_Stops;
        }
    }
    if (!readsSame(entry, cached, bfc, resolution.top))
        return Placement_Differs;
    replay(entry, cached, bfc, resolution.top);
    out->top = resolution.top;
    out->cleared = resolution.cleared;
    return Placement_Placed;
}

// Returns true if the box of `node` may read the inline size of its containing block. A percentage in its margins,
// padding, inline size or inline limits reads it. So may a replaced box, the fallback of a scroll container, and the
// sizes of an orthogonal flow root or a box out of flow. An anonymous flex item, whose result the first node of its run
// keeps, has no sizes or edges of its own. Kept out of line from entryFor, because most lookups return before reaching
// it.
PR_NOINLINE bool readsContainingInline(const LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space) {
    const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, n.style);
    if (n.kind == PrLayoutNodeKind_Replaced || layoutIsScrollContainer(box) || layoutSpaceIsOutOfFlow(space) ||
            layoutIsOrthogonalRoot(state.tree, node, space.mode))
        return true;
    bool horizontal = layoutIsHorizontal(space.mode);
    bool reads;
    if (horizontal)
        reads = layoutLengthReadsBase(box.width) || layoutLengthReadsBase(box.minWidth) ||
            layoutLengthReadsBase(box.maxWidth);
    else
        reads = layoutLengthReadsBase(box.height) || layoutLengthReadsBase(box.minHeight) ||
            layoutLengthReadsBase(box.maxHeight);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        reads = reads || layoutLengthReadsBase(box.margin[side]) || layoutLengthReadsBase(box.padding[side]);
    return reads;
}

// Returns the flags of what `requests` ask (LayoutCacheFlag_Requests), or none for a box that is not a flex item.
PR_FORCE_INLINE LayoutCacheFlags requestFlags(const LayoutRequests* PR_NULLABLE requests) {
    if (requests == nullptr)
        return 0;
    const LayoutRequests& r = *requests;
    LayoutCacheFlags flags = 0;
    if (r.inlineAxis.kind == LayoutRequestKind_Fixed)
        flags |= LayoutCacheFlag_InlineFixed;
    if (r.blockAxis.kind == LayoutRequestKind_Fixed)
        flags |= LayoutCacheFlag_BlockFixed | (r.blockAxis.definite ? LayoutCacheFlag_BlockDefinite : 0);
    if (r.blockAxis.kind == LayoutRequestKind_Content)
        flags |= LayoutCacheFlag_BlockContent;
    return flags;
}

// Returns what an entry keeps of `space` and `requests` as its available size: a fixed inline size, or the available
// size.
PR_FORCE_INLINE PrLayoutUnit availableKey(const LayoutSpace& space, const LayoutRequests* PR_NULLABLE requests) {
    if (requests != nullptr && requests->inlineAxis.kind == LayoutRequestKind_Fixed)
        return requests->inlineAxis.size;
    return space.available;
}

// Returns what an entry keeps of `space` and `requests` as the size it stretches to: a fixed block size, or that size.
PR_FORCE_INLINE PrLayoutUnit stretchKey(const LayoutSpace& space, const LayoutRequests* PR_NULLABLE requests) {
    if (requests != nullptr && requests->blockAxis.kind == LayoutRequestKind_Fixed)
        return requests->blockAxis.size;
    return space.stretchBlock;
}

// Returns true if a result laid out when a different size was available would be the same if the box resolves to the
// same inline size. This holds for an in-flow block container that, if it is in a context, saw no floats. A box whose
// inline size fills the available size resolves to a different size, and is not compared.
bool mayResize(const LayoutCacheEntry& entry, const LayoutSpace& space) {
    return entry.fragment->kind == LayoutFragmentKind_Box && !layoutSpaceIsOutOfFlow(space) &&
        (entry.flags & LayoutResultFlag_FillsAvailable) == 0 && (entry.flags & LayoutCacheFlag_InlineFixed) == 0 &&
        ((entry.flags & LayoutCacheFlag_InContext) == 0 || (entry.flags & LayoutCacheFlag_Plain) != 0);
}

// Returns the entry of `node`, laid out at an available or containing inline size different from that of `space`, if it
// is the result of the box in `space` at `requests` by the rules of entryFor. Otherwise returns null. Kept out of line
// from entryFor, because most lookups return before reaching it.
PR_NOINLINE const LayoutCacheEntry* PR_NULLABLE resizedEntry(const LayoutState& state, LayoutNodeIndex node,
        const LayoutCacheEntry& entry, const LayoutSpace& space, const LayoutRequests* PR_NULLABLE requests,
        const LayoutResolvedInline* PR_NULLABLE resolved) {
    bool availableDiffers = entry.available != availableKey(space, requests);
    if (availableDiffers && (resolved == nullptr || !mayResize(entry, space) ||
            layoutLogicalSize(space.mode, entry.fragment->extent).inlineSize != resolved->inlineSize))
        return nullptr;
    if (entry.containingInline != space.containing.inlineSize && readsContainingInline(state, node, space))
        return nullptr;
    return &entry;
}

// Returns true if the space holds inputs beyond its sizes that the context record of an entry keeps: outside markers,
// or the inputs that a table gives a cell.
bool hasExtras(const LayoutSpace& space) {
    return !layoutLineExtentIsEmpty(space.marker) || space.cell != nullptr;
}

// Returns true if the context record of an entry holds the same outside markers as `space`, and the same inputs from a
// table to a cell.
bool sameExtras(const LayoutCacheContext& context, const LayoutSpace& space) {
    return context.marker == space.marker && layoutCellSpacesEqual(context.cell, layoutCellSpaceFromSpace(space));
}

// Returns the entry of `node` if it was laid out in `space` at `requests` and nothing in its subtree has changed since,
// or null otherwise. An entry that differs in the available size is valid only if `resolved` gives the inline size of
// its result.
const LayoutCacheEntry* PR_NULLABLE entryFor(const LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, const LayoutResolvedInline* PR_NULLABLE resolved) {
    const LayoutCache& cache = state.tree->cache;
    const LayoutCacheEntry& entry = cache.entries.data[node];
    if (!state.reuse || (layoutTreeNode(state.tree, node).dirty & ~LayoutDirtyFlag_OutOfFlow) != 0 ||
            entry.fragment == nullptr || (entry.flags & LayoutCacheFlag_Requests) != requestFlags(requests) ||
            (((entry.flags & LayoutResultFlag_ReadsBlockBase) != 0 || (entry.flags & LayoutCacheFlag_BlockFixed) != 0) &&
                entry.stretchBlock != stretchKey(space, requests)) ||
            ((entry.flags & LayoutResultFlag_ReadsBlockBase) != 0 &&
                entry.containingBlock != space.containing.blockSize) ||
            entry.mode != space.mode || ((entry.flags & LayoutCacheFlag_InContext) != 0) != (space.bfc != nullptr))
        return nullptr;
    if ((entry.flags & LayoutResultFlag_ReadsFallback) != 0 &&
            cache.contexts.data[entry.context].fallback != space.fallback)
        return nullptr;
    bool extrasDiffer;
    if ((entry.flags & LayoutCacheFlag_SpaceExtras) != 0)
        extrasDiffer = !sameExtras(cache.contexts.data[entry.context], space);
    else
        extrasDiffer = hasExtras(space);
    if (extrasDiffer)
        return nullptr;
    bool availableDiffers = entry.available != availableKey(space, requests);
    if (!availableDiffers && entry.containingInline == space.containing.inlineSize)
        return &entry;
    // A box that fills the available size resolves to a different inline size if a different size is available.
    if (availableDiffers && (entry.flags & LayoutResultFlag_FillsAvailable) != 0)
        return nullptr;
    return resizedEntry(state, node, entry, space, requests, resolved);
}

// --- Placements ---

// Returns the placement record of `node`, and makes one if there is none.
LayoutPlacement& placementOf(LayoutCache& cache, LayoutNodeIndex node) {
    if (cache.placements.count <= node) {
        uint32_t from = cache.placements.count;
        cache.placements.resize(node + 1);
        for (uint32_t i = from; i < cache.placements.count; ++i)
            cache.placements.data[i] = LayoutPlacement{.parent = kPrLayoutNodeNone, .offset = PrLayoutVec{}};
    }
    return cache.placements.data[node];
}

// Records that `node` is a box with the fragment `fragment` at `offset` in the fragment of its parent. A box that moves
// makes every memoized position stale. A new fragment retires the old one, and the boxes in it are placed in turn.
void placeBox(LayoutCache& cache, LayoutNodeIndex node, LayoutNodeIndex parent, const LayoutFragment* fragment,
        PrLayoutVec offset) {
    ++cache.published;
    LayoutPlacement& placement = placementOf(cache, node);
    if (placement.parent != parent || placement.offset != offset) {
        placement.parent = parent;
        placement.offset = offset;
        ++cache.placed;
    }
    if (placement.fragment == fragment)
        return;
    const LayoutFragment* old = placement.fragment;
    layoutFragmentRetain(fragment);
    placement.fragment = fragment;
    layoutCacheRetire(cache, old);
    layoutFragmentRelease(old);
    // Placing the boxes inside may grow the placements array, which moves this record.
    if (fragment->childCount != 0)
        layoutCachePlace(cache, node, fragment);
}

// Records that the inline box or text `node` starts on the line at `at` in the fragment of its parent. Its position is
// the position of its parent, so no memo reads `at`. A node that was placed before as a box with a fragment of its own
// gives up that fragment.
void placeStart(LayoutCache& cache, LayoutNodeIndex node, LayoutNodeIndex parent, LayoutPieceAt at) {
    ++cache.published;
    LayoutPlacement& placement = placementOf(cache, node);
    if (placement.fragment != nullptr) {
        layoutCacheRetire(cache, placement.fragment);
        layoutFragmentRelease(placement.fragment);
        placement.fragment = nullptr;
        ++cache.placed;
    }
    if (placement.parent != parent)
        ++cache.placed;
    placement.parent = parent;
    placement.at = at;
}

// Records that no layout placed the inline box or text `node`. A node that was placed before as a box with a fragment
// of its own gives up that fragment.
void placeNone(LayoutCache& cache, LayoutNodeIndex node) {
    LayoutPlacement& placement = placementOf(cache, node);
    if (placement.fragment == nullptr && placement.parent == kPrLayoutNodeNone)
        return;
    ++cache.published;
    ++cache.placed;
    if (placement.fragment != nullptr) {
        layoutCacheRetire(cache, placement.fragment);
        layoutFragmentRelease(placement.fragment);
    }
    placement = LayoutPlacement{.parent = kPrLayoutNodeNone, .offset = PrLayoutVec{}};
}

void placeWithin(LayoutCache& cache, LayoutNodeIndex parent, const LayoutFragment* fragment, PrLayoutVec offset,
    uint32_t child, bool nested);

// Records the boxes on the line `line`, which is at `offset` in the fragment of `parent` and at `at` among its
// children. These are the atomic inlines, the inline boxes that open on the line, and the texts on it. The children are
// visited last to first, so that the last record of a text is of its first line, provided the lines are also visited
// last to first.
//
// A line that is `nested` in an anonymous box inside another has no LayoutPieceAt. Its atomic inlines are placed by
// their offsets, and its inline boxes and texts are recorded as placed by no layout. The boxes in an anonymous inline
// table on the line are recorded as those of an anonymous box.
void placeLine(LayoutCache& cache, LayoutNodeIndex parent, const LayoutFragment* line, PrLayoutVec offset,
        LayoutPieceAt at, bool nested = false) {
    for (uint32_t i = line->childCount; i-- > 0;) {
        const LayoutChild& child = line->children[i];
        const LayoutFragment* c = child.fragment;
        switch (c->kind) {
            case LayoutFragmentKind_Text:
                if (nested)
                    placeNone(cache, c->node);
                else
                    placeStart(cache, c->node, parent, at);
                break;
            case LayoutFragmentKind_Inline:
                if (nested)
                    placeNone(cache, c->node);
                else if (c->part.opens)
                    placeStart(cache, c->node, parent, at);
                break;
            case LayoutFragmentKind_Box:
            case LayoutFragmentKind_Replaced:
                if (c->node != kPrLayoutNodeNone && (c->flags & LayoutFragmentFlag_Continuation) == 0)
                    placeBox(cache, c->node, parent, c, offset + child.offset);
                else
                    placeWithin(cache, parent, c, offset + child.offset, at.child, true);
                break;
            default: break;
        }
    }
}

// Records the boxes in `fragment`, a row around a block or an anonymous box, which is at `offset` in the fragment of
// `parent` and within the child `child` of the container. These are the boxes with fragments of their own, and the
// lines of an anonymous box.
void placeWithin(LayoutCache& cache, LayoutNodeIndex parent, const LayoutFragment* fragment, PrLayoutVec offset,
        uint32_t child, bool nested = false) {
    for (uint32_t i = fragment->childCount; i-- > 0;) {
        const LayoutChild& inner = fragment->children[i];
        const LayoutFragment* c = inner.fragment;
        PrLayoutVec at = offset + inner.offset;
        // A line is found from its container's child and its index in that child. A line in an anonymous box inside
        // another, as a table's fixup makes, has no such place.
        if (c->kind == LayoutFragmentKind_Line)
            placeLine(cache, parent, c, at, LayoutPieceAt{child, i}, nested);
        else if (c->kind == LayoutFragmentKind_Around)
            placeWithin(cache, parent, c, at, child, nested || (c->flags & LayoutFragmentFlag_Continuation) != 0);
        else if (c->kind == LayoutFragmentKind_Box &&
                (c->node == kPrLayoutNodeNone || (c->flags & LayoutFragmentFlag_Continuation) != 0))
            placeWithin(cache, parent, c, at, child, true);
        else if ((c->kind == LayoutFragmentKind_Box || c->kind == LayoutFragmentKind_Replaced) &&
                c->node != kPrLayoutNodeNone)
            placeBox(cache, c->node, parent, c, at);
    }
}

// Takes the result for layoutCacheFind and layoutCacheFindResized, as each function describes.
PR_FORCE_INLINE bool find(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, const LayoutResolvedInline* PR_NULLABLE resolved, LayoutResult* out) {
    const LayoutCacheEntry* entry = entryFor(state, node, space, requests, resolved);
    if (entry == nullptr)
        return false;
    out->start = LayoutStrut{entry->startPositive, entry->startNegative};
    out->end = LayoutStrut{entry->endPositive, entry->endNegative};
    if (const LayoutBfc* bfc = space.bfc; bfc != nullptr) {
        // If it read nothing and there is nothing to read, with no floats, no floor and no floats waiting, it reads the
        // same values wherever it is.
        const LayoutMarginChain& chain = bfc->chain;
        bool nothing = (entry->flags & LayoutCacheFlag_Plain) != 0 && chain.floor == kLayoutOffsetNone &&
            chain.adjoining == 0 && bfc->containing.exclusions->floats.count == 0;
        Placement placement = Placement_Placed;
        if (nothing) {
            out->readLo = entry->readLo;
            out->readHi = entry->readHi;
        } else {
            placement = placeInContext(state.tree->cache, *entry, *bfc, out);
        }
        if (placement == Placement_Differs)
            return false;
        if (placement == Placement_Stops)
            return true;
    }
    // A change out of flow below it was for its holder to lay out.
    LayoutTreeNode& reused = state.tree->nodes.data[node];
    if (reused.dirty != 0)
        reused.dirty = 0;
    // A result taken at a different available size becomes the result at this size, which the next layout of the box
    // compares.
    if (resolved != nullptr) {
        LayoutCacheEntry& kept = state.tree->cache.entries.data[node];
        kept.available = availableKey(space, requests);
        kept.marginInlineStart = resolved->marginInlineStart;
    }
    layoutFragmentRetain(entry->fragment);
    out->fragment = entry->fragment;
    out->marginInlineStart = entry->marginInlineStart;
    out->flags = LayoutResultFlags(entry->flags & LayoutResultFlag_All);
    state.readsFallback = state.readsFallback || (entry->flags & LayoutResultFlag_ReadsFallback) != 0;
    ++state.stats->blocksReused;
    return true;
}

// Returns true if the record `r` of `node` answers a measurement in `space` at `requests`. It must hold an answer, and
// its space must be the same in everything its result read.
bool recordHolds(const LayoutState& state, LayoutNodeIndex node, const LayoutMeasureRecord& r,
        const LayoutSpace& space, const LayoutRequests& requests, LayoutMeasureKind kind) {
    if ((r.flags & LayoutCacheFlag_Measured) == 0 || (r.flags & LayoutCacheFlag_Requests) != requestFlags(&requests) ||
            r.mode != space.mode || r.available != availableKey(space, &requests) ||
            (kind == LayoutMeasureKind_Baselines && (r.flags & LayoutCacheFlag_SizeOnly) != 0) ||
            !layoutCellSpacesEqual(r.cell, layoutCellSpaceFromSpace(space)))
        return false;
    bool readsBlockBase = (r.flags & LayoutResultFlag_ReadsBlockBase) != 0;
    if ((readsBlockBase || (r.flags & LayoutCacheFlag_BlockFixed) != 0) &&
            r.stretchBlock != stretchKey(space, &requests))
        return false;
    if (readsBlockBase && r.containingBlock != space.containing.blockSize)
        return false;
    if ((r.flags & LayoutResultFlag_ReadsFallback) != 0 && r.fallback != space.fallback)
        return false;
    return r.containingInline == space.containing.inlineSize || !readsContainingInline(state, node, space);
}

// Empties the measurement record `r`.
void clearRecord(LayoutMeasureRecord& r) {
    layoutBaselinesReset(r.baselines);
    r = LayoutMeasureRecord{};
}

// Drops the measurements of `node`, and frees its records for another box.
void dropMeasures(LayoutCache& cache, LayoutNodeIndex node) {
    if (node >= cache.measureAt.count || cache.measureAt.data[node] == kLayoutMeasureNone)
        return;
    uint32_t first = cache.measureAt.data[node];
    for (uint32_t i = 0; i < kLayoutMeasureSlots; ++i)
        clearRecord(cache.measures.data[first + i]);
    cache.freeMeasures.push(first);
    cache.measureAt.data[node] = kLayoutMeasureNone;
}

} // namespace

void layoutCacheInit(LayoutCache& cache) {
    cache = LayoutCache{};
    cache.placed = 1;
    cache.contexts.emplace() = LayoutCacheContext{};
    cache.absolutes.emplace() = LayoutAbsoluteEntry{};
}

void layoutCacheReset(LayoutCache& cache) {
    for (uint32_t i = 0; i < cache.measures.count; ++i)
        layoutBaselinesReset(cache.measures.data[i].baselines);
    cache.measures.reset();
    cache.measureAt.reset();
    cache.freeMeasures.reset();
    for (uint32_t i = 0; i < cache.entries.count; ++i)
        layoutFragmentRelease(cache.entries.data[i].fragment);
    cache.entries.reset();
    for (uint32_t i = 0; i < cache.contexts.count; ++i) {
        cache.contexts.data[i].view.reset();
        cache.contexts.data[i].placed.reset();
    }
    cache.contexts.reset();
    cache.freeContexts.reset();
    cache.intrinsics.reset();
    cache.absolutes.reset();
    for (uint32_t i = 0; i < cache.placements.count; ++i)
        layoutFragmentRelease(cache.placements.data[i].fragment);
    cache.placements.reset();
    cache.retired.reset();
    cache.positioned.reset();
    cache.unpositioned.reset();
    for (uint32_t i = 0; i < cache.lines.count; ++i)
        destroyLines(cache.lines.data[i]);
    cache.lines.reset();
}

void layoutCacheForget(LayoutCache& cache, LayoutNodeIndex node) {
    // The entries cover every node made, and the other per-node arrays of the cache cover only nodes made before them.
    // A node past the end of the entries is therefore new, and the cache holds nothing for it.
    if (node >= cache.entries.count) {
        while (cache.entries.count <= node)
            cache.entries.emplace() = LayoutCacheEntry{};
        return;
    }
    if (node < cache.lines.count) {
        destroyLines(cache.lines.data[node]);
        cache.lines.data[node] = nullptr;
    }
    LayoutCacheEntry& entry = cache.entries.data[node];
    layoutFragmentRelease(entry.fragment);
    freeContext(cache, entry);
    // The last placement record takes the place of the record of the node, which keeps the records dense.
    uint32_t at = entry.absolute;
    if (at != kLayoutCacheAbsoluteNone) {
        uint32_t last = cache.absolutes.count - 1;
        if (at != last) {
            cache.absolutes.data[at] = cache.absolutes.data[last];
            cache.entries.data[cache.absolutes.data[at].node].absolute = at;
        }
        cache.absolutes.pop();
    }
    entry = LayoutCacheEntry{};
    if (node < cache.placements.count) {
        LayoutPlacement& placement = cache.placements.data[node];
        layoutCacheRetire(cache, placement.fragment);
        layoutFragmentRelease(placement.fragment);
        placement = LayoutPlacement{.parent = kPrLayoutNodeNone, .offset = PrLayoutVec{}};
        ++cache.placed;
    }
    if (node < cache.intrinsics.count)
        cache.intrinsics.data[node] = LayoutIntrinsicEntry{};
    dropMeasures(cache, node);
}

void layoutCacheRetire(LayoutCache& cache, const LayoutFragment* PR_NULLABLE fragment) {
    if (fragment == nullptr)
        return;
    cache.retired.push(fragment);
    // A box with a node retires its own fragment when its placement is replaced. Every other fragment in this one, such
    // as an anonymous box or the content of a line, retires with it.
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutFragment* child = fragment->children[i].fragment;
        bool placed = child->node != kPrLayoutNodeNone &&
            (child->kind == LayoutFragmentKind_Box || child->kind == LayoutFragmentKind_Replaced);
        if (!placed)
            layoutCacheRetire(cache, child);
    }
}

void layoutCacheInvalidateIntrinsic(LayoutCache& cache, LayoutNodeIndex node) {
    if (node < cache.intrinsics.count) {
        cache.intrinsics.data[node].valid = false;
        cache.intrinsics.data[node].measured = false;
    }
}

void layoutCacheInvalidate(LayoutCache& cache, LayoutNodeIndex node) {
    layoutCacheInvalidateIntrinsic(cache, node);
    LayoutCacheEntry& entry = cache.entries.data[node];
    layoutFragmentRelease(entry.fragment);
    entry.fragment = nullptr;
}

bool layoutCacheFindAbsolute(LayoutState& state, LayoutNodeIndex node, const LayoutAbsoluteBlock& containing,
        PrLayoutVec at, LayoutStaticEdges edges, LayoutChild* out) {
    if (!state.reuse || layoutTreeNode(state.tree, node).dirty != 0)
        return false;
    const LayoutCache& cache = state.tree->cache;
    const LayoutCacheEntry& entry = cache.entries.data[node];
    if (entry.fragment == nullptr || entry.absolute == kLayoutCacheAbsoluteNone)
        return false;
    const LayoutAbsoluteEntry& a = cache.absolutes.data[entry.absolute];
    if (!prMemEquals(&a.containing, &containing.rect, sizeof(PrLayoutRect)) || a.mode != containing.mode || a.at != at ||
            a.edges != edges)
        return false;
    bool readsFallback = (entry.flags & LayoutResultFlag_ReadsFallback) != 0;
    if (readsFallback && cache.contexts.data[entry.context].fallback != containing.fallback)
        return false;
    layoutFragmentRetain(entry.fragment);
    *out = LayoutChild{entry.fragment, a.offset};
    state.readsFallback = state.readsFallback || readsFallback;
    ++state.stats->blocksReused;
    return true;
}

void layoutCacheStoreAbsolute(LayoutState& state, LayoutNodeIndex node, const LayoutAbsoluteBlock& containing,
        PrLayoutVec at, LayoutStaticEdges edges, PrLayoutVec offset) {
    if (!state.reuse)
        return;
    LayoutCache& cache = state.tree->cache;
    LayoutCacheEntry& entry = cache.entries.data[node];
    if (entry.absolute == kLayoutCacheAbsoluteNone) {
        entry.absolute = cache.absolutes.count;
        cache.absolutes.emplace();
    }
    cache.absolutes.data[entry.absolute] = LayoutAbsoluteEntry{
        .node = node,
        .containing = containing.rect,
        .at = at,
        .offset = offset,
        .mode = containing.mode,
        .edges = edges,
    };
}

void layoutCachePlace(LayoutCache& cache, LayoutNodeIndex node, const LayoutFragment* fragment) {
    for (uint32_t i = fragment->childCount; i-- > 0;) {
        const LayoutChild& child = fragment->children[i];
        const LayoutFragment* c = child.fragment;
        bool continues = (c->flags & LayoutFragmentFlag_Continuation) != 0;
        switch (c->kind) {
            case LayoutFragmentKind_Line:
                placeLine(cache, node, c, child.offset, LayoutPieceAt{i, kLayoutPieceNone});
                break;
            case LayoutFragmentKind_Around: placeWithin(cache, node, c, child.offset, i, continues); break;
            case LayoutFragmentKind_Box:
                if (c->node != kPrLayoutNodeNone && !continues)
                    placeBox(cache, c->node, node, c, child.offset);
                else
                    placeWithin(cache, node, c, child.offset, i, continues);
                break;
            case LayoutFragmentKind_Replaced: placeBox(cache, c->node, node, c, child.offset); break;
            default: break;
        }
    }
}

bool layoutCacheFind(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, LayoutResult* out) {
    return find(state, node, space, requests, nullptr, out);
}

bool layoutCacheMayResize(const LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests) {
    const LayoutCacheEntry& entry = state.tree->cache.entries.data[node];
    return state.reuse && entry.fragment != nullptr && entry.available != availableKey(space, requests) &&
        (layoutTreeNode(state.tree, node).dirty & ~LayoutDirtyFlag_OutOfFlow) == 0 && mayResize(entry, space) &&
        !layoutIsOrthogonalRoot(state.tree, node, space.mode);
}

bool layoutCacheFindResized(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, const LayoutResolvedInline& resolved, LayoutResult* out) {
    return find(state, node, space, requests, &resolved, out);
}

void layoutCacheStore(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, const LayoutResult& result, const LayoutReads* PR_NULLABLE reads) {
    if (!state.reuse)
        return;
    LayoutTree* tree = state.tree;
    tree->nodes.data[node].dirty = 0;
    LayoutCacheEntry& entry = tree->cache.entries.data[node];
    layoutFragmentRetain(result.fragment);
    layoutFragmentRelease(entry.fragment);
    uint32_t context = entry.context;
    uint32_t absolute = entry.absolute;
    entry = LayoutCacheEntry{
        .available = availableKey(space, requests),
        .containingInline = space.containing.inlineSize,
        .containingBlock = space.containing.blockSize,
        .context = context,
        .fragment = result.fragment,
        .startPositive = result.start.positive,
        .startNegative = result.start.negative,
        .endPositive = result.end.positive,
        .endNegative = result.end.negative,
        .marginInlineStart = result.marginInlineStart,
        .stretchBlock = stretchKey(space, requests),
        .mode = space.mode,
        .flags = LayoutCacheFlags(result.flags | requestFlags(requests)),
        .absolute = absolute,
    };
    if ((result.flags & LayoutResultFlag_ReadsFallback) != 0)
        contextOf(tree->cache, entry).fallback = space.fallback;
    if (hasExtras(space)) {
        LayoutCacheContext& extras = contextOf(tree->cache, entry);
        extras.marker = space.marker;
        extras.cell = layoutCellSpaceFromSpace(space);
        entry.flags |= LayoutCacheFlag_SpaceExtras;
    }
    if (reads == nullptr)
        return;
    const LayoutBfc& bfc = *space.bfc;
    PrLayoutUnit top = result.top != kLayoutOffsetNone ? result.top : layoutMarginChainEnd(bfc.chain, result.start);
    recordContext(tree->cache, entry, bfc, *reads, top, result.cleared);
}

bool layoutCacheFindMeasure(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests& requests, LayoutMeasureKind kind, PrLayoutUnit* blockSize, LayoutBaselines* baselines) {
    const LayoutCache& cache = state.tree->cache;
    if (!state.reuse || node >= cache.intrinsics.count || !cache.intrinsics.data[node].measured)
        return false;
    uint32_t first = cache.measureAt.data[node];
    for (uint32_t i = 0; i < kLayoutMeasureSlots; ++i) {
        const LayoutMeasureRecord& r = cache.measures.data[first + i];
        if (!recordHolds(state, node, r, space, requests, kind))
            continue;
        *blockSize = r.blockSize;
        *baselines = r.baselines;
        ++state.stats->measuresReused;
        return true;
    }
    return false;
}

void layoutCacheStoreMeasure(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests& requests, LayoutMeasureKind kind, LayoutResultFlags flags,
        PrLayoutUnit blockSize, const LayoutBaseline& first, const LayoutBaseline& last) {
    if (!state.reuse)
        return;
    LayoutCache& cache = state.tree->cache;
    while (cache.measureAt.count <= node)
        cache.measureAt.push(kLayoutMeasureNone);
    while (cache.intrinsics.count < state.tree->nodes.count)
        cache.intrinsics.push(LayoutIntrinsicEntry{});
    if (cache.measureAt.data[node] == kLayoutMeasureNone) {
        uint32_t at = cache.measures.count;
        if (cache.freeMeasures.count != 0) {
            at = cache.freeMeasures.last();
            cache.freeMeasures.pop();
        } else {
            for (uint32_t i = 0; i < kLayoutMeasureSlots; ++i)
                cache.measures.push(LayoutMeasureRecord{});
        }
        cache.measureAt.data[node] = at;
    }
    // The newest record is first, so that a lookup finds it first. A record of the same request that has no baselines
    // is replaced by one that has. Otherwise the oldest record is replaced. Records that an invalidation left behind
    // are cleared.
    LayoutMeasureRecord* slots = cache.measures.data + cache.measureAt.data[node];
    bool& measured = cache.intrinsics.data[node].measured;
    uint32_t replaced = kLayoutMeasureSlots - 1;
    for (uint32_t i = 0; measured && i < kLayoutMeasureSlots; ++i) {
        if (recordHolds(state, node, slots[i], space, requests, LayoutMeasureKind_Size)) {
            replaced = i;
            break;
        }
    }
    if (measured && replaced == kLayoutMeasureSlots - 1 && (slots[replaced].flags & LayoutCacheFlag_Measured) != 0 &&
            !recordHolds(state, node, slots[replaced], space, requests, LayoutMeasureKind_Size))
        ++state.stats->measuresReplaced;
    if (measured) {
        clearRecord(slots[replaced]);
    } else {
        for (uint32_t i = 0; i < kLayoutMeasureSlots; ++i)
            clearRecord(slots[i]);
    }
    measured = true;
    for (uint32_t i = replaced; i > 0; --i)
        slots[i] = slots[i - 1];
    slots[0] = LayoutMeasureRecord{
        .available = availableKey(space, &requests),
        .containingInline = space.containing.inlineSize,
        .containingBlock = space.containing.blockSize,
        .stretchBlock = stretchKey(space, &requests),
        .fallback = space.fallback,
        .mode = space.mode,
        .flags = LayoutCacheFlags(flags | requestFlags(&requests) | LayoutCacheFlag_Measured |
            (kind == LayoutMeasureKind_Size ? LayoutCacheFlag_SizeOnly : 0)),
        .blockSize = blockSize,
        .cell = layoutCellSpaceFromSpace(space),
    };
    layoutBaselinesKeep(slots[0].baselines, first, last);
}

LayoutLineRecord& layoutCacheLines(LayoutCache& cache, LayoutNodeIndex first) {
    while (cache.lines.count <= first)
        cache.lines.push(nullptr);
    if (cache.lines.data[first] == nullptr)
        cache.lines.data[first] = allocZeroed<LayoutLineRecord>();
    return *cache.lines.data[first];
}

void layoutLineRecordClear(LayoutLineRecord& record) {
    for (uint32_t i = 0; i < record.lines.count; ++i)
        layoutFragmentRelease(record.lines.data[i].fragment);
    record.lines.clear();
    record.atomics.clear();
    record.collected = 0;
}

bool layoutCacheFindIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input,
        LayoutIntrinsicSizes* out) {
    const LayoutCache& cache = state.tree->cache;
    if (!state.reuse || node >= cache.intrinsics.count)
        return false;
    const LayoutIntrinsicEntry& entry = cache.intrinsics.data[node];
    if (!entry.valid || entry.mode != input.mode ||
            (entry.readsPercentBlock && entry.percentBlock != input.percentBlock) ||
            (entry.readsFallback && entry.fallback != input.fallback))
        return false;
    ++state.stats->intrinsicsReused;
    *out = LayoutIntrinsicSizes{
        .minContent = entry.minContent,
        .maxContent = entry.maxContent,
        .readsPercentBlock = entry.readsPercentBlock,
        .readsFallback = entry.readsFallback,
        .fillsAvailable = entry.fillsAvailable,
    };
    state.readsFallback = state.readsFallback || entry.readsFallback;
    return true;
}

void layoutCacheStoreIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input,
        const LayoutIntrinsicSizes& sizes) {
    if (!state.reuse)
        return;
    LayoutCache& cache = state.tree->cache;
    while (cache.intrinsics.count < state.tree->nodes.count)
        cache.intrinsics.push(LayoutIntrinsicEntry{});
    cache.intrinsics.data[node] = LayoutIntrinsicEntry{
        .minContent = sizes.minContent,
        .maxContent = sizes.maxContent,
        .percentBlock = input.percentBlock,
        .fallback = input.fallback,
        .mode = input.mode,
        .valid = true,
        .measured = cache.intrinsics.data[node].measured,
        .readsPercentBlock = sizes.readsPercentBlock,
        .readsFallback = sizes.readsFallback,
        .fillsAvailable = sizes.fillsAvailable,
    };
}

} // namespace pr_impl
