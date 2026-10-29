// Exclusions: the floats of a block formatting context, the space they leave, and the record of what a layout read of
// them.
//
// CSS2 - https://drafts.csswg.org/css2/
//
// A block formatting context keeps the margin boxes of its floats in its own coordinates. The line axis runs from the
// line-left edge of its content box, and the block axis from the block-start edge of its content box. Every box within
// a context has the writing mode of the context, apart from its direction, so these axes are the same for all of them.
//
// Every query of the space the floats leave is recorded in the reads of the layout that made it, so that the result of
// a layout can be reused if it would read the same values again (layout_cache.h).

#pragma once

#include "pr/array.h"
#include "pr/layout_style.h"

#include "layout/src/layout_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Floats

// The sides of the line a float can take, as bits.
enum {
    LayoutLineSide_Left = 1 << 0,
    LayoutLineSide_Right = 1 << 1,
    LayoutLineSide_Both = LayoutLineSide_Left | LayoutLineSide_Right,
};
typedef uint8_t LayoutLineSides;

// No offset: an aggregate of no floats, a read of nothing, or a block offset not yet known.
constexpr PrLayoutUnit kLayoutOffsetNone = PrLayoutUnit{kLayoutUnitMin};

// The margin box of a float, in the coordinates of its context, and its side. An exclusion with no block size excludes
// nothing.
struct LayoutExclusion {
    PrLayoutUnit left;
    PrLayoutUnit right;
    PrLayoutUnit top;
    PrLayoutUnit bottom;
    LayoutLineSides side;
};

// Returns true if two exclusions are the same.
inline bool operator==(const LayoutExclusion& a, const LayoutExclusion& b) {
    return a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom && a.side == b.side;
}

// The number of floats a context holds, and the values that only grow as floats are added: the block-start of the last
// float (CSS2/#float-position rule 5), and the greatest block-end on each side and over all floats. A context can be
// cut back to a mark.
struct LayoutExclusionMark {
    uint32_t count;
    PrLayoutUnit lastTop;
    PrLayoutUnit lowest[2];         // by side: left, right
    PrLayoutUnit lowestAll;
};

// The floats of one block formatting context, in placement order.
struct LayoutExclusions {
    Array<LayoutExclusion> floats;
    LayoutExclusionMark mark;
};

// Initializes exclusions to empty, and frees them, leaving them empty.
void layoutExclusionsInit(LayoutExclusions& exclusions);
void layoutExclusionsReset(LayoutExclusions& exclusions);

// Adds a float and updates the values that grow with it.
void layoutExclusionsAdd(LayoutExclusions& exclusions, const LayoutExclusion& exclusion);

// Cuts the floats back to `mark`.
void layoutExclusionsCut(LayoutExclusions& exclusions, const LayoutExclusionMark& mark);

// ---------------------------------------------------------------------------------------------------------------------
// Reads

// A floor that a layout read, from the floats that existed when the layout began. Holds the value of the floor and the
// least offset it was compared with. A floor below that offset bounds nothing.
struct LayoutFloorRead {
    bool read;
    PrLayoutUnit value;
    PrLayoutUnit limit;
};

// What a layout of a box and its subtree read of the floats of its context, in the coordinates of the context, while
// the layout runs. These are the floats that existed when the layout began, not those it placed, which are its output.
struct LayoutReads {
    LayoutExclusionMark entry;      // the floats it reads
    PrLayoutUnit lo;                // the block range covered by its space queries; empty if lo >= hi
    PrLayoutUnit hi;
    LayoutFloorRead clearance[2];   // the lowest block-ends by side
    LayoutFloorRead lastTop;
};

// Starts the reads of a layout that begins now.
LayoutReads layoutReadsBegin(const LayoutExclusions& exclusions);

// Notes that a layout read the floats over [start, end), or a floor compared with `limit`.
inline void layoutReadsRange(LayoutReads& reads, PrLayoutUnit start, PrLayoutUnit end) {
    if (start.raw >= end.raw)
        return;
    if (reads.lo.raw >= reads.hi.raw) {
        reads.lo = start;
        reads.hi = end;
        return;
    }
    reads.lo = layoutUnitMin(reads.lo, start);
    reads.hi = layoutUnitMax(reads.hi, end);
}
inline void layoutReadsFloor(LayoutFloorRead& floor, PrLayoutUnit limit) {
    floor.limit = floor.read ? layoutUnitMin(floor.limit, limit) : limit;
    floor.read = true;
}

