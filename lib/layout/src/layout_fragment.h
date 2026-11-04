// Fragments: the output of layout for boxes, immutable once made.
//
// A fragment is one allocation that holds its children or its glyphs, and is shared by reference count. A layout that
// reuses the result of a subtree holds the fragments of the previous layout. A reader of the previous layout's
// fragments keeps them while the next layout is made. Geometry is physical and in layout units, and each child is
// offset from the top left of its parent.

#pragma once

#include "pr/atomic.h"
#include "pr/draw.h"
#include "pr/font.h"
#include "pr/geom.h"
#include "pr/layout_query.h"
#include "pr/raw_array.h"
#include "pr/search.h"
#include "pr/vec.h"

#include "layout/src/layout_index.h"
#include "layout/src/layout_logical.h"
#include "layout/src/layout_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutFragment;

// ---------------------------------------------------------------------------------------------------------------------
// Fragments

// The kind of a fragment.
enum {
    LayoutFragmentKind_Box,         // an element's block box, or an anonymous block box with no node
    LayoutFragmentKind_Line,        // a line box, whose children are the content of the line in tree order, each at its
                                    // visual place: the parts of an inline box, then its content, with each box in it
                                    // treated the same way (css-position-4/#paint-a-box-in-a-line-box)
    LayoutFragmentKind_Inline,      // the part of an inline box on one line, with no children
    LayoutFragmentKind_Text,        // the glyphs of one text node on a line, in one direction and orientation
    LayoutFragmentKind_Replaced,    // a replaced element's box, with no children

    // The row of an inline box around a block-level box inside it. The row spans the content box of the container, is
    // as tall as the border box of the block, and paints nothing. Its one child is the row of the next inline box
    // inward, or the block itself.
    LayoutFragmentKind_Around,

    // The place that a box out of flow would have had in the flow of its parent, at its static position, with no extent
    // and no children (css-position-3/#staticpos-rect). It names the box, whose fragment is held by the fragment of its
    // holder.
    LayoutFragmentKind_Placeholder,

    // The place of a float in tree order on the line that its inline content puts it on, with no extent and no
    // children. It names the float, whose fragment is held as a child by the parent of the line (LayoutFragmentAnchor).
    LayoutFragmentKind_FloatAnchor,
};
typedef uint8_t LayoutFragmentKind;

// How the glyphs of a Text fragment are turned on its line (css-writing-modes-4/#text-orientation). Upright glyphs are
// placed as drawn, in the fragment's own space. Turned glyphs are placed as on a horizontal line: from the line-left,
// line-over corner of the fragment, along the line and down from its line-over edge. Painting then turns them a quarter
// turn together with that frame. A clockwise turn puts the line-over edge on the right, and a counter-clockwise turn
// puts it on the left.
enum {
    LayoutTextTurn_None,
    LayoutTextTurn_Clockwise,
    LayoutTextTurn_CounterClockwise,
};
typedef uint8_t LayoutTextTurn;

// Physical sides of a box, as flags by PrRectSide.
enum {
    LayoutSideFlag_Top = 1 << PrRectSide_Top,
    LayoutSideFlag_Right = 1 << PrRectSide_Right,
    LayoutSideFlag_Bottom = 1 << PrRectSide_Bottom,
    LayoutSideFlag_Left = 1 << PrRectSide_Left,
};
typedef uint8_t LayoutSideFlags;

// Properties of a fragment beyond its kind.
enum {
    LayoutFragmentFlag_Float = 1 << 0,          // Box, Replaced: the box of a float
    LayoutFragmentFlag_OutOfFlow = 1 << 1,      // Box, Replaced: a box out of flow, held by its holder's fragment
    LayoutFragmentFlag_Holds = 1 << 2,          // Box: some of its children are boxes out of flow, which it holds

    LayoutFragmentFlag_Placeholders = 1 << 3,   // it is a placeholder, or its subtree holds one, counting the children
                                                // of every fragment except boxes out of flow
    LayoutFragmentFlag_Pending = 1 << 4,        // Box, Line, Around: it lists boxes out of flow (layoutFragmentPending)

