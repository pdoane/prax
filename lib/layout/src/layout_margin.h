// Margins: adjoining margins as they collapse, and where they resolve in a block formatting context.
//
// CSS2 - https://drafts.csswg.org/css2/
//
// The block-start edge of a box's border box in its context is known once the margins that collapse with its
// block-start margin are known: those before it, its own, and those of its first children that collapse through its
// block-start edge. These margins form a chain from the last content edge before them. The chain grows down the tree
// until something ends it, such as a line, a block-start border or padding, a box that avoids floats or has clearance,
// or the end of a box that does not collapse through. The chain then resolves at its origin plus the margins collected
// since (CSS2/#collapsing-margins).
//
// A float met while a chain is open waits for the chain to resolve. It is placed where the margins resolve, before the
// content that resolves them, and that content flows around it. A layout that resolves a chain while floats are waiting
// stops there. The box that holds the waiting floats then lays out its children again with the resolution given
// (CSS2/#float-position).

#pragma once

#include "layout/src/layout_exclusion.h"
#include "layout/src/layout_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Struts

// Adjoining margins as they collapse: the largest positive one and the most negative one.
struct LayoutStrut {
    PrLayoutUnit positive;
    PrLayoutUnit negative;
};

// Returns the strut of one margin.
inline LayoutStrut layoutStrut(PrLayoutUnit margin) {
    return margin > kLayoutUnitZero ? LayoutStrut{margin, kLayoutUnitZero} : LayoutStrut{kLayoutUnitZero, margin};
}

// Returns the margins of two struts collapsed together.
inline LayoutStrut operator+(LayoutStrut a, LayoutStrut b) {
    return LayoutStrut{layoutUnitMax(a.positive, b.positive), layoutUnitMin(a.negative, b.negative)};
}
inline LayoutStrut& operator+=(LayoutStrut& a, LayoutStrut b) {
    return a = a + b;
}

// Returns the space that collapsed margins take.
inline PrLayoutUnit layoutStrutSize(LayoutStrut strut) {
    return strut.positive + strut.negative;
}

// ---------------------------------------------------------------------------------------------------------------------
// Chains

// The margins that collapse toward a border box edge whose block offset in its context is not yet known. Offsets are in
// the context's coordinates.
struct LayoutMarginChain {
    PrLayoutUnit origin;            // the block offset of the last content edge, where the chain starts
    LayoutStrut strut;              // the margins collected since the origin

    // Where the chain resolves, as given by a layout that started over to place the floats waiting for it, or
    // kLayoutOffsetNone.
    PrLayoutUnit resolved;

    // `floor` is the bottom of the floats that are cleared by a box whose margins are in the chain, or
    // kLayoutOffsetNone. `floorOrigin` is where the margins before that box resolve, which is where they resolve if
    // that box has clearance (CSS2/#flow-control). `clearsAdjoining` is true if that box clears floats adjoining the
    // chain, and so has clearance.
    PrLayoutUnit floor;
    PrLayoutUnit floorOrigin;
    bool clearsAdjoining;

    // The floats adjoining the chain from outside the box that holds it. They wait for the chain to resolve, or are
    // placed where `resolved` says.
    LayoutLineSides adjoining;
};

// Returns a chain from the content edge at `origin` with no margins.
inline LayoutMarginChain layoutMarginChainAt(PrLayoutUnit origin) {
    return LayoutMarginChain{origin, LayoutStrut{}, kLayoutOffsetNone, kLayoutOffsetNone, kLayoutOffsetNone, false, 0};
}

// Returns where the chain resolves with `more` margins collapsed into it, if no layout has said where.
inline PrLayoutUnit layoutMarginChainEnd(const LayoutMarginChain& chain, LayoutStrut more = LayoutStrut{}) {
    return chain.origin + layoutStrutSize(chain.strut + more);
}

// Adds a floor of clearance at `floor`, for a box whose margins are about to join the chain. If the chain had no floor,
// the margins before that box resolve where the chain ends now.
inline void layoutMarginChainClear(LayoutMarginChain& chain, PrLayoutUnit floor) {
    if (floor == kLayoutOffsetNone)
        return;
    if (chain.floor == kLayoutOffsetNone)
        chain.floorOrigin = layoutMarginChainEnd(chain);
    chain.floor = layoutUnitMax(chain.floor, floor);
}

// Where a chain resolves.
struct LayoutResolution {
    bool waits;                     // true if floats wait for it at `at`; the layout stops there
    bool cleared;                   // true if clearance puts the border box at the floor
    PrLayoutUnit at;                // where the margins resolve
    PrLayoutUnit top;               // where the border box is
};

// Returns where the chain resolves if it ends at `end` with all its margins collapsed. The margins resolve where a
// layout said, or else at `end`. The border box is at the floor if clearance puts it there. If `waiting` floats wait
// for the chain and no layout has said where it resolves, the chain waits at that point.
inline LayoutResolution layoutMarginChainResolve(const LayoutMarginChain& chain, PrLayoutUnit end, bool waiting) {
    PrLayoutUnit at = chain.resolved != kLayoutOffsetNone ? chain.resolved : end;
    // The box has clearance if the cleared floats adjoin the chain, or if the border box would otherwise not be past
    // them (CSS2/#flow-control).
    bool cleared = chain.floor != kLayoutOffsetNone && (chain.clearsAdjoining || at.raw <= chain.floor.raw);
    if (chain.resolved == kLayoutOffsetNone && waiting)
        return LayoutResolution{true, cleared, cleared ? chain.floorOrigin : end, kLayoutOffsetNone};
    return LayoutResolution{false, cleared, at, cleared ? chain.floor : at};
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
