// Flex layout: a flex container's items, sized and placed along its line.

#include "layout/src/layout_flex.h"

#include "pr/alloc.h"
#include "pr/float.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_position.h"
#include "layout/src/layout_replaced.h"
#include "layout/src/layout_sizing.h"

namespace pr_impl {

// Where a flex item sits in the cross axis of its line once its alignment is resolved
// (css-flexbox-2/#align-items-property). It is flush with the cross-start or cross-end edge of the line, centred,
// stretched from cross-start, or placed with its first or last baseline on the shared baseline of the line.
enum {
    LayoutFlexPlace_Start,
    LayoutFlexPlace_End,
    LayoutFlexPlace_Center,
    LayoutFlexPlace_Stretch,
    LayoutFlexPlace_Baseline,
    LayoutFlexPlace_LastBaseline,
};
typedef uint8_t LayoutFlexPlace;

// An item of a flex container being laid out: what its style gives, and what the algorithm finds. Sizes are sizes of
// its border box, except during flexing, which works in the main size of its content box (css-flexbox-2/#main-sizing),
// in units that may be fractional.
struct LayoutFlexItem {
    LayoutNodeIndex node;           // for an anonymous item, the first node of its run, which keeps its caches
    const PrLayoutBoxStyle* box;
    const PrLayoutFlexStyle* flex;
    bool orthogonal;                // an orthogonal flow root, whose inline axis is its container's block axis
    bool mainInline;                // true if its inline axis is the main axis; otherwise its block axis is
    bool collapsed;                 // true if its visibility is collapse (layoutIsCollapsedItem)
    PrLayoutUnit strut;             // if collapsed: the cross size of its line when it was visible

    // Its preferred sizes and limits in the main and cross axes of the container.
    PrLayoutLength mainSize;
    PrLayoutLength minMainSize;
    PrLayoutLength maxMainSize;
    PrLayoutLength crossSize;
    PrLayoutLength minCrossSize;
    PrLayoutLength maxCrossSize;

    LayoutEdges e;                  // in the container's writing mode, with auto margins as zero
    PrLayoutUnit edgesMain;         // its padding and borders across the main axis
    PrLayoutUnit edgesCross;
    PrLayoutUnit marginsMain;       // its margins across the main axis
    PrLayoutUnit marginsCross;
    int32_t order;

    // The flex base size of its content box, which may be negative, its limits, its hypothetical main size, the main
    // size that flexing gives it, its flex factor, and whether flexing has frozen it.
    double base;
    double minMain;
    double maxMain;
    double hypothetical;
    double target;
    double factor;                  // its grow or shrink factor, depending on whether the line grows or shrinks
    bool frozen;

    bool definiteMain;              // true if its main size is definite once flexed (css-flexbox-2/#definite-sizes)
    bool stretched;                 // true if its cross size is the line's cross size
    bool safe;                      // true if its cross alignment falls back to cross-start when it overflows its line
    LayoutFlexPlace place;
    PrLayoutUnit main;              // its used main size
    PrLayoutUnit cross;             // its provisional cross size, then its hypothetical one, and then its used one

    // If it is main-inline in a row, the border-box cross size that its style fixes regardless of its content. It is
    // negative if its content decides the size, which is then the size its measurement gives.
    PrLayoutUnit styledCross;

    // Its baselines as its measurement at its hypothetical cross size exports them, if it was measured.
    LayoutBaseline first;
    LayoutBaseline last;

    PrLayoutUnit mainOffset;        // offset of its border box in the content box along the main axis
    PrLayoutUnit crossOffset;       // from the cross-start edge of its line, and then in the content box
};

// A line of a flex container being laid out: a run of its items in order-modified document order, and what the
// algorithm finds (css-flexbox-2/#flex-lines).
struct LayoutFlexLine {
    uint32_t first;                 // index of its first item, counted from the container's first item
    uint32_t count;
    PrLayoutUnit cross;             // its cross size
    PrLayoutUnit offset;            // offset of its cross-start edge from the cross-start edge of the content box
    PrLayoutUnit strut;             // the largest strut of the collapsed items it held, the least its cross size is

    // The extents above and below the shared baseline of its items aligned by first baseline, and of its items aligned
    // by last baseline (css-align-3/#baseline-sharing-group).
    PrLayoutUnit ascent;
    PrLayoutUnit descent;
    PrLayoutUnit lastAscent;
    PrLayoutUnit lastDescent;
};

namespace {

// Converts `size`, a size that the style of a box styled by `box` gives, to a content-box size. If box-sizing measures
// the border box, returns what `size` leaves inside `edges`. Otherwise returns `size` itself. Nothing floors the result
// at zero, because a flex base size is not floored (css-flexbox-2/#flex-base-size).
double contentOf(const PrLayoutBoxStyle& box, PrLayoutUnit size, PrLayoutUnit edges) {
    return box.boxSizing == PrLayoutBoxSizing_BorderBox ? double(size.raw - edges.raw) : double(size.raw);
}

// Returns the unit nearest to `raw` units.
PrLayoutUnit unitOf(double raw) {
    return prLayoutUnitClamped(int64_t(prF64Round(raw)));
}

// Returns true if a flex direction's main axis is the inline axis.
bool isRow(PrLayoutFlexDirection direction) {
    return direction == PrLayoutFlexDirection_Row || direction == PrLayoutFlexDirection_RowReverse;
}

// Returns where an item aligned by `alignSelf` sits in a line of a container in the mode `container`. The cross-start
// of the line is the logical side `crossStart`, or the other end of the axis if `crossReverse` is true. Sets `*safe` to
// whether its overflow position is safe (css-align-3/#positional-values, css-align-3/#overflow-values).
//
// An align-self of auto takes the container's `alignItems`. Normal means stretch, and stretch means flex-start if
// `stretches` is false. Baseline alignment requires the block axis of the item to be the cross axis, and otherwise
// falls back to safe self-start or safe self-end (css-align-3/#baseline-values). `left` and `right` are not values of
// align-self, and are treated as start.
LayoutFlexPlace crossPlaceOf(PrLayoutAlignment alignSelf, PrLayoutAlignment alignItems, bool stretches,
        bool crossReverse, LayoutMode container, LayoutSide crossStart, const LayoutTree* tree, LayoutNodeIndex node,
        bool* safe) {
    PrLayoutAlignment align = alignSelf;
    if ((align & kPrLayoutAlignmentValueMask) == PrLayoutAlignment_Auto)
        align = alignItems;
    *safe = (align & PrLayoutAlignmentFlag_Safe) != 0;
    PrLayoutAlignment value = PrLayoutAlignment(align & kPrLayoutAlignmentValueMask);
    switch (value) {
        case PrLayoutAlignment_Auto:
        case PrLayoutAlignment_Normal:
        case PrLayoutAlignment_Stretch: return stretches ? LayoutFlexPlace_Stretch : LayoutFlexPlace_Start;
        case PrLayoutAlignment_Center: return LayoutFlexPlace_Center;
        case PrLayoutAlignment_FlexStart: return LayoutFlexPlace_Start;
        case PrLayoutAlignment_FlexEnd: return LayoutFlexPlace_End;
        default: break;
    }
    LayoutMode mode = layoutOwnMode(tree, node);
    LayoutFlexPlace start = crossReverse ? LayoutFlexPlace_End : LayoutFlexPlace_Start;
    LayoutFlexPlace end = crossReverse ? LayoutFlexPlace_Start : LayoutFlexPlace_End;
    // The item's own start along the cross axis: its inline-start if its inline axis is the cross axis, and otherwise
    // its block-start.
    bool crossIsInline = crossStart == LayoutSide_InlineStart;
    LayoutSide own =
        layoutIsOrthogonal(mode, container) != crossIsInline ? LayoutSide_InlineStart : LayoutSide_BlockStart;
    bool sameStart = layoutPhysicalSide(mode, own) == layoutPhysicalSide(container, crossStart);
    LayoutFlexPlace selfStart = sameStart ? start : end;
    LayoutFlexPlace selfEnd = sameStart ? end : start;
    bool baselines = !crossIsInline && !layoutIsOrthogonal(mode, container);
    switch (value) {
        case PrLayoutAlignment_End: return end;
        case PrLayoutAlignment_SelfStart: return selfStart;
        case PrLayoutAlignment_SelfEnd: return selfEnd;
        case PrLayoutAlignment_Baseline:
            if (baselines)
                return LayoutFlexPlace_Baseline;
            *safe = true;
            return selfStart;
        case PrLayoutAlignment_LastBaseline:
            if (baselines)
                return LayoutFlexPlace_LastBaseline;
            *safe = true;
            return selfEnd;
        default: return start;
    }
}

// What one layout of a flex container reads: the container, its axes, and its content box.
struct Flex {
    LayoutState& state;
    LayoutNodeIndex node;
    const PrLayoutFlexStyle& flex;
    const LayoutSpace& content;     // the space its items are laid out in, apart from their requests
    bool row;                       // true if its main axis is its inline axis
    bool reverse;                   // true if its items run from the end of the main axis
    bool crossReverse;              // true if its cross-start is the end of the cross axis (wrap-reverse)
    bool singleLine;                // true if its items are on one line, as nowrap puts them
    PrLayoutUnit inlineSize;        // the inline size of its content box
    PrLayoutUnit blockSize;         // the block size of its content box, or negative if the items decide it
    PrLayoutUnit minBlock;
    PrLayoutUnit maxBlock;
    PrLayoutUnit mainGap;           // between two items on a line
    PrLayoutUnit crossGap;          // between two lines
    uint32_t mark;                  // index of its first item in the scratch items
    uint32_t count;                 // number of its items, less the collapsed ones once they leave its lines
    uint32_t collapsed;             // number of its collapsed items
    uint32_t lineMark;              // the first of its lines in the scratch lines
    uint32_t lineCount;

    // The items whose baselines are its first and last baselines (css-flexbox-2/#flex-baselines), or `count` if it has
    // none.
    uint32_t firstBaseline;
    uint32_t lastBaseline;