    // Box: some of its children are lines, and each float among its children is anchored on one of them.
    LayoutFragmentFlag_Lines = 1 << 5,

    // Box: a table. Its children are its captions and the parts of its grid. After its children and its list, its
    // allocation holds its grid's rectangle, which its decoration paints over, and its grid's collapsed borders
    // (layoutFragmentTableGrid).
    LayoutFragmentFlag_Table = 1 << 6,

    // Text, Inline: the fragment is on the first formatted line of its container, so its node paints in its record for
    // that line (layoutNodeLineStyle).
    LayoutFragmentFlag_FirstLine = 1 << 7,

    // Box: a grid container. After its children and its list, its allocation holds the used sizes of its grid's tracks
    // (layoutFragmentGridTracks).
    LayoutFragmentFlag_Grid = 1 << 8,

    LayoutFragmentFlag_Reversed = 1 << 9,       // Text: at an odd bidi level, so the order of its text runs leftward
    LayoutFragmentFlag_Break = 1 << 10,         // Text: the forced break that ends its line, with no glyphs

    // Box, Around: a piece of a box that a fragmentation break crossed, after its first piece (layout_fragmentation.h).
    // Placements pass through it as through an anonymous box, so that a box's placement is its first piece.
    LayoutFragmentFlag_Continuation = 1 << 11,

    LayoutFragmentFlag_Column = 1 << 12,        // Box: a column of a multi-column container, with no node
    LayoutFragmentFlag_Page = 1 << 13,          // Box: a page of a paginated tree, with no node

    // Text: white space that hangs at the end of its line (css-text-3/#hanging). It reaches no scrollable overflow,
    // because a hanging glyph is not part of the line's content (css-text-3/#white-space-phase-2).
    LayoutFragmentFlag_Hangs = 1 << 14,
};
typedef uint16_t LayoutFragmentFlags;

// A box's first or last baseline (css-align-3/#baseline-set).
enum {
    LayoutBaselineEdge_First,
    LayoutBaselineEdge_Last,
    LayoutBaselineEdge_Count,
};
typedef uint8_t LayoutBaselineEdge;

// Sources of the set of other baselines for a baseline, besides a font group of the tree, whose baselines are relative
// to its alphabetic baseline. kLayoutBaselineSynthesized is the margin box of the box
// (css-inline-3/#baseline-synthesis-box). kLayoutBaselineMarginEdge is the margin edge of a scroll container, for which
// every baseline of the set is the alphabetic one.
constexpr uint32_t kLayoutBaselineSynthesized = 0xffffffffu;
constexpr uint32_t kLayoutBaselineMarginEdge = 0xfffffffeu;

// The ranges that scroll containers clamped the exported baselines of a box to (css-align-3/#baseline-export), and the
// position of each baseline before clamping. Indexed by LayoutBaselineEdge, and measured from the block-start edge of
// the border box. Successive clamps compose into one range.
struct LayoutBaselineClamps {
    PrLayoutUnit unclamped[LayoutBaselineEdge_Count];
    PrLayoutUnit lo[LayoutBaselineEdge_Count];
    PrLayoutUnit hi[LayoutBaselineEdge_Count];
    bool clamped[LayoutBaselineEdge_Count];
};

// Marks an edge at which a box fragment has no baseline (LayoutFragmentBox).
constexpr PrLayoutUnit kLayoutBaselineNone = PrLayoutUnit{INT32_MIN};

// A baseline that a box exports, measured from the block-start edge of its border box. Holds its alphabetic position
// and the source of its set. If a scroll container clamped it, also holds its position before clamping and the range it
// was clamped to. Each other baseline of the set lies at its offset in the font of the source from the unclamped
// position, toward the line-under side, clamped to the same range.
struct LayoutBaseline {
    PrLayoutUnit offset;
    PrLayoutUnit unclamped;
    PrLayoutUnit lo;
    PrLayoutUnit hi;
    uint32_t source;                // a font group, kLayoutBaselineMarginEdge or kLayoutBaselineSynthesized
    bool present;
    bool clamped;
};

