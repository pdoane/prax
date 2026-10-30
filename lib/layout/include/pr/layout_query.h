// Geometry queries: the boxes of a tree's last layout, hit testing, and scroll state.
//
// Every query reads the tree as of its last prLayoutTreeLayout. The cost of a query for a node's boxes is proportional
// to the depth of the node, plus the number of parts of an inline box or line rectangles of a text.

#pragma once

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/geom.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

// The coordinates a geometry query answers in.
enum {
    // Viewport coordinates as painted: after the transforms of ancestor boxes and the scroll offsets of enclosing
    // scroll containers and the viewport.
    PrLayoutCoordinates_Viewport,

    // Initial containing block coordinates as laid out, before any transform or scroll.
    PrLayoutCoordinates_Layout,
};
typedef uint8_t PrLayoutCoordinates;

// Returns the border boxes of an element's boxes, or the line rectangles of a text node, from the last layout, in
// `coordinates`, in content order (cssom-view-1/#dom-element-getclientrects). An inline box's boxes are its parts on
// lines and its rows around the block-level boxes inside it, each row across its container's content box and as tall as
// the block's border box. A box under a rotation or skew yields its bounding rectangle, rounded outward to layout
// units. A box under a projection yields the bounding rectangle of the part of it that is drawn: the part nearer the
// eye than drawing's cut (kPrPathPolylineNearPlane) is cut away, and a box wholly behind the eye yields an empty
// rectangle at the origin. An element that generates no box, or an erased node, has none. Writes up to `capacity`
// rectangles to `out` and returns the total count.
uint32_t prLayoutNodeBoxes(PrLayoutTree* tree, PrLayoutNode node, PrLayoutCoordinates coordinates,
    PrLayoutRect* PR_NULLABLE out, uint32_t capacity);

// Returns the union of the node's boxes in `coordinates`, or an empty rectangle at the origin if it has none.
PrLayoutRect prLayoutNodeBounds(PrLayoutTree* tree, PrLayoutNode node, PrLayoutCoordinates coordinates);

// Returns the content box of a replaced element, which its content fills, from the top left of its border box, as the
// last layout gave it. Returns an empty rectangle at the origin for a node that is not a replaced element or has no
// box.
PrLayoutRect prLayoutNodeReplacedContent(PrLayoutTree* tree, PrLayoutNode node);

// Returns the part of the node's bounds in viewport coordinates that the last layout shows: cut, in the axes that each
// clips, by the padding box of every box that clips the node's boxes along their placements, and by the viewport. A box
// under a transform is cut by the bounds of the clip as it maps, so the part may be larger than what is drawn, never
// smaller. Returns an empty rectangle at the origin if nothing of the node shows. A widget that animates asks for
// frames only while it shows.
PrLayoutRect prLayoutNodeShownBounds(PrLayoutTree* tree, PrLayoutNode node);

// Maps `point`, in viewport coordinates as painted, into the coordinates of the node's first box (prLayoutNodeBoxes),
// with the box's top left at the origin, through the inverse of every transform and scroll offset between them. The
// result is where the point lands in the box's own plane, which a box under a projection needs for positions within it,
// such as a slider's value under the pointer. Returns false if the node has no box, or if the point does not reach the
// box's plane: beyond a projection's horizon, or under a transform that collapses.
bool prLayoutNodeLocalPoint(PrLayoutTree* tree, PrLayoutNode node, PrLayoutVec point, PrLayoutVec* out);

// Maps `count` points in place from the coordinates of the node's first box (prLayoutNodeBoxes), with the box's top
// left at the origin, to viewport coordinates as painted, through every transform and scroll offset between them, as
// the box is drawn. This is the inverse of prLayoutNodeLocalPoint. Points are in CSS pixels. Returns false if the node
// has no box, or if a projection puts a point behind the eye.
bool prLayoutNodeMapFromBox(PrLayoutTree* tree, PrLayoutNode node, PrDVec2* PR_NULLABLE points, uint32_t count);

// Returns the physical extent of the content box of an element's box from the last layout: its border box less the
// borders and the padding, whose percentages resolved against its containing block. Returns zero for an inline box,
// text, an element that generates no box, or an erased node, because no single box holds their content.
PrLayoutVec prLayoutNodeContentExtent(const PrLayoutTree* tree, PrLayoutNode node);

