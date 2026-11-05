// Paged media: a tree's viewport root laid out as pages, which layoutViewport lays out (layout_block.h), and the boxes
// in the pages' margins.

#include "pr/layout_page.h"
#include "layout/src/layout_page.h"

#include "pr/assert.h"
#include "pr/draw_list.h"
#include "pr/search.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_query.h"
#include "layout/src/layout_scene.h"
#include "layout/src/layout_sizing.h"
#include "layout/src/layout_tree.h"

namespace pr_impl {

namespace {

// The physical axes of a page.
enum {
    Axis_X,
    Axis_Y,
};
typedef uint8_t Axis;

// What a margin box's style gives it along one physical axis: its padding and borders across the axis, its margins at
// the axis's start and end (left and right, or top and bottom), and its sizing properties there.
struct Extent {
    PrLayoutUnit edges;
    PrLayoutUnit margin[2];         // auto margins are zero
    bool marginAuto[2];
    PrLayoutLength size;
    PrLayoutLength min;
    PrLayoutLength max;
};

// A margin box of a page as it is sized: its containing block in the page box, what its style gives it in each axis by
// Axis, and its used border-box size and its border box's offset from the containing block's top left, by Axis.
struct MarginBox {
    LayoutNodeIndex node;
    const PrLayoutBoxStyle* style;
    LayoutMode mode;
    PrLayoutRect containing;
    Extent axis[2];
    PrLayoutUnit size[2];
    PrLayoutUnit start[2];
};

// Returns the extent of `rect` along `axis`.
PrLayoutUnit lengthOf(const PrLayoutRect& rect, Axis axis) {
    return axis == Axis_X ? rect.right - rect.left : rect.bottom - rect.top;
}

// Returns the extent of `rect`.
PrLayoutVec extentOf(const PrLayoutRect& rect) {
    return PrLayoutVec{rect.right - rect.left, rect.bottom - rect.top};
}

// Returns a margin box of `node` in `containing`. Its margins resolve against the containing block's width on the left
// and the right, and against its height on the top and the bottom (css-page-3/#page-properties).
MarginBox marginBoxOf(const LayoutState& state, LayoutNodeIndex node, PrLayoutRect containing) {
    const LayoutTree* tree = state.tree;
    MarginBox b{};
    b.node = node;
    b.style = &layoutBoxStyle(state.styles, layoutTreeNode(tree, node).style);
    b.mode = layoutOwnMode(tree, node);
    b.containing = containing;
    PrLayoutVec extent = extentOf(containing);
    LayoutLogicalSize logical = layoutLogicalSize(b.mode, extent);
    LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(*b.style, b.mode), logical.inlineSize);
    PrLayoutUnit edges[PrRectSide_Count]{};
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
        edges[layoutPhysicalSide(b.mode, side)] = e.padding[side] + e.border[side];
    const PrRectSide sides[2][2] = {{PrRectSide_Left, PrRectSide_Right}, {PrRectSide_Top, PrRectSide_Bottom}};
    for (Axis axis = Axis_X; axis <= Axis_Y; ++axis) {
        Extent& x = b.axis[axis];
        PrLayoutUnit base = axis == Axis_X ? extent.x : extent.y;
        for (uint32_t end = 0; end < 2; ++end) {
            PrRectSide side = sides[axis][end];
            x.edges = x.edges + edges[side];
            PrLayoutLength margin = b.style->margin[side];
            x.marginAuto[end] = layoutLengthIsAuto(margin);
            x.margin[end] = x.marginAuto[end] ? kLayoutUnitZero : layoutResolve(state.styles, margin, base);
        }
        x.size = axis == Axis_X ? b.style->width : b.style->height;
        x.min = axis == Axis_X ? b.style->minWidth : b.style->minHeight;
        x.max = axis == Axis_X ? b.style->maxWidth : b.style->maxHeight;
    }
    return b;
}

// Returns the border-box size that `length` gives the box `b` along `axis` against `base`, or a negative size if the
// length does not fix one.
PrLayoutUnit fixedSize(const LayoutState& state, const MarginBox& b, Axis axis, PrLayoutLength length, PrLayoutUnit base) {
    if (!layoutResolves(length, base))
        return layoutUnit(-1);
    return layoutBorderSize(*b.style, layoutResolve(state.styles, length, base), b.axis[axis].edges);
}

// Sizes the box `b` across its margin along `axis`, which is `room` long, as css-page-3/#fixed-sizing says. `outer` is
// the end of the axis at the page's edge, 0 for the start and 1 for the end, whose margin gives way if the box's size
// and margins are all given.
void fixAxis(const LayoutState& state, MarginBox& b, Axis axis, PrLayoutUnit room, uint32_t outer) {
    const Extent& x = b.axis[axis];
    PrLayoutUnit margin[2] = {x.margin[0], x.margin[1]};
    bool autos[2] = {x.marginAuto[0], x.marginAuto[1]};
    PrLayoutUnit size = fixedSize(state, b, axis, x.size, room);
    if (layoutIsDefinite(size)) {
        // Auto margins are zero if the box and its other margins do not fit.
        if (size + margin[0] + margin[1] > room)
            autos[0] = autos[1] = false;
    } else {
        // An auto size fills the margin, and its auto margins are zero.
        autos[0] = autos[1] = false;
        size = room - margin[0] - margin[1];
    }
    size = layoutClampSize(state.styles, *b.style, layoutUnitMax(size, x.edges), x.min, x.max, room, x.edges,
        layoutUnit(-1), layoutNoKeyword);
    PrLayoutUnit left = room - size - margin[0] - margin[1];
    if (autos[0] && autos[1])
        margin[0] = margin[0] + left / 2;
    else if (autos[0] || (!autos[1] && outer == 0))
        margin[0] = margin[0] + left;
    b.size[axis] = size;
    b.start[axis] = margin[0];
}

// What one box of a side gives the sizing of the side's boxes along it (css-page-3/#variable-sizing), in outer sizes:
// border-box sizes with the margins. A box that the page does not generate has an outer size of zero.
struct Share {
    bool sized;                     // false if its size is auto
    PrLayoutUnit outer;             // if sized
    PrLayoutUnit minContent;        // if not sized
    PrLayoutUnit maxContent;
};

// Sets the outer sizes of two boxes that share `room`: an auto size takes what the other leaves, and two auto sizes
// share it in proportion to their content (css-page-3/#flex-fit).
void share(const Share& a, const Share& b, PrLayoutUnit room, PrLayoutUnit* outA, PrLayoutUnit* outB) {
    if (a.sized || b.sized) {
        *outA = a.sized ? a.outer : room - b.outer;
        *outB = b.sized ? b.outer : room - a.outer;
        return;
    }
    double space = 0;
    double baseA = 0;
    double baseB = 0;
    double factorA = 0;
    double factorB = 0;
    if (a.maxContent + b.maxContent < room) {
        space = prLayoutUnitToPxDouble(room - a.maxContent - b.maxContent);
        baseA = factorA = prLayoutUnitToPxDouble(a.maxContent);
        baseB = factorB = prLayoutUnitToPxDouble(b.maxContent);
    } else {
        space = prLayoutUnitToPxDouble(room - a.minContent - b.minContent);
        baseA = prLayoutUnitToPxDouble(a.minContent);
        baseB = prLayoutUnitToPxDouble(b.minContent);
        if (a.minContent + b.minContent < room) {
            factorA = prLayoutUnitToPxDouble(a.maxContent - a.minContent);
            factorB = prLayoutUnitToPxDouble(b.maxContent - b.minContent);
        } else {
            factorA = baseA;
            factorB = baseB;
        }
    }
    if (factorA + factorB <= 0)
        factorA = factorB = 1;
    *outA = prLayoutUnitFromPxDouble(baseA + space * factorA / (factorA + factorB));
    *outB = prLayoutUnitFromPxDouble(baseB + space * factorB / (factorA + factorB));
}

// Returns the space that the box `b` is laid out in, in its own writing mode.
LayoutSpace spaceOf(const MarginBox& b) {
    PrLayoutVec extent = extentOf(b.containing);
    LayoutLogicalSize logical = layoutLogicalSize(b.mode, extent);
    return LayoutSpace{
        .mode = b.mode,
        .available = logical.inlineSize,
        .percentInline = logical.inlineSize,
        .percentBlock = logical.blockSize,
        .containing = logical,
        .blockLimit = layoutUnit(-1),
        .fallback = extent,
    };
}

// Sets a box's border-box min-content and max-content sizes along `axis`. Along its inline axis these are its intrinsic
// sizes. Along its block axis, both are its block size at its used size across the side.
void contentSizes(LayoutState& state, const MarginBox& b, Axis axis, PrLayoutUnit* minContent, PrLayoutUnit* maxContent) {
    LayoutSpace space = spaceOf(b);
    if ((axis == Axis_X) == layoutIsHorizontal(b.mode)) {
        LayoutIntrinsicSizes sizes = layoutIntrinsicSizes(state, b.node, layoutIntrinsicInputFromSpace(space));
        *minContent = sizes.minContent;
        *maxContent = sizes.maxContent;
        return;
    }
    Axis across = axis == Axis_X ? Axis_Y : Axis_X;
    LayoutRequests requests{.inlineAxis = {.size = b.size[across], .kind = LayoutRequestKind_Fixed, .definite = true}};
    LayoutMeasurement measured = layoutMeasure(state, b.node, space, requests, LayoutMeasureKind_Size);
    *minContent = measured.blockSize;
    *maxContent = measured.blockSize;
}

// Sizes and places along `axis` the boxes `row` of one side, from its start, each null if the page does not generate
// it, as css-page-3/#variable-sizing says: the first at the start of the side, the second centered on it, and the third
// at its end. Each box is already sized across the side.
void shareAxis(LayoutState& state, MarginBox* PR_NULLABLE const row[3], Axis axis) {
    PrLayoutUnit room{};
    for (uint32_t i = 0; i < 3; ++i) {
        if (row[i] != nullptr)
            room = lengthOf(row[i]->containing, axis);
    }
    // The sizes that min and max sizes put in place of the boxes' own, by box, or negative for none.
    PrLayoutUnit given[3] = {layoutUnit(-1), layoutUnit(-1), layoutUnit(-1)};
    PrLayoutUnit contentMin[3]{};
    PrLayoutUnit contentMax[3]{};
    bool measured[3]{};
    PrLayoutUnit outer[3]{};
    auto resolve = [&]() {
        Share shares[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            MarginBox* b = row[i];
            if (b == nullptr) {
                shares[i] = Share{.sized = true};
                continue;
            }
            const Extent& x = b->axis[axis];
            PrLayoutUnit margins = x.margin[0] + x.margin[1];
            PrLayoutUnit size = layoutIsDefinite(given[i]) ? given[i] : fixedSize(state, *b, axis, x.size, room);
            if (layoutIsDefinite(size)) {
                shares[i] = Share{.sized = true, .outer = size + margins};
                continue;
            }
            if (!measured[i])
                contentSizes(state, *b, axis, &contentMin[i], &contentMax[i]);
            measured[i] = true;
            shares[i] = Share{.minContent = contentMin[i] + margins, .maxContent = contentMax[i] + margins};
        }
        if (row[1] == nullptr) {
            share(shares[0], shares[2], room, &outer[0], &outer[2]);
            return;
        }
        // The middle box shares the side with an imaginary box twice the larger of the other two, which keeps it
        // centered, and an auto side box takes half of what it leaves.
        auto twice = [](PrLayoutUnit a, PrLayoutUnit b) { return layoutUnitMax(a, b) + layoutUnitMax(a, b); };
        Share sides{.sized = shares[0].sized && shares[2].sized};
        if (sides.sized) {
            sides.outer = twice(shares[0].outer, shares[2].outer);
        } else {
            auto least = [](const Share& s) { return s.sized ? s.outer : s.minContent; };
            auto most = [](const Share& s) { return s.sized ? s.outer : s.maxContent; };
            sides.minContent = twice(least(shares[0]), least(shares[2]));
            sides.maxContent = twice(most(shares[0]), most(shares[2]));
        }
        PrLayoutUnit rest{};
        share(shares[1], sides, room, &outer[1], &rest);
        PrLayoutUnit half = (room - outer[1]) / 2;
        outer[0] = shares[0].sized ? shares[0].outer : half;
        outer[2] = shares[2].sized ? shares[2].outer : half;
    };
    // The boxes take their own sizes, then a max size in place of a size past it, then a min size in place of a size
    // short of it (css-page-3/#variable-minmax).
    resolve();
    for (uint32_t pass = 0; pass < 2; ++pass) {
        bool again = false;
        for (uint32_t i = 0; i < 3; ++i) {
            MarginBox* b = row[i];
            if (b == nullptr)
                continue;
            const Extent& x = b->axis[axis];
            PrLayoutUnit used = outer[i] - x.margin[0] - x.margin[1];
            PrLayoutUnit limit = fixedSize(state, *b, axis, pass == 0 ? x.max : x.min, room);
            if (layoutIsDefinite(limit) && (pass == 0 ? used > limit : used < limit)) {
                given[i] = limit;
                again = true;
            }
        }
        if (again)
            resolve();
    }
    for (uint32_t i = 0; i < 3; ++i) {
        MarginBox* b = row[i];
        if (b == nullptr)
            continue;
        const Extent& x = b->axis[axis];
        PrLayoutUnit at = kLayoutUnitZero;
        if (i == 1)
            at = (room - outer[1]) / 2;
        else if (i == 2)
            at = room - outer[2];
        b->size[axis] = layoutUnitMax(outer[i] - x.margin[0] - x.margin[1], x.edges);
        b->start[axis] = at + x.margin[0];
    }
}

// Lays out the box `b` at its used size and appends its fragment to `out` at its place in the page box.
void layOutMarginBox(LayoutState& state, const MarginBox& b, RawArray<LayoutChild>& out) {
    LayoutLogicalSize size = layoutLogicalSize(b.mode, PrLayoutVec{b.size[Axis_X], b.size[Axis_Y]});
    LayoutRequests requests{
        .inlineAxis = {.size = size.inlineSize, .kind = LayoutRequestKind_Fixed, .definite = true},
        .blockAxis = {.size = size.blockSize, .kind = LayoutRequestKind_Fixed, .definite = true},
    };
    LayoutResult result = layoutFinal(state, b.node, spaceOf(b), requests);
    PrLayoutVec at{b.containing.left + b.start[Axis_X], b.containing.top + b.start[Axis_Y]};
    out.push(LayoutChild{result.fragment, at});
}

// The geometry of a sheet's marks (prLayoutPageBoxSheet), in px: 96 px are 72 pt.
constexpr PrLayoutUnit kMarkMinStart = layoutUnitPx(8);     // 6 pt, the closest that a crop mark comes to the page box
constexpr PrLayoutUnit kMarkLength = layoutUnitPx(32);      // 24 pt, the length of a crop mark
constexpr PrLayoutUnit kMarkPadding = layoutUnitPx(8);      // 6 pt between the marks and the sheet's edge
constexpr float kMarkWidth = 2.0f / 3;                      // 0.5 pt
constexpr float kCrossRadius = 8;                           // 6 pt, the radius of a registration mark's circle
constexpr float kCrossArm = 12;                             // 9 pt, the length of each arm of a registration mark

// Returns how far outside the page box a crop mark starts for a box of `bleed`: past the bleed area, so that no mark
// falls in what the page renders, and at least kMarkMinStart from the page box (css-page-3/#marks).
PrLayoutUnit marksStart(PrLayoutUnit bleed) {
    return layoutUnitMax(bleed, kMarkMinStart);
}

// Records the marks of `box`, whose page box is `trim` in the list's pixels.
void paintMarks(const PrLayoutPageBox& box, PrRect trim, PrDrawList* list) {
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = kMarkWidth;
    PrDrawPaint paint{.color = prColor16FromColor(PrColor{0, 0, 0, 1}), .stroke = &stroke};
    float start = prLayoutUnitToPx(marksStart(box.bleed));
    float end = start + prLayoutUnitToPx(kMarkLength);
    if ((box.marks & PrLayoutPageMarkFlag_Crop) != 0) {
        // Each corner has two lines, which extend the two edges that meet there.
        const float xs[2] = {trim.left, trim.right};
        const float ys[2] = {trim.top, trim.bottom};
        for (uint32_t i = 0; i < 2; ++i) {
            float out = i == 0 ? -1.0f : 1.0f;
            for (uint32_t k = 0; k < 2; ++k) {
                float down = k == 0 ? -1.0f : 1.0f;
                prDrawListLine(list, PrVec2{xs[i] + out * start, ys[k]}, PrVec2{xs[i] + out * end, ys[k]}, paint);
                prDrawListLine(list, PrVec2{xs[i], ys[k] + down * start}, PrVec2{xs[i], ys[k] + down * end}, paint);
            }
        }
    }
    if ((box.marks & PrLayoutPageMarkFlag_Cross) != 0) {
        float middle = (start + end) / 2;
        float cx = (trim.left + trim.right) / 2;
        float cy = (trim.top + trim.bottom) / 2;
        const PrVec2 centers[4] = {
            {cx, trim.top - middle}, {trim.right + middle, cy}, {cx, trim.bottom + middle}, {trim.left - middle, cy},
        };
        for (PrVec2 c : centers) {
            prDrawListEllipse(list, c, PrVec2{kCrossRadius, kCrossRadius}, 0, paint);
            prDrawListLine(list, PrVec2{c.x - kCrossArm, c.y}, PrVec2{c.x + kCrossArm, c.y}, paint);
            prDrawListLine(list, PrVec2{c.x, c.y - kCrossArm}, PrVec2{c.x, c.y + kCrossArm}, paint);
        }
    }
}

} // namespace