// A child of a fragment, and the position of its top left in its parent.
struct LayoutChild {
    const LayoutFragment* fragment;
    PrLayoutVec offset;
};

// What a static position aligns of a box out of flow in each physical axis (css-position-3/#resolving-inset-auto):
// either an edge of its margin box or its center is placed at the position. A block or inline formatting context gives
// the inline-start and block-start edges of its own writing mode, whatever the writing mode of the containing block. A
// flex container aligns the box as its sole item.
enum {
    LayoutStaticEdge_Right = 1 << 0,    // the right edge, else the left
    LayoutStaticEdge_Bottom = 1 << 1,   // the bottom edge, else the top
    LayoutStaticEdge_CenterX = 1 << 2,  // the center in the horizontal axis, in place of an edge
    LayoutStaticEdge_CenterY = 1 << 3,  // the center in the vertical axis, in place of an edge
    LayoutStaticEdge_SafeX = 1 << 4,    // a box that overflows its inset-modified containing block in the horizontal
                                        // axis is aligned at its start edge instead (css-align-3/#overflow-values)
    LayoutStaticEdge_SafeY = 1 << 5,    // the same in the vertical axis
};
typedef uint8_t LayoutStaticEdges;

// A box out of flow in the subtree of a fragment that the fragment does not hold, and its static position: the point at
// which its static edges sit, from the top left of the fragment. A box that came through a line, a row or an anonymous
// block box may be inside a positioned inline box, and the block container of that inline box is then its holder.
struct LayoutPending {
    LayoutNodeIndex node;
    PrLayoutVec at;
    LayoutStaticEdges edges;
    bool inLines;                   // true if it came to the list through a line, a row or an anonymous block box
    bool fixed;                     // true if its position is fixed (LayoutFragmentPlaceholder::fixed)
    bool running;                   // true if it is a running box (LayoutFragmentPlaceholder::running)
};

// The first and last baselines of a box, indexed by LayoutBaselineEdge and measured from the block-start edge of its
// border box, in the writing mode of the space it was laid out in. Each is kLayoutBaselineNone if the box has none.
// Also holds the source of the set of each baseline, and the clamps that scroll containers applied to them, or null if
// none did. The struct owns the clamps. A box fragment and a measurement store baselines in this form
// (layoutBaselinesKeep).
struct LayoutBaselines {
    PrLayoutUnit offset[LayoutBaselineEdge_Count];
    uint32_t source[LayoutBaselineEdge_Count];
    LayoutBaselineClamps* PR_NULLABLE clamps;
};

// Keeps the clamps of `first` and `last` in `into`, which holds none, if either of them was clamped
// (layoutBaselinesKeep).
void layoutBaselinesKeepClamps(LayoutBaselines& into, const LayoutBaseline& first, const LayoutBaseline& last);

// Returns `baseline`, the baseline kept at `edge`, with the clamp kept for it (layoutBaselinesAt).
LayoutBaseline layoutBaselinesClamped(const LayoutBaselines& kept, LayoutBaselineEdge edge, LayoutBaseline baseline);

// Frees the kept clamps and leaves none.
void layoutBaselinesReset(LayoutBaselines& kept);

// The data of a Box fragment: its baselines, and the inline size of the containing block it was laid out in, which its
// padding resolved against. Queries read that size from the fragment, because the layout entry of the box may hold a
// different result.
struct LayoutFragmentBox {
    LayoutBaselines baselines;
    PrLayoutUnit containingInline;
};

// A range of the glyphs of a text fragment in one font at one em: the index of the font among the fragment's fonts, the
// index of the glyph after the last glyph in the range, and the em of its glyphs relative to the fragment's size, which
// is one unless shaping synthesized the glyphs smaller, as small capitals or a subscript (PrTextGlyph::scale).
struct LayoutFragmentFontRange {
    uint32_t font;
    uint32_t glyphEnd;
    float scale;
};

// A place where a caret can stand in a text fragment: the start of a cluster, as an offset in its node's text, and the
// distance along the line from the fragment's line-left edge to the cluster's line-left edge, in px.
struct LayoutCaretStop {
    uint32_t byte;
    float along;
};

