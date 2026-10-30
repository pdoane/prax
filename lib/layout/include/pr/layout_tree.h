// Layout trees: nodes styled by records, laid out into fragments and painted into draw lists.
//
// A tree is plain data. A node is an element, a text node, a replaced element or a line break. An element generates
// boxes according to its record's display value. A text node is styled by its parent's record. A replaced element
// (CSS2/#replaced-element) is a childless box whose content the client supplies as data, sized from that content's
// natural dimensions. Nodes hold no pointers to client objects, so a client can build, compare and modify trees as
// plain data. A client maps its own objects to nodes with keys (prLayoutNodeSetKey).
//
// Layout runs on the roots that it is given: nodes with no parent, each in a containing block of its own. The first is
// the viewport's, which the tree's scenes show. Painting (pr/layout_scene.h) and geometry queries (pr/layout_query.h)
// read the result of the last prLayoutTreeLayout, so a layout must follow every change before painting or querying.
// Geometry is in layout units (PrLayoutUnit), relative to the top left of the containing block of the node's root: the
// initial containing block for the viewport's root.

#pragma once

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/geom.h"
#include "pr/layout_style.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Trees

// What a tree lays text out with, and the target pixel ratio that it lays out and paints for. The tree holds a
// reference to `font`. It shapes with `shaper`, which outlives it, on the thread that uses it.
//
// A tree that lays out no text needs neither. A font group with no text font, of its own or from `font`, has no faces.
// Its size is 0, and its struts and baselines are at its line's edge. Text needs `shaper`, and a text font for each
// font group that sets it.
typedef struct PrLayoutTreeDesc {
    PrTextFont* PR_NULLABLE font;       // the text font of a font group that names none
    PrTextShaper* PR_NULLABLE shaper;
    float targetPixelRatio;             // target pixels per px, which font metrics snap to; finite and above zero
} PrLayoutTreeDesc;

// Creates an empty tree whose tables of records and groups hold only their defaults. One thread uses a tree at a time.
PrLayoutTree* prLayoutTreeCreate(const PrLayoutTreeDesc* desc);

// Destroys the tree, its nodes, and what it interned.
void prLayoutTreeDestroy(PrLayoutTree* PR_NULLABLE tree);

// Sets or returns the target pixels per px that the tree's font metrics and leading snap to, finite and above zero.
// Setting a new ratio lays the tree out again at its next layout.
void prLayoutTreeSetTargetPixelRatio(PrLayoutTree* tree, float ratio);
float prLayoutTreeTargetPixelRatio(const PrLayoutTree* tree);

// Sets or returns the color of the canvas beneath the viewport's root, straight alpha, which is transparent until it is
// set. The canvas shows it wherever the root's background does not cover it, as a document's canvas shows the Canvas
// system color (css-backgrounds-3/#special-backgrounds). A scene reports the color that fills its canvas
// (prLayoutSceneCanvasColor).
void prLayoutTreeSetCanvasColor(PrLayoutTree* tree, PrColor16 color);
PrColor16 prLayoutTreeCanvasColor(const PrLayoutTree* tree);

// Sets or returns whether a user's wheel anywhere over the viewport waits for the tree's document, which may cancel it,
// as a wheel over a box of PrLayoutWheelEvents_Blocking does. It is passive until it is set. A scene holds the value.
void prLayoutTreeSetViewportWheelEvents(PrLayoutTree* tree, PrLayoutWheelEvents events);
PrLayoutWheelEvents prLayoutTreeViewportWheelEvents(const PrLayoutTree* tree);

// Returns the text font of a font group that names none (PrLayoutTreeDesc::font), or null if the tree has none.
PrTextFont* PR_NULLABLE prLayoutTreeDefaultFont(const PrLayoutTree* tree);

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

// A handle to a node of a tree. Handles are never reused: after the node is erased, its handle refers to no node. This
// makes it safe to hold handles across erases, as a compositor does for the nodes in its committed scenes.
typedef uint64_t PrLayoutNode;