void layoutPageMargins(LayoutState& state, const PrLayoutPageBox& box, const LayoutNodeIndex* boxes,
        RawArray<LayoutChild>& out) {
    PrLayoutUnit width = box.extent.x;
    PrLayoutUnit height = box.extent.y;
    PrLayoutUnit left = box.margin[PrRectSide_Left];
    PrLayoutUnit top = box.margin[PrRectSide_Top];
    PrLayoutUnit right = width - box.margin[PrRectSide_Right];
    PrLayoutUnit bottom = height - box.margin[PrRectSide_Bottom];
    PrLayoutUnit zero = kLayoutUnitZero;
    // The containing block of each box: a corner of the page box's margins, or the margin between two corners.
    const PrLayoutRect containing[PrLayoutMarginBox_Count] = {
        {zero, zero, left, top},
        {left, zero, right, top},
        {left, zero, right, top},
        {left, zero, right, top},
        {right, zero, width, top},
        {right, top, width, bottom},
        {right, top, width, bottom},
        {right, top, width, bottom},
        {right, bottom, width, height},
        {left, bottom, right, height},
        {left, bottom, right, height},
        {left, bottom, right, height},
        {zero, bottom, left, height},
        {zero, top, left, bottom},
        {zero, top, left, bottom},
        {zero, top, left, bottom},
    };
    MarginBox laid[PrLayoutMarginBox_Count]{};
    MarginBox* found[PrLayoutMarginBox_Count]{};
    for (uint32_t i = 0; i < PrLayoutMarginBox_Count; ++i) {
        if (boxes[i] == kPrLayoutNodeNone)
            continue;
        laid[i] = marginBoxOf(state, boxes[i], containing[i]);
        found[i] = &laid[i];
    }
    // The page's edge is the outer end of each margin: the top of the top margin, the right of the right margin.
    auto fix = [&](uint32_t which, Axis axis, uint32_t outer) {
        if (found[which] != nullptr)
            fixAxis(state, *found[which], axis, lengthOf(containing[which], axis), outer);
    };
    fix(PrLayoutMarginBox_TopLeftCorner, Axis_X, 0);
    fix(PrLayoutMarginBox_BottomLeftCorner, Axis_X, 0);
    fix(PrLayoutMarginBox_TopRightCorner, Axis_X, 1);
    fix(PrLayoutMarginBox_BottomRightCorner, Axis_X, 1);
    fix(PrLayoutMarginBox_TopLeftCorner, Axis_Y, 0);
    fix(PrLayoutMarginBox_TopRightCorner, Axis_Y, 0);
    fix(PrLayoutMarginBox_BottomLeftCorner, Axis_Y, 1);
    fix(PrLayoutMarginBox_BottomRightCorner, Axis_Y, 1);
    // Each side's boxes from its start: left to right along the top and the bottom, top to bottom along the left and
    // the right, with the end of the axis across the side that is at the page's edge.
    struct Side {
        PrLayoutMarginBox boxes[3];
        Axis along;
        uint32_t outer;
    };
    const Side sides[] = {
        {{PrLayoutMarginBox_TopLeft, PrLayoutMarginBox_TopCenter, PrLayoutMarginBox_TopRight}, Axis_X, 0},
        {{PrLayoutMarginBox_BottomLeft, PrLayoutMarginBox_BottomCenter, PrLayoutMarginBox_BottomRight}, Axis_X, 1},
        {{PrLayoutMarginBox_LeftTop, PrLayoutMarginBox_LeftMiddle, PrLayoutMarginBox_LeftBottom}, Axis_Y, 0},
        {{PrLayoutMarginBox_RightTop, PrLayoutMarginBox_RightMiddle, PrLayoutMarginBox_RightBottom}, Axis_Y, 1},
    };
    for (const Side& side : sides) {
        MarginBox* row[3] = {found[side.boxes[0]], found[side.boxes[1]], found[side.boxes[2]]};
        if (row[0] == nullptr && row[1] == nullptr && row[2] == nullptr)
            continue;
        Axis across = side.along == Axis_X ? Axis_Y : Axis_X;
        for (PrLayoutMarginBox which : side.boxes)
            fix(which, across, side.outer);
        shareAxis(state, row, side.along);
    }
    for (uint32_t i = 0; i < PrLayoutMarginBox_Count; ++i) {
        if (found[i] != nullptr)
            layOutMarginBox(state, *found[i], out);
    }
}