// The data of a Text fragment: its em size in pixels, and its glyphs in the order they are placed. Each glyph is placed
// by its origin, either from the top left of the fragment or in the frame of its turn (LayoutTextTurn). Upright glyphs
// on a vertical line are placed down the line.
//
// The fonts of the fragment are distinct, and it holds a reference to each (layoutFragmentTextFonts). The glyphs are
// divided into ranges of one font and one scale each (layoutFragmentTextRanges). Each range holds at least one glyph,
// the ranges cover the glyphs once and in order, and adjacent ranges differ in font or scale. The fonts are stored
// right after the fragment, followed by the ranges, then the glyphs, then the caret stops (layoutFragmentTextStops). A
// break fragment has no fonts, ranges or glyphs.
//
// The caret stops are in the order the glyphs are placed, one at the start of each cluster, so their offsets fall in
// the order of the text, or against it in a reversed fragment. The fragment holds the bytes of its node's text from its
// first stop's offset in the order of the text up to `end`. A cluster reaches along the line to the next stop, and the
// last to the fragment's advance.
struct LayoutFragmentText {
    PrFontGlyph* PR_NULLABLE glyphs;
    float size;
    uint32_t glyphCount;
    uint32_t fontCount;
    uint32_t rangeCount;
    uint32_t stopCount;
    uint32_t end;
};

// The data of a Replaced fragment: its content box, from the top left of its border box.
struct LayoutFragmentReplaced {
    PrLayoutRect content;
};

// The data of an Inline fragment: its place in the group of its box on its line. The parts of a box on one line are
// consecutive among the children of the line, in visual order from the start edge of the box. The content of the box on
// that line follows its last part. The first part of the box on the line where the box starts is its opening part, and
// its last part on the line where the box ends is its closing part.
struct LayoutFragmentInline {
    uint32_t enclosed;              // on the last part of a group: how many later children of the line are the box's
    bool opens;
    bool closes;
};

// The data of a FloatAnchor fragment: the position of the float's fragment among the children of the parent of the
// line, relative to the index of the line there. It is kLayoutAnchorNone if the float is waiting to be placed and is
// not there.
struct LayoutFragmentAnchor {
    int32_t sibling;
};

// The data of a Placeholder fragment: whether the position of its box is fixed. Its pending entry carries this to the
// boxes that may hold it (LayoutPending).
struct LayoutFragmentPlaceholder {
    bool fixed;
    bool running;                   // true if its box is running, which no box holds (PrLayoutPosition_Running)
};

// Marks an anchor that has no float fragment among the siblings of its line (LayoutFragmentAnchor).
constexpr int32_t kLayoutAnchorNone = INT32_MIN;

// A fragment. Each field serves the kinds that its comment names. Each kind that has fields of its own has its member
// in the union at the end.
struct LayoutFragment {
    mutable Atomic<uint32_t> references;
    LayoutFragmentKind kind;
    union {
        LayoutSideFlags broken;     // Inline, Box: the sides at which the box breaks across lines or fragmentainers,
                                    // which have no border
        LayoutStaticEdges edges;    // Placeholder: the edges its static position gives the box
        LayoutTextTurn turn;        // Text: how its glyphs are turned
    };
    LayoutFragmentFlags flags;

    // The element or text node that the fragment belongs to, or kPrLayoutNodeNone for an anonymous box or a line.
    LayoutNodeIndex node;

    // Box, Inline and Replaced: the border box. Line: the line box. Text: the advance, rounded up, and the content area
    // of its face across the line.
    PrLayoutVec extent;

    // Box, Line and Around: the area that the fragment and its descendants reach, from its top left. The area may reach
    // left of or above the fragment, and holds at least its own box (layoutFragmentSetOverflow). For a scroll
    // container, it is instead the scrollable overflow, which starts from the padding box. Its ancestors do not read
    // it, because the container clips. The other kinds reach no further than their own box.
    PrLayoutRect overflow;

    uint32_t childCount;            // Box, Line, Around
    LayoutChild* PR_NULLABLE children;

