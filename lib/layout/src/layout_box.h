// Boxes: what the style of a node makes of it. This covers the kind of box it generates, how it takes part in the
// layout of its parent, and what it does to painting.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-display-4 - https://drafts.csswg.org/css-display-4/
// css-overflow-3 - https://drafts.csswg.org/css-overflow-3/
// css-writing-modes-4 - https://drafts.csswg.org/css-writing-modes-4/
//
// Every algorithm asks these questions instead of reading display, float or overflow values, so that each answer is
// given in one place.

#pragma once

#include "pr/layout_query.h"
#include "pr/layout_style.h"

#include "layout/src/layout_exclusion.h"
#include "layout/src/layout_fragment.h"
#include "layout/src/layout_logical.h"
#include "layout/src/layout_style.h"
#include "layout/src/layout_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Writing modes

// Returns the used writing mode that the style of `node` gives it (layoutUsedMode). A text node has the writing mode of
// its parent. So does an anonymous flex item, whose style is kept by the first node of its run.
inline LayoutMode layoutOwnMode(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutUsedMode(layoutFontStyle(tree->styles, layoutNodeStyle(tree, node)));
}

// Returns true if the box of `node` is an orthogonal flow root in a containing block in `outer`. Such a box is a block
// container whose lines would cross the lines of its containing block. It is laid out in its own writing mode, in a
// space that its parent makes (css-writing-modes-4/#orthogonal-flows). A replaced box has no lines, and is sized in the
// axes of its containing block. An anonymous flex item is in the writing mode of its container.
inline bool layoutIsOrthogonalRoot(const LayoutTree* tree, LayoutNodeIndex node, LayoutMode outer) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    // Only the axis of the writing mode decides it. The direction does not change the axis.
    return layoutKindIsElement(n.kind) && n.vertical == layoutIsHorizontal(outer);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tables

// Returns true if a box styled by `box` is a table, block-level or inline-level (css-tables-3/#table-structure).
inline bool layoutIsTable(const PrLayoutBoxStyle& box) {
    return box.display == PrLayoutDisplay_Table || box.display == PrLayoutDisplay_InlineTable;
}

// Returns true if a box of `display` is a table-internal box or a caption, which belongs inside a table
// (css-tables-3/#terminology).
inline bool layoutDisplayIsTablePart(PrLayoutDisplay display) {
    return display >= PrLayoutDisplay_TableRowGroup && display <= PrLayoutDisplay_TableCaption;
}

// Returns true if a box of `display` is a row group of any kind: a body, a header or a footer.
inline bool layoutDisplayIsRowGroup(PrLayoutDisplay display) {
    return display == PrLayoutDisplay_TableRowGroup || display == PrLayoutDisplay_TableHeaderGroup ||
        display == PrLayoutDisplay_TableFooterGroup;
}

// Returns true if a box of `display` is a table-track or table-track-group box: a row, a column or a group of them.
// Such a box paints no decoration of its own. Its background is painted behind the cells in it, by the table, or by the
// box itself if it paints whole.
inline bool layoutDisplayIsTrack(PrLayoutDisplay display) {
    return layoutDisplayIsRowGroup(display) || display == PrLayoutDisplay_TableRow ||
        display == PrLayoutDisplay_TableColumnGroup || display == PrLayoutDisplay_TableColumn;
}

// ---------------------------------------------------------------------------------------------------------------------
// Overflow

// Returns true if a box of `display` is never a scroll container, whatever its overflow: a table, or a table's track or
// track group. A table clips its content if its overflow is hidden or clip, but does not scroll it. Overflow does not
// apply to tracks and track groups (css-tables-3/#global-style-overrides).
inline bool layoutDisplayNeverScrolls(PrLayoutDisplay display) {
    return display >= PrLayoutDisplay_Table && display <= PrLayoutDisplay_TableColumn;
}

// Returns true if a box styled by `box` is a scroll container, which clips its content and scrolls it. A box is a
// scroll container if its overflow in either axis is hidden or scroll (css-overflow-3/#scroll-container), unless it is
// a table or a part of one other than a cell or caption. Overflow clip clips without making a scroll container.
inline bool layoutIsScrollContainer(const PrLayoutBoxStyle& box) {
    auto scrolls = [](PrLayoutOverflow overflow) {
        return overflow == PrLayoutOverflow_Hidden || overflow == PrLayoutOverflow_Scroll;
    };
    return (scrolls(box.overflowX) || scrolls(box.overflowY)) && !layoutDisplayNeverScrolls(box.display);
}

// The axes in which a box clips its content.
struct LayoutClips {
    bool x;
    bool y;
};

// Returns the axes in which a box styled by `box` clips its content (css-overflow-3/#overflow-control). A scroll
// container clips in both axes, whatever its pair of values. A box that only clips does so in the axes whose overflow
// is clip, and a table also in those whose overflow is hidden. A table's track or track group clips nothing.
inline LayoutClips layoutClips(const PrLayoutBoxStyle& box) {
    if (box.overflowX == PrLayoutOverflow_Visible && box.overflowY == PrLayoutOverflow_Visible)
        return LayoutClips{false, false};
    if (layoutIsScrollContainer(box))
        return LayoutClips{true, true};
    auto clips = [&](PrLayoutOverflow overflow) {
        if (overflow == PrLayoutOverflow_Hidden)
            return box.display == PrLayoutDisplay_Table || box.display == PrLayoutDisplay_InlineTable;
        return overflow == PrLayoutOverflow_Clip && !layoutDisplayIsTrack(box.display);
    };
    return LayoutClips{clips(box.overflowX), clips(box.overflowY)};
}

// Returns the area that `fragment` adds to the overflow of its parent, from its own top left
// (css-overflow-3/#scrollable-overflow). The area is its box and, in each axis that it does not clip, what it reaches
// past its box. A box that clips cuts its content at its clip edge, which lies within its border box, so only its
// border box counts in that axis.
PrLayoutRect layoutPassedOverflow(const LayoutTree* tree, const LayoutFragment* fragment);

// Sets the overflow of `fragment`, whose children are already placed, to its own box and what each child adds at its
// position.
void layoutFragmentSetOverflow(const LayoutTree* tree, LayoutFragment* fragment);