    LayoutFlexItem& item(uint32_t i) { return state.scratch->flex->items.data[mark + i]; }
    LayoutFlexLine& line(uint32_t i) { return state.scratch->flex->lines.data[lineMark + i]; }

    // The cross size of the content box if it is definite, which is also the cross size of its line if it has one line.
    // This is the block size of a row, which may be indefinite, or the inline size of a column.
    PrLayoutUnit definiteCross() const { return row ? blockSize : inlineSize; }
};

// Returns the inline padding and borders of the item: those across the main axis if it is main-inline, and otherwise
// those across the cross axis.
PrLayoutUnit edgesInlineOf(const LayoutFlexItem& it) {
    return it.mainInline ? it.edgesMain : it.edgesCross;
}

// Returns the intrinsic sizes of the item `it` in the content box of the container, in its own inline axis, with the
// edges it resolves there. The block-axis percentages of an orthogonal item resolve against the inline size of the
// container.
LayoutIntrinsicSizes intrinsicOf(Flex& f, const LayoutFlexItem& it) {
    LayoutIntrinsicInput input = layoutIntrinsicInputFromSpace(f.content);
    if (it.orthogonal) {
        input.mode = layoutOwnMode(f.state.tree, it.node);
        input.percentBlock = f.inlineSize;
    }
    LayoutLogicalBox logical = layoutLogicalBox(*it.box, input.mode);
    PrLayoutUnit measured = layoutEdgesInline(layoutEdgesFromBox(f.state.styles, logical, layoutUnit(-1)));
    return layoutWithEdges(layoutIntrinsicSizes(f.state, it.node, input), measured, edgesInlineOf(it));
}

// Returns a measurement of `kind` of the item `node` at the border-box inline size `inlineSize`, with `block` as the
// request for its block size.
LayoutMeasurement measureItem(Flex& f, LayoutNodeIndex node, PrLayoutUnit inlineSize, LayoutRequest block,
        LayoutMeasureKind kind) {
    LayoutRequests requests{
        .inlineAxis = LayoutRequest{.size = inlineSize, .kind = LayoutRequestKind_Fixed, .definite = true},
        .blockAxis = block,
    };
    return layoutMeasure(f.state, node, f.content, requests, kind);
}

// Returns the stretch-fit size of an item's cross axis: the container's definite cross size less the item's margins
// (css-sizing-3/#stretch-fit-size), which a cross size of stretch takes.
PrLayoutUnit stretchSize(const Flex& f, const LayoutFlexItem& it) {
    LayoutSide crossStart = f.row ? LayoutSide_BlockStart : LayoutSide_InlineStart;
    LayoutSide crossEnd = f.row ? LayoutSide_BlockEnd : LayoutSide_InlineEnd;
    return layoutStretchFit(f.definiteCross(), it.e.margin[crossStart], it.e.margin[crossEnd]);
}

// Returns the cross size that a main-block item has before flexing, which is its inline size
// (css-flexbox-2/#cross-sizing). A stretched item is stretched to the content box of the container. Any other item is
// sized by its style, and an auto size fits the content box. A stretched item of a multi-line container also fits the
// content box, and is stretched once its line is sized. If the cross size of the content box is indefinite, as for a
// row of auto height, an orthogonal item fits the fallback that its writing mode gives it
// (css-writing-modes-4/#orthogonal-auto).
PrLayoutUnit provisionalCross(Flex& f, const LayoutFlexItem& it) {
    PrLayoutUnit available = f.inlineSize;
    PrLayoutUnit base = f.inlineSize;
    if (f.row) {
        available = f.blockSize;
        base = f.content.percentBlock;
        if (!layoutIsDefinite(available)) {
            available = layoutLogicalSize(f.content.mode, f.content.fallback).blockSize;
            if (layoutIsDefinite(f.content.blockLimit))
                available = layoutUnitMin(available, f.content.blockLimit);
            f.state.readsFallback = true;
        }
    }
    LayoutSide crossStart = f.row ? LayoutSide_BlockStart : LayoutSide_InlineStart;
    LayoutSide crossEnd = f.row ? LayoutSide_BlockEnd : LayoutSide_InlineEnd;
    PrLayoutUnit stretch = layoutStretchFit(available, it.e.margin[crossStart], it.e.margin[crossEnd]);
    bool measured = false;
    LayoutIntrinsicSizes sizes{};
    auto keyword = [&](PrLayoutLength length) {
        if (!measured)
            sizes = intrinsicOf(f, it);
        measured = true;
        return layoutKeywordSize(length, sizes, stretch);
    };
    PrLayoutUnit size = kLayoutUnitZero;
    if (it.stretched && f.singleLine && (!f.row || layoutIsDefinite(f.blockSize)))
        size = layoutUnitMax(stretch, it.edgesCross);
    else if (layoutLengthIsStretch(it.crossSize) && layoutIsDefinite(f.definiteCross()))
        size = layoutUnitMax(stretchSize(f, it), it.edgesCross);
    else if (layoutResolves(it.crossSize, base))
        size = layoutBorderSize(*it.box, layoutResolve(f.state.styles, it.crossSize, base), it.edgesCross);
    else
        size = keyword(layoutLengthIsContentBased(it.crossSize) ? it.crossSize : prLayoutLengthFitContent());
    return layoutClampSize(f.state.styles, *it.box, size, it.minCrossSize, it.maxCrossSize, base, it.edgesCross,
        stretch, keyword);
}

// Returns the preferred aspect ratio of the item, in the writing mode whose inline axis is its own. That is the item's
// own writing mode if it is orthogonal, and otherwise the container's.
LayoutAspect aspectOf(const Flex& f, const LayoutFlexItem& it) {
    LayoutMode mode = it.orthogonal ? layoutOwnMode(f.state.tree, it.node) : f.content.mode;
    return layoutNodeAspect(f.state.tree, it.node, *it.box, mode);
}

// mainThrough returns the border-box main size that the border-box cross size `cross` gives through the ratio `aspect`
// of the item. crossThrough returns the cross size that a main size gives.
PrLayoutUnit mainThrough(const LayoutFlexItem& it, const LayoutAspect& aspect, PrLayoutUnit cross) {
    if (it.mainInline)
        return layoutAspectInline(aspect, cross, it.edgesMain, it.edgesCross);
    return layoutAspectBlock(aspect, cross, it.edgesCross, it.edgesMain);
}
PrLayoutUnit crossThrough(const LayoutFlexItem& it, const LayoutAspect& aspect, PrLayoutUnit main) {
    if (it.mainInline)
        return layoutAspectBlock(aspect, main, it.edgesMain, it.edgesCross);
    return layoutAspectInline(aspect, main, it.edgesCross, it.edgesMain);
}

// Returns the cross size of a main-block item at its main size (css-flexbox-2/#hypothetical-cross-size). An item with a
// preferred aspect ratio whose cross size is auto, and that is not stretched, takes its cross size from its main size
// through the ratio, clamped by its cross limits. Any other item keeps its provisional cross size.
PrLayoutUnit hypotheticalCross(Flex& f, const LayoutFlexItem& it) {
    if (it.stretched || !layoutLengthIsAuto(it.crossSize))
        return it.cross;
    LayoutAspect aspect = aspectOf(f, it);
    if (aspect.ratio <= 0)
        return it.cross;
    PrLayoutUnit base = f.row ? f.content.percentBlock : f.inlineSize;
    return layoutClampSize(f.state.styles, *it.box, crossThrough(it, aspect, it.main), it.minCrossSize,
        it.maxCrossSize, base, it.edgesCross, layoutUnit(-1), layoutNoKeyword);
}

// The main sizes that the preferred aspect ratio of an item gives, as content-box main sizes. Each is negative if the
// ratio gives none (css-flexbox-2/#algo-main-item, css-flexbox-2/#min-size-auto).
struct RatioSizes {
    double base;                    // the flex base size from a definite cross size, if the basis is content
    double transferred;             // the transferred size suggestion
    double least;                   // the definite cross minimum through the ratio, which floors the content suggestion
    double most;                    // the definite cross maximum through the ratio, which caps the content suggestion
    bool replaced;                  // true if the content suggestion yields to the transferred one, not exceeds it
};

// An item with no preferred aspect ratio.
constexpr RatioSizes kNoRatio{.base = -1, .transferred = -1, .least = -1, .most = -1};

// The content-box sizes that decide the hypothetical main size of an item (css-flexbox-2/#flex-base-size,
// css-flexbox-2/#min-size-auto).
struct MainSizes {
    double base;                    // its flex base size, which may be negative
    double min;
    double max;
    bool contentBased;
};

// Returns the main sizes of an item styled by `box` and `flex`, from `mainLength` and its limits. Their percentages
// resolve against `base` if it is definite, and `edges` are the padding and borders across the main axis. `content`
// gives the main size of the content box for a sizing keyword, and for min-content it gives the content size
// suggestion. An auto basis uses the preferred size. A basis of content, or one that does not resolve, uses the
// max-content size of the content unless it names another keyword. The automatic minimum of a scroll container is zero.
// The specified size suggestion of a `compressible` item, a semi-replaced one such as a form control, resolves its
// percentages against zero (css-sizing-3/#replaced-percentage-min-contribution).
template <typename Content>
MainSizes mainSizesOf(const LayoutStyles* styles, const PrLayoutBoxStyle& box, const PrLayoutFlexStyle& flex,
        PrLayoutLength mainLength, PrLayoutLength minLength, PrLayoutLength maxLength,
        PrLayoutUnit base, PrLayoutUnit edges, const RatioSizes& ratio, bool compressible, Content content) {
    auto resolved = [&](PrLayoutLength length) { return contentOf(box, layoutResolve(styles, length, base), edges); };
    MainSizes out{};
    PrLayoutLength basis = flex.basis;
    if (layoutLengthIsAuto(basis))
        basis = mainLength;
    out.contentBased = !layoutResolves(basis, base) || layoutLengthIsContentBased(basis);
    // A basis of content, whether named or given by an auto size, takes the base that the ratio gives.
    bool contentBasis = !layoutLengthIsContentBased(basis) && !layoutResolves(basis, base);
    if (out.contentBased && contentBasis && ratio.base >= 0) {
        out.contentBased = false;
        out.base = ratio.base;
    } else {
        if (out.contentBased)
            out.base = content(layoutLengthIsContentBased(basis) ? basis : prLayoutLengthMaxContent());
        else
            out.base = resolved(basis);
    }
    out.max = 1e18;
    if (layoutLengthIsContentBased(maxLength))
        out.max = content(maxLength);
    else if (layoutResolves(maxLength, base))
        out.max = resolved(maxLength);
    if (layoutLengthIsAuto(minLength)) {
        // The automatic minimum starts from the content size suggestion, clamped by the cross limits through the ratio.
        // It then takes the lesser of that and the transferred size suggestion for a replaced item, and the greater of
        // the two for any other item. Finally it is capped by the specified size suggestion and the maximum.
        if (!layoutIsScrollContainer(box)) {
            double minimum = content(prLayoutLengthMinContent());
            if (ratio.most >= 0)
                minimum = prF64Min(minimum, ratio.most);
            if (ratio.least >= 0)
                minimum = prF64Max(minimum, ratio.least);
            if (ratio.transferred >= 0)
                minimum = ratio.replaced ? prF64Min(minimum, ratio.transferred) : prF64Max(minimum, ratio.transferred);
            if (layoutResolves(mainLength, base) && !layoutLengthIsContentBased(mainLength)) {
                PrLayoutUnit specified = compressible ? kLayoutUnitZero : base;
                minimum = prF64Min(minimum, contentOf(box, layoutResolve(styles, mainLength, specified), edges));
            }
            out.min = prF64Max(0, prF64Min(minimum, out.max));
        }
    } else if (layoutLengthIsContentBased(minLength)) {
        out.min = content(minLength);
    } else if (layoutResolves(minLength, base)) {
        out.min = resolved(minLength);
    }
    return out;
}

// Returns the main sizes that the preferred aspect ratio of the item gives. Its cross size is definite if its preferred
// cross size resolves, or if it is stretched in a single line of definite cross size (css-flexbox-2/#definite-sizes).
// Each size is clamped by the cross limits that resolve.
RatioSizes ratioSizesOf(Flex& f, const LayoutFlexItem& it) {
    LayoutAspect aspect = aspectOf(f, it);
    if (aspect.ratio <= 0)
        return kNoRatio;
    PrLayoutUnit base = f.row ? f.content.percentBlock : f.inlineSize;
    auto inner = [&](PrLayoutUnit cross) { return double((mainThrough(it, aspect, cross) - it.edgesMain).raw); };
    auto clamped = [&](PrLayoutUnit cross) {
        return layoutClampSize(f.state.styles, *it.box, cross, it.minCrossSize, it.maxCrossSize, base, it.edgesCross,
            layoutUnit(-1), layoutNoKeyword);
    };
    auto limit = [&](PrLayoutLength length) {
        if (!layoutResolves(length, base))
            return -1.0;
        return inner(layoutBorderSize(*it.box, layoutResolve(f.state.styles, length, base), it.edgesCross));
    };
    RatioSizes sizes{
        .base = -1,
        .transferred = -1,
        .least = limit(it.minCrossSize),
        .most = limit(it.maxCrossSize),
        .replaced = layoutTreeNode(f.state.tree, it.node).kind == PrLayoutNodeKind_Replaced,
    };
    if (layoutResolves(it.crossSize, base)) {
        PrLayoutUnit cross =
            layoutBorderSize(*it.box, layoutResolve(f.state.styles, it.crossSize, base), it.edgesCross);
        sizes.transferred = inner(clamped(cross));
        sizes.base = sizes.transferred;
    } else if (layoutLengthIsStretch(it.crossSize) && layoutIsDefinite(f.definiteCross())) {
        sizes.transferred = inner(clamped(layoutUnitMax(stretchSize(f, it), it.edgesCross)));
        sizes.base = sizes.transferred;
    } else if (it.stretched && f.singleLine && layoutIsDefinite(f.definiteCross())) {
        LayoutSide crossStart = f.row ? LayoutSide_BlockStart : LayoutSide_InlineStart;
        LayoutSide crossEnd = f.row ? LayoutSide_BlockEnd : LayoutSide_InlineEnd;
        PrLayoutUnit stretch = layoutStretchFit(f.definiteCross(), it.e.margin[crossStart], it.e.margin[crossEnd]);
        sizes.base = inner(clamped(layoutUnitMax(stretch, it.edgesCross)));
    }
    return sizes;
}

// Finds the flex base size, the main limits and the hypothetical main size of the item
// (css-flexbox-2/#hypothetical-main-size). If the item is main-block, also finds its provisional cross size, at which
// the block size of its content is measured.
void sizeMain(Flex& f, LayoutFlexItem& it) {
    // The base that the main percentages of the item resolve against: the content box of the container, if its main
    // size is definite.
    PrLayoutUnit base = f.row ? f.inlineSize : f.content.percentBlock;
    if (!it.mainInline)
        it.cross = provisionalCross(f, it);

    // The main size of the content. A main-inline item takes it from its intrinsic sizes. A main-block item takes it
    // from a measurement of its block size at its cross size, asking for the size of its content.
    bool measured = false;
    LayoutIntrinsicSizes sizes{};
    LayoutMeasurement content{};
    auto ofContent = [&](PrLayoutLength keyword) -> double {
        if (!measured) {
            if (it.mainInline)
                sizes = intrinsicOf(f, it);
            else
                content = measureItem(f, it.node, it.cross, LayoutRequest{.kind = LayoutRequestKind_Content},
                    LayoutMeasureKind_Size);
        }
        measured = true;
        if (!it.mainInline)
            return double(content.blockSize.raw - it.edgesMain.raw);
        LayoutSide mainStart = f.row ? LayoutSide_InlineStart : LayoutSide_BlockStart;
        LayoutSide mainEnd = f.row ? LayoutSide_InlineEnd : LayoutSide_BlockEnd;
        PrLayoutUnit available = layoutStretchFit(base, it.e.margin[mainStart], it.e.margin[mainEnd]);
        return double(layoutKeywordSize(keyword, sizes, available).raw - it.edgesMain.raw);
    };
    bool compressible = layoutTreeNode(f.state.tree, it.node).kind == PrLayoutNodeKind_SemiReplaced;
    MainSizes main = mainSizesOf(f.state.styles, *it.box, *it.flex, it.mainSize, it.minMainSize, it.maxMainSize, base,
        it.edgesMain, ratioSizesOf(f, it), compressible, ofContent);
    it.base = main.base;
    it.minMain = main.min;
    it.maxMain = main.max;
    // A definite flex basis, or a container of definite main size, makes the flexed size definite.
    it.definiteMain = f.row || !main.contentBased || layoutIsDefinite(f.content.percentBlock);
    it.hypothetical = prF64Max(0, prF64Max(prF64Min(it.base, it.maxMain), it.minMain));
}

// Returns the total of the gaps between `count` items on a line.
double gapsOf(const Flex& f, uint32_t count) {
    return count > 1 ? double(f.mainGap.raw) * double(count - 1) : 0;
}

// Returns the main size of the content box of the container (css-flexbox-2/#line-sizing). This is the inline size of a
// row, or the block size of a column if the container has its own. Otherwise it is the sum of the hypothetical outer
// sizes of the items and the gaps between them, clamped. The sum covers every item if `visible` is true, and otherwise
// every item except the collapsed ones (css-flexbox-2/#algo-visibility).
double containerMain(Flex& f, bool visible) {
    if (f.row)
        return double(f.inlineSize.raw);
    if (layoutIsDefinite(f.blockSize))
        return double(f.blockSize.raw);
    double sum = gapsOf(f, visible ? f.count : f.count - f.collapsed);
    for (uint32_t i = 0; i < f.count; ++i) {
        const LayoutFlexItem& it = f.item(i);
        if (!visible && it.collapsed)
            continue;
        sum += it.hypothetical + double(it.edgesMain.raw + it.marginsMain.raw);
    }
    if (layoutIsDefinite(f.maxBlock))
        sum = prF64Min(sum, double(f.maxBlock.raw));
    return prF64Max(sum, double(f.minBlock.raw));
}

// Returns the outer hypothetical main size of an item, which line breaking sums (css-flexbox-2/#algo-line-break). It is
// zero for a collapsed item if `visible` is false.
double outerHypothetical(const LayoutFlexItem& it, bool visible) {
    return !visible && it.collapsed ? 0 : it.hypothetical + double(it.edgesMain.raw + it.marginsMain.raw);
}

// Collects the items into lines in the scratch lines (css-flexbox-2/#algo-line-break). A single-line container puts all
// items on one line. Otherwise each line takes as many items as fit in `main` units, with the gaps between them, and at
// least one item. A collapsed item counts as visible if `visible` is true, and otherwise as zero size, though it still
// takes a gap as any item does.
void collectLines(Flex& f, double main, bool visible) {
    Array<LayoutFlexLine>& lines = f.state.scratch->flex->lines;
    f.lineMark = lines.count;
    uint32_t first = 0;
    while (first < f.count || f.lineCount == 0) {
        uint32_t end = first + 1;
        if (f.singleLine) {
            end = f.count;
        } else if (first < f.count) {
            double used = outerHypothetical(f.item(first), visible);
            while (end < f.count && used + double(f.mainGap.raw) + outerHypothetical(f.item(end), visible) <= main) {
                used += double(f.mainGap.raw) + outerHypothetical(f.item(end), visible);
                ++end;
            }
        }
        end = end < f.count ? end : f.count;
        lines.push(LayoutFlexLine{.first = first, .count = end - first});
        ++f.lineCount;
        first = end;
    }
}

// Drops the collapsed items from the lines once they are collected (css-flexbox-2/#algo-visibility). Each line keeps
// the largest strut of the collapsed items it held. From here on those items are ignored, as if their display were
// none. The visible items move down over them, keeping their order.
void dropCollapsed(Flex& f) {
    uint32_t kept = 0;
    for (uint32_t l = 0; l < f.lineCount; ++l) {
        LayoutFlexLine& line = f.line(l);
        uint32_t first = kept;
        for (uint32_t i = line.first; i < line.first + line.count; ++i) {
            const LayoutFlexItem& it = f.item(i);
            if (it.collapsed) {
                line.strut = layoutUnitMax(line.strut, it.strut);
                continue;
            }
            if (kept != i)
                f.item(kept) = f.item(i);
            ++kept;
        }
        line.first = first;
        line.count = kept - first;
    }
    f.count = kept;
}

// Returns the main size that an unfrozen item takes from `remaining` free space, before its limits apply. `grows` says
// whether the line grows. The factors of the unfrozen items of the line sum to `factors`, and their scaled shrink
// factors sum to `scaled`.
double unclampedTarget(const LayoutFlexItem& it, double remaining, bool grows, double factors, double scaled) {
    if (remaining != 0 && grows && factors > 0)
        return it.base + remaining * it.factor / factors;
    if (remaining != 0 && !grows && scaled > 0)
        return it.base - prF64Abs(remaining) * it.factor * it.base / scaled;
    return it.base;
}

// Resolves the flexible lengths of the items of `line` in `main` units (css-flexbox-2/#resolve-flexible-lengths). The
// gaps between the items are taken from `main` first.
void resolveFlexibleLengths(Flex& f, const LayoutFlexLine& line, double main) {
    uint32_t first = line.first;
    uint32_t end = line.first + line.count;
    main -= gapsOf(f, line.count);
    double hypothetical = 0;
    for (uint32_t i = first; i < end; ++i) {
        const LayoutFlexItem& it = f.item(i);
        hypothetical += it.hypothetical + double(it.edgesMain.raw + it.marginsMain.raw);
    }
    bool grows = hypothetical < main;
    for (uint32_t i = first; i < end; ++i) {
        LayoutFlexItem& it = f.item(i);
        it.factor = double(grows ? it.flex->grow : it.flex->shrink);
        it.frozen = it.factor == 0 || (grows && it.base > it.hypothetical) || (!grows && it.base < it.hypothetical);
        it.target = it.frozen ? it.hypothetical : it.base;
    }
    auto freeSpace = [&] {
        double used = 0;
        for (uint32_t i = first; i < end; ++i) {
            const LayoutFlexItem& it = f.item(i);
            used += (it.frozen ? it.target : it.base) + double(it.edgesMain.raw + it.marginsMain.raw);
        }
        return main - used;
    };
    double initial = freeSpace();
    for (;;) {
        double factors = 0;
        double scaled = 0;
        bool any = false;
        for (uint32_t i = first; i < end; ++i) {
            const LayoutFlexItem& it = f.item(i);
            if (it.frozen)
                continue;
            any = true;
            factors += it.factor;
            scaled += it.factor * it.base;
        }
        if (!any)
            break;
        f.state.stats->flexItemsFlexed += line.count;
        double remaining = freeSpace();
        if (factors < 1 && prF64Abs(initial * factors) < prF64Abs(remaining))
            remaining = initial * factors;
        // Each unfrozen item takes its share of the free space and is clamped by its limits.
        double violation = 0;
        for (uint32_t i = first; i < end; ++i) {
            LayoutFlexItem& it = f.item(i);
            if (it.frozen)
                continue;
            double target = unclampedTarget(it, remaining, grows, factors, scaled);
            it.target = prF64Max(0, prF64Max(prF64Min(target, it.maxMain), it.minMain));
            violation += it.target - target;
        }
        // If there is no violation, every item freezes. A positive violation freezes the items that the clamp raised,
        // and a negative one freezes those it lowered.
        any = false;
        for (uint32_t i = first; i < end; ++i) {
            LayoutFlexItem& it = f.item(i);
            if (it.frozen)
                continue;
            double unclamped = unclampedTarget(it, remaining, grows, factors, scaled);
            if (violation == 0 || (violation > 0 && it.target > unclamped) || (violation < 0 && it.target < unclamped))
                it.frozen = true;
            any = any || !it.frozen;
        }
        if (!any)
            break;
    }
    // Each size is rounded at the point where the sizes before it end, so that the sizes of the line sum to their total
    // within one unit.
    double sum = 0;
    PrLayoutUnit placed = kLayoutUnitZero;
    for (uint32_t i = first; i < end; ++i) {
        LayoutFlexItem& it = f.item(i);
        sum += it.target;
        PrLayoutUnit rounded = unitOf(sum);
        it.main = layoutUnitMax(rounded - placed + it.edgesMain, it.edgesMain);
        placed = rounded;
    }
}

// Returns what the container asks of the final layout of an item, in the item's own axes. The main size and the used
// cross size are fixed, except that a main-inline item that is not stretched decides its own block size.
LayoutRequests finalRequests(const LayoutFlexItem& it) {
    LayoutRequest fixedCross{.size = it.cross, .kind = LayoutRequestKind_Fixed, .definite = true};
    LayoutRequest fixedMain{.size = it.main, .kind = LayoutRequestKind_Fixed, .definite = it.definiteMain};
    if (!it.mainInline)
        return LayoutRequests{.inlineAxis = fixedCross, .blockAxis = fixedMain};
    return LayoutRequests{.inlineAxis = fixedMain, .blockAxis = it.stretched ? fixedCross : LayoutRequest{}};
}

// Returns the baseline of an item at its used size, from the block-start edge of its border box. If its hypothetical
// cross size is its used cross size, the baseline comes from its measurement at that size. Otherwise it comes from a
// measurement at its final request.
LayoutBaseline usedBaseline(Flex& f, const LayoutFlexItem& it, LayoutBaselineEdge edge) {
    if (it.mainInline && !it.stretched)
        return edge == LayoutBaselineEdge_First ? it.first : it.last;
    LayoutRequests requests = finalRequests(it);
    LayoutMeasurement m = layoutMeasure(f.state, it.node, f.content, requests, LayoutMeasureKind_Baselines);
    return edge == LayoutBaselineEdge_First ? m.first : m.last;
}

// Distributes the free space `free` among `count` subjects, which are the items of a line or the lines of a container,
// by `align`. Returns the offset of the first subject from the start of the axis, which is its flex-start if `reverse`
// is false. Sets `*between` to the space added between each two subjects beyond the gap
// (css-align-3/#positional-values, css-align-3/#distribution-values).
//
// `start` and `end` are the axis's own start and end. A distributed value falls back to safe flex-start or safe center,
// and safe alignment of a subject that overflows is flex-start (css-align-3/#overflow-values). Any other value is
// flex-start.
double distribute(PrLayoutAlignment align, bool reverse, double free, uint32_t count, double* between) {
    *between = 0;
    PrLayoutAlignment value = PrLayoutAlignment(align & kPrLayoutAlignmentValueMask);
    bool safe = (align & PrLayoutAlignmentFlag_Safe) != 0;
    if (value == PrLayoutAlignment_Start)
        value = reverse ? PrLayoutAlignment_FlexEnd : PrLayoutAlignment_FlexStart;
    else if (value == PrLayoutAlignment_End)
        value = reverse ? PrLayoutAlignment_FlexStart : PrLayoutAlignment_FlexEnd;
    switch (value) {
        case PrLayoutAlignment_SpaceBetween:
            if (free < 0 || count < 2)
                return 0;
            *between = free / (count - 1);
            return 0;
        case PrLayoutAlignment_SpaceAround:
        case PrLayoutAlignment_SpaceEvenly:
            if (free < 0)
                return 0;
            *between = free / (value == PrLayoutAlignment_SpaceAround ? count : count + 1);
            return value == PrLayoutAlignment_SpaceAround ? *between / 2 : *between;
        default: break;
    }
    if (safe && free < 0)
        return 0;
    switch (value) {
        case PrLayoutAlignment_FlexEnd: return free;
        case PrLayoutAlignment_Center: return free / 2;
        default: return 0;
    }
}

// Distributes the free space `free` of a line of `count` items by justify-content
// (css-flexbox-2/#justify-content-property). Returns the offset of the first item from the main-start edge, and sets
// `*between` as distribute does. `left` and `right` are the line-left and line-right sides of the container if its main
// axis is its inline axis. They are the physical left and right if its main axis is horizontal, as in a vertical
// column, and are otherwise treated as start (css-align-3/#positional-values).
double justify(const Flex& f, double free, uint32_t count, double* between) {
    PrLayoutAlignment align = f.flex.justifyContent;
    PrLayoutAlignment value = PrLayoutAlignment(align & kPrLayoutAlignmentValueMask);
    if (value == PrLayoutAlignment_Left || value == PrLayoutAlignment_Right) {
        bool left = value == PrLayoutAlignment_Left;
        if (f.row) {
            LayoutSide side = left ? layoutLineLeft(f.content.mode) : layoutLineRight(f.content.mode);
            value = side == LayoutSide_InlineStart ? PrLayoutAlignment_Start : PrLayoutAlignment_End;
        } else if (!layoutIsHorizontal(f.content.mode)) {
            bool startIsLeft = layoutPhysicalSide(f.content.mode, LayoutSide_BlockStart) == PrRectSide_Left;
            value = left == startIsLeft ? PrLayoutAlignment_Start : PrLayoutAlignment_End;
        } else {
            value = PrLayoutAlignment_Start;
        }
        align = PrLayoutAlignment((align & ~kPrLayoutAlignmentValueMask) | value);
    }
    return distribute(align, f.reverse, free, count, between);
}

// Returns the distance in a row from the block-start edge of the margin box of an item to its baseline of `edge`. If
// the item has no such baseline, the block-end edge of its border box stands in for it
// (css-flexbox-2/#baseline-participation).
PrLayoutUnit baselineAbove(const LayoutFlexItem& it, LayoutBaselineEdge edge) {
    const LayoutBaseline& b = edge == LayoutBaselineEdge_First ? it.first : it.last;
    return it.e.margin[LayoutSide_BlockStart] + (b.present ? b.offset : it.cross);
}

// Finds the items whose baselines are the first and last baselines of the container (css-flexbox-2/#flex-baselines).
// They come from the startmost and endmost lines along the block axis, and wrap-reverse puts the first line last.
//
// If the main axis is the inline axis, an item of the line that is aligned by baseline gives the baseline. For the
// first set, this is the first item aligned by first baseline, or else by last baseline. For the last set, it is the
// last item aligned by last baseline, or else by first baseline. If no item is aligned by baseline, the
// inline-startmost or inline-endmost item of the line gives it. If the main axis is the block axis, the block-startmost
// or block-endmost item of the line, by flex-direction, gives it.
void findBaselineItems(Flex& f) {
    f.firstBaseline = f.count;
    f.lastBaseline = f.count;
    if (f.count == 0)
        return;
    // Lines are stacked along the block axis in a row, and along the inline axis in a column, whose first line is the
    // inline-startmost. In a row, the startmost line along the block axis is the first line unless wrap-reverse makes
    // it the last.
    bool linesReversed = f.row && f.crossReverse;
    const LayoutFlexLine& start = f.line(linesReversed ? f.lineCount - 1 : 0);
    const LayoutFlexLine& end = f.line(linesReversed ? 0 : f.lineCount - 1);
    auto aligned = [&](const LayoutFlexLine& line, LayoutFlexPlace place, bool last) {
        for (uint32_t k = 0; k < line.count; ++k) {
            uint32_t i = line.first + (last ? line.count - 1 - k : k);
            if (f.item(i).place == place)
                return i;
        }
        return f.count;
    };
    if (f.row) {
        f.firstBaseline = aligned(start, LayoutFlexPlace_Baseline, false);
        if (f.firstBaseline == f.count)
            f.firstBaseline = aligned(start, LayoutFlexPlace_LastBaseline, false);
        f.lastBaseline = aligned(end, LayoutFlexPlace_LastBaseline, true);
        if (f.lastBaseline == f.count)
            f.lastBaseline = aligned(end, LayoutFlexPlace_Baseline, true);
    }
    // Otherwise the startmost item of the line along the main axis gives it, and flex-direction may reverse the axis. A
    // line may hold only collapsed items, and so have no item.
    if (f.firstBaseline == f.count && start.count != 0)
        f.firstBaseline = f.reverse ? start.first + start.count - 1 : start.first;
    if (f.lastBaseline == f.count && end.count != 0)
        f.lastBaseline = f.reverse ? end.first : end.first + end.count - 1;
}

// Finds the hypothetical cross sizes of the items of line `l`, the baseline groups they share, and the cross size of
// the line (css-flexbox-2/#algo-cross-item, css-flexbox-2/#algo-cross-line). The line takes the cross size of the
// container if the container is single-line and its cross size is definite. Otherwise it takes the size its items need,
// clamped if the container is single-line.
//
// An item of a row whose style fixes its cross size is measured only if its baselines are read: if it aligns by them,
// or if a solve for the baselines of the container takes them from it. A stretched item in a line of the container's
// cross size takes the cross size of the line, and its baselines are found at that size if they are read
// (css-flexbox-2/#definite-sizes, its third case).
void sizeLine(Flex& f, uint32_t l, LayoutContentGoal goal) {
    LayoutFlexLine line = f.line(l);
    uint32_t end = line.first + line.count;
    PrLayoutUnit definite = f.singleLine ? f.definiteCross() : layoutUnit(-1);
    bool exports = goal == LayoutContentGoal_Baselines;
    PrLayoutUnit largest = kLayoutUnitZero;
    for (uint32_t i = line.first; i < end; ++i) {
        // A measurement may grow the scratch items, so the item is looked up again after it.
        const LayoutFlexItem& before = f.item(i);
        bool aligns = before.place == LayoutFlexPlace_Baseline || before.place == LayoutFlexPlace_LastBaseline;
        bool exported = i == f.firstBaseline || i == f.lastBaseline;
        bool baselines = aligns || (exports && !before.stretched && exported);
        PrLayoutUnit cross = kLayoutUnitZero;
        if (!before.mainInline) {
            cross = hypotheticalCross(f, before);
        } else if (layoutIsDefinite(before.styledCross) && !baselines) {
            cross = before.styledCross;
        } else if (!before.stretched || !layoutIsDefinite(definite)) {
            LayoutMeasurement m = measureItem(f, before.node, before.main, LayoutRequest{},
                baselines ? LayoutMeasureKind_Baselines : LayoutMeasureKind_Size);
            cross = m.blockSize;
            f.item(i).first = m.first;
            f.item(i).last = m.last;
        }
        LayoutFlexItem& it = f.item(i);
        it.cross = cross;
        PrLayoutUnit outer = it.cross + it.marginsCross;
        if (it.place == LayoutFlexPlace_Baseline) {
            PrLayoutUnit above = baselineAbove(it, LayoutBaselineEdge_First);
            line.ascent = layoutUnitMax(line.ascent, above);
            line.descent = layoutUnitMax(line.descent, outer - above);
        } else if (it.place == LayoutFlexPlace_LastBaseline) {
            PrLayoutUnit above = baselineAbove(it, LayoutBaselineEdge_Last);
            line.lastAscent = layoutUnitMax(line.lastAscent, above);
            line.lastDescent = layoutUnitMax(line.lastDescent, outer - above);
        } else {
            largest = layoutUnitMax(largest, outer);
        }
    }
    if (layoutIsDefinite(definite)) {
        line.cross = definite;
    } else {
        line.cross = layoutUnitMax(layoutUnitMax(largest, line.ascent + line.descent),
            line.lastAscent + line.lastDescent);
        if (f.singleLine && layoutIsDefinite(f.maxBlock))
            line.cross = layoutUnitMin(line.cross, f.maxBlock);
        if (f.singleLine)
            line.cross = layoutUnitMax(line.cross, f.minBlock);
    }
    line.cross = layoutUnitMax(line.cross, line.strut);
    f.line(l) = line;
}

// Returns the cross size of the content box (css-flexbox-2/#algo-cross-container), which is its own size if definite,
// and otherwise the size of its lines, clamped. Sets `*lines` to the part taken by the cross sizes of the lines and the
// gaps between them. The stretch value of align-content grows the lines of a multi-line container to fill its cross
// size (css-flexbox-2/#algo-line-stretch). This applies to a definite cross size, as css-flexbox-2 says, and also to a
// cross size that the limits give if the lines leave it short.
PrLayoutUnit sizeContainerCross(Flex& f, PrLayoutUnit* lines) {
    *lines = layoutUnit(f.crossGap.raw * int32_t(f.lineCount - 1));
    for (uint32_t l = 0; l < f.lineCount; ++l)
        *lines = *lines + f.line(l).cross;
    PrLayoutUnit cross = f.definiteCross();
    if (!layoutIsDefinite(cross)) {
        cross = *lines;
        if (layoutIsDefinite(f.maxBlock))
            cross = layoutUnitMin(cross, f.maxBlock);
        cross = layoutUnitMax(cross, f.minBlock);
    }
    PrLayoutAlignment content = PrLayoutAlignment(f.flex.alignContent & kPrLayoutAlignmentValueMask);
    bool stretchLines = content == PrLayoutAlignment_Normal || content == PrLayoutAlignment_Stretch;
    if (!f.singleLine && stretchLines && *lines < cross) {
        PrLayoutUnit placed = kLayoutUnitZero;
        double free = double((cross - *lines).raw);
        for (uint32_t l = 0; l < f.lineCount; ++l) {
            PrLayoutUnit end = unitOf(free * double(l + 1) / double(f.lineCount));
            f.line(l).cross = f.line(l).cross + end - placed;
            placed = end;
        }
        *lines = cross;
    }
    return cross;
}

// Stretches the stretched items of `line` to its cross size, clamped by their limits (css-flexbox-2/#algo-stretch).
void stretchItems(Flex& f, const LayoutFlexLine& line) {
    PrLayoutUnit base = f.row ? f.content.percentBlock : f.inlineSize;
    for (uint32_t i = line.first; i < line.first + line.count; ++i) {
        LayoutFlexItem& it = f.item(i);
        if (!it.stretched)
            continue;
        PrLayoutUnit stretch = layoutUnitMax(line.cross - it.marginsCross, it.edgesCross);
        it.cross = layoutClampSize(f.state.styles, *it.box, stretch, it.minCrossSize, it.maxCrossSize, base,
            it.edgesCross, stretch, layoutNoKeyword);
    }
}

// Places the items of `line` along the main axis, in `main` units (css-flexbox-2/#main-alignment). Auto margins take
// the free space first, then justify-content places the items in what is left, with the gaps between them.
void placeMain(Flex& f, const LayoutFlexLine& line, double main) {
    LayoutSide mainStart = f.row ? LayoutSide_InlineStart : LayoutSide_BlockStart;
    LayoutSide mainEnd = f.row ? LayoutSide_InlineEnd : LayoutSide_BlockEnd;
    uint32_t end = line.first + line.count;
    double used = gapsOf(f, line.count);
    uint32_t autoMargins = 0;
    for (uint32_t i = line.first; i < end; ++i) {
        const LayoutFlexItem& it = f.item(i);
        used += double(it.main.raw + it.marginsMain.raw);
        autoMargins += uint32_t(it.e.marginAuto[mainStart]) + uint32_t(it.e.marginAuto[mainEnd]);
    }
    double free = main - used;
    double share = autoMargins != 0 && free > 0 ? free / autoMargins : 0;
    double between = 0;
    double cursor = autoMargins != 0 && free > 0 ? 0 : justify(f, free, line.count, &between);
    between += double(f.mainGap.raw);
    for (uint32_t i = line.first; i < end; ++i) {
        LayoutFlexItem& it = f.item(i);
        // The margin before an item along the line is on its main-start side, which is its logical end side if the
        // direction reverses.
        LayoutSide first = f.reverse ? mainEnd : mainStart;
        LayoutSide last = f.reverse ? mainStart : mainEnd;
        double before = double(it.e.margin[first].raw) + (it.e.marginAuto[first] ? share : 0);
        double after = double(it.e.margin[last].raw) + (it.e.marginAuto[last] ? share : 0);
        double start = cursor + before;
        it.mainOffset = f.reverse ? unitOf(main - start) - it.main : unitOf(start);
        cursor = start + double(it.main.raw) + after + between;
    }
}

// Places the items of `line` within its cross size, each by its auto margins or its alignment, measured from the
// cross-start edge of the line (css-flexbox-2/#cross-alignment). Auto margins take only positive space, so an item that
// overflows its line is flush with cross-start.
void placeCross(Flex& f, const LayoutFlexLine& line) {
    LayoutSide crossStart = f.row ? LayoutSide_BlockStart : LayoutSide_InlineStart;
    LayoutSide crossEnd = f.row ? LayoutSide_BlockEnd : LayoutSide_InlineEnd;
    LayoutSide startSide = f.crossReverse ? crossEnd : crossStart;
    LayoutSide endSide = f.crossReverse ? crossStart : crossEnd;
    for (uint32_t i = line.first; i < line.first + line.count; ++i) {
        LayoutFlexItem& it = f.item(i);
        PrLayoutUnit space = line.cross - it.cross - it.marginsCross;
        PrLayoutUnit offset = kLayoutUnitZero;
        uint32_t autos = uint32_t(it.e.marginAuto[startSide]) + uint32_t(it.e.marginAuto[endSide]);
        if (autos != 0) {
            if (space > kLayoutUnitZero && it.e.marginAuto[startSide])
                offset = autos == 2 ? layoutUnit(space.raw / 2) : space;
        } else if (!it.safe || space >= kLayoutUnitZero) {
            // A first-baseline group is flush with the cross-start edge of the line, and a last-baseline group with its
            // cross-end edge (css-flexbox-2/#align-items-property). The extents of the line are measured from the
            // block-start of the items, which wrap-reverse makes the cross-end edge. In that case the offset of an item
            // from cross-start is what lies below its baseline, measured from the baseline of the group.
            PrLayoutUnit outer = it.cross + it.marginsCross;
            switch (it.place) {
                case LayoutFlexPlace_Center: offset = layoutUnit(space.raw / 2); break;
                case LayoutFlexPlace_End: offset = space; break;
                case LayoutFlexPlace_Baseline: {
                    PrLayoutUnit above = baselineAbove(it, LayoutBaselineEdge_First);
                    offset = f.crossReverse ? line.descent - (outer - above) : line.ascent - above;
                    break;
                }
                case LayoutFlexPlace_LastBaseline: {
                    PrLayoutUnit above = baselineAbove(it, LayoutBaselineEdge_Last);
                    if (f.crossReverse)
                        offset = line.cross - (line.lastAscent - above) - outer;
                    else
                        offset = line.cross - line.lastDescent - above;
                    break;
                }
                default: break;
            }
        }
        it.crossOffset = offset + it.e.margin[startSide];
    }
}

// Places the lines within `cross`, the cross size of the content box, by align-content
// (css-flexbox-2/#align-content-property). Their cross sizes and the gaps between them take `lines` of it. A
// single-line container does not read align-content. Then converts the offset of each item from the cross-start edge of
// its line to the offset of its border box from the start of the cross axis, which is the cross-end edge if the
// container is wrap-reverse. Stretch, which already grew the lines where it could, and the baseline values act as
// flex-start.
void placeLines(Flex& f, PrLayoutUnit cross, PrLayoutUnit lines) {
    double between = 0;
    double cursor = 0;
    if (!f.singleLine) {
        PrLayoutAlignment align = f.flex.alignContent;
        PrLayoutAlignment value = PrLayoutAlignment(align & kPrLayoutAlignmentValueMask);
        if (value == PrLayoutAlignment_Left || value == PrLayoutAlignment_Right)
            align = PrLayoutAlignment_Start;
        cursor = distribute(align, f.crossReverse, double((cross - lines).raw), f.lineCount, &between);
    }
    between += double(f.crossGap.raw);
    for (uint32_t l = 0; l < f.lineCount; ++l) {
        LayoutFlexLine& line = f.line(l);
        line.offset = unitOf(cursor);
        cursor += double(line.cross.raw) + between;
        PrLayoutUnit start = f.crossReverse ? cross - line.offset - line.cross : line.offset;
        for (uint32_t i = line.first; i < line.first + line.count; ++i) {
            LayoutFlexItem& it = f.item(i);
            it.crossOffset = start + (f.crossReverse ? line.cross - it.crossOffset - it.cross : it.crossOffset);
        }
    }
}

// Returns how a static position aligns a box out of flow in the main axis of the container, from main-start. The box is
// treated as the sole item of a line that justify-content places (css-flexbox-2/#abspos-items). space-between acts as
// flex-start, and space-around and space-evenly act as center.
LayoutStaticAlign staticMain(const Flex& f) {
    double between = 0;
    double free = 2;
    double at = justify(f, free, 1, &between);
    return at == 0 ? LayoutStaticAlign_Start : at == free ? LayoutStaticAlign_End : LayoutStaticAlign_Center;
}

// Appends a placeholder for each child out of flow of the container, at its static position
// (css-flexbox-2/#abspos-items, css-position-3/#staticpos-rect). The child is aligned in the content box, which is
// `main` units long and `cross` across from `origin`, as the sole item of the container would be. It is aligned by
// justify-content and its own align-self, with stretch and baseline alignment acting as flex-start.
void placeOutOfFlow(Flex& f, LayoutLogicalOffset origin, double main, PrLayoutUnit cross) {
    LayoutTree* tree = f.state.tree;
    LayoutSide crossStart = f.row ? LayoutSide_BlockStart : LayoutSide_InlineStart;
    LayoutStaticAlign mainAlign = staticMain(f);
    // Main-start is the end of the axis if the items run from its end.
    if (f.reverse && mainAlign != LayoutStaticAlign_Center)
        mainAlign = mainAlign == LayoutStaticAlign_Start ? LayoutStaticAlign_End : LayoutStaticAlign_Start;
    PrLayoutUnit mainSize = unitOf(main);
    layoutEachItem(tree, f.node, [&](uint32_t c, const PrLayoutBoxStyle&) {
        if (!layoutIsOutOfFlow(tree, c))
            return;
            // Baseline alignment acts as start, and last-baseline alignment as end.
        bool safe = false;
        PrLayoutAlignment align = layoutFlexStyle(f.state.styles, layoutTreeNode(tree, c).style).alignSelf;
        if ((align & kPrLayoutAlignmentValueMask) == PrLayoutAlignment_Auto)
            align = f.flex.alignItems;
        PrLayoutAlignment value = PrLayoutAlignment(align & kPrLayoutAlignmentValueMask);
        if (value == PrLayoutAlignment_Baseline)
            align = PrLayoutAlignment_Start;
        else if (value == PrLayoutAlignment_LastBaseline)
            align = PrLayoutAlignment_End;
        LayoutFlexPlace place = crossPlaceOf(align, align, false, f.crossReverse, f.content.mode, crossStart, tree, c, &safe);
        LayoutStaticAlign crossAlign = LayoutStaticAlign_Start;
        if (place == LayoutFlexPlace_Center)
            crossAlign = LayoutStaticAlign_Center;
        else if (place == LayoutFlexPlace_End || place == LayoutFlexPlace_LastBaseline)
            crossAlign = LayoutStaticAlign_End;
            // Cross-start is the axis's end in a wrap-reverse container.
        if (f.crossReverse && crossAlign != LayoutStaticAlign_Center)
            crossAlign = crossAlign == LayoutStaticAlign_Start ? LayoutStaticAlign_End : LayoutStaticAlign_Start;
        auto at = [](LayoutStaticAlign align, PrLayoutUnit size) {
            if (align == LayoutStaticAlign_Start)
                return kLayoutUnitZero;
            if (align == LayoutStaticAlign_End)
                return size;
            return layoutUnit(size.raw / 2);
        };
        LayoutStaticAlign inlineAlign = f.row ? mainAlign : crossAlign;
        LayoutStaticAlign blockAlign = f.row ? crossAlign : mainAlign;
        LayoutLogicalOffset offset{at(inlineAlign, f.row ? mainSize : cross), at(blockAlign, f.row ? cross : mainSize)};
        bool mainSafe = (f.flex.justifyContent & PrLayoutAlignmentFlag_Safe) != 0;
        LayoutStaticEdges edges = layoutStaticEdgesAligned(f.content.mode, inlineAlign, blockAlign,
            f.row ? mainSafe : safe, f.row ? safe : mainSafe);
        LayoutFragment* placeholder = layoutFragmentCreatePlaceholder(c, edges, layoutIsFixed(tree, c));
        f.state.scratch->children.push(LayoutLogicalChild{placeholder, origin + offset});
    }, [](uint32_t, uint32_t) {});
}

// Solves the flex container `node` as layoutFlexContent does, and sets `*linesCross` to the cross sizes of its lines
// and the gaps between them.
LayoutContentOutcome solve(LayoutState& state, LayoutNodeIndex node, const LayoutContentInput& input,
        PrLayoutUnit* linesCross) {
    LayoutContentGoal goal = input.goal;
    LayoutLogicalOffset origin = input.origin;
    bool builds = goal == LayoutContentGoal_Build;
    LayoutTree* tree = state.tree;
    if (state.scratch->flex == nullptr)
        state.scratch->flex = layoutFlexScratchCreate();
    Array<LayoutFlexItem>& items = state.scratch->flex->items;
    const PrLayoutFlexStyle& flex = layoutFlexStyle(state.styles, layoutTreeNode(tree, node).style);
    PrLayoutFlexDirection direction = flex.direction;
    bool row = isRow(direction);
    PrLayoutUnit inlineSize = input.content->containing.inlineSize;
    // A gap resolves against the size of the content box in its axis, which may be indefinite in the block axis.
    PrLayoutUnit columnGap = layoutGapResolve(state.styles, flex.columnGap, inlineSize);
    PrLayoutUnit rowGap = layoutGapResolve(state.styles, flex.rowGap, input.content->percentBlock);
    Flex f{
        .state = state,
        .node = node,
        .flex = flex,
        .content = *input.content,
        .row = row,
        .reverse = direction == PrLayoutFlexDirection_RowReverse || direction == PrLayoutFlexDirection_ColumnReverse,
        .crossReverse = flex.wrap == PrLayoutFlexWrap_WrapReverse,
        .singleLine = flex.wrap == PrLayoutFlexWrap_NoWrap,
        .inlineSize = inlineSize,
        .blockSize = input.blockSize,
        .minBlock = input.minBlock,
        .maxBlock = input.maxBlock,
        .mainGap = row ? columnGap : rowGap,
        .crossGap = row ? rowGap : columnGap,
        .mark = items.count,
        .count = 0,
        .collapsed = 0,
        .lineMark = 0,
        .lineCount = 0,
        .firstBaseline = 0,
        .lastBaseline = 0,
    };
    LayoutSide crossStart = row ? LayoutSide_BlockStart : LayoutSide_InlineStart;
    LayoutSide crossEnd = row ? LayoutSide_BlockEnd : LayoutSide_InlineEnd;

    // Collects the items in order-modified document order (css-display-4/#order-modified-document-order), and counts
    // the children that are out of flow. Their static positions are found once the content box is sized.
    Array<LayoutLogicalChild>& children = state.scratch->children;
    uint32_t outOfFlow = 0;
    auto addItem = [&](LayoutNodeIndex c, const PrLayoutBoxStyle& child, const PrLayoutFlexStyle& childFlex) {
        LayoutFlexItem it{};
        it.node = c;
        it.box = &child;
        it.flex = &childFlex;
        it.orthogonal = layoutIsOrthogonalRoot(tree, c, f.content.mode);
        it.mainInline = row != it.orthogonal;
        it.collapsed = layoutKindIsElement(layoutTreeNode(tree, c).kind) && layoutIsCollapsedItem(tree, c, child);
        f.collapsed += uint32_t(it.collapsed);
        it.order = childFlex.order;
        LayoutLogicalBox logical = layoutLogicalBox(child, f.content.mode);
        it.mainSize = row ? logical.inlineSize : logical.blockSize;
        it.minMainSize = row ? logical.minInlineSize : logical.minBlockSize;
        it.maxMainSize = row ? logical.maxInlineSize : logical.maxBlockSize;
        it.crossSize = row ? logical.blockSize : logical.inlineSize;
        it.minCrossSize = row ? logical.minBlockSize : logical.minInlineSize;
        it.maxCrossSize = row ? logical.maxBlockSize : logical.maxInlineSize;
        it.e = layoutEdgesFromBox(state.styles, logical, f.inlineSize);
        PrLayoutUnit edgesInline = layoutEdgesInline(it.e);
        PrLayoutUnit edgesBlock = layoutEdgesBlock(it.e);
        PrLayoutUnit marginsInline = it.e.margin[LayoutSide_InlineStart] + it.e.margin[LayoutSide_InlineEnd];
        PrLayoutUnit marginsBlock = it.e.margin[LayoutSide_BlockStart] + it.e.margin[LayoutSide_BlockEnd];
        it.edgesMain = row ? edgesInline : edgesBlock;
        it.edgesCross = row ? edgesBlock : edgesInline;
        it.marginsMain = row ? marginsInline : marginsBlock;
        it.marginsCross = row ? marginsBlock : marginsInline;
        // An auto margin in the cross axis places the item, and its align-self has no effect. The item neither
        // stretches nor takes part in baseline alignment (css-flexbox-2/#align-items-property).
        bool autoCross = it.e.marginAuto[crossStart] || it.e.marginAuto[crossEnd];
        bool stretches = layoutLengthIsAuto(it.crossSize) && !autoCross;
        if (autoCross)
            it.place = LayoutFlexPlace_Start;
        else
            it.place = crossPlaceOf(childFlex.alignSelf, flex.alignItems, stretches, f.crossReverse, f.content.mode,
                crossStart, tree, c, &it.safe);
        it.stretched = it.place == LayoutFlexPlace_Stretch;
        it.styledCross = layoutUnit(-1);
        auto limitFixed = [](PrLayoutLength limit) {
            return !layoutLengthIsContentBased(limit) && !layoutLengthIsStretch(limit);
        };
        if (row && !it.orthogonal && !it.stretched && layoutResolves(logical.blockSize, f.content.percentBlock) &&
                limitFixed(logical.minBlockSize) && limitFixed(logical.maxBlockSize))
            it.styledCross = layoutBlockBase(state.styles, child, logical, it.e, f.content.percentBlock).size;
        items.push(it);
        ++f.count;
    };
    layoutEachItem(tree, node, [&](uint32_t c, const PrLayoutBoxStyle& child) {
        if (!layoutIsOutOfFlow(tree, c)) {
            addItem(c, child, layoutFlexStyle(state.styles, layoutTreeNode(tree, c).style));
        } else if (builds) {
            ++outOfFlow;
        }
    },
        // A run of text is an anonymous item (css-flexbox-2/#flex-items).
        [&](uint32_t first, uint32_t) { addItem(first, layoutAnonymousBoxStyle(), layoutAnonymousFlexStyle()); });
    // A stable sort by order, which leaves the items of most containers unchanged.
    for (uint32_t i = 1; i < f.count; ++i) {
        for (uint32_t k = i; k > 0 && f.item(k - 1).order > f.item(k).order; --k) {
            LayoutFlexItem moved = f.item(k);
            f.item(k) = f.item(k - 1);
            f.item(k - 1) = moved;
        }
    }

    // Finds the main sizes (css-flexbox-2/#main-sizing), the lines, and the flexed sizes on each line
    // (css-flexbox-2/#resolve-flexible-lengths). An item is copied out of the scratch items while its sizing lays out
    // boxes, because the items and lines of a nested flex container grow the scratch arrays.
    for (uint32_t i = 0; i < f.count; ++i) {
        LayoutFlexItem it = f.item(i);
        sizeMain(f, it);
        f.item(i) = it;
    }
    // If any items are collapsed, a first round sizes and stretches the lines with every item visible. The strut of
    // each collapsed item is the cross size of its line. The round that counts then collects the collapsed items at
    // zero size and drops them (css-flexbox-2/#algo-visibility).
    if (f.collapsed != 0) {
        double visibleMain = containerMain(f, true);
        collectLines(f, visibleMain, true);
        for (uint32_t l = 0; l < f.lineCount; ++l)
            resolveFlexibleLengths(f, f.line(l), visibleMain);
        findBaselineItems(f);
        for (uint32_t l = 0; l < f.lineCount; ++l)
            sizeLine(f, l, LayoutContentGoal_Size);
        PrLayoutUnit visibleLines = kLayoutUnitZero;
        sizeContainerCross(f, &visibleLines);
        for (uint32_t l = 0; l < f.lineCount; ++l) {
            const LayoutFlexLine& line = f.line(l);
            for (uint32_t i = line.first; i < line.first + line.count; ++i)
                f.item(i).strut = line.cross;
        }
        state.scratch->flex->lines.count = f.lineMark;
        f.lineCount = 0;
    }
    double main = containerMain(f, false);
    collectLines(f, main, false);
    if (f.collapsed != 0)
        dropCollapsed(f);
    for (uint32_t l = 0; l < f.lineCount; ++l)
        resolveFlexibleLengths(f, f.line(l), main);

    // Finds the cross sizes (css-flexbox-2/#cross-sizing). Main-inline items are measured at their main sizes, and
    // main-block items take theirs from before flexing or from their ratios. Then the lines are sized, and the
    // stretched items are stretched.
    findBaselineItems(f);
    for (uint32_t l = 0; l < f.lineCount; ++l)
        sizeLine(f, l, goal);
    PrLayoutUnit lines = kLayoutUnitZero;
    PrLayoutUnit cross = sizeContainerCross(f, &lines);
    for (uint32_t l = 0; l < f.lineCount; ++l)
        stretchItems(f, f.line(l));

    // Places the items of each line along the main axis and in the cross axis of the line, then places the lines in the
    // content box (css-flexbox-2/#main-alignment, css-flexbox-2/#cross-alignment).
    for (uint32_t l = 0; l < f.lineCount; ++l) {
        placeMain(f, f.line(l), main);
        placeCross(f, f.line(l));
    }
    placeLines(f, cross, lines);

    LayoutContentOutcome outcome{};
    outcome.contentBlock = row ? cross : unitOf(main);
    *linesCross = lines;

    // Lays out the items at their final requests, and finds the baselines of the container (findBaselineItems). The
    // baselines come from the fragments of the items if the solve builds, and from their measurements if the solve is
    // for baselines. An item with no baseline in the block axis of the container has one synthesized from the block-end
    // edge of its border box.
    auto exported = [&](const LayoutFlexItem& it, LayoutBaseline baseline) {
        PrLayoutUnit blockOffset = row ? it.crossOffset : it.mainOffset;
        if (!baseline.present || it.orthogonal)
            baseline = layoutBaselineAt(row ? it.cross : it.main, kLayoutBaselineSynthesized);
        return layoutBaselineMoved(baseline, blockOffset);
    };
    uint32_t built = 0;
    for (uint32_t i = 0; i < f.count; ++i) {
        LayoutFlexItem it = f.item(i);
        if (builds) {
            LayoutResult result = layoutFinal(state, it.node, f.content, finalRequests(it));
            LayoutLogicalOffset relative = layoutRelativeOffset(state.styles, *it.box, f.content);
            LayoutLogicalOffset at;
            if (row)
                at = LayoutLogicalOffset{it.mainOffset, it.crossOffset};
            else
                at = LayoutLogicalOffset{it.crossOffset, it.mainOffset};
            children.push(LayoutLogicalChild{result.fragment, origin + at + relative});
            if (input.scrolls) {
                PrLayoutUnit itemInline = row ? it.main : it.cross;
                PrLayoutUnit itemBlock = row ? it.cross : it.main;
                LayoutLogicalOffset start{
                    at.inlineOffset - it.e.margin[LayoutSide_InlineStart],
                    at.blockOffset - it.e.margin[LayoutSide_BlockStart],
                };
                LayoutLogicalOffset end{
                    at.inlineOffset + itemInline + it.e.margin[LayoutSide_InlineEnd],
                    at.blockOffset + itemBlock + it.e.margin[LayoutSide_BlockEnd],
                };
                if (built++ != 0) {
                    start = {
                        layoutUnitMin(start.inlineOffset, outcome.itemsStart.inlineOffset),
                        layoutUnitMin(start.blockOffset, outcome.itemsStart.blockOffset),
                    };
                    end = {
                        layoutUnitMax(end.inlineOffset, outcome.itemsEnd.inlineOffset),
                        layoutUnitMax(end.blockOffset, outcome.itemsEnd.blockOffset),
                    };
                }
                outcome.itemsStart = start;
                outcome.itemsEnd = end;
            }
            if (i == f.firstBaseline)
                outcome.first = exported(it, layoutFragmentBaseline(result.fragment, LayoutBaselineEdge_First));
            if (i == f.lastBaseline)
                outcome.last = exported(it, layoutFragmentBaseline(result.fragment, LayoutBaselineEdge_Last));
        } else if (goal == LayoutContentGoal_Baselines) {
            if (i == f.firstBaseline)
                outcome.first = exported(it, usedBaseline(f, it, LayoutBaselineEdge_First));
            if (i == f.lastBaseline)
                outcome.last = exported(it, usedBaseline(f, it, LayoutBaselineEdge_Last));
        }
    }
    if (outOfFlow != 0)
        placeOutOfFlow(f, origin, main, cross);
    items.count = f.mark;
    state.scratch->flex->lines.count = f.lineMark;
    return outcome;
}

} // namespace

LayoutFlexScratch* layoutFlexScratchCreate(void) {
    return allocZeroed<LayoutFlexScratch>();
}

void layoutFlexScratchDestroy(LayoutFlexScratch* PR_NULLABLE scratch) {
    if (scratch == nullptr)
        return;
    scratch->items.reset();
    scratch->lines.reset();
    prMemoryFree(scratch);
}

LayoutContentOutcome layoutFlexContent(LayoutState& state, LayoutNodeIndex node, const LayoutContentInput& input) {
    PrLayoutUnit linesCross = kLayoutUnitZero;
    LayoutContentOutcome outcome = solve(state, node, input, &linesCross);
    ++(input.goal == LayoutContentGoal_Build ? state.stats->flexLaidOut : state.stats->flexSolved);
    return outcome;
}

LayoutIntrinsicSizes layoutFlexIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input) {
    LayoutTree* tree = state.tree;
    const PrLayoutFlexStyle& flex = layoutFlexStyle(state.styles, layoutTreeNode(tree, node).style);
    bool row = isRow(flex.direction);
    bool singleLine = flex.wrap == PrLayoutFlexWrap_NoWrap;
    LayoutIntrinsicSizes sizes{};
    uint32_t count = 0;
    // A row sums the contributions of its items, except that the min-content size of a multi-line row is the largest
    // contribution. Both sizes of a column are the largest contribution (css-flexbox-2/#intrinsic-main-sizes-compat,
    // css-flexbox-2/#intrinsic-main-sizes-multiline, css-flexbox-2/#intrinsic-cross-sizes).
    auto add = [&](PrLayoutUnit minContent, PrLayoutUnit maxContent, bool reads) {
        sizes.maxContent = row ? sizes.maxContent + maxContent : layoutUnitMax(sizes.maxContent, maxContent);
        sizes.minContent =
            row && singleLine ? sizes.minContent + minContent : layoutUnitMax(sizes.minContent, minContent);
        sizes.readsPercentBlock = sizes.readsPercentBlock || reads;
        ++count;
    };
    // An item of a row contributes its ordinary contribution (layoutContribution), capped by its hypothetical main size
    // if it cannot grow, and floored by it if it cannot shrink. A preferred size, if there is one, is the contribution,
    // and the automatic minimum reaches the contribution only through the hypothetical size.
    // css-flexbox-2/#intrinsic-item-contributions instead takes the larger of the content and preferred sizes and
    // clamps by the automatic minimum. Under a min-content or max-content constraint, a base that comes from the
    // content is sized under that constraint (css-flexbox-2/#flex-base-size).
    auto rowItem = [&](LayoutNodeIndex child, const PrLayoutBoxStyle& box, const PrLayoutFlexStyle& itemFlex) {
        LayoutIntrinsicSizes content{};
        bool flexible = itemFlex.grow != 0 && itemFlex.shrink != 0;
        LayoutIntrinsicSizes contribution = layoutContribution(state, child, input, flexible ? nullptr : &content);
        if (flexible) {
            add(contribution.minContent, contribution.maxContent, contribution.readsPercentBlock);
            return;
        }
        LayoutLogicalBox logical = layoutLogicalBox(box, input.mode);
        LayoutEdges e = layoutEdgesFromBox(state.styles, logical, layoutUnit(-1));
        PrLayoutUnit edges = layoutEdgesInline(e);
        PrLayoutUnit margins = e.margin[LayoutSide_InlineStart] + e.margin[LayoutSide_InlineEnd];
        // A base that comes from the content of the item, through an auto or content basis, is sized under the
        // constraint. It is the min-content size under a min-content constraint, and the max-content size under a
        // max-content constraint. A sizing keyword gives its own size.
        PrLayoutLength basis = layoutLengthIsAuto(itemFlex.basis) ? logical.inlineSize : itemFlex.basis;
        bool constrained = !layoutLengthIsContentBased(basis);
        PrLayoutUnit result[2] = {contribution.minContent, contribution.maxContent};
        for (uint32_t kind = 0; kind < 2; ++kind) {
            PrLayoutUnit ofKind = kind == 0 ? content.minContent : content.maxContent;
            auto ofContent = [&](PrLayoutLength length) -> double {
                bool min = length.bits == prLayoutLengthMinContent().bits;
                bool max = length.bits == prLayoutLengthMaxContent().bits;
                PrLayoutUnit size = min ? content.minContent : max && !constrained ? content.maxContent : ofKind;
                return double(size.raw - edges.raw);
            };
            MainSizes main = mainSizesOf(state.styles, box, itemFlex, logical.inlineSize, logical.minInlineSize,
                logical.maxInlineSize, layoutUnit(-1), edges, kNoRatio, false, ofContent);
            double hypothetical = prF64Max(0, prF64Max(prF64Min(main.base, main.max), main.min));
            PrLayoutUnit outer = layoutUnitMax(unitOf(hypothetical) + edges, edges) + margins;
            if (itemFlex.grow == 0)
                result[kind] = layoutUnitMin(result[kind], outer);
            if (itemFlex.shrink == 0)
                result[kind] = layoutUnitMax(result[kind], outer);
        }
        add(result[0], layoutUnitMax(result[0], result[1]),
            contribution.readsPercentBlock || content.readsPercentBlock);
    };
    layoutEachItem(tree, node, [&](uint32_t child, const PrLayoutBoxStyle& childBox) {
            // A collapsed item takes no part in a main size (css-flexbox-2/#intrinsic-main-sizes). Its strut keeps its
            // part in a cross size.
        if (layoutPositionIsOutOfFlow(childBox.position) || (row && layoutIsCollapsedItem(tree, child, childBox)))
            return;
        if (row && !layoutIsOrthogonalRoot(tree, child, input.mode)) {
            rowItem(child, childBox, layoutFlexStyle(state.styles, layoutTreeNode(tree, child).style));
            return;
        }
        LayoutIntrinsicSizes contribution = layoutContribution(state, child, input);
        add(contribution.minContent, contribution.maxContent, contribution.readsPercentBlock);
    },
        // The contribution of an anonymous item is its intrinsic sizes, because it has no sizes or edges of its own.
        [&](uint32_t first, uint32_t) {
            LayoutIntrinsicSizes anonymous = layoutIntrinsicSizes(state, first, input);
            add(anonymous.minContent, anonymous.maxContent, anonymous.readsPercentBlock);
        });
    // Adds the gaps of a single line, which resolve against zero because the inline size of the container is indefinite
    // (css-gaps-1/#gap-percent). A multi-line min-content size has no gaps.
    if (row && count > 1) {
        PrLayoutUnit gaps = layoutUnit(layoutGapResolve(state.styles, flex.columnGap,
            kLayoutUnitZero).raw * int32_t(count - 1));
        sizes.maxContent = sizes.maxContent + gaps;
        if (singleLine)
            sizes.minContent = sizes.minContent + gaps;
    }
    // The max-content size of a multi-line column is the sum of the cross sizes of its lines and the gaps between them
    // (css-flexbox-2/#intrinsic-cross-sizes). The lines are laid out with the largest max-content contribution as the
    // cross space of each item, if the block size or its limit breaks the column into lines. Its min-content size is
    // its largest contribution, as for a single line.
    if (!row && !singleLine && count > 1 &&
            (layoutIsDefinite(input.percentBlock) || layoutIsDefinite(input.blockLimit))) {
        LayoutSpace content{
            .mode = input.mode,
            .available = sizes.maxContent,
            .percentInline = sizes.maxContent,
            .percentBlock = input.percentBlock,
            .containing = LayoutLogicalSize{sizes.maxContent, input.percentBlock},
            .blockLimit = input.blockLimit,
            .fallback = input.fallback,
        };
        LayoutContentInput columns{
            .content = &content,
            .blockSize = input.percentBlock,
            .minBlock = kLayoutUnitZero,
            .maxBlock = input.blockLimit,
            .goal = LayoutContentGoal_Size,
        };
        solve(state, node, columns, &sizes.maxContent);
        ++state.stats->flexSolved;
        sizes.readsPercentBlock = true;
    }
    sizes.maxContent = layoutUnitMax(sizes.maxContent, sizes.minContent);
    return sizes;
}

} // namespace pr_impl