    // Box: the range of its children that are floats or hold floats, through block boxes that do not float; empty if
    // there are none.
    uint32_t floatsFirst;
    uint32_t floatsEnd;

    union {
        LayoutFragmentBox box;
        LayoutFragmentText text;
        LayoutFragmentReplaced replaced;
        LayoutFragmentInline part;
        LayoutFragmentAnchor anchor;
        LayoutFragmentPlaceholder placeholder;
    };
};

// Every box, line and text run of a layout has a fragment. Its size where a pointer is 64 bits:
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(LayoutFragment) == 88);
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Making and sharing

// Creates a box, line or inline fragment with one reference, and with room for `childCount` children and a list of
// `pendingCount` boxes out of flow. The maker fills these in before sharing the fragment.
LayoutFragment* layoutFragmentCreate(LayoutFragmentKind kind, LayoutNodeIndex node, uint32_t childCount,
    uint32_t pendingCount = 0);

// Returns the list of a fragment that was made with room for one, for its maker to fill in.
LayoutPending* layoutFragmentPendingStorage(LayoutFragment* fragment);

// Creates a placeholder with one reference for the box out of flow `node`, whose static position gives `edges` and
// whose position is `position`.
LayoutFragment* layoutFragmentCreatePlaceholder(LayoutNodeIndex node, LayoutStaticEdges edges, PrLayoutPosition position);

// Creates an anchor with one reference for the float `node`. The anchor carries the placeholder flag of `flags`, which
// are the flags of the float's fragment. Its maker sets where the float's fragment is.
LayoutFragment* layoutFragmentCreateAnchor(LayoutNodeIndex node, LayoutFragmentFlags flags);

// Returns the index of the first box out of flow that `fragment` holds, or its child count if it holds none. The held
// boxes are its last children. The cost grows with the logarithm of the number of children.
inline uint32_t layoutFragmentHeldFirst(const LayoutFragment* fragment) {
    if ((fragment->flags & LayoutFragmentFlag_Holds) == 0)
        return fragment->childCount;
    return partitionPoint(0, fragment->childCount, [&](uint32_t i) {
        return (fragment->children[i].fragment->flags & LayoutFragmentFlag_OutOfFlow) == 0;
    });
}

// Returns true if the fragment is a point among its parent's children rather than a box. Placeholders and anchors are
// points: they reach nothing and are never hit.
inline bool layoutFragmentIsPoint(const LayoutFragment* fragment) {
    return fragment->kind == LayoutFragmentKind_Placeholder || fragment->kind == LayoutFragmentKind_FloatAnchor;
}

// Returns true if the fragment adds to its parent's scrollable overflow: it is neither a point nor white space that
// hangs.
inline bool layoutFragmentReachesOverflow(const LayoutFragment* fragment) {
    return !layoutFragmentIsPoint(fragment) && (fragment->flags & LayoutFragmentFlag_Hangs) == 0;
}

// Returns the number of bytes that a text fragment of `fontCount` fonts, `rangeCount` font ranges, `glyphCount` glyphs
// and `stopCount` caret stops allocates. The fonts follow the fragment with no padding, the ranges follow the fonts,
// the glyphs follow the ranges, and the stops follow the glyphs.
inline size_t layoutFragmentTextBytes(uint32_t fontCount, uint32_t rangeCount, uint32_t glyphCount, uint32_t stopCount) {
    return sizeof(LayoutFragment) + sizeof(const PrFont*) * fontCount + sizeof(LayoutFragmentFontRange) * rangeCount +
        sizeof(PrFontGlyph) * glyphCount + sizeof(LayoutCaretStop) * stopCount;
}

// Creates a text fragment with one reference, of `glyphCount` glyphs in the `fontCount` distinct fonts `fonts`, divided
// into the `rangeCount` font ranges `ranges`, and with room for `stopCount` caret stops. The fragment holds a reference
// to each font. The maker fills in the glyphs, the stops and the end of its bytes, and sets `text.stopCount` to the
// number of stops it filled. A fragment with no glyphs, a break, has no fonts and no ranges.
LayoutFragment* layoutFragmentCreateText(LayoutNodeIndex node, float size,
    const PrFont* PR_NONNULL const* PR_NULLABLE fonts, uint32_t fontCount,
    const LayoutFragmentFontRange* PR_NULLABLE ranges, uint32_t rangeCount, uint32_t glyphCount, uint32_t stopCount);