PrLayoutPageBox layoutPageBoxFit(PrLayoutPageBox box) {
    box.extent.x = layoutUnitMax(box.extent.x, layoutUnitPx(1));
    box.extent.y = layoutUnitMax(box.extent.y, layoutUnitPx(1));
    for (PrLayoutUnit& margin : box.margin)
        margin = layoutUnitMax(margin, kLayoutUnitZero);
    auto fit = [](PrLayoutUnit size, PrLayoutUnit& a, PrLayoutUnit& b) {
        if (a + b > size) {
            a = layoutUnitMin(a, size);
            b = size - a;
        }
    };
    fit(box.extent.x, box.margin[PrRectSide_Left], box.margin[PrRectSide_Right]);
    fit(box.extent.y, box.margin[PrRectSide_Top], box.margin[PrRectSide_Bottom]);
    // A negative bleed leaves a bleed area of at least zero.
    PrLayoutUnit half = layoutUnitMin(box.extent.x, box.extent.y) / 2;
    box.bleed = layoutUnitMax(box.bleed, -half);
    return box;
}

} // namespace pr_impl

using namespace pr_impl;

PrLayoutPageBox prLayoutPageBoxDefault(void) {
    PrLayoutPageBox box{};
    box.extent = PrLayoutVec{layoutUnitPx(816), layoutUnitPx(1056)};
    return box;
}