// The null node handle.
enum { kPrLayoutNodeNone = 0 };

// The kind of a node. Its flags say whether the node is a box that its record styles, rather than content of its
// parent's lines, and whether it is an element, whose children are laid out in it.
enum {
    PrLayoutNodeKindFlag_Box = 1u << 2,
    PrLayoutNodeKindFlag_Element = 1u << 3,

    PrLayoutNodeKind_Text = 0,
    PrLayoutNodeKind_LineBreak = 1,     // a forced line break, as HTML renders br
    PrLayoutNodeKind_Replaced = PrLayoutNodeKindFlag_Box,
    PrLayoutNodeKind_Element = PrLayoutNodeKindFlag_Box | PrLayoutNodeKindFlag_Element,

    // An element that HTML lays out as a widget (html/#button-layout). In block flow at a justify-self of normal, its
    // auto inline size is its fit-content size rather than the stretch-fit size. Everywhere else it is laid out as an
    // element.
    PrLayoutNodeKind_SemiReplaced = PrLayoutNodeKind_Element | 1,
};
typedef uint8_t PrLayoutNodeKind;

// Creates a node as the last child of element `parent`, or detached if `parent` is kPrLayoutNodeNone. The node is an
// element styled by record `style`, a text node of `text`, or a replaced element styled by `style` with no natural
// dimensions and no content. Creating a tree in order, each node under its parent, is cheaper than inserting each node
// afterward.
PrLayoutNode prLayoutNodeCreateElement(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutStyleIndex style);
PrLayoutNode prLayoutNodeCreateText(PrLayoutTree* tree, PrLayoutNode parent, PrStringView text);
PrLayoutNode prLayoutNodeCreateReplaced(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutStyleIndex style);

// Creates a semi-replaced element in the same way as prLayoutNodeCreateElement creates an element.
PrLayoutNode prLayoutNodeCreateSemiReplaced(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutStyleIndex style);

// Creates a line break in the same way as prLayoutNodeCreateElement creates an element. A line break has no children
// and forces a line break in its inline formatting context, whatever its white space. Only its `clear` and its display
// are read from its record. Its line is set in its parent's font, like the text around it.
PrLayoutNode prLayoutNodeCreateLineBreak(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutStyleIndex style);

// Removes the node from its parent and destroys it and its descendants.
void prLayoutTreeErase(PrLayoutTree* tree, PrLayoutNode node);

// Returns true if `node` is a node the tree created and has not erased.
bool prLayoutNodeIsValid(const PrLayoutTree* tree, PrLayoutNode node);

// Returns the kind of a node.
PrLayoutNodeKind prLayoutNodeKind(const PrLayoutTree* tree, PrLayoutNode node);

// Returns the viewport's root in the last layout (prLayoutTreeLayout), or kPrLayoutNodeNone.
PrLayoutNode prLayoutTreeRoot(const PrLayoutTree* tree);

// Inserts detached `node` as a child of element `parent`, before child `before`, or last if `before` is
// kPrLayoutNodeNone. The node is not a root of the last layout.
void prLayoutTreeInsert(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutNode node, PrLayoutNode before);

// Detaches the node from its parent. The node and its descendants remain valid and can be inserted again.
void prLayoutTreeRemove(PrLayoutTree* tree, PrLayoutNode node);

// Tree navigation. Each returns kPrLayoutNodeNone if there is no such node.
PrLayoutNode prLayoutNodeParent(const PrLayoutTree* tree, PrLayoutNode node);
PrLayoutNode prLayoutNodeFirstChild(const PrLayoutTree* tree, PrLayoutNode node);
PrLayoutNode prLayoutNodeLastChild(const PrLayoutTree* tree, PrLayoutNode node);
PrLayoutNode prLayoutNodePreviousSibling(const PrLayoutTree* tree, PrLayoutNode node);
PrLayoutNode prLayoutNodeNextSibling(const PrLayoutTree* tree, PrLayoutNode node);