// Returns the fonts of a text fragment, of which it has `text.fontCount`.
inline const PrFont* PR_NONNULL const* PR_NONNULL layoutFragmentTextFonts(const LayoutFragment* fragment) {
    return reinterpret_cast<const PrFont* PR_NONNULL const*>(fragment + 1);
}

// Returns the font ranges of a text fragment, of which it has `text.rangeCount`.
inline const LayoutFragmentFontRange* layoutFragmentTextRanges(const LayoutFragment* fragment) {
    return reinterpret_cast<const LayoutFragmentFontRange*>(layoutFragmentTextFonts(fragment) +
        fragment->text.fontCount);
}

// Returns the caret stops of a text fragment, of which it has `text.stopCount`.
inline LayoutCaretStop* layoutFragmentTextStops(const LayoutFragment* fragment) {
    const uint8_t* glyphs = reinterpret_cast<const uint8_t*>(layoutFragmentTextRanges(fragment) +
        fragment->text.rangeCount);
    return reinterpret_cast<LayoutCaretStop*>(const_cast<uint8_t*>(glyphs) +
        sizeof(PrFontGlyph) * fragment->text.glyphCount);
}

// Records the exported baselines of a box fragment: their positions, the sources of their sets, and their clamps. A Box
// fragment is made with none.
void layoutFragmentSetBaselines(LayoutFragment* fragment, const LayoutBaseline& first, const LayoutBaseline& last);

// Returns the baseline of a fragment at `edge`, with its set. Returns none for a fragment that is not a box.
LayoutBaseline layoutFragmentBaseline(const LayoutFragment* fragment, LayoutBaselineEdge edge);

// Returns a baseline at `offset` whose set comes from `source`, unclamped.
inline LayoutBaseline layoutBaselineAt(PrLayoutUnit offset, uint32_t source) {
    return LayoutBaseline{.offset = offset, .unclamped = offset, .source = source, .present = true};
}

// Keeps `first` and `last` in `into`, which holds no clamps. Allocates clamps only if one of them was clamped.
inline void layoutBaselinesKeep(LayoutBaselines& into, const LayoutBaseline& first, const LayoutBaseline& last) {
    into.offset[LayoutBaselineEdge_First] = first.present ? first.offset : kLayoutBaselineNone;
    into.source[LayoutBaselineEdge_First] = first.present ? first.source : kLayoutBaselineSynthesized;
    into.offset[LayoutBaselineEdge_Last] = last.present ? last.offset : kLayoutBaselineNone;
    into.source[LayoutBaselineEdge_Last] = last.present ? last.source : kLayoutBaselineSynthesized;
    if ((first.present && first.clamped) || (last.present && last.clamped))
        layoutBaselinesKeepClamps(into, first, last);
}

// Returns the baseline kept at `edge`, with its set and its clamp.
inline LayoutBaseline layoutBaselinesAt(const LayoutBaselines& kept, LayoutBaselineEdge edge) {
    if (kept.offset[edge] == kLayoutBaselineNone)
        return LayoutBaseline{.source = kLayoutBaselineSynthesized};
    LayoutBaseline baseline = layoutBaselineAt(kept.offset[edge], kept.source[edge]);
    return kept.clamps == nullptr ? baseline : layoutBaselinesClamped(kept, edge, baseline);
}

// Returns `baseline` moved down by `by`, together with its range.
inline LayoutBaseline layoutBaselineMoved(LayoutBaseline baseline, PrLayoutUnit by) {
    baseline.offset += by;
    baseline.unclamped += by;
    baseline.lo += by;
    baseline.hi += by;
    return baseline;
}