PrLayoutVec prLayoutPageBoxArea(const PrLayoutPageBox* box) {
    PrLayoutPageBox fit = layoutPageBoxFit(*box);
    return PrLayoutVec{
        fit.extent.x - fit.margin[PrRectSide_Left] - fit.margin[PrRectSide_Right],
        fit.extent.y - fit.margin[PrRectSide_Top] - fit.margin[PrRectSide_Bottom],
    };
}

PrLayoutPageSheet prLayoutPageBoxSheet(const PrLayoutPageBox* box) {
    PrLayoutPageBox fit = layoutPageBoxFit(*box);
    PrLayoutUnit room = layoutUnitMax(fit.bleed, kLayoutUnitZero);
    if (fit.marks != PrLayoutPageMarkFlag_None)
        room = marksStart(fit.bleed) + kMarkLength + kMarkPadding;
    PrLayoutRect trim{room, room, room + fit.extent.x, room + fit.extent.y};
    return PrLayoutPageSheet{
        .extent = PrLayoutVec{fit.extent.x + room * 2, fit.extent.y + room * 2},
        .bleed =
            PrLayoutRect{trim.left - fit.bleed, trim.top - fit.bleed, trim.right + fit.bleed, trim.bottom + fit.bleed},
        .trim = trim,
    };
}