// ---------------------------------------------------------------------------------------------------------------------
// Styles and text

// Sets the record of a node, or the text of a text node. A text node's record is the one its text is set and painted
// in. It may be kPrLayoutStyleNone, meaning its parent's record, which the text node uses until one is set. A producer
// gives this to text whose properties inherit from an element that generates no box, such as an element with display
// contents (css-display-4/#valdef-display-contents). Every other node has a record.
void prLayoutNodeSetStyle(PrLayoutTree* tree, PrLayoutNode node, PrLayoutStyleIndex style);
void prLayoutNodeSetText(PrLayoutTree* tree, PrLayoutNode text, PrStringView value);

// Sets the record that styles `element` on the first formatted line of its block container
// (css-pseudo-4/#first-line-styling), or kPrLayoutStyleNone for none. The text directly in the element is set and
// painted in that record on that line. A block container's record for the line styles its root inline box there: the
// text directly in the container and the line's strut. The producer gives each element on the line a record that
// inherits from its parent's record for the line. An element without one is styled by its own record there.
void prLayoutNodeSetFirstLineStyle(PrLayoutTree* tree, PrLayoutNode element, PrLayoutStyleIndex style);

// Returns the record of a node as set, or kPrLayoutStyleNone for a text node that takes its parent's; or the text of a
// text node, which is valid until any text in the tree changes.
PrLayoutStyleIndex prLayoutNodeStyle(const PrLayoutTree* tree, PrLayoutNode node);
PrStringView prLayoutNodeText(const PrLayoutTree* tree, PrLayoutNode text);

// ---------------------------------------------------------------------------------------------------------------------
// Highlights and the caret
//
// A position in text is a caret before a byte of a text node's text, at an offset from 0 to the text's size. The
// queries of pr/layout_query.h say where the last layout puts a position.

// A position in the text of the text node `node`, as a caret before byte `offset`.
typedef struct PrLayoutTextPosition {
    PrLayoutNode node;
    uint32_t offset;
} PrLayoutTextPosition;

// Which of two places a position stands at where it ends one part of the text and starts another, as at a soft wrap or
// a level boundary: the start of the part after it in the order of the text, or the end of the part before it.
enum {
    PrLayoutTextAffinity_Downstream,
    PrLayoutTextAffinity_Upstream,
};
typedef uint8_t PrLayoutTextAffinity;

// A highlight of a range of a text node's text, such as a selection (css-pseudo-4/#highlight-pseudos): the bytes from
// `start` to `end`, whose boxes (prLayoutNodeTextRangeBoxes) are filled with `background` behind the text, and whose
// glyphs are painted in `color`. A color with zero alpha keeps the text's own color.
typedef struct PrLayoutHighlight {
    uint32_t start;
    uint32_t end;
    PrColor16 color;
    PrColor16 background;
} PrLayoutHighlight;

// Sets the highlights of a text node, copied from `highlights`, which paint in order, each over those before it. A
// count of zero removes them. The part of a glyph that a highlight covers is painted in the highlight's color, so a
// highlight may divide a glyph.
void prLayoutNodeSetHighlights(PrLayoutTree* tree, PrLayoutNode text, const PrLayoutHighlight* PR_NULLABLE highlights,
    uint32_t count);

// Returns the number of highlights of a text node, and writes up to `capacity` of them to `out`.
uint32_t prLayoutNodeHighlights(const PrLayoutTree* tree, PrLayoutNode text, PrLayoutHighlight* PR_NULLABLE out,
    uint32_t capacity);

// The caret that a tree paints (css-ui-4/#caret): a bar 1 px wide at `position`, which `affinity` places where the
// position stands at two places, in `color`, across the content area of the text there. The caret paints over its text
// and the text's highlights.
typedef struct PrLayoutCaret {
    PrLayoutTextPosition position;
    PrLayoutTextAffinity affinity;
    PrColor16 color;
} PrLayoutCaret;