// Sets the overflow of the scroll container `fragment`, styled by `box` in its used writing mode `mode`, to its
// scrollable overflow (css-overflow-3/#scrollable-overflow). The scrollable overflow covers its padding box, what its
// content reaches, and the bounds of its in-flow content surrounded by its used padding `padding`, indexed by
// PrRectSide, so that the padding shows past the content when it is scrolled to its end.
//
// The bounds are the border boxes of its children, found through anonymous boxes, rows and lines, together with `flow`,
// the extent of its in-flow content and the margins it holds. Nothing past the block-start or inline-start edge of its
// padding box, which are the sides of its origin, is reached.
void layoutScrollerSetOverflow(const LayoutTree* tree, LayoutFragment* fragment, const PrLayoutBoxStyle& box,
    const PrLayoutUnit* padding, LayoutMode mode, PrLayoutRect flow);

// Returns the scrollable overflow of a scroll container `fragment` styled by `box`, from the top left of its padding
// box, which is the unscrolled position of the scrollport.
inline PrLayoutRect layoutScrollContent(const LayoutFragment* fragment, const PrLayoutBoxStyle& box) {
    return layoutRectMoved(fragment->overflow, PrLayoutVec{-box.border[PrRectSide_Left], -box.border[PrRectSide_Top]});
}

// Returns the scrolling area of `fragment`, a box that is not a scroll container, styled by `box` in its used writing
// mode `mode` (cssom-view-1/#scrolling-area). The area is measured from the top left of its padding box. It covers its
// padding box and what its children add to its overflow, cut at the sides of its origin in the same way as the area of
// a scroll container.
PrLayoutRect layoutScrollingArea(const LayoutTree* tree, const LayoutFragment* fragment, const PrLayoutBoxStyle& box,
    LayoutMode mode);

// layoutScrollRange returns the scroll offsets that a port of `portExtent` can take over `content`, its scrollable
// overflow from its unscrolled top left, as a rectangle from the least offset to the greatest. layoutScrollClamp
// returns `offset` kept within that range. An offset is the position of the port relative to its unscrolled position,
// which is the scroll origin, so an offset below zero scrolls toward the right or bottom edge of an origin.
PrLayoutRect layoutScrollRange(PrLayoutVec portExtent, PrLayoutRect content);
PrLayoutVec layoutScrollClamp(PrLayoutVec offset, PrLayoutRect range);

// Returns the axes along which a user may scroll a box with the overflow of `box`. The viewport uses the axes of the
// root, for which visible overflow scrolls.
PrLayoutScrollFlags layoutUserAxes(const PrLayoutBoxStyle& box);

// Returns the axes along which a user's scroll that a box with the style `box` cannot take chains to the containers
// around it: the axes that the box does not scroll, and those whose overscroll behavior chains.
PrLayoutScrollFlags layoutChainAxes(const PrLayoutBoxStyle& box);

// ---------------------------------------------------------------------------------------------------------------------
// Taking part in a parent's layout

// Returns true if a box of `position` is absolutely positioned or running, which takes it out of flow
// (css-position-3/#position-property, css-gcpm-3/#running-elements).
inline bool layoutPositionIsOutOfFlow(PrLayoutPosition position) {
    return position == PrLayoutPosition_Absolute || position == PrLayoutPosition_Fixed ||
        position == PrLayoutPosition_Running;
}

// Returns true if the box of the node is out of flow (css-position-3/#abspos-layout). This is an element whose position
// is absolute, fixed or running, other than the root, which is laid out as static whatever its position
// (CSS2/#dis-pos-flo).
inline bool layoutIsOutOfFlow(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutTreeNode(tree, node).positioned && node != tree->root;
}

// Returns true if the position of the node is fixed, which places its box against the viewport if no ancestor holds it
// (css-position-3/#valdef-position-fixed).
inline bool layoutIsFixed(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutBoxStyle(tree->styles, layoutTreeNode(tree, node).style).position == PrLayoutPosition_Fixed;
}

// Returns true if a box styled by `box` is a flex container (css-flexbox-2/#flex-containers).
inline bool layoutIsFlexContainer(const PrLayoutBoxStyle& box) {
    return box.display == PrLayoutDisplay_Flex || box.display == PrLayoutDisplay_InlineFlex;
}

// Returns true if a box styled by `box` is a grid container (css-grid-1/#grid-containers).
inline bool layoutIsGridContainer(const PrLayoutBoxStyle& box) {
    return box.display == PrLayoutDisplay_Grid || box.display == PrLayoutDisplay_InlineGrid;
}

// Returns true if a box styled by `box` is a math box (mathml-core/#new-display-math-value).
inline bool layoutIsMathBox(const PrLayoutBoxStyle& box) {
    return box.display == PrLayoutDisplay_BlockMath || box.display == PrLayoutDisplay_InlineMath;
}

// Returns true if a math box of `kind` is a token, whose content is laid out in flow and which its math layout then
// measures by its ink (layout_math.h).
inline bool layoutMathKindIsToken(PrLayoutMathKind kind) {
    return kind == PrLayoutMathKind_Token || kind == PrLayoutMathKind_Operator;
}

// How a box lays out its children, which names the algorithm that lays out its content (layout_content.h): in flow, as
// the items of a flex or grid container, as the parts of a table, or by math layout. A table's row groups, rows, column
// groups and columns also hold table parts. A math box's children are its items, as a flex container's are. The kinds
// that make items of the children follow the others (layoutChildrenAreItems).
enum {
    LayoutChildren_Flow,
    LayoutChildren_Table,
    LayoutChildren_Flex,
    LayoutChildren_Grid,
    LayoutChildren_Math,
};
typedef uint8_t LayoutChildren;