// Returns `baseline` clamped to [lo, hi], composed with any range it was clamped to before. Clamping to [a, b] and then
// to [lo, hi] is the same as clamping to [clamp(a; lo, hi), clamp(b; lo, hi)].
LayoutBaseline layoutBaselineClamped(LayoutBaseline baseline, PrLayoutUnit lo, PrLayoutUnit hi);

// Returns true if a float is among the children of the fragment, or among their children through block boxes that do
// not float.
inline bool layoutFragmentHoldsFloats(const LayoutFragment* fragment) {
    return fragment->floatsFirst < fragment->floatsEnd;
}

// The boxes out of flow in the subtree of a fragment that the fragment does not hold, each with its static position
// from the top left of the fragment. They follow the children in the allocation of the fragment. A fragment that lists
// none stores nothing.
struct LayoutPendingList {
    uint32_t count;
    const LayoutPending* PR_NULLABLE entries;
};

// Returns the boxes out of flow that a Box, Line or Around fragment lists, in tree order.
LayoutPendingList layoutFragmentPending(const LayoutFragment* fragment);

// Adds or releases a reference. The last release frees the fragment and releases its children.
void layoutFragmentRetain(const LayoutFragment* fragment);
void layoutFragmentRelease(const LayoutFragment* PR_NULLABLE fragment);

// ---------------------------------------------------------------------------------------------------------------------
// Grids of tables

// A segment of a table's collapsed borders (css-tables-3/#drawing-collapsed-borders-2). Its rectangle is relative to
// the top left of the table's fragment. The segment is drawn as the side `side` of the rectangle, as thick as the whole
// rectangle.
struct LayoutGridBorder {
    PrLayoutRect rect;
    PrColor16 color;
    PrDrawBorderStyle style;
    uint8_t side;                   // a PrRectSide
};

// The grid data that a table's fragment holds: the grid's rectangle relative to the fragment's top left, and the grid's
// collapsed borders.
struct LayoutTableGrid {
    PrLayoutRect rect;
    uint32_t borderCount;
    const LayoutGridBorder* PR_NULLABLE borders;
};

// Returns a table fragment (LayoutFragmentFlag_Table) made from `built`, whose grid is `rect` and has the `count`
// collapsed borders `borders`. The function takes `built`, whose children move to the new fragment, and frees it.
LayoutFragment* layoutFragmentTable(LayoutFragment* built, PrLayoutRect rect,
    const LayoutGridBorder* PR_NULLABLE borders, uint32_t count);

// Returns the grid data that a table's fragment holds.
LayoutTableGrid layoutFragmentTableGrid(const LayoutFragment* table);

// ---------------------------------------------------------------------------------------------------------------------
// Tracks of grid containers

// The tracks of one axis of a grid container's grid, as its layout sized them: the number of tracks of its implicit
// grid, the index of the explicit grid's first line among its lines, the number of repetitions of its template's
// automatic repetition (zero if it has none), and the used size of each track.
struct LayoutGridAxisTracks {
    uint32_t count;
    int32_t explicitStart;
    uint32_t repetitions;
    const PrLayoutUnit* PR_NULLABLE sizes;
};

// Returns a grid container's fragment (LayoutFragmentFlag_Grid) made from `built`, whose tracks are those of `axes`, by
// PrLayoutGridAxis. The function takes `built`, whose children move to the new fragment, and frees it.
LayoutFragment* layoutFragmentGrid(LayoutFragment* built, const LayoutGridAxisTracks* axes);

// Returns the tracks along `axis` that a grid container's fragment holds.
LayoutGridAxisTracks layoutFragmentGridTracks(const LayoutFragment* grid, PrLayoutGridAxis axis);

// ---------------------------------------------------------------------------------------------------------------------
// Pieces of inline boxes

// Marks a piece that is not in an anonymous block (LayoutPieceAt). The child of the container is the line itself, or a
// row.
constexpr uint32_t kLayoutPieceNone = 0xffffffffu;

// The place of a line or a row among the children of a block container. `child` is the child of the container. If that
// child is an anonymous block, `line` is the line among its children, and otherwise kLayoutPieceNone.
struct LayoutPieceAt {
    uint32_t child;
    uint32_t line;
};