// Sets the caret that the tree paints, or removes it if `caret` is null. A caret whose node is erased paints nowhere.
void prLayoutTreeSetCaret(PrLayoutTree* tree, const PrLayoutCaret* PR_NULLABLE caret);

// Writes the tree's caret to `out`. Returns false, and writes nothing, if the tree has none.
bool prLayoutTreeCaret(const PrLayoutTree* tree, PrLayoutCaret* out);

// ---------------------------------------------------------------------------------------------------------------------
// Keys

// Sets or returns the client's key for a node. The tree stores keys but never interprets them. Zero, the initial value,
// means no key; a nonzero key must be unique within the tree.
void prLayoutNodeSetKey(PrLayoutTree* tree, PrLayoutNode node, uint64_t key);
uint64_t prLayoutNodeKey(const PrLayoutTree* tree, PrLayoutNode node);

// Returns the node with key `key`, or kPrLayoutNodeNone if there is none.
PrLayoutNode prLayoutNodeFromKey(const PrLayoutTree* tree, uint64_t key);

// ---------------------------------------------------------------------------------------------------------------------
// Replaced elements

// The type of a replaced element's content.
enum {
    PrLayoutContentKind_None,       // nothing is drawn
    PrLayoutContentKind_DrawList,   // a draw list
    PrLayoutContentKind_Painter,    // a painter, drawn at the size that fitting the natural size gives it
    PrLayoutContentKind_Embed,      // a slot that the scene's player fills (PrLayoutScenePlayOptions)
    PrLayoutContentKind_Image,      // an image, stretched over the natural size
};
typedef uint8_t PrLayoutContentKind;

// A replaced element's content. A draw list is in the content's natural coordinate space, with its origin at the top
// left and its size the natural size; an axis with no natural size uses the content box's size. An image covers that
// space. Painting maps the space onto the content box according to object-fit and object-position, and clips to the
// content box. A painter draws at the size that the mapping gives the space, rather than being scaled to it, and the
// tree caches its drawing until that size changes or the client calls prLayoutNodeInvalidateContent.
typedef struct PrLayoutContent {
    PrLayoutContentKind kind;
    union {
        PrDrawList* list;           // DrawList: the tree freezes it and retains a reference
        PrLayoutPainter painter;    // Painter: the tree retains a reference if the painter shares its context
        uint64_t embed;             // Embed: a nonzero id the player uses to look up what fills the slot
        const PrDrawImage* image;   // Image: the tree retains a reference
    };

    // Image: the brightest color that the image paints, as a multiple of SDR white, or zero for no limit
    // (PrDrawImageFill::limit).
    float limit;

    PrLayoutImageRendering rendering;   // Image

    // A number that the client changes whenever the content draws something else while its other members stay the same,
    // as a painter's drawing does. Content set again at another revision is drawn again.
    uint64_t revision;
} PrLayoutContent;

// Sets a replaced element's natural dimensions or its content. Natural dimensions invalidate layout; content
// invalidates only painting.
void prLayoutNodeSetNaturalSize(PrLayoutTree* tree, PrLayoutNode replaced, PrLayoutNaturalSize size);
void prLayoutNodeSetContent(PrLayoutTree* tree, PrLayoutNode replaced, const PrLayoutContent* content);

// Returns a replaced element's natural dimensions or its content.
PrLayoutNaturalSize prLayoutNodeNaturalSize(const PrLayoutTree* tree, PrLayoutNode replaced);
PrLayoutContent prLayoutNodeContent(const PrLayoutTree* tree, PrLayoutNode replaced);

// Invalidates the cached drawing of a replaced element's painter, so the next paint calls it again. A client calls this
// when state the painter reads has changed.
void prLayoutNodeInvalidateContent(PrLayoutTree* tree, PrLayoutNode replaced);

// ---------------------------------------------------------------------------------------------------------------------
// Scrolling