// Returns how an element styled by `box`, of the math kind `kind`, lays out its children. A token's content is in flow.
// A table cell of a math kind, as an mtd is, lays out its content as a math row (mathml-core/#table-or-matrix-mtable).
inline LayoutChildren layoutChildrenFromBox(const PrLayoutBoxStyle& box, PrLayoutMathKind kind) {
    if (layoutIsMathBox(box))
        return layoutMathKindIsToken(kind) ? LayoutChildren_Flow : LayoutChildren_Math;
    if (box.display == PrLayoutDisplay_TableCell && kind != PrLayoutMathKind_None)
        return LayoutChildren_Math;
    if (layoutIsFlexContainer(box))
        return LayoutChildren_Flex;
    if (layoutIsGridContainer(box))
        return LayoutChildren_Grid;
    if (layoutIsTable(box) || layoutDisplayIsRowGroup(box.display) || box.display == PrLayoutDisplay_TableRow ||
            box.display == PrLayoutDisplay_TableColumnGroup || box.display == PrLayoutDisplay_TableColumn)
        return LayoutChildren_Table;
    return LayoutChildren_Flow;
}

// Returns true if a box that lays out its children as `children` makes items of them, as a flex or grid container does.
// Each child in flow is a block-level box that floats in no context and takes part in no inline formatting context of
// its parent, and each run of text is an anonymous item (css-flexbox-2/#flex-items, css-grid-1/#grid-items). Items
// paint in order-modified document order, and z-index applies to them whatever their position.
inline bool layoutChildrenAreItems(LayoutChildren children) {
    return children >= LayoutChildren_Flex;
}

// Returns true if the parent of the node is a flex container.
inline bool layoutInFlexContainer(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex parent = layoutTreeNode(tree, node).parent;
    return parent != kPrLayoutNodeNone && layoutTreeNode(tree, parent).children == LayoutChildren_Flex;
}

// Returns true if the parent of the node makes items of its children (layoutChildrenAreItems), so that the node is an
// item if it is in flow.
inline bool layoutInItemContainer(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex parent = layoutTreeNode(tree, node).parent;
    return parent != kPrLayoutNodeNone && layoutChildrenAreItems(layoutTreeNode(tree, parent).children);
}

// Returns true if the parent of the node lays out its children by math layout, so that the node is a child of a math
// box whose content does not wrap.
inline bool layoutInMath(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex parent = layoutTreeNode(tree, node).parent;
    return parent != kPrLayoutNodeNone && layoutTreeNode(tree, parent).children == LayoutChildren_Math;
}

// Returns true if the parent of the node holds the parts of a table: a table, a row group, a row or a column group.
inline bool layoutInTable(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex parent = layoutTreeNode(tree, node).parent;
    return parent != kPrLayoutNodeNone && layoutTreeNode(tree, parent).children == LayoutChildren_Table;
}

// Returns true if the element `node`, styled by `box`, is a collapsed flex item. This is an in-flow item of a flex
// container whose visibility is collapse, which its container lays out as taking no room in its line
// (css-flexbox-2/#visibility-collapse).
inline bool layoutIsCollapsedItem(const LayoutTree* tree, LayoutNodeIndex node, const PrLayoutBoxStyle& box) {
    return box.collapsed && !layoutPositionIsOutOfFlow(box.position) && layoutInFlexContainer(tree, node);
}

// Returns true if the node is a child of a table column, or a child of a column group that is not a column. Such a node
// generates no box (css-tables-3/#fixup-algorithm).
bool layoutIsInColumn(const LayoutTree* tree, LayoutNodeIndex node);

// Returns true if the node has no box in the final tree, in which case its subtree has none either. An element has no
// box if its display is none, it is running, or it is a collapsed flex item. A text node or a line break has a box
// whenever its parent has one. A child of a table column has no box, and neither has a child of a column group that is
// not a column.
inline bool layoutIsAbsent(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.parent != kPrLayoutNodeNone && layoutTreeNode(tree, n.parent).children == LayoutChildren_Table &&
            layoutIsInColumn(tree, node))
        return true;
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    // A running box is out of flow, which the node's bit says without its record.
    return box.display == PrLayoutDisplay_None ||
        (n.positioned && box.position == PrLayoutPosition_Running && node != tree->root) ||
        layoutIsCollapsedItem(tree, node, box);
}

// The preferred aspect ratio of a box in a writing mode (css-sizing-4/#aspect-ratio).
struct LayoutAspect {
    double ratio;                   // its inline size over its block size, or 0 if it has none
    bool borderBox;                 // true if the ratio relates border boxes; otherwise it relates content boxes
    bool natural;                   // true if the ratio is the natural ratio of a replaced box
};

// Returns the natural ratio of the replaced box of `node`, its width over its height. Uses its natural size if it has
// both dimensions, or else its natural ratio, and returns zero if it has neither.
double layoutNaturalRatio(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the preferred aspect ratio of `node`, styled by `box`, in `mode`. A replaced box with a natural ratio uses it
// if its aspect-ratio is auto or is auto with a ratio. Any other box has the ratio that its aspect-ratio gives, if any.
inline LayoutAspect layoutNodeAspect(const LayoutTree* tree, LayoutNodeIndex node, const PrLayoutBoxStyle& box,
        LayoutMode mode) {
    LayoutAspect aspect{};
    double ratio = 0;
    bool defers = box.aspectRatio == 0 || box.aspectRatioAuto;
    if (defers && layoutTreeNode(tree, node).kind == PrLayoutNodeKind_Replaced)
        ratio = layoutNaturalRatio(tree, node);
    if (ratio > 0) {
        aspect.natural = true;
    } else if (box.aspectRatio > 0) {
        ratio = double(box.aspectRatio);
        aspect.borderBox = !box.aspectRatioAuto && box.boxSizing == PrLayoutBoxSizing_BorderBox;
    } else {
        return aspect;
    }
    aspect.ratio = layoutIsHorizontal(mode) ? ratio : 1 / ratio;
    return aspect;
}

// layoutAspectBlock returns the border-box block size that the border-box inline size `size` gives through `aspect`,
// with the padding and borders `edgesInline` and `edgesBlock` across each axis. The result is never less than
// `edgesBlock`. layoutAspectInline returns the inline size that a block size gives in the same way.
inline PrLayoutUnit layoutAspectBlock(const LayoutAspect& aspect, PrLayoutUnit size, PrLayoutUnit edgesInline,
        PrLayoutUnit edgesBlock) {
    double raw;
    if (aspect.borderBox)
        raw = double(size.raw) / aspect.ratio - double(edgesBlock.raw);
    else
        raw = double(layoutUnitMax(kLayoutUnitZero, size - edgesInline).raw) / aspect.ratio;
    return prLayoutUnitClamped(int64_t(raw > 0 ? raw + 0.5 : 0)) + edgesBlock;
}
inline PrLayoutUnit layoutAspectInline(const LayoutAspect& aspect, PrLayoutUnit size, PrLayoutUnit edgesInline,
        PrLayoutUnit edgesBlock) {
    double raw;
    if (aspect.borderBox)
        raw = double(size.raw) * aspect.ratio - double(edgesInline.raw);
    else
        raw = double(layoutUnitMax(kLayoutUnitZero, size - edgesBlock).raw) * aspect.ratio;
    return prLayoutUnitClamped(int64_t(raw > 0 ? raw + 0.5 : 0)) + edgesInline;
}

// Returns true if the box of the node floats (CSS2/#floats). This is an element whose float is not none, except for the
// root, a box out of flow, whose float computes to none (CSS2/#dis-pos-flo), and a flex or grid item, whose float is
// not read.
inline bool layoutIsFloat(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak || node == tree->root)
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    return box.floating != PrLayoutFloat_None && box.display != PrLayoutDisplay_None &&
        !layoutPositionIsOutOfFlow(box.position) && !layoutInItemContainer(tree, node);
}