void prLayoutPageBoxPaint(const PrLayoutPageBox* box, PrColor16 canvas, PrVec2 origin, PrDrawList* list) {
    PrLayoutPageBox fit = layoutPageBoxFit(*box);
    PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&fit);
    PrRect bleed = prRectOffset(prLayoutRectToPx(sheet.bleed), origin);
    PrRect trim = prRectOffset(prLayoutRectToPx(sheet.trim), origin);
    // The page's background paints over the bleed area (css-page-3/#painting), and the canvas's over the page area.
    if (prFloat16ToFloat(fit.background.a) > 0)
        prDrawListRect(list, bleed, {.color = fit.background, .edges = PrDrawEdges_Snapped});
    if (prFloat16ToFloat(canvas.a) > 0) {
        PrRect area{
            trim.left + prLayoutUnitToPx(fit.margin[PrRectSide_Left]),
            trim.top + prLayoutUnitToPx(fit.margin[PrRectSide_Top]),
            trim.right - prLayoutUnitToPx(fit.margin[PrRectSide_Right]),
            trim.bottom - prLayoutUnitToPx(fit.margin[PrRectSide_Bottom]),
        };
        prDrawListRect(list, area, {.color = canvas, .edges = PrDrawEdges_Snapped});
    }
    if (fit.marks != PrLayoutPageMarkFlag_None)
        paintMarks(fit, trim, list);
}