// Sets or returns the scroll offset of an element's scroll container, or of the viewport for kPrLayoutNodeNone. The
// viewport scrolls the document and uses the root's overflow value; the root itself does not clip
// (css-overflow-3/#overflow-propagation). The tree stores the offset unclamped. Painting and queries clamp it to the
// scroll range of the last layout. Setting an offset drops the container's scroll anchor (prLayoutTreeLayout), so the
// next layout chooses another from where the container then shows.
void prLayoutNodeSetScroll(PrLayoutTree* tree, PrLayoutNode element, PrLayoutVec offset);
PrLayoutVec prLayoutNodeScroll(const PrLayoutTree* tree, PrLayoutNode element);

// A move of a scroll container's offset that scroll anchoring made: the element's, or the viewport's for
// kPrLayoutNodeNone.
typedef struct PrLayoutScrollAdjustment {
    PrLayoutNode node;
    PrLayoutVec delta;
} PrLayoutScrollAdjustment;

// Writes to `out` up to `capacity` of the moves that scroll anchoring made since the last call, one for each container,
// and returns how many it wrote. A client calls it until it writes none. A document fires its scroll events for them.
uint32_t prLayoutTreeTakeScrollAdjustments(PrLayoutTree* tree, PrLayoutScrollAdjustment* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Layout

// A root that a layout lays out: a box with no parent, an element or a replaced element such as the root svg element of
// an SVG document, and the extent of its containing block.
typedef struct PrLayoutRoot {
    PrLayoutNode node;
    PrLayoutVec extent;
} PrLayoutRoot;

// Lays out `count` roots, at least one, committing an open frame first (pr/layout_frame.h).
//
// The first root is the viewport's, in a viewport of its extent, which is the initial containing block
// (css-display-4/#initial-containing-block). Its node may be kPrLayoutNodeNone for a viewport with no content. Its
// background is the canvas's, the viewport scrolls it and takes its overflow, and fixed boxes are placed in the
// viewport. A scene shows it.
//
// Each other root is laid out in a containing block of its extent, which holds every box out of flow under it, as an
// embedded document's root is: by a replaced element's painter (prLayoutNodeDraw), which draws it in its content.
// Viewport units in it are still the viewport's.
//
// Layout is incremental: a block box reuses its cached result, subtree included, when nothing in the subtree changed
// and its constraint space is the same. A change confined to a record's paint groups invalidates no layout.
//
// A layout keeps what each scrolled container shows in place as content moves (css-scroll-anchoring-1). Before it lays
// out, it chooses an anchor for each container whose offset is away from its origin in its block axis and has none,
// from the last layout's boxes: the deepest node near the start of what the container shows. After it lays out, it
// moves the container's offset by as far as the anchor moved in that axis, unless a change that suppresses anchoring
// reached the anchor in a layout in the same viewport extent (css-scroll-anchoring-1/#suppression-triggers). A layout
// that has nothing to lay out anchors nothing.
void prLayoutTreeLayout(PrLayoutTree* tree, const PrLayoutRoot* roots, uint32_t count);

// Work counts for the last layout and paint, for tests, benchmarks and debugging. The counters name the work that the
// implementation does, and change when it does.
typedef struct PrLayoutTreeStats {
    uint32_t blocksLaidOut;         // block boxes laid out
    uint32_t blocksReused;          // block boxes whose cached result was reused, subtree included
    uint32_t blocksRestarted;       // block boxes laid out again to place floats where the margins before them resolve
    uint32_t linesLaidOut;          // lines laid out in inline formatting contexts
    uint32_t linesBroken;           // lines broken: one for each line laid out, and one for each retry of a line
    uint32_t linesReused;           // lines taken from the last layout of their context at another block size
    uint32_t inlinesShaped;         // inline formatting contexts whose text was shaped rather than taken from cache
    uint32_t inlinesLeveled;        // of those, the contexts whose bidi levels were resolved, which may reorder
    uint32_t intrinsicsComputed;    // boxes whose intrinsic sizes were computed
    uint32_t intrinsicsReused;      // boxes whose cached intrinsic sizes were reused
    uint32_t measuresComputed;      // measurements of flex items and table cells computed rather than cached
    uint32_t measuresReused;        // measurements taken from cache
    uint32_t measuresReplaced;      // measurements kept in place of an older one of the same box, which is lost
    uint32_t handoffsTaken;         // final layouts that took the layout a measurement made
    uint32_t flexSolved;            // flex containers solved for a measurement, without their items' final layouts
    uint32_t flexLaidOut;           // flex containers laid out
    uint32_t flexItemsFlexed;       // items visited by the rounds of resolving flexible lengths, one per item a round
    uint32_t tablesLaidOut;         // tables whose grids were laid out, not counting their cells' layouts
    uint32_t gridsSolved;           // grid containers solved for a measurement, without their items' final layouts
    uint32_t gridsLaidOut;          // grid containers laid out
    uint32_t boxFragmentsMade;      // fragments made of block containers, flex containers, anonymous and replaced boxes
    uint32_t childrenPublished;     // placements recorded when the layout published its final tree

    // A frame's commit (pr/layout_frame.h): the number of created nodes that continued a committed node and of those
    // created new; of the nodes continued, the number that the frame found away from the next committed node in
    // preorder; the number of committed nodes that no creation continued, erased with their subtrees; and the number of
    // committed parents whose children it relinked.
    uint32_t frameNodesKept;
    uint32_t frameNodesMade;
    uint32_t frameNodesJumped;
    uint32_t frameNodesGone;
    uint32_t frameParentsRelinked;

    uint32_t layersRecorded;        // layer recordings recorded
    uint32_t layersReused;          // layer recordings reused from cache
    uint32_t subtreesRecorded;      // block subtree recordings recorded and cached for later paints
    uint32_t subtreesReused;        // block subtree recordings reused from cache
    uint32_t commandsRecorded;      // draw commands recorded rather than reused

    // Text, for judging how text divides into placed pieces. Leaves are the parts of a text item on a line that are
    // placed separately, and are temporary. Text fragments are what placing leaves creates, and what painting records.
    uint32_t textLeavesPlaced;      // text leaves placed on lines laid out
    uint32_t textFragmentsMade;     // text fragments made, and the bytes of their allocations
    uint64_t textFragmentBytes;
    uint32_t lineChildrenMade;      // children of the line fragments made: text, inline box parts and atomic inlines
    uint32_t textFragmentsRecorded; // text fragments recorded, and the glyph commands they recorded, of the commands
    uint32_t glyphCommandsRecorded;

    // Boxes and text nodes recorded again in a paint that followed no layout change, although nothing they or their
    // descendants paint had changed: work that a finer cache would avoid.
    uint32_t boxesRepaintedUnchanged;

    // The number of stacking contexts whose lists, of the boxes that paint whole in them, painting and hit tests found
    // from the box tree rather than the cache since the last layout or paint began.
    uint32_t stackingListsFound;
} PrLayoutTreeStats;

// Returns the work counts of the last layout and paint. Each layout resets the counts.
PrLayoutTreeStats prLayoutTreeStats(const PrLayoutTree* tree);

// The first difference prLayoutTreeVerify found.
typedef struct PrLayoutTreeDifference {
    PrLayoutNode node;              // the node it concerns, or kPrLayoutNodeNone for the tree as a whole
    PrString what;                  // a description; the caller releases it with prStringReset
} PrLayoutTreeDifference;

// Returns true if the tree's incremental work was correct and bounded. Correct: the cached layout matches a full layout
// with no reuse, each cached recording matches a fresh recording, and geometry queries match placing every box.
// Bounded: the last layout did no work unless an input that layout reads had changed, and the last paint recorded
// nothing unless an input that recordings read had changed. On failure, writes the first difference to `out` if it is
// not null. Expensive; for tests and for debugging invalidation.
bool prLayoutTreeVerify(PrLayoutTree* tree, PrLayoutTreeDifference* PR_NULLABLE out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