// Adds what `inner` read to `outer`. `inner` records a layout nested inside the layout that `outer` records. Adds the
// range of `inner` and its floors, since its floors read floats that `outer` reads too.
void layoutReadsMerge(LayoutReads& outer, const LayoutReads& inner);

// ---------------------------------------------------------------------------------------------------------------------
// Containing blocks

// The place of a containing block among the floats of its block formatting context: the floats of the context, the
// reads of the layout that queries them, and the content box of the containing block on the line axis.
struct LayoutContainingBlock {
    LayoutExclusions* exclusions;
    LayoutReads* reads;
    PrLayoutUnit lineLeft;
    PrLayoutUnit lineRight;
};

// Returns the inline-start offset, in the containing block of `direction`, of a box `size` long whose line-left edge is
// at `left`. layoutLineLeftFromInlineStart does the reverse.
inline PrLayoutUnit layoutInlineStartFromLeft(const LayoutContainingBlock& cb, PrTextDirection direction,
                                              PrLayoutUnit left, PrLayoutUnit size) {
    return direction == PrTextDirection_Ltr ? left - cb.lineLeft : cb.lineRight - (left + size);
}
inline PrLayoutUnit layoutLineLeftFromInlineStart(const LayoutContainingBlock& cb, PrTextDirection direction,
                                                  PrLayoutUnit inlineStart, PrLayoutUnit size) {
    return direction == PrTextDirection_Ltr ? cb.lineLeft + inlineStart : cb.lineRight - inlineStart - size;
}

// ---------------------------------------------------------------------------------------------------------------------
// Queries

// The space between floats on a line, and whether no float met the range it was asked for.
struct LayoutSpan {
    PrLayoutUnit left;
    PrLayoutUnit right;
    bool free;
};

// A block offset and the space there.
struct LayoutFit {
    PrLayoutUnit y;
    LayoutSpan span;
};

// Returns the space within the containing block between the floats that meet [y, y + h): past the line-right edge of
// every left float and before the line-left edge of every right float. A range of no size meets the floats that hold y.
// The Among form considers only the first `count` floats, those placed before a mark.
LayoutSpan layoutExclusionsAvailableAmong(const LayoutContainingBlock& cb, PrLayoutUnit y, PrLayoutUnit h,
                                          uint32_t count);
inline LayoutSpan layoutExclusionsAvailable(const LayoutContainingBlock& cb, PrLayoutUnit y, PrLayoutUnit h) {
    return layoutExclusionsAvailableAmong(cb, y, h, cb.exclusions->floats.count);
}

// Returns the least block-end of a float past y, which is where the space can widen, or kLayoutOffsetNone if there is
// none.
PrLayoutUnit layoutExclusionsNextEdge(const LayoutContainingBlock& cb, PrLayoutUnit y);

// Returns the least offset at or below `y` at which the space over [offset, offset + h) is at least `inlineSize` wide,
// or no float meets that range, together with the space at that offset. Each step moves down to a distinct float
// block-end, so the search takes at most one step for each float.
LayoutFit layoutExclusionsFirstFit(const LayoutContainingBlock& cb, PrLayoutUnit y, PrLayoutUnit h,
                                   PrLayoutUnit inlineSize);

// Returns the greatest block-end of the floats on `sides`, or kLayoutOffsetNone if there is none. Records the read as a
// floor compared with `limit`.
PrLayoutUnit layoutExclusionsClearance(const LayoutContainingBlock& cb, LayoutLineSides sides, PrLayoutUnit limit);

// Places a float whose margin box is `inlineSize` by `blockSize` on `side`, at or below `top` and below the floats on
// `clears` (CSS2/#float-position). Adds the float and returns its margin box.
LayoutExclusion layoutExclusionsPlace(const LayoutContainingBlock& cb, PrLayoutUnit inlineSize,
                                      PrLayoutUnit blockSize, LayoutLineSides side, LayoutLineSides clears,
                                      PrLayoutUnit top);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