// Returns true if the node takes part in an inline formatting context. It does if it is text or a line break, or if its
// box is inline-level, does not float, and is not a flex or grid item.
bool layoutIsInlineLevel(const LayoutTree* tree, LayoutNodeIndex node);

// Returns true if the node, in an inline formatting context, is an atomic inline: a single box that lines break around
// (CSS2/#inline-formatting, css-writing-modes-4/#block-flow). Atomic inlines are replaced elements, inline-block,
// inline-flex and inline-grid boxes, and inline boxes whose writing mode differs from their parent's, which are laid
// out as inline-blocks. A flex or grid item is never an atomic inline.
bool layoutIsAtomicInline(const LayoutTree* tree, LayoutNodeIndex node);

// Returns true if the element `node` is laid out as an inline box, which paints on its lines in the recording of its
// block. Its display is inline, its writing mode is the same as its parent's, it does not float, and it is not a flex
// or grid item. A block-level box inside an inline box is a block box (CSS2/#anonymous-block-level). The As form asks
// the question for the element styled by `style`, as a change to its record would leave it.
bool layoutIsInlineBoxAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style);
inline bool layoutIsInlineBox(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutIsInlineBoxAs(tree, node, layoutTreeNode(tree, node).style);
}

// The writing mode of a containing block, and its font group or kLayoutFontGroupNone. A box styled with that font group
// has that writing mode, so the writing mode need not be read.
struct LayoutOuterMode {
    LayoutMode mode;
    PrLayoutGroupIndex font;
};

// No font group (LayoutOuterMode::font).
constexpr uint32_t kLayoutFontGroupNone = 0xffffffffu;

// Returns true if the box of `node`, styled by `box`, establishes a block formatting context for its content
// (CSS2/#block-formatting, css-writing-modes-4/#block-flow, css-position-3/#position-property). These boxes are the
// root, a float, a box out of flow, a scroll container, flow-root, inline-block, an outside marker, a flex or grid
// container, a table, a table cell, a caption, a multi-column container (css-multicol-1/#the-multi-column-model), and a
// box whose writing mode differs from that of its containing block, `outer`. The content of a flex or grid container or
// a table is independent of its context (css-flexbox-2/#flex-containers, css-grid-1/#grid-containers,
// css-tables-3/#fixup-algorithm).
inline bool layoutEstablishesContext(const LayoutTree* tree, LayoutNodeIndex node, const PrLayoutBoxStyle& box,
        LayoutOuterMode outer) {
    // The displays whose boxes establish a context, by PrLayoutDisplay.
    constexpr uint32_t kDisplays = 1u << PrLayoutDisplay_FlowRoot | 1u << PrLayoutDisplay_InlineBlock |
        1u << PrLayoutDisplay_Marker | 1u << PrLayoutDisplay_Flex | 1u << PrLayoutDisplay_InlineFlex |
        1u << PrLayoutDisplay_Grid | 1u << PrLayoutDisplay_InlineGrid | 1u << PrLayoutDisplay_Table |
        1u << PrLayoutDisplay_InlineTable | 1u << PrLayoutDisplay_TableCell | 1u << PrLayoutDisplay_TableCaption |
        1u << PrLayoutDisplay_PageMargin | 1u << PrLayoutDisplay_BlockMath | 1u << PrLayoutDisplay_InlineMath;
    static_assert(PrLayoutDisplay_InlineMath < 32, "every display has a bit");
    if ((kDisplays >> box.display & 1u) != 0 || node == tree->root || box.floating != PrLayoutFloat_None ||
            layoutPositionIsOutOfFlow(box.position) || layoutIsScrollContainer(box))
        return true;
    PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
    if (layoutHasColumns(tree->styles, style))
        return true;
    if (layoutStyle(tree->styles, style).groups[PrLayoutGroup_Font] == outer.font)
        return false;
    return layoutFontStyle(tree->styles, style).writingMode != outer.mode.writingMode;
}

// Returns true if the in-flow block-level box of `node`, styled by `box`, must not overlap the floats in the context of
// its parent (CSS2/#floats). This is a replaced box, or a box that establishes a context of its own.
inline bool layoutAvoidsFloats(const LayoutTree* tree, LayoutNodeIndex node, const PrLayoutBoxStyle& box,
        LayoutOuterMode outer) {
    return layoutTreeNode(tree, node).kind == PrLayoutNodeKind_Replaced || layoutEstablishesContext(tree, node, box, outer);
}

// Returns the mode that the box of `node` gives its content, which is its own mode, together with its font group.
inline LayoutOuterMode layoutNodeOuterMode(const LayoutTree* tree, LayoutNodeIndex node) {
    PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
    PrLayoutGroupIndex font = layoutStyle(tree->styles, style).groups[PrLayoutGroup_Font];
    return LayoutOuterMode{layoutOwnMode(tree, node), font};
}

