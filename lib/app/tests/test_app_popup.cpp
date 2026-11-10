// Popup placement: the side and the alignment against the anchor, and the adjustments that keep a popup on its display.

#include "pr/app.h"

#include "app/src/app_popup.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// The work area of the tests: a display of 200 by 100.
constexpr PrRect kWork{0, 0, 200, 100};

// Returns true if two rectangles are equal.
bool same(PrRect a, PrRect b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a popup goes on its side of the anchor, aligned at the start, the center or the end, past its offset") {
    PrRect anchor{40, 10, 80, 20};
    PrAppPopupPlacement below{.side = PrAppPopupSide_Below};
    CHECK(same(appPopupPlace(below, anchor, PrVec2{30, 20}, kWork, false), PrRect{40, 20, 70, 40}));
    below.align = PrAppPopupAlign_End;
    CHECK(same(appPopupPlace(below, anchor, PrVec2{30, 20}, kWork, false), PrRect{50, 20, 80, 40}));
    below.align = PrAppPopupAlign_Center;
    below.offset = PrVec2{0, 2};
    CHECK(same(appPopupPlace(below, anchor, PrVec2{30, 20}, kWork, false), PrRect{45, 22, 75, 42}));
    PrAppPopupPlacement after{.side = PrAppPopupSide_After};
    CHECK(same(appPopupPlace(after, anchor, PrVec2{30, 20}, kWork, false), PrRect{80, 10, 110, 30}));
    // A right-to-left layout puts After to the left and the start at the right.
    CHECK(same(appPopupPlace(after, anchor, PrVec2{30, 20}, kWork, true), PrRect{10, 10, 40, 30}));
    CHECK(same(appPopupPlace(PrAppPopupPlacement{}, anchor, PrVec2{30, 20}, kWork, true), PrRect{50, 20, 80, 40}));
}

TEST_CASE("a popup that leaves the work area flips, then slides, then shrinks, as far as its adjustments allow") {
    PrRect anchor{40, 70, 80, 80};
    PrAppPopupPlacement placement{.side = PrAppPopupSide_Below};
    CHECK(same(appPopupPlace(placement, anchor, PrVec2{30, 30}, kWork, false), PrRect{40, 80, 70, 110}));
    placement.adjust = PrAppPopupAdjustFlag_Flip;
    CHECK(same(appPopupPlace(placement, anchor, PrVec2{30, 30}, kWork, false), PrRect{40, 40, 70, 70}));
    // A popup taller than either side's room slides to the work area's edge.
    placement.adjust = PrAppPopupAdjustFlag_Flip | PrAppPopupAdjustFlag_Slide;
    CHECK(same(appPopupPlace(placement, anchor, PrVec2{30, 75}, kWork, false), PrRect{40, 25, 70, 100}));
    // A popup taller than the work area shrinks to it.
    placement.adjust = PrAppPopupAdjustFlag_Slide | PrAppPopupAdjustFlag_Resize;
    CHECK(same(appPopupPlace(placement, anchor, PrVec2{30, 150}, kWork, false), PrRect{40, 0, 70, 100}));
}

TEST_CASE("a popup flips by its side's axis alone, and slides along the other") {
    // Below the anchor, the popup runs past the bottom and the right. Above it, it fits vertically, so it flips there
    // and slides left.
    PrRect anchor{180, 70, 200, 80};
    PrAppPopupPlacement placement{
        .side = PrAppPopupSide_Below,
        .adjust = PrAppPopupAdjustFlag_Flip | PrAppPopupAdjustFlag_Slide,
    };
    CHECK(same(appPopupPlace(placement, anchor, PrVec2{40, 30}, kWork, false), PrRect{160, 40, 200, 70}));
}
