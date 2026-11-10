// The placement of a popup against its anchor.

#include "app/src/app_popup.h"

namespace pr_impl {

namespace {

// Returns the start of a span of `length` aligned by `align` against [low, high]. `startLow` is true if the alignment's
// start is the low end.
float alignedOn(PrAppPopupAlign align, float low, float high, float length, bool startLow) {
    if (align == PrAppPopupAlign_Center)
        return (low + high - length) / 2;
    bool atLow = (align == PrAppPopupAlign_Start) == startLow;
    return atLow ? low : high - length;
}

PrAppPopupSide oppositeOf(PrAppPopupSide side) {
    switch (side) {
        case PrAppPopupSide_Below:
            return PrAppPopupSide_Above;
        case PrAppPopupSide_Above:
            return PrAppPopupSide_Below;
        case PrAppPopupSide_After:
            return PrAppPopupSide_Before;
        default:
            return PrAppPopupSide_After;
    }
}

PrRect placedOn(PrAppPopupSide side, const PrAppPopupPlacement& placement, PrRect anchor, PrVec2 extent,
        bool rightToLeft) {
    float left = 0;
    float top = 0;
    if (side == PrAppPopupSide_Below || side == PrAppPopupSide_Above) {
        top = side == PrAppPopupSide_Below ? anchor.bottom : anchor.top - extent.y;
        left = alignedOn(placement.align, anchor.left, anchor.right, extent.x, !rightToLeft);
    } else {
        bool right = (side == PrAppPopupSide_After) != rightToLeft;
        left = right ? anchor.right : anchor.left - extent.x;
        top = alignedOn(placement.align, anchor.top, anchor.bottom, extent.y, true);
    }
    left += placement.offset.x;
    top += placement.offset.y;
    return PrRect{left, top, left + extent.x, top + extent.y};
}

bool within(PrRect rect, PrRect work) {
    return rect.left >= work.left && rect.top >= work.top && rect.right <= work.right && rect.bottom <= work.bottom;
}

// Returns true if `rect` lies within `work` along the axis that leaves `side` of the anchor.
bool withinAcross(PrAppPopupSide side, PrRect rect, PrRect work) {
    if (side == PrAppPopupSide_Below || side == PrAppPopupSide_Above)
        return rect.top >= work.top && rect.bottom <= work.bottom;
    return rect.left >= work.left && rect.right <= work.right;
}

} // namespace

PrRect appPopupPlace(const PrAppPopupPlacement& placement, PrRect anchor, PrVec2 extent, PrRect work, bool rightToLeft) {
    // A flip moves the popup only across its side's axis, so only that axis decides it.
    PrAppPopupSide side = placement.side;
    PrRect rect = placedOn(side, placement, anchor, extent, rightToLeft);
    if (!withinAcross(side, rect, work) && (placement.adjust & PrAppPopupAdjustFlag_Flip) != 0) {
        PrRect flipped = placedOn(oppositeOf(side), placement, anchor, extent, rightToLeft);
        if (withinAcross(side, flipped, work))
            rect = flipped;
    }
    if (!within(rect, work) && (placement.adjust & PrAppPopupAdjustFlag_Slide) != 0) {
        float width = rect.right - rect.left;
        float height = rect.bottom - rect.top;
        float left = rect.left > work.right - width ? work.right - width : rect.left;
        float top = rect.top > work.bottom - height ? work.bottom - height : rect.top;
        left = left < work.left ? work.left : left;
        top = top < work.top ? work.top : top;
        rect = PrRect{left, top, left + width, top + height};
    }
    if (!within(rect, work) && (placement.adjust & PrAppPopupAdjustFlag_Resize) != 0) {
        rect.left = rect.left < work.left ? work.left : rect.left;
        rect.top = rect.top < work.top ? work.top : rect.top;
        rect.right = rect.right > work.right ? work.right : rect.right;
        rect.bottom = rect.bottom > work.bottom ? work.bottom : rect.bottom;
    }
    return rect;
}

} // namespace pr_impl