// Returns the sides of the line that a float or clear value names by its parts, in a containing block of `direction`
// (css-logical-1/#float-clear).
LayoutLineSides layoutLineSides(bool left, bool right, bool start, bool end, PrTextDirection direction);

// Returns the side of the line that a float styled by `box` takes in a containing block of `direction`.
inline LayoutLineSides layoutFloatSide(const PrLayoutBoxStyle& box, PrTextDirection direction) {
    PrLayoutFloat floating = box.floating;
    return layoutLineSides(floating == PrLayoutFloat_Left, floating == PrLayoutFloat_Right,
        floating == PrLayoutFloat_InlineStart, floating == PrLayoutFloat_InlineEnd, direction);
}

// Returns the sides of the floats that a box styled by `box` clears in a containing block of `direction`, or zero.
PR_FORCE_INLINE LayoutLineSides layoutClearSides(const PrLayoutBoxStyle& box, PrTextDirection direction) {
    PrLayoutClear clear = box.clear;
    if (clear == PrLayoutClear_None)
        return 0;
    return layoutLineSides(clear == PrLayoutClear_Left || clear == PrLayoutClear_Both,
        clear == PrLayoutClear_Right || clear == PrLayoutClear_Both,
        clear == PrLayoutClear_InlineStart, clear == PrLayoutClear_InlineEnd, direction);
}

// ---------------------------------------------------------------------------------------------------------------------
// Children

// How a child of a block container takes part in its layout: not at all, in an inline formatting context, as a float,
// as a block-level box in flow, out of flow, through a placeholder alone, or as a part of the anonymous table that
// wraps it with its consecutive siblings of the same kind (css-tables-3/#fixup-algorithm). An outside marker takes no
// part in its parent's flow, and its list item lays it out apart (layoutNodeMarker).
enum {
    LayoutChildKind_None,
    LayoutChildKind_Inline,
    LayoutChildKind_Float,
    LayoutChildKind_Block,
    LayoutChildKind_OutOfFlow,
    LayoutChildKind_TablePart,
};
typedef uint8_t LayoutChildKind;

// Returns true if a child of `kind` is block-level and in flow: a block-level box, or a part of an anonymous table.
inline bool layoutChildKindIsBlock(LayoutChildKind kind) {
    return kind == LayoutChildKind_Block || kind == LayoutChildKind_TablePart;
}

// Returns how a node of `kind` takes part in the layout of its parent, if it is not the root. `box` is its style if it
// is not text. `positioned` is its LayoutTreeNode::positioned.
inline LayoutChildKind layoutChildKindFromStyle(PrLayoutNodeKind kind, const PrLayoutBoxStyle* PR_NULLABLE box,
        bool positioned) {
    if (kind == PrLayoutNodeKind_Text)
        return LayoutChildKind_Inline;
    if (box->display == PrLayoutDisplay_None || box->display == PrLayoutDisplay_Marker ||
            box->display == PrLayoutDisplay_PageMargin)
        return LayoutChildKind_None;
    if (kind == PrLayoutNodeKind_LineBreak)
        return LayoutChildKind_Inline;
    if (positioned)
        return LayoutChildKind_OutOfFlow;
    if (box->floating != PrLayoutFloat_None)
        return LayoutChildKind_Float;
    if (layoutDisplayIsTablePart(box->display))
        return LayoutChildKind_TablePart;
    return prLayoutDisplayIsInlineLevel(box->display) ? LayoutChildKind_Inline : LayoutChildKind_Block;
}

// Returns how `node` takes part in the layout of its parent. The root is never out of flow and never floats.
inline LayoutChildKind layoutChildKind(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    LayoutChildKind kind = n.childKind;
    if ((kind == LayoutChildKind_OutOfFlow || kind == LayoutChildKind_Float) && node == tree->root) {
        bool inlineLevel = prLayoutDisplayIsInlineLevel(layoutBoxStyle(tree->styles, n.style).display);
        return inlineLevel ? LayoutChildKind_Inline : LayoutChildKind_Block;
    }
    return kind;
}

// Looks at the children out of flow from `node` on, up to the next sibling in flow, skipping siblings that generate no
// box. If that sibling is not inline-level, none of these children starts a run of inline-level children, and the
// function returns the last of them. Otherwise `node` starts the run that the sibling is in, and the function returns
// kPrLayoutNodeNone.
LayoutNodeIndex layoutOutOfFlowNoRun(const LayoutTree* tree, LayoutNodeIndex node);

// Returns true if the out-of-flow child `node` starts the run of inline-level children that it adjoins. `noRunLast` is
// the last of the children out of flow around `node` that start no run, if one look-ahead found it, or none. Keeping it
// saves a look-ahead from each of those children. It is reset to none once `node` is that child.
inline bool layoutOutOfFlowStartsRun(const LayoutTree* tree, LayoutNodeIndex node, LayoutNodeIndex& noRunLast) {
    if (noRunLast == kPrLayoutNodeNone) {
        noRunLast = layoutOutOfFlowNoRun(tree, node);
        if (noRunLast == kPrLayoutNodeNone)
            return true;
    }
    if (node == noRunLast)
        noRunLast = kPrLayoutNodeNone;
    return false;
}

// Returns true if the node is the content of an anonymous item when its parent makes items of its children: text, or a
// line break whose display is not none (css-flexbox-2/#flex-items, css-grid-1/#grid-items).
inline bool layoutIsRunContent(const LayoutTreeNode& n) {
    return (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak) &&
        n.childKind != LayoutChildKind_None;
}