void prLayoutTreeSetPagination(PrLayoutTree* tree, const PrLayoutPagination* PR_NULLABLE pagination) {
    PrLayoutPagination none{};
    tree->pagination = pagination != nullptr ? *pagination : none;
    layoutTreeForgetLayout(tree);
}

PrLayoutPageContext prLayoutNodeStartPage(const PrLayoutTree* tree, PrLayoutNode root) {
    return layoutStartPageContext(tree, layoutNodeIndex(tree, root));
}

uint32_t prLayoutTreePageCount(const PrLayoutTree* tree) {
    return tree->pages.count;
}

PrLayoutPage prLayoutTreePage(const PrLayoutTree* tree, uint32_t index) {
    PR_ASSERT(index < tree->pages.count);
    return tree->pages.data[index];
}

uint32_t prLayoutNodePage(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    LayoutNodeIndex index = layoutNodeIndex(t, node);
    if (t->pages.count == 0 || index == kPrLayoutNodeNone)
        return UINT32_MAX;
    const RawArray<LayoutRunningPage>& running = t->runningPages;
    uint32_t at = partitionPoint(0, running.count, [&](uint32_t i) { return running.data[i].node < index; });
    if (at < running.count && running.data[at].node == index)
        return running.data[at].page;
    if (!layoutNodeLaidOut(t, index))
        return UINT32_MAX;
    // The pages are stacked down the viewport's content.
    PrLayoutUnit y = layoutNodePosition(t, index).y;
    uint32_t after = partitionPoint(0, t->pages.count, [&](uint32_t i) { return t->pages.data[i].offset.y <= y; });
    return after != 0 ? after - 1 : 0;
}