// Returns the inline size of the containing block of an element's box from the last layout, which the percentages of
// its margins and padding resolved against (css-box-4/#margin-physical, css-box-4/#padding-physical). Returns a
// negative size for an inline box, text, an element that generates no box, or an erased node.
PrLayoutUnit prLayoutNodeContainingInlineSize(const PrLayoutTree* tree, PrLayoutNode node);

// An axis of a grid (css-grid-1/#grid-concepts): its columns, along the grid container's inline axis, or its rows,
// along its block axis.
enum {
    PrLayoutGridAxis_Columns,
    PrLayoutGridAxis_Rows,
    PrLayoutGridAxis_Count,
};
typedef uint8_t PrLayoutGridAxis;

// Writes up to `capacity` of the used sizes of the tracks of the grid container `node` along `axis` to `out`, from the
// start of its implicit grid, as its last layout sized them, and returns the number of its tracks. A collapsed track's
// size is zero (css-grid-1/#collapsed-grid-track). Returns zero if `node` is not a grid container with a box.
uint32_t prLayoutNodeGridTrackSizes(const PrLayoutTree* tree, PrLayoutNode node, PrLayoutGridAxis axis,
    PrLayoutUnit* PR_NULLABLE out, uint32_t capacity);

// Writes up to `capacity` of the names that the template of the grid container `node` gives the lines along `axis` to
// `out`, and returns the number of names. Each name's line is counted from the start of the implicit grid of the
// container's last layout, whose automatic repetition is repeated as that layout repeated it. The names are in order of
// their lines, and those of one line in the order that the template gives them. Names that areas give their edges are
// not among them. Returns zero if `node` is not a grid container with a box.
uint32_t prLayoutNodeGridLineNames(const PrLayoutTree* tree, PrLayoutNode node, PrLayoutGridAxis axis,
    PrLayoutGridLineName* PR_NULLABLE out, uint32_t capacity);

// Returns the text that the tree interned for `name` (prLayoutGridNameIntern), valid for the tree's life.
PrStringView prLayoutGridNameText(const PrLayoutTree* tree, PrLayoutGridName name);

// Returns the used width of the border at `side` of a box styled as `node` is: its style's width snapped as a line
// width at the tree's target pixel ratio (css-values-4/#snap-as-a-line-width). Returns zero for text or an erased node.
PrLayoutUnit prLayoutNodeBorderWidth(const PrLayoutTree* tree, PrLayoutNode node, PrRectSide side);

// ---------------------------------------------------------------------------------------------------------------------
// Text positions
//
// A position in text (PrLayoutTextPosition) stands where the last layout gives it a place where some part of the text
// holds it: the edge of a cluster, the end of a line that a preserved segment break ends, or a point inside a cluster
// that stands for several characters. Such a cluster is a ligature, or a space that a run of white space collapsed to,
// and a position inside it stands at the fraction of its advance that the bytes before the position make of its bytes.
// A position in text that no line holds, such as white space that collapsed away at the start of a line, stands where
// the text nearest it in the order of the text starts or ends. In bidi text, a cluster's start in the order of the text
// is its right edge at an odd level, and two positions that a level boundary separates may stand far apart.

// Writes the caret before byte `offset` of the text node `text`, clamped to the text's size, from the last layout, in
// `coordinates`, to `out`. The caret has no extent along its line and spans the content area of the text across it, so
// it is a vertical segment on a horizontal line. Returns false if the last layout drew none of the node's text.
bool prLayoutNodeTextCaret(PrLayoutTree* tree, PrLayoutNode text, uint32_t offset, PrLayoutTextAffinity affinity,
    PrLayoutCoordinates coordinates, PrLayoutRect* out);

// Returns the rectangles that the bytes from `start` to `end` of the text node `text` cover in the last layout, in
// `coordinates`, in content order: one for each part of the text on a line in one direction that holds some of them,
// across the content area of the text (cssom-view-1/#dom-range-getclientrects). Writes up to `capacity` rectangles to
// `out` and returns the total count.
uint32_t prLayoutNodeTextRangeBoxes(PrLayoutTree* tree, PrLayoutNode text, uint32_t start, uint32_t end,
    PrLayoutCoordinates coordinates, PrLayoutRect* PR_NULLABLE out, uint32_t capacity);

// Writes the position in the text of `root` or its descendants, in the last layout, nearest `point` in viewport
// coordinates as painted, to `out` (cssom-view-1/#dom-document-caretpositionfrompoint). The nearest line is the one
// nearest the point across lines, and the nearest position on it the nearest along it. Returns false if the last layout
// drew none of their text, or if the point does not reach the plane of any line that holds it.
bool prLayoutTextPositionFromPoint(PrLayoutTree* tree, PrLayoutNode root, PrLayoutVec point, PrLayoutTextPosition* out);

