// Exclusions: the floats of a block formatting context, the space they leave, and the record of what a layout read of
// them.
//
// CSS2 - https://drafts.csswg.org/css2/

#include "layout/src/layout_exclusion.h"

#include "layout/src/layout_fragmentation.h"

namespace pr_impl {

namespace {

// The smallest range that a query covers. A query of no size reads the floats that hold its offset.
constexpr PrLayoutUnit kLayoutUnitOne = PrLayoutUnit{1};

// Notes that a query read the floats over [y, y + h).
void readRange(LayoutReads& reads, PrLayoutUnit y, PrLayoutUnit h) {
    layoutReadsRange(reads, y, y + layoutUnitMax(h, kLayoutUnitOne));
}

bool excludes(const LayoutExclusion& e) {
    return e.top.raw < e.bottom.raw;
}

// Returns true if the block range of a float meets [y, y + h), or holds y if h is zero.
bool meets(const LayoutExclusion& e, PrLayoutUnit y, PrLayoutUnit h) {
    if (!excludes(e))
        return false;
    if (h.raw <= 0)
        return e.top.raw <= y.raw && y.raw < e.bottom.raw;
    return e.top.raw < (y + h).raw && e.bottom.raw > y.raw;
}

} // namespace

void layoutExclusionsInit(LayoutExclusions& exclusions) {
    exclusions.floats = RawArray<LayoutExclusion>{};
    exclusions.broken = RawArray<LayoutBrokenFloat>{};
    PrLayoutUnit none = kLayoutOffsetNone;
    exclusions.mark = LayoutExclusionMark{0, none, {none, none}, none};
}

void layoutExclusionsReset(LayoutExclusions& exclusions) {
    for (uint32_t i = 0; i < exclusions.broken.count; ++i)
        layoutBreakTokenRelease(exclusions.broken.data[i].token);
    exclusions.broken.reset();
    exclusions.floats.reset();
    layoutExclusionsInit(exclusions);
}

void layoutExclusionsAdd(LayoutExclusions& exclusions, const LayoutExclusion& exclusion) {
    exclusions.floats.push(exclusion);
    LayoutExclusionMark& mark = exclusions.mark;
    mark.count = exclusions.floats.count;
    mark.lastTop = exclusion.top;
    uint32_t side = exclusion.side == LayoutLineSide_Left ? 0 : 1;
    mark.lowest[side] = layoutUnitMax(mark.lowest[side], exclusion.bottom);
    mark.lowestAll = layoutUnitMax(mark.lowestAll, exclusion.bottom);
}

void layoutExclusionsCut(LayoutExclusions& exclusions, const LayoutExclusionMark& mark) {
    PR_ASSERT(mark.count <= exclusions.floats.count);
    exclusions.floats.count = mark.count;
    exclusions.mark = mark;
    while (exclusions.broken.count != 0 && exclusions.broken.last().exclusion >= mark.count) {
        layoutBreakTokenRelease(exclusions.broken.last().token);
        exclusions.broken.pop();
    }
}

LayoutReads layoutReadsBegin(const LayoutExclusions& exclusions) {
    LayoutReads reads{};
    reads.entry = exclusions.mark;
    return reads;
}

void layoutReadsMerge(LayoutReads& outer, const LayoutReads& inner) {
    layoutReadsRange(outer, inner.lo, inner.hi);
    for (uint32_t side = 0; side < 2; ++side) {
        if (inner.clearance[side].read)
            layoutReadsFloor(outer.clearance[side], inner.clearance[side].limit);
    }
    if (inner.lastTop.read)
        layoutReadsFloor(outer.lastTop, inner.lastTop.limit);
}

LayoutSpan layoutExclusionsAvailableAmong(const LayoutContainingBlock& cb, PrLayoutUnit y, PrLayoutUnit h, uint32_t count) {
    readRange(*cb.reads, y, h);
    LayoutSpan span{cb.lineLeft, cb.lineRight, true};
    const RawArray<LayoutExclusion>& floats = cb.exclusions->floats;
    PR_ASSERT(count <= floats.count);
    for (uint32_t i = 0; i < count; ++i) {
        const LayoutExclusion& e = floats.data[i];
        if (!meets(e, y, h))
            continue;
        span.free = false;
        if (e.side == LayoutLineSide_Left)
            span.left = layoutUnitMax(span.left, e.right);
        else
            span.right = layoutUnitMin(span.right, e.left);
    }
    return span;
}

PrLayoutUnit layoutExclusionsNextEdge(const LayoutContainingBlock& cb, PrLayoutUnit y) {
    PrLayoutUnit next = kLayoutOffsetNone;
    const RawArray<LayoutExclusion>& floats = cb.exclusions->floats;
    for (uint32_t i = 0; i < floats.count; ++i) {
        const LayoutExclusion& e = floats.data[i];
        if (excludes(e) && e.bottom.raw > y.raw && (next == kLayoutOffsetNone || e.bottom.raw < next.raw))
            next = e.bottom;
    }
    readRange(*cb.reads, y, next != kLayoutOffsetNone ? next - y : kLayoutUnitZero);
    return next;
}

LayoutFit layoutExclusionsFirstFit(const LayoutContainingBlock& cb, PrLayoutUnit y, PrLayoutUnit h,
        PrLayoutUnit inlineSize) {
    for (;;) {
        LayoutSpan span = layoutExclusionsAvailable(cb, y, h);
        if (inlineSize.raw <= (span.right - span.left).raw || span.free)
            return LayoutFit{y, span};
        // A float meets the range, so at least one float ends past y.
        PrLayoutUnit next = layoutExclusionsNextEdge(cb, y);
        PR_ASSERT(next != kLayoutOffsetNone && next.raw > y.raw);
        y = next;
    }
}

PrLayoutUnit layoutExclusionsClearance(const LayoutContainingBlock& cb, LayoutLineSides sides, PrLayoutUnit limit) {
    PrLayoutUnit floor = kLayoutOffsetNone;
    for (uint32_t side = 0; side < 2; ++side) {
        if ((sides & (1 << side)) == 0)
            continue;
        layoutReadsFloor(cb.reads->clearance[side], limit);
        floor = layoutUnitMax(floor, cb.exclusions->mark.lowest[side]);
    }
    return floor;
}

LayoutExclusion layoutExclusionsPlace(const LayoutContainingBlock& cb, PrLayoutUnit inlineSize,
        PrLayoutUnit blockSize, LayoutLineSides side, LayoutLineSides clears, PrLayoutUnit top) {
    // Rule 5: the float goes no higher than an earlier float. It also goes below the floats it clears.
    layoutReadsFloor(cb.reads->lastTop, top);
    PrLayoutUnit y = layoutUnitMax(top, cb.exclusions->mark.lastTop);
    if (clears != 0) {
        PrLayoutUnit floor = layoutExclusionsClearance(cb, clears, y);
        if (floor != kLayoutOffsetNone)
            y = layoutUnitMax(y, floor);
    }
    // Rules 1 to 3, 7 and 9: the float goes beside the floats it meets on its side, and not past those on the other
    // side. Rule 8: it goes as high as it can before going as far to its side as it can. A float that is too wide for
    // any space sticks out at an offset where no float meets it.
    LayoutFit fit = layoutExclusionsFirstFit(cb, y, layoutUnitMax(blockSize, kLayoutUnitZero), inlineSize);
    LayoutExclusion placed{};
    placed.side = side;
    placed.left = side == LayoutLineSide_Left ? fit.span.left : fit.span.right - inlineSize;
    placed.right = placed.left + inlineSize;
    placed.top = fit.y;
    placed.bottom = fit.y + blockSize;
    layoutExclusionsAdd(*cb.exclusions, placed);
    return placed;
}

} // namespace pr_impl