// Returns the first content of the run of children of a flex or grid container that holds `node`. Searches back from
// `node`, past children that generate no box, to the first child that generates a box. The anonymous item of the run
// keeps its caches under the returned node. Returns kPrLayoutNodeNone if `node` generates no box and no content
// precedes it in its run.
LayoutNodeIndex layoutRunFirst(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the last content of the run that starts at `first`.
LayoutNodeIndex layoutRunLast(const LayoutTree* tree, LayoutNodeIndex first);

// Returns true if the run from `first` through `last` is text made only of white space, which is not rendered
// (css-flexbox-2/#flex-items, css-grid-1/#grid-items).
bool layoutRunIsBlank(const LayoutTree* tree, LayoutNodeIndex first, LayoutNodeIndex last);

// Return the styles of the box of an anonymous flex or grid item (css-display-4/#anonymous). It is a block box whose
// non-inherited properties have their initial values. Its inherited properties come from its container.
const PrLayoutBoxStyle& layoutAnonymousBoxStyle(void);
const PrLayoutFlexStyle& layoutAnonymousFlexStyle(void);

// The box styles of an anonymous table cell and of an anonymous block-level table (css-tables-3/#fixup-boxes). Their
// properties that do not inherit have their initial values.
const PrLayoutBoxStyle& layoutAnonymousCellStyle(void);
const PrLayoutBoxStyle& layoutAnonymousTableStyle(void);

// How a table cell is aligned in the rows that it spans (css-tables-3/#row-layout): at the top of its first row, in the
// middle, at the bottom of its last row, or with its first baseline on the baseline of its first row.
enum {
    LayoutCellAlign_Top,
    LayoutCellAlign_Middle,
    LayoutCellAlign_Bottom,
    LayoutCellAlign_Baseline,
};
typedef uint8_t LayoutCellAlign;

// Returns how a table cell styled by `box` is aligned in its rows, from the longhands of its vertical-align. A
// baseline-shift of top or bottom aligns it to the top or the bottom, and an alignment-baseline of middle to the
// middle. Every other value aligns it by its baseline.
inline LayoutCellAlign layoutCellAlignFromBox(const PrLayoutBoxStyle& box) {
    if (box.baselineShift.bits == prLayoutBaselineShiftKeyword(PrLayoutKeyword_Top).bits)
        return LayoutCellAlign_Top;
    if (box.baselineShift.bits == prLayoutBaselineShiftKeyword(PrLayoutKeyword_Bottom).bits)
        return LayoutCellAlign_Bottom;
    if (box.alignmentBaseline == PrLayoutAlignmentBaseline_Middle)
        return LayoutCellAlign_Middle;
    return LayoutCellAlign_Baseline;
}

// Visits the children of the flex or grid container `node` in order. Calls `item` with each child that is an item or
// out of flow, and its box. Calls `run` with the first and last content of each run of text and line breaks that is an
// anonymous item. A blank run is not an item (css-flexbox-2/#flex-items, css-grid-1/#grid-items).
template <typename Item, typename Run>
void layoutEachItem(const LayoutTree* tree, LayoutNodeIndex node, Item item, Run run) {
    const LayoutTreeNode& parent = layoutTreeNode(tree, node);
    PR_ASSERT(layoutChildrenAreItems(parent.children));
    for (uint32_t c = parent.first; c != kPrLayoutNodeNone; c = layoutTreeNode(tree, c).next) {
        const LayoutTreeNode& n = layoutTreeNode(tree, c);
        if (layoutIsRunContent(n)) {
            LayoutNodeIndex last = layoutRunLast(tree, c);
            if (!layoutRunIsBlank(tree, c, last))
                run(c, last);
            c = last;
        } else if (layoutChildKind(tree, c) != LayoutChildKind_None) {
            item(c, layoutBoxStyle(tree->styles, n.style));
        }
    }
}

// Returns the outside marker of the block container `node`: its first child, if the child's display is Marker, or
// kPrLayoutNodeNone. Only a first child that takes no part in the flow pays for the look at its style.
inline LayoutNodeIndex layoutNodeMarker(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex first = layoutTreeNode(tree, node).first;
    if (first == kPrLayoutNodeNone)
        return kPrLayoutNodeNone;
    const LayoutTreeNode& n = layoutTreeNode(tree, first);
    if (n.childKind != LayoutChildKind_None || n.kind != PrLayoutNodeKind_Element ||
            layoutBoxStyle(tree->styles, n.style).display != PrLayoutDisplay_Marker)
        return kPrLayoutNodeNone;
    return first;
}

// Returns true if the node is a text node that holds only document white space (css-text-3/#white-space-processing).
bool layoutIsBlankText(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the last node of the run of table parts that starts at `first` and ends before `end`. An anonymous table
// wraps the run (css-tables-3/#fixup-algorithm). The run holds consecutive children of kind LayoutChildKind_TablePart,
// and the children between them that generate no box or hold only white space.
LayoutNodeIndex layoutTablePartRunLast(const LayoutTree* tree, LayoutNodeIndex first, LayoutNodeIndex end);

// Visits the siblings from `first` through `last` in order. They are the children of a block container, or the children
// that an anonymous box wraps. Calls `block` with each child that is block-level, floats or is out of flow, and its
// box. Calls `inlines` with the first and last child of each run of inline-level children, as an inline formatting
// context lays it out. Calls `table` with the first and last child of each run of table parts, which an anonymous table
// wraps (layoutTablePartRunLast).
//
// If no child is block-level and in flow, all the children form one run, and `inlines` is told that the run is alone.
// Otherwise a run among block-level children is wrapped in an anonymous block box (css-display-4/#anonymous). Such a
// run starts at an inline-level child that generates a box, or at a child out of flow before one, and takes the floats
// and the children out of flow that follow it. A child out of flow never makes an anonymous block box of its own.
template <typename Block, typename Inlines, typename Table>
void layoutEachChildIn(const LayoutTree* tree, LayoutNodeIndex first, LayoutNodeIndex last, Block block,
        Inlines inlines, Table table) {
    LayoutNodeIndex end = layoutTreeNode(tree, last).next;
    bool blocks = false;
    for (uint32_t c = first; c != end && !blocks; c = layoutTreeNode(tree, c).next)
        blocks = layoutChildKindIsBlock(layoutChildKind(tree, c));
    if (!blocks) {
        inlines(first, last, true);
        return;
    }
    // The last of the children out of flow being visited that start no run, which one look-ahead found for all of them,
    // or none.
    LayoutNodeIndex noRunLast = kPrLayoutNodeNone;
    for (uint32_t c = first; c != end; c = layoutTreeNode(tree, c).next) {
        LayoutChildKind kind = layoutChildKind(tree, c);
        if (kind == LayoutChildKind_None)
            continue;
        if (kind == LayoutChildKind_TablePart) {
            uint32_t runLast = layoutTablePartRunLast(tree, c, end);
            table(c, runLast);
            c = runLast;
            continue;
        }
        if (kind != LayoutChildKind_Inline &&
                !(kind == LayoutChildKind_OutOfFlow && layoutOutOfFlowStartsRun(tree, c, noRunLast))) {
            block(c, layoutBoxStyle(tree->styles, layoutTreeNode(tree, c).style));
            continue;
        }
        uint32_t runLast = c;
        for (uint32_t n = layoutTreeNode(tree, c).next; n != end; n = layoutTreeNode(tree, n).next) {
            if (layoutChildKindIsBlock(layoutChildKind(tree, n)))
                break;
            runLast = n;
        }
        inlines(c, runLast, false);
        c = runLast;
    }
}

// Visits the children of the block container `node` in order, as layoutEachChildIn does. The container's outside marker
// is in no run. layoutEachItem visits the children of a flex or grid container, and layoutEachTableBox those of a
// table.
template <typename Block, typename Inlines, typename Table>
void layoutEachChild(const LayoutTree* tree, LayoutNodeIndex node, Block block, Inlines inlines, Table table) {
    const LayoutTreeNode& parent = layoutTreeNode(tree, node);
    PR_ASSERT(!layoutChildrenAreItems(parent.children));
    // The marker is in no run, because collecting a run clears the marks of the nodes in it.
    LayoutNodeIndex first = parent.first;
    if (first != kPrLayoutNodeNone && layoutNodeMarker(tree, node) == first)
        first = layoutTreeNode(tree, first).next;
    if (first != kPrLayoutNodeNone)
        layoutEachChildIn(tree, first, parent.last, block, inlines, table);
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

// Returns true if z-index applies to the box of `node`, styled by `box` (css-position-3/#stacking). It applies if the
// box is positioned, or is a flex or grid item or a page-margin box, whose z-index applies whatever its position
// (css-flexbox-2/#painting, css-grid-1/#z-order, css-page-3/#painting). It never applies to the root, which is laid out
// as static whatever its position.
PR_FORCE_INLINE bool layoutZIndexApplies(const LayoutTree* tree, LayoutNodeIndex node, const PrLayoutBoxStyle& box) {
    return (box.position != PrLayoutPosition_Static || box.display == PrLayoutDisplay_PageMargin ||
        layoutInItemContainer(tree, node)) && node != tree->root;
}

// Returns the stack level of the box of `node` in its stacking context (CSS2/#stack-level): its z-index if z-index
// applies and is not auto, and 0 otherwise. A box at a level other than 0 makes a stacking context. The As form of this
// function, and of the functions after it, asks the question for the box styled by `style`, as a change to its record
// would leave it.
int32_t layoutStackLevelAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style);
inline int32_t layoutStackLevel(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutStackLevelAs(tree, node, layoutTreeNode(tree, node).style);
}

// Returns true if the box of `node` makes a stacking context, which contains the stack levels of its descendants
// (CSS2/#stacking-context). These boxes are the root, a positioned box whose z-index is not auto, a fixed or sticky
// box, a page-margin box, a box with opacity below 1 (css-color-4/#transparency), a box with a transform if transforms
// apply to it (css-transforms-1/#transform-rendering), and a box that a compositor animates, because an animated
// property has the effect of will-change (web-animations-1/#side-effects-section).
bool layoutIsStackingContextAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style);
inline bool layoutIsStackingContext(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutIsStackingContextAs(tree, node, layoutTreeNode(tree, node).style);
}

// Returns true if the box of `node`, which has a fragment of its own and is styled by `style`, is painted whole in the
// last phase of its stacking context. This is the case if it is positioned or makes a stacking context. A transform
// other than none makes one whatever its matrix (css-transforms-1/#transform-rendering, css-color-4/#transparency), and
// so does a flex or grid item whose z-index is not auto (css-flexbox-2/#painting, css-grid-1/#z-order), and a
// page-margin box (css-page-3/#painting).
//
// Painting late is separate from containing the stack levels of the descendants of the box, which only a stacking
// context does. A positioned box at z-index auto paints as a stacking container
// (css-position-4/#paint-a-stacking-container), and the boxes in it at other levels belong to the stacking context
// around it.
bool layoutPaintsWholeAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style);

