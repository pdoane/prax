// Positions in text fragments: where a caret stands along a fragment's line, and which offset a position is nearest.

#include "layout/src/layout_text_position.h"

#include "layout/src/layout_style.h"
#include "layout/src/layout_tree.h"
#include "layout/src/layout_unit.h"

namespace pr_impl {

namespace {

// A cluster of a text fragment: its bytes [start, end) in its node's text, and its edges along the line.
struct Cluster {
    uint32_t start;
    uint32_t end;
    float left;
    float right;
};

// Returns true if the fragment's line runs down the page, or up it if the fragment is turned counter-clockwise.
bool isVertical(const LayoutTree* tree, const LayoutFragment* fragment) {
    return layoutStyleTraits(tree->styles, layoutFragmentStyle(tree, fragment)).vertical;
}

// Returns the cluster at visual position `i` among the fragment's caret stops, whose line reaches `length` px.
Cluster clusterAt(const LayoutFragment* fragment, uint32_t i, float length) {
    const LayoutCaretStop* stops = layoutFragmentTextStops(fragment);
    uint32_t count = fragment->text.stopCount;
    bool reversed = (fragment->flags & LayoutFragmentFlag_Reversed) != 0;
    uint32_t after;
    if (reversed)
        after = (i > 0 ? stops[i - 1].byte : fragment->text.end);
    else
        after = (i + 1 < count ? stops[i + 1].byte : fragment->text.end);
    float right = i + 1 < count ? stops[i + 1].along : length;
    return Cluster{stops[i].byte, after, stops[i].along, right > stops[i].along ? right : stops[i].along};
}

} // namespace

uint32_t layoutTextStart(const LayoutFragment* fragment) {
    PR_ASSERT(fragment->kind == LayoutFragmentKind_Text && fragment->text.stopCount != 0);
    const LayoutCaretStop* stops = layoutFragmentTextStops(fragment);
    bool reversed = (fragment->flags & LayoutFragmentFlag_Reversed) != 0;
    return stops[reversed ? fragment->text.stopCount - 1 : 0].byte;
}

float layoutTextLength(const LayoutTree* tree, const LayoutFragment* fragment) {
    return prLayoutUnitToPx(isVertical(tree, fragment) ? fragment->extent.y : fragment->extent.x);
}

float layoutTextCaretAlong(const LayoutTree* tree, const LayoutFragment* fragment, uint32_t offset) {
    uint32_t start = layoutTextStart(fragment);
    uint32_t end = fragment->text.end;
    bool reversed = (fragment->flags & LayoutFragmentFlag_Reversed) != 0;
    float length = layoutTextLength(tree, fragment);
    offset = offset < start ? start : offset > end ? end : offset;
    uint32_t count = fragment->text.stopCount;
    for (uint32_t i = 0; i < count; ++i) {
        Cluster c = clusterAt(fragment, i, length);
        // A transform can make several glyphs of one source byte, as of a German sharp S, which leaves clusters with no
        // bytes. The offset stands before the first of them.
        if (offset == c.start)
            return reversed ? c.right : c.left;
        if (offset < c.start || offset >= c.end)
            continue;
        float fraction = float(offset - c.start) / float(c.end - c.start);
        float width = c.right - c.left;
        return reversed ? c.right - fraction * width : c.left + fraction * width;
    }
    // The end of the text, past the last cluster in the order of the text.
    return reversed ? layoutFragmentTextStops(fragment)[0].along : clusterAt(fragment, count - 1, length).right;
}

uint32_t layoutTextOffsetAt(const LayoutTree* tree, const LayoutFragment* fragment, float along) {
    // A position past a break would be on the next line.
    if ((fragment->flags & LayoutFragmentFlag_Break) != 0)
        return layoutFragmentTextStops(fragment)[0].byte;
    bool reversed = (fragment->flags & LayoutFragmentFlag_Reversed) != 0;
    float length = layoutTextLength(tree, fragment);
    uint32_t count = fragment->text.stopCount;
    uint32_t i = 0;
    while (i + 1 < count && along >= clusterAt(fragment, i, length).right)
        ++i;
    Cluster c = clusterAt(fragment, i, length);
    bool leftHalf = along < (c.left + c.right) / 2;
    return leftHalf != reversed ? c.start : c.end;
}

PrLayoutRect layoutTextSpanRect(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec at, float from,
        float to) {
    float lo = from < to ? from : to;
    float hi = from < to ? to : from;
    PrLayoutRect rect = layoutRectAt(at, fragment->extent);
    if (!isVertical(tree, fragment)) {
        rect.right = at.x + prLayoutUnitFromPx(hi);
        rect.left = at.x + prLayoutUnitFromPx(lo);
    } else if (fragment->turn == LayoutTextTurn_CounterClockwise) {
        PrLayoutUnit bottom = rect.bottom;
        rect.top = bottom - prLayoutUnitFromPx(hi);
        rect.bottom = bottom - prLayoutUnitFromPx(lo);
    } else {
        rect.bottom = at.y + prLayoutUnitFromPx(hi);
        rect.top = at.y + prLayoutUnitFromPx(lo);
    }
    return rect;
}

} // namespace pr_impl