void prLayoutTreeSetPageMargins(PrLayoutTree* tree, const PrLayoutNode* PR_NULLABLE boxes, uint32_t count) {
    LayoutTree* t = tree;
    uint32_t total = boxes != nullptr ? count * PrLayoutMarginBox_Count : 0;
    bool same = t->pageMargins.count == total;
    for (uint32_t i = 0; same && i < total; ++i)
        same = t->pageMargins.data[i] == boxes[i];
    if (same)
        return;
    t->pageMargins.clear();
    for (uint32_t i = 0; i < total; ++i)
        t->pageMargins.push(boxes[i]);
    t->pendingLayout |= LayoutInputFlag_Layout;
    if (t->root != kPrLayoutNodeNone)
        t->nodes.data[t->root].dirty |= LayoutDirtyFlag_Margins;
}

void prLayoutTreePageMargins(const PrLayoutTree* tree, uint32_t index, PrLayoutNode* boxes) {
    const LayoutTree* t = tree;
    for (uint32_t i = 0; i < PrLayoutMarginBox_Count; ++i) {
        uint32_t at = index * PrLayoutMarginBox_Count + i;
        PrLayoutNode node = at < t->pageMargins.count ? t->pageMargins.data[at] : PrLayoutNode(kPrLayoutNodeNone);
        boxes[i] = layoutNodeIsValid(t, node) ? node : PrLayoutNode(kPrLayoutNodeNone);
    }
}