// Returns true if the inline box styled by `style` is painted whole. It is if it is positioned, or makes a stacking
// context through its opacity or because a compositor animates it. A transform does not apply to an inline box. Its
// parts on lines, its rows around the blocks in it, and its floats then paint together (layout_walk.h).
bool layoutInlinePaintsWhole(const LayoutStyles* styles, PrLayoutStyleIndex style);

// Returns true if a box with a fragment of its own, styled by `style`, has a layer of its own, which a scene plays with
// its effects. It does if it makes a stacking context through its opacity or transform, or if a compositor animates it.
bool layoutOwnsLayer(const LayoutStyles* styles, PrLayoutStyleIndex style);

// Returns true if the inline box styled by `style` has a layer of its own: its opacity is below 1, or a compositor
// animates it.
bool layoutInlineOwnsLayer(const LayoutStyles* styles, PrLayoutStyleIndex style);

// Returns true if the box of `node` has a layer of its own, as decided by layoutInlineOwnsLayer for an inline box and
// by layoutOwnsLayer for any other box.
bool layoutHasLayerAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style);
inline bool layoutHasLayer(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutHasLayerAs(tree, node, layoutTreeNode(tree, node).style);
}

// Returns the traits of the record `style` (LayoutStyleTraits), from its groups.
LayoutStyleTraits layoutStyleTraitsFromIndex(const LayoutStyles* styles, PrLayoutStyleIndex style);

// ---------------------------------------------------------------------------------------------------------------------
// Containing blocks of boxes out of flow

// Returns true if a transform applies to the box of `node` and makes it a containing block
// (css-transforms-1/#transformable-element). This is the case if its record says it is transformed and it is not laid
// out as an inline box.
bool layoutIsTransformed(const LayoutTree* tree, LayoutNodeIndex node);