// ---------------------------------------------------------------------------------------------------------------------
// Containing blocks

// Returns true if the box of `node` is the containing block of the absolutely positioned boxes in it
// (prLayoutNodeHoldsAbsolute), or of the fixed boxes in it (prLayoutNodeHoldsFixed), as its record says
// (css-position-3/#def-cb). A box holds absolutely positioned boxes if it is positioned or a transform applies to it,
// and holds fixed boxes if a transform applies to it. A transform applies to any box except an inline box
// (css-transforms-1/#transformable-element). Text and line breaks hold none.
bool prLayoutNodeHoldsAbsolute(const PrLayoutTree* tree, PrLayoutNode node);
bool prLayoutNodeHoldsFixed(const PrLayoutTree* tree, PrLayoutNode node);

// ---------------------------------------------------------------------------------------------------------------------
// Hit testing

// Hit tests `point` in viewport coordinates, as painted. Returns the nodes whose boxes contain the point, topmost
// first: a text node by its line rectangles, an element by its border box. Boxes clipped at the point are excluded. A
// box under a transform contains the point if the transform's inverse maps the point into it, so a box under a
// projection is hit where it is drawn, and a point that maps behind the eye hits nothing in that box's plane. A box
// that hides its back and shows it is not hit, and neither is a box that is not visible or whose pointer events are
// none. Each node appears once, at its topmost box. Writes up to `capacity` nodes to `out` and returns the total count.
uint32_t prLayoutTreeHitTest(PrLayoutTree* tree, PrLayoutVec point, PrLayoutNode* PR_NULLABLE out, uint32_t capacity);

// Hit tests `point` as prLayoutTreeHitTest does, in a root other than the viewport's (PrLayoutRoot), such as the
// content of an SVG foreignObject that a replaced element's painter draws (prLayoutNodeDraw). `point` is in the root's
// coordinates, from the top left of its containing block. Returns zero if the last layout did not lay out `root`.
uint32_t prLayoutNodeHitTest(PrLayoutTree* tree, PrLayoutNode root, PrLayoutVec point,
    PrLayoutNode* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Scroll state

// Axes of a scroll container: those along which a user can scroll it, or along which a scroll that it cannot take
// chains to the containers around it.
enum {
    PrLayoutScrollFlag_X = 1 << 0,
    PrLayoutScrollFlag_Y = 1 << 1,
};
typedef uint8_t PrLayoutScrollFlags;

// The scroll state of a block box or the viewport, as of the last layout (css-overflow-3/#scroll-container). An offset
// is the scrollport's displacement from its unscrolled position, the scroll origin, which is the corner of the padding
// box that is block-start and inline-start in the box's writing mode. The offset is negative toward an origin at the
// right or bottom edge (cssom-view-1/#scroll-an-element). A box that is not a scroll container has an empty range.
typedef struct PrLayoutScrollInfo {
    PrLayoutVec offset;             // the scroll offset, within the range
    PrLayoutRect range;             // smallest scroll offsets as left and top, greatest as right and bottom
    PrLayoutVec portExtent;         // the scrollport: the padding box, or the viewport's extent
    PrLayoutRect content;           // the scrolling area, from the unscrolled scrollport's top left, holding it
    PrLayoutScrollFlags userAxes;   // the axes a user can scroll

    // The axes along which a user's scroll that the container cannot take passes on to the containers around it
    // (css-overscroll-1/#scroll-chaining). A scroll passes along an axis that the user cannot scroll.
    PrLayoutScrollFlags chainAxes;

    bool scrollContainer;           // true for a scroll container; always true for the viewport, never for the root
} PrLayoutScrollInfo;

// Writes the scroll state of the element, or of the viewport for kPrLayoutNodeNone, to `out`. Returns false and writes
// nothing if the element generates no block box.
bool prLayoutNodeScrollInfo(PrLayoutTree* tree, PrLayoutNode element, PrLayoutScrollInfo* out);

// Returns the scroll container that scrolls the box of `node`, as of the last layout: the nearest scroll container that
// contains it, following a box out of flow to its containing block (css-position-3/#def-cb). Returns kPrLayoutNodeNone
// for the viewport.
PrLayoutNode prLayoutNodeScrollContainer(const PrLayoutTree* tree, PrLayoutNode node);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