// The kind of a piece of an inline box.
enum {
    LayoutPieceKind_Group,          // the parts of the box on a line, followed by its content there
    LayoutPieceKind_Row,            // the row of the box around a block inside it
};
typedef uint8_t LayoutPieceKind;

// A piece of an inline box, in the content space of its container. The parts of a group are the children of the line
// from `first` to `parts`, and its content is the children from `parts` to `end`. A row is reached from the child of
// the container by descending through the one child of each of the `depth` rows around it.
struct LayoutPiece {
    LayoutPieceKind kind;
    const LayoutFragment* fragment; // Group: the line; Row: the row
    PrLayoutVec offset;
    LayoutPieceAt at;               // Group: the line's place; Row: the container's child that holds it
    uint32_t first;
    uint32_t parts;
    uint32_t end;
    uint32_t depth;
};

// The pieces of an inline box in its container, in tree order from its opening part (CSS2/#painting-order). On each
// line that the box is on, the pieces are its group of parts and its content there. There is also a row around each
// block inside the box. The floats of the box are anchored in the content of its groups.
//
// Each step looks at the children of the container from the last piece to the next, so iteration costs the number of
// pieces of the box, not the number of lines of the container.
struct LayoutPieces {
    const LayoutFragment* container;
    PrLayoutVec base;               // the content space of the container
    LayoutNodeIndex node;
    LayoutPieceAt at;               // the next line or child to look at
    bool done;
};

// Returns the pieces of the inline box `node` in `container`, whose content is at `base`, starting from the line
// `opening`, which holds its opening part.
inline LayoutPieces layoutPiecesFrom(const LayoutFragment* container, PrLayoutVec base, LayoutNodeIndex node,
        LayoutPieceAt opening) {
    return LayoutPieces{container, base, node, opening, false};
}

// Sets `*out` to the next piece of `pieces` and returns true, or returns false after the closing part of the box.
bool layoutPiecesNext(LayoutPieces& pieces, LayoutPiece* out);

// Returns the end of the group whose parts start at child `first` of `line`, which is past the content after its last
// part. Sets `*parts` to the index past its last part.
uint32_t layoutGroupEnd(const LayoutFragment* line, uint32_t first, uint32_t* parts);

// ---------------------------------------------------------------------------------------------------------------------
// Building

// A child of a fragment being made, and the position of its inline-start, block-start corner in its parent. Layout
// places children logically, before the size of their parent is known.
struct LayoutLogicalChild {
    const LayoutFragment* fragment;
    LayoutLogicalOffset offset;
};

// Derives the fields of `fragment` that summarize its children: the range of the children that are floats or hold
// floats, and the flags that say that a child is a line, that the children hold a placeholder, and that the fragment
// holds boxes out of flow, which are its last children. Every fragment with children is summarized once its children
// are set, whether a builder or a caller sets them.
void layoutFragmentSummarize(LayoutFragment* fragment);

// Creates a fragment of `kind` and `size` in `mode`, with one reference. Its children are the entries of `children`
// from `mark` on, each at the physical offset that corresponds to its logical one. Removes them from `children`. The
// list of boxes out of flow of the fragment gathers the lists of its children, each moved by the offset of the child,
// and its placeholders, in order.
LayoutFragment* layoutFragmentBuild(RawArray<LayoutLogicalChild>& children, uint32_t mark, LayoutFragmentKind kind,
    LayoutNodeIndex node, LayoutMode mode, LayoutLogicalSize size);

// Returns the fragment of a holder, made from `built`, which this function takes and frees. The new fragment has the
// children of `built`, then the `heldCount` boxes out of flow of `held` that it holds, each at its physical offset. Its
// list is the `pendingCount` boxes of `pending`.
LayoutFragment* layoutFragmentHold(LayoutFragment* built, const LayoutChild* PR_NULLABLE held, uint32_t heldCount,
    const LayoutPending* PR_NULLABLE pending, uint32_t pendingCount);

// Releases the children of `children` from `mark` on, and removes them.
void layoutFragmentDiscard(RawArray<LayoutLogicalChild>& children, uint32_t mark);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