// Returns true if the box of `node` is the containing block of every box out of flow in it: a transform applies to it
// and it is not laid out as an inline box, or it has a filter or a backdrop filter and is not a root
// (filter-effects-1/#FilterProperty, filter-effects-2/#BackdropFilterProperty). A filtered inline box holds them as a
// positioned inline box holds absolutely positioned boxes.
bool layoutHoldsAll(const LayoutTree* tree, LayoutNodeIndex node);

// layoutHoldsAbsolute returns true if the box of `node` is the containing block of the absolutely positioned boxes in
// it, and layoutHoldsFixed returns true if it is the containing block of the fixed boxes in it
// (css-position-3/#def-cb). A positioned box holds absolutely positioned boxes, and a box that holds all holds both
// (layoutHoldsAll). A positioned inline box is the containing block of the absolutely positioned boxes in it, but the
// block container whose lines hold it lays them out (layoutNodeHolder).
inline bool layoutHoldsAbsolute(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return false;
    return layoutBoxStyle(tree->styles, n.style).position != PrLayoutPosition_Static || layoutHoldsAll(tree, node);
}
inline bool layoutHoldsFixed(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutHoldsAll(tree, node);
}

// Returns true if `holder` is the containing block of a box out of flow in it, whose position is `position`: fixed, or
// absolute otherwise. No box holds a running box.
inline bool layoutHoldsPosition(const LayoutTree* tree, LayoutNodeIndex holder, PrLayoutPosition position) {
    if (position == PrLayoutPosition_Running)
        return false;
    return position == PrLayoutPosition_Fixed ? layoutHoldsFixed(tree, holder) : layoutHoldsAbsolute(tree, holder);
}

// The position of the node's box.
inline PrLayoutPosition layoutNodePositionScheme(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutBoxStyle(tree->styles, layoutTreeNode(tree, node).style).position;
}

// Returns the box whose layout lays out the box out of flow `node` and holds its fragment. Sets `*containing` to the
// box whose edges it is placed against (css-position-3/#def-cb), which is the nearest ancestor that holds it. If that
// ancestor is a positioned inline box, the block container of the inline box lays the box out. Returns
// kPrLayoutNodeNone if no ancestor holds it, in which case the viewport holds it.
LayoutNodeIndex layoutNodeHolder(const LayoutTree* tree, LayoutNodeIndex node, LayoutNodeIndex* PR_NULLABLE containing = nullptr);

// ---------------------------------------------------------------------------------------------------------------------
// Positions of boxes

// The containers that a box can paint in, other than another box (layoutNodeContainer). kLayoutContainerViewport is the
// content of the viewport, which the viewport scrolls. kLayoutContainerOutside is no container, outside the viewport.
constexpr LayoutNodeIndex kLayoutContainerViewport = kPrLayoutNodeNone;
constexpr LayoutNodeIndex kLayoutContainerOutside = 0xfffffffeu;

// The bit that names the container of a sticky box (layoutNodeSticks) by the box's node. That container moves the box
// and what paints in it by the box's sticky offset, and clips nothing. A sticky box that clips its content is also the
// container of its content, named by its node alone, inside its sticky container. Node indices stay below the bit.
constexpr LayoutNodeIndex kLayoutContainerSticky = 0x80000000u;

// Returns true if `container` is the container of a sticky box.
inline bool layoutContainerIsSticky(LayoutNodeIndex container) {
    return container != kLayoutContainerOutside && (container & kLayoutContainerSticky) != 0;
}

// Returns the box of `container`, a container other than the viewport's content or none.
inline LayoutNodeIndex layoutContainerBox(LayoutNodeIndex container) {
    return container & ~kLayoutContainerSticky;
}

// Returns true if the box of `node` clips its content in an axis (layoutClips). Only the block box of an element can
// clip, and the root does not, because its overflow belongs to the viewport.
bool layoutNodeClips(const LayoutTree* tree, LayoutNodeIndex node);

// Returns true if the box of `node` is sticky positioned, so that it paints in a container of its own that moves it by
// its sticky offset (layout_sticky.h). The root does not stick.
bool layoutNodeSticks(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the container that the sticky box `box` would paint in if it did not stick: the container around its own.
LayoutNodeIndex layoutStickyOuterContainer(const LayoutTree* tree, LayoutNodeIndex box);

// Returns the innermost sticky inline box that moves `node` with it, or kPrLayoutNodeNone. Placements pass over inline
// boxes, so such a box is found in the tree: among the inline boxes between a node in flow and its block container, or
// as the containing block of a box out of flow.
LayoutNodeIndex layoutStickyInlineAround(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the position, in the laid-out content of the viewport, of the box whose fragment holds the fragments of
// `node`. That box is `node` itself, or its block container for an inline box or a text. The position is the sum of the
// placements from the box to the viewport (LayoutPlacement), before any transform or scroll.
PrLayoutVec layoutNodePosition(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the container that `node` paints in, which clips or moves it. This is the sticky container of a sticky box
// itself, then that of a sticky inline box around it (layoutStickyInlineAround), and otherwise the nearest box along
// the placements from the box whose fragment holds `node` that clips its content or sticks. If there is none, it is the
// content of the viewport, or kLayoutContainerOutside for a fixed box that the viewport holds. A box out of flow
// therefore escapes the boxes between its holder and its placeholder.
LayoutNodeIndex layoutNodeContainer(const LayoutTree* tree, LayoutNodeIndex node);

// layoutNodePositioned returns layoutNodePosition and layoutNodeContainer of `node` together, at the cost of one read
// of each memo. layoutBoxPositioned does the same for a box with a fragment of its own, and skips the test of what kind
// of box it is.
LayoutPositioned layoutNodePositioned(const LayoutTree* tree, LayoutNodeIndex node);
LayoutPositioned layoutBoxPositioned(const LayoutTree* tree, LayoutNodeIndex box);

// Returns the first box whose current memo of its position or container (LayoutPositioned) differs from the result of
// summing its placements again, or kPrLayoutNodeNone if there is none.
LayoutNodeIndex layoutPositionsVerify(const LayoutTree* tree);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