void prLayoutTreePaintPage(PrLayoutTree* tree, uint32_t index, PrDrawList* list) {
    PR_ASSERT(index < tree->pages.count);
    PrLayoutPage page = tree->pages.data[index];
    PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&page.box);
    const PrLayoutScene* scene = layoutScene(tree);
    prLayoutPageBoxPaint(&page.box, prLayoutSceneCanvasColor(scene), PrVec2{0, 0}, list);
    prDrawListPushClipRect(list, prLayoutRectToPx(sheet.bleed), PrDrawEdges_Snapped);
    // Every scroll container but the viewport plays at its offset.
    RawArray<PrLayoutSceneScroll> scrolls;
    scrolls.resize(prLayoutSceneScrolls(tree, scene, nullptr, 0));
    prLayoutSceneScrolls(tree, scene, scrolls.data, scrolls.count);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < scrolls.count; ++i) {
        if (scrolls.data[i].node != kPrLayoutNodeNone)
            scrolls.data[kept++] = scrolls.data[i];
    }
    scrolls.count = kept;
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = scrolls.data;
    options.scrollCount = scrolls.count;
    options.canvasFilled = true;
    // The page box's top left is at its offset in the viewport's content, and at the trim's top left on the sheet.
    prDrawListPushTransformDAffine2(list, PrDAffine2{
        1, 0, 0, 1, prLayoutUnitToPxDouble(sheet.trim.left - page.offset.x),
        prLayoutUnitToPxDouble(sheet.trim.top - page.offset.y),
    });
    prLayoutScenePlay(scene, list, &options);
    prDrawListPopTransform(list);
    prDrawListPopClip(list);
    scrolls.reset();
    prLayoutSceneRelease(scene);
}
