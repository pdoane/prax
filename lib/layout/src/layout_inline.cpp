// Inline layout: the lines of an inline formatting context beside the floats of its block formatting context, and its
// intrinsic sizes.

#include "layout/src/layout_inline.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/utf8.h"

#include "layout/src/layout_bidi.h"
#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_float.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_justify.h"
#include "layout/src/layout_line.h"
#include "layout/src/layout_line_align.h"
#include "layout/src/layout_position.h"
#include "layout/src/layout_shaping.h"
#include "layout/src/layout_sizing.h"
#include "layout/src/layout_table.h"

namespace pr_impl {

// The kind of a leaf of a line. Leaves are what the line places, in visual order.
enum {
    LayoutLineLeafKind_Text,        // glyphs of one item, at one level and in one orientation, in any faces
    LayoutLineLeafKind_Tab,         // a preserved tab, which advances as measured in the order of the text
    LayoutLineLeafKind_Break,       // a preserved segment break of a text item, which has no advance
    LayoutLineLeafKind_Atomic,
    LayoutLineLeafKind_Empty,       // the place of an inline box with nothing else on the line
    LayoutLineLeafKind_Placeholder, // the place of a box out of flow
    LayoutLineLeafKind_Anchor,      // the place of a float
};
typedef uint8_t LayoutLineLeafKind;

// A leaf of a line: the bytes [from, to) of an item, or the Open item of an empty box at its place. Also holds how far
// the leaf advances along the line before justification, and its level (UAX9 L1).
struct LayoutLineLeaf {
    LayoutItemIndex item;
    uint32_t from;
    uint32_t to;
    PrLayoutUnit advance;
    float glyphs;                   // Text: the advance of its glyphs, in px; Tab: its advance to its stop
    uint32_t glyphStart;            // Text: the range of its glyphs in the shaping
    uint32_t glyphEnd;
    LayoutLineLeafKind kind;
    uint8_t level;
    bool hangs;                     // true if it is white space that ends the line and hangs
    bool contentBefore;             // Placeholder: true if content that keeps the line from being phantom precedes it
};

// An inline box on a line: its Open item, its Close item if its end is on the line, the leaves it holds in the order of
// the text, how many boxes are around it, and whether its start is on the line.
struct LayoutLineBox {
    uint32_t open;
    uint32_t close;                 // or kNoItem
    uint32_t first;
    uint32_t end;
    uint32_t depth;
    bool starts;
};

// A part of an inline box on a line: the visual positions [start, end] of the leaves it holds there, the Open or Close
// item whose edge it has on its left and on its right, or kNoItem for none, and where it is being made.
struct LayoutLinePart {
    uint32_t box;
    uint32_t start;
    uint32_t end;
    uint32_t left;
    uint32_t right;
    uint32_t child;                   // its index among the children being built
    PrLayoutUnit x;                   // the start of its margin box along the line
    LayoutFragment* PR_NULLABLE made; // its fragment, once made
};

// The anchor of a float, made on a line of the segment being laid out: the anchor, the item of the float, and the index
// of the line among the children being built. Once the floats of the segment are placed, the anchor finds the fragment
// of the float from that index.
struct LayoutLineAnchor {
    LayoutFragment* anchor;
    LayoutItemIndex item;
    uint32_t line;
};

// The glyphs of a text leaf in the face of one shaping run: the index past its last glyph in the shaping, the font and
// metrics of the face, the offset of its dominant baseline below its alphabetic baseline, and the position of its
// glyphs across the fragment of the leaf, in px.
struct LayoutLeafFace {
    uint32_t glyphEnd;
    const PrFont* font;
    const LayoutFaceMetrics* metrics;
    PrLayoutUnit dominant;
    float across;
};

// A placeholder on the line being placed: its index among the children being built, how far relative positioning moves
// it in the frame of the line, and whether content of the line comes before it.
struct LayoutLinePlaceholder {
    uint32_t child;
    LayoutLogicalOffset move;
    bool contentBefore;
};

// A float placed on the line being made: its item, and its margin box in the block formatting context.
struct LayoutInlinePlaced {
    LayoutItemIndex item;
    LayoutExclusion box;
};

// A leaf of the line being placed, by its place among the frame's leaves.
typedef uint32_t LayoutLeafIndex;

// An inline box of the line being placed, by its place among the frame's line boxes.
typedef uint32_t LayoutLineBoxIndex;

// How many inline boxes hold a box of the line being placed, around it: 0 for a box in the root inline box alone.
typedef uint32_t LayoutLineBoxDepth;

// The floats of the run being laid out, and those of the line being made.
struct LayoutInlineFloats {
    Array<bool> placed;               // by LayoutItemIndex: true if the float is placed
    Array<uint32_t> child;            // by LayoutItemIndex: the float's index among the children built, or kNoItem
    Array<LayoutLineAnchor> anchors;  // made on the lines of the segment

    // By LayoutItemIndex and one past the last: the first float at or after the item, or the count.
    Array<uint32_t> next;

    Array<uint32_t> forced;           // the floats that the line being made must place after it
    Array<uint32_t> deferred;         // the floats that the line places after it, in order
    Array<LayoutInlinePlaced> onLine; // the floats that the line places beside it
};

// The scratch memory of one layout or measurement of a run. A run nested in an atomic inline or a block of another run
// uses the frame of the next depth, so it never touches the frame of the outer run.
struct LayoutInlineFrame {
    LayoutInlineShaping own;        // the shaping of the run, if the tree caches none
    LayoutInlineSegment segment;    // the segment being laid out or measured

    // By item: what the layout or measurement resolves for each item.
    Array<PrLayoutStyleIndex> styles; // the record that styles the item now
    Array<PrLayoutUnit> advances;     // its inline size on the line beside its text (resolveEdge, resolveItems)
    Array<PrLayoutUnit> margins;      // Open, Close: the margin part of its advance

    // Open, Close: true if the box has a non-zero inline-axis margin, border or padding at this end, which keeps the
    // line it is on from being a phantom line (css-inline-3/#invisible-line-boxes).
    Array<bool> edges;

    // Atomic, Float: its layout. The frame holds the reference to each fragment until a line or a placement takes it.
    Array<LayoutAtomic> atomics;

    Array<float> position;          // by byte: the width of what precedes it on one unbroken line (measure)
    Array<uint32_t> open;           // the Open items of the inline boxes open on the line being made, outermost first
    LayoutLineContent line;         // the content of the line being broken or made
    bool lineCollected;             // true if `line` holds a line of the segment being laid out
    bool lineAligned;               // true if the aligner holds the alignment of `line`, which reaches `lineExtent`
    LayoutLineExtent lineExtent;
    LayoutInlineFloats floats;
    LayoutLineAligner aligner;
    bool aligned;                   // true if `aligner` holds the run's items, aligned by the first segment with lines
    LayoutJustifier justifier;

    // The line being placed: its leaves and boxes in the order of the text, its levels, the visual order of the leaves,
    // and the parts of the boxes.
    Array<LayoutLineLeaf> leaves;
    Array<LayoutLineBox> lineBoxes;
    Array<LayoutLineBoxIndex> boxStack;    // the boxes open while the items of the line are read
    Array<LayoutLineBoxIndex> closed;      // the boxes in the order the items of the line end them
    Array<PrUnicodeBidiClass> lineClasses; // by code point of the line
    Array<uint8_t> lineCodeLevels;         // by code point of the line
    Array<uint8_t> lineLevels;             // by byte of the line, from its first
    Array<uint8_t> leafLevels;
    Array<LayoutLeafIndex> order;       // by visual position: the leaf there
    Array<uint32_t> visual;             // by LayoutLeafIndex: its visual position
    Array<uint32_t> positions;          // scratch: the visual positions of the leaves of a box
    Array<LayoutLinePart> parts;        // in the order they open
    Array<uint32_t> closing;            // the parts, in the order they close

    // The children of a line that bidi reorders, put back in tree order. `leafChild` holds, by leaf, the child it made
    // or kNoItem. `leafStart` holds, by leaf and one past the last, where the content from that leaf on starts in tree
    // order. `groupParts` holds, for each box starting at `groupStart`, its parts in visual order from its start edge,
    // and `contentStart` holds where its content starts in tree order. `logical` holds the indices of the children in
    // tree order.
    Array<uint32_t> leafChild;
    Array<uint32_t> leafStart;
    Array<uint32_t> groupParts;
    Array<uint32_t> groupStart;         // by LayoutLineBoxIndex and one past the last
    Array<uint32_t> contentStart;       // by LayoutLineBoxIndex
    Array<uint32_t> logical;
    Array<LayoutLogicalChild> reordered;

    // Relative positioning on the line being placed, in the frame of the line. Holds how far each box moves, together
    // with the boxes around it, and how far each leaf moves, by the innermost box that holds it. Also holds the
    // children being built that are placeholders.
    Array<LayoutLogicalOffset> boxMoves;    // by LayoutLineBoxIndex
    Array<LayoutLogicalOffset> depthMoves;  // by LayoutLineBoxDepth: the move of the last box opened at that depth
    Array<LayoutLogicalOffset> leafMoves;   // by LayoutLeafIndex
    Array<LayoutLinePlaceholder> placeholders;

    // The text leaf being placed: the faces of its glyphs in the order of the text, its distinct fonts, and its font
    // ranges in the order its glyphs are placed.
    Array<LayoutLeafFace> leafFaces;
    Array<const PrFont*> leafFonts;
    Array<LayoutFragmentFontRange> leafRanges;

    // Intrinsic sizing.
    Array<PrLayoutUnit> maxAdvances;    // by LayoutItemIndex: its advance at max-content
    Array<bool> clearing;               // by byte: true if a float that clears comes before it

    LayoutWordScratch words;            // the opportunities inside a word that fits no line (breakWord)
};

// The frames of inline layout, one for each depth of nesting. Each frame is made the first time a layout reaches its
// depth, and is kept, so that repeated layouts allocate nothing.
struct LayoutInlineScratch {
    Array<LayoutInlineFrame*> frames;
    uint32_t depth;                 // the number of frames in use
};

namespace {

// Marks no item.
constexpr uint32_t kNoItem = UINT32_MAX;

// ---------------------------------------------------------------------------------------------------------------------
// Frames

// Takes the frame of the next depth of nesting, and makes it if no layout has reached that depth before.
LayoutInlineFrame& enterFrame(LayoutScratch* scratch) {
    if (scratch->inlines == nullptr)
        scratch->inlines = allocZeroed<LayoutInlineScratch>();
    LayoutInlineScratch& s = *scratch->inlines;
    if (s.depth == s.frames.count)
        s.frames.push(allocZeroed<LayoutInlineFrame>());
    return *s.frames.data[s.depth++];
}

// Gives back the frame of the deepest depth in use.
void leaveFrame(LayoutScratch* scratch) {
    PR_ASSERT(scratch->inlines != nullptr && scratch->inlines->depth != 0);
    --scratch->inlines->depth;
}

void destroyFrame(LayoutInlineFrame* f) {
    layoutShapingReset(f->own);
    f->styles.reset();
    f->advances.reset();
    f->margins.reset();
    f->edges.reset();
    f->atomics.reset();
    f->position.reset();
    f->open.reset();
    layoutLineContentReset(f->line);
    f->floats.placed.reset();
    f->floats.child.reset();
    f->floats.anchors.reset();
    f->floats.next.reset();
    f->floats.forced.reset();
    f->floats.deferred.reset();
    f->floats.onLine.reset();
    layoutLineAlignerReset(f->aligner);
    layoutJustifierReset(f->justifier);
    f->leaves.reset();
    f->lineBoxes.reset();
    f->boxStack.reset();
    f->closed.reset();
    f->lineClasses.reset();
    f->lineCodeLevels.reset();
    f->lineLevels.reset();
    f->leafLevels.reset();
    f->order.reset();
    f->visual.reset();
    f->positions.reset();
    f->parts.reset();
    f->closing.reset();
    f->leafChild.reset();
    f->leafStart.reset();
    f->groupParts.reset();
    f->groupStart.reset();
    f->contentStart.reset();
    f->logical.reset();
    f->reordered.reset();
    f->boxMoves.reset();
    f->depthMoves.reset();
    f->leafMoves.reset();
    f->placeholders.reset();
    f->leafFaces.reset();
    f->leafFonts.reset();
    f->leafRanges.reset();
    f->maxAdvances.reset();
    f->clearing.reset();
    layoutWordScratchReset(f->words);
    prMemoryFree(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Items

// Returns the padding of a side, resolved against `containing`, the inline size of the containing block, and never
// negative.
PrLayoutUnit paddingOf(const LayoutState& state, const LayoutLogicalBox& box, LayoutSide side,
                       PrLayoutUnit containing) {
    return layoutUnitMax(kLayoutUnitZero, layoutResolve(state.styles, box.padding[side], containing));
}

// Returns true if a length is non-zero as specified. A non-zero percentage counts whatever it resolves to, so that
// whether a line is a phantom line does not depend on the size of its container (css-inline-3/#invisible-line-boxes
// does not say).
bool specifiedNonZero(PrLayoutLength length) {
    return length.bits != prLayoutLengthPx(0).bits && length.bits != prLayoutLengthPercent(0).bits;
}

// Resolves the edge of an inline box at the end that its Open or Close item `i` represents, in a container in `mode`.
// The edge is the margin, border and padding on the start or end side of the box, by its own direction, with
// percentages of `containing`. Sets the advance and margin of the item, and whether any of the three is non-zero, each
// judged on its own (css-inline-3/#invisible-line-boxes).
void resolveEdge(const LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t, uint32_t i,
                 LayoutMode mode, PrLayoutUnit containing) {
    PrLayoutStyleIndex style = f.styles.data[i];
    mode.direction = layoutUsedMode(layoutFontStyle(state.styles, style)).direction;
    LayoutLogicalBox box = layoutLogicalBox(layoutBoxStyle(state.styles, style), mode);
    LayoutSide side =
        t.items.data[i].kind == LayoutInlineItemKind_Open ? LayoutSide_InlineStart : LayoutSide_InlineEnd;
    f.margins.data[i] = layoutResolve(state.styles, box.margin[side], containing);
    f.advances.data[i] = f.margins.data[i] + box.border[side] + paddingOf(state, box, side, containing);
    f.edges.data[i] = specifiedNonZero(box.margin[side]) || box.border[side] != kLayoutUnitZero
                      || specifiedNonZero(box.padding[side]);
}

// Converts `baseline`, measured from the block-start edge of a box `blockSize` long, to a measure from its block-end
// edge instead. The range that a scroll container clamped it to is converted too.
LayoutBaseline flipped(LayoutBaseline baseline, PrLayoutUnit blockSize) {
    PrLayoutUnit lo = baseline.lo;
    baseline.offset = blockSize - baseline.offset;
    baseline.unclamped = blockSize - baseline.unclamped;
    baseline.lo = blockSize - baseline.hi;
    baseline.hi = blockSize - lo;
    return baseline;
}

// Returns the baseline that an atomic inline aligns by, measured from the line-over edge of its border box on a line in
// `mode` (css-inline-3/#baseline-source). `margin` is its margin by side in the frame of the line (layoutLineFrame).
//
// baseline-source picks its first or its last baseline. auto picks the last for an inline-block, which is also how an
// inline box of another writing mode is laid out, and the first for an inline-flex or inline-grid box and for an inline
// table, whose first baseline is the baseline of its first row (css-tables-3/#row-layout). Under auto, an inline-block
// that is a scroll container aligns by its line-under margin edge, with every baseline of its set there
// (css-align-3/#baseline-export). A box with no such baseline aligns by the line-under edge of its margin box, with its
// set synthesized from its margin box (CSS2/#leading). Replaced boxes and boxes in orthogonal flows are among such
// boxes.
LayoutBaseline baselineBySource(const PrLayoutBoxStyle& box, const LayoutResult& result, const PrLayoutUnit* margin,
                                LayoutMode mode) {
    PrLayoutUnit blockSize = layoutLogicalSize(mode, result.fragment->extent).blockSize;
    PrLayoutUnit edge = blockSize + margin[LayoutSide_BlockEnd];
    bool lastByDefault = !layoutIsFlexContainer(box) && !layoutIsGridContainer(box) && !layoutIsTable(box);
    if (box.baselineSource == PrLayoutBaselineSource_Auto && lastByDefault
        && (result.flags & LayoutResultFlag_Scrolls) != 0)
        return layoutBaselineAt(edge, kLayoutBaselineMarginEdge);
    bool first = box.baselineSource == PrLayoutBaselineSource_First
                 || (box.baselineSource == PrLayoutBaselineSource_Auto && !lastByDefault);
    LayoutBaseline baseline =
        layoutFragmentBaseline(result.fragment, first ? LayoutBaselineEdge_First : LayoutBaselineEdge_Last);
    if (!baseline.present)
        return layoutBaselineAt(edge, kLayoutBaselineSynthesized);
    return layoutLineOverIsBlockStart(mode) ? baseline : flipped(baseline, blockSize);
}

// Returns how far relative positioning moves an inline-level box styled by `box` from its place on a line of the block
// container whose content box is `space` (CSS2/#relative-positioning). The offset is in the frame of the line
// (layoutLineFrame), from its line-left and line-over sides. Percentages resolve against the containing block, which is
// the block container.
LayoutLogicalOffset relativeOnLine(const LayoutState& state, const PrLayoutBoxStyle& box, const LayoutSpace& space) {
    LayoutLogicalOffset relative = layoutRelativeOffset(state.styles, box, space);
    if (space.mode.direction == PrTextDirection_Rtl)
        relative.inlineOffset = kLayoutUnitZero - relative.inlineOffset;
    if (!layoutLineOverIsBlockStart(space.mode))
        relative.blockOffset = kLayoutUnitZero - relative.blockOffset;
    return relative;
}

// Lays out the atomic inline of `item` in `space`, the content box of the block container of its run, which is its
// containing block (CSS2/#containing-block-details). The layout cache keeps its result, as for a block-level box, but
// not that of an anonymous inline table, which has no edges and is not positioned. Its block-axis margins and baseline
// are in the frame of the lines of the container, from their line-over side (layoutLineFrame).
LayoutAtomic layoutAtomic(LayoutState& state, const LayoutInlineItem& item, const LayoutSpace& space) {
    LayoutAtomic atomic{};
    if (item.wraps) {
        LayoutResult result = layoutAnonymousTable(state, item.node, item.runLast, space);
        atomic.fragment = result.fragment;
        atomic.baseline = baselineBySource(layoutAnonymousTableStyle(), result, atomic.margin, space.mode);
        return atomic;
    }
    LayoutNodeIndex node = item.node;
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    atomic.relative = relativeOnLine(state, box, space);
    LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(box, space.mode), space.containing.inlineSize);
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
        atomic.margin[side] = e.margin[side];
    if (!layoutLineOverIsBlockStart(space.mode)) {
        atomic.margin[LayoutSide_BlockStart] = e.margin[LayoutSide_BlockEnd];
        atomic.margin[LayoutSide_BlockEnd] = e.margin[LayoutSide_BlockStart];
    }
    LayoutResult result = layoutBlock(state, node, space);
    atomic.fragment = result.fragment;
    atomic.baseline = baselineBySource(box, result, atomic.margin, space.mode);
    return atomic;
}

// Returns the node whose record styles `item`. An anonymous inline table inherits from the inline box that holds it, so
// it takes the record of that box, and layoutInlineItemBox gives it the initial box style.
LayoutNodeIndex itemNode(const LayoutState& state, const LayoutInlineItem& item) {
    return item.wraps ? layoutTreeNode(state.tree, item.node).parent : item.node;
}

// Returns the record that styles `item`.
PrLayoutStyleIndex itemStyle(const LayoutState& state, const LayoutInlineItem& item) {
    return layoutNodeStyle(state.tree, itemNode(state, item));
}

// Takes the record of each item of `t` into the frame: those of the first formatted line if `t` is lined
// (layoutNodeLineStyle), and the nodes' own otherwise.
void takeStyles(const LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t) {
    if (t.lined) {
        for (uint32_t i = 0; i < t.items.count; ++i)
            f.styles.data[i] = layoutNodeLineStyle(state.tree, itemNode(state, t.items.data[i]));
        return;
    }
    for (uint32_t i = 0; i < t.items.count; ++i)
        f.styles.data[i] = itemStyle(state, t.items.data[i]);
}

// Sizes the per-item arrays of the frame for the shaping `t`.
void sizeItems(LayoutInlineFrame& f, const LayoutInlineShaping& t) {
    f.styles.resize(t.items.count);
    f.advances.resize(t.items.count);
    f.margins.resize(t.items.count);
    f.edges.resize(t.items.count);
}

// Resolves each item of the run in `space`, the content box of its block container. Takes the record of each item from
// its node, because a change to what the node paints may have replaced it. Resolves the edges of the inline boxes
// against the inline size of the space, and lays out the atomic inlines and floats. The containing block of an atomic
// inline is the block container, through any inline boxes around it (CSS2/#containing-block-details).
void resolveItems(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t, const LayoutSpace& space) {
    sizeItems(f, t);
    f.atomics.resize(t.items.count);
    takeStyles(state, f, t);
    for (uint32_t i = 0; i < t.items.count; ++i) {
        const LayoutInlineItem& item = t.items.data[i];
        f.advances.data[i] = kLayoutUnitZero;
        f.margins.data[i] = kLayoutUnitZero;
        f.edges.data[i] = false;
        LayoutAtomic& atomic = f.atomics.data[i];
        atomic = LayoutAtomic{};
        switch (item.kind) {
            case LayoutInlineItemKind_Atomic:
                atomic = layoutAtomic(state, item, space);
                f.advances.data[i] = layoutAtomicMarginBox(atomic, space.mode).inlineSize;
                break;
            case LayoutInlineItemKind_Float: atomic = layoutFloat(state, item.node, space); break;
            case LayoutInlineItemKind_Open:
            case LayoutInlineItemKind_Close: resolveEdge(state, f, t, i, space.mode,
                                                         space.containing.inlineSize); break;
            default: break;
        }
    }
}

// Releases the fragments of the atomic inlines and floats of the frame that no line or placement took.
void releaseAtomics(LayoutInlineFrame& f) {
    for (uint32_t i = 0; i < f.atomics.count; ++i)
        layoutFragmentRelease(f.atomics.data[i].fragment);
    f.atomics.clear();
}

// Records which items of `t` are floats, none of them placed yet.
void startFloats(LayoutInlineFloats& floats, const LayoutInlineShaping& t) {
    floats.placed.clear();
    floats.placed.resize(t.items.count);
    floats.child.clear();
    floats.child.resize(t.items.count);
    floats.next.clear();
    floats.next.resize(t.items.count + 1);
    floats.next.data[t.items.count] = t.items.count;
    for (uint32_t i = t.items.count; i-- > 0;) {
        floats.placed.data[i] = false;
        floats.child.data[i] = kNoItem;
        floats.next.data[i] = t.items.data[i].kind == LayoutInlineItemKind_Float ? i : floats.next.data[i + 1];
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Measuring

// How far the width of a line may exceed its space and still fit, to allow for the error of summing advances.
constexpr float kFitTolerance = 1.0f / 64;

// Finds the position of each byte offset of the segment of the frame along one unbroken line, with `advances` by item.
// The position is the sum of the advances of the bytes before it and of the items around those bytes. The start of a
// box goes with the byte after it, and its end with the byte before it. An item at either end of the segment goes with
// the nearest byte of the segment, so an empty box there counts toward the piece beside it. A segment with no bytes has
// one position, zero.
void measure(const LayoutInlineShaping& t, LayoutInlineFrame& f, const PrLayoutUnit* advances) {
    uint32_t start = f.segment.byteStart;
    uint32_t size = f.segment.byteEnd - start;
    f.position.clear();
    f.position.reserve(t.text.count + 1);
    f.position.count = t.text.count + 1;
    // Indexed from the start of the segment.
    float* position = f.position.data + start;
    const float* advance = t.advance.data + start;
    memZero(position, (size + 1) * sizeof(float));
    for (uint32_t i = f.segment.itemStart; i < f.segment.itemEnd && size != 0; ++i) {
        const LayoutInlineItem& item = t.items.data[i];
        uint32_t at = item.start - start;
        // position[b + 1] holds what goes with byte b.
        if (item.kind == LayoutInlineItemKind_Open || item.kind == LayoutInlineItemKind_Atomic
            || item.kind == LayoutInlineItemKind_Float)
            position[at < size ? at + 1 : size] += prLayoutUnitToPx(advances[i]);
        else if (item.kind == LayoutInlineItemKind_Close)
            position[at > 0 ? at : 1] += prLayoutUnitToPx(advances[i]);
    }
    float x = 0;
    for (uint32_t b = 0; b <= size; ++b) {
        x += position[b];
        if (b != 0)
            x += advance[b - 1];
        position[b] = x;
    }
}

// Returns how far a preserved tab advances from `x`, measured along its line from the starting content edge of the
// block (css-text-3/#tab-size-property). It advances to the next tab stop, or to the one after that if the next is
// nearer than the minimum tab advance.
float tabAdvance(const LayoutInlineShaping& t, float x) {
    if (t.tabSize <= 0)
        return 0;
    float stop = (floorF32(x / t.tabSize) + 1) * t.tabSize;
    if (stop - x < t.tabMinimum)
        stop += t.tabSize;
    return stop - x;
}

// Measures a line from its first byte as it extends one byte at a time. The width includes the advances of the bytes
// and of the items along the line, and each preserved tab up to its tab stop. The width leaves out the white space that
// ends the line and collapses or hangs (css-text-3/#white-space-phase-2). `origin` is the offset of the start of the
// line from the starting content edge of the block, which tab stops are measured from.
struct LinePen {
    const LayoutInlineShaping& t;
    const LayoutInlineFrame& f;
    float origin;
    uint32_t at;                    // the byte measured to
    float x;                        // the width of the bytes before it
    float trailing;                 // the advance of the trailing white space that collapses or hangs

    // Extends the line to `end`.
    void to(uint32_t end) {
        for (; at < end; ++at) {
            float step = f.position.data[at + 1] - f.position.data[at];
            float advance = t.advance.data[at];
            LayoutSpaceFlags spaces = t.spaces.data[at];
            x += step;
            if ((spaces & LayoutSpaceFlag_Tab) != 0) {
                advance = tabAdvance(t, origin + x);
                x += advance;
            }
            bool hangs = (spaces & (LayoutSpaceFlag_Collapsible | LayoutSpaceFlag_Hangs)) != 0;
            trailing = hangs ? trailing + advance : 0;
        }
    }

    // Returns the width of the line so far, less what hangs.
    float width() const { return x - trailing; }
};

// Returns the width of the line from `start` to `end`, which starts `origin` from the starting content edge of the
// block.
float widthOf(const LayoutInlineShaping& t, const LayoutInlineFrame& f, uint32_t start, uint32_t end, float origin) {
    LinePen pen{.t = t, .f = f, .origin = origin, .at = start};
    pen.to(end);
    return pen.width();
}

// Returns the end of a line that starts at `start`, `origin` from the starting content edge of the block, and that no
// opportunity before `end` lets fit in `inlineSize`. The end is the last opportunity that overflow-wrap adds before
// `end` and that fits, or the first one it adds if none fits, or `end` if it adds none. The shaping holds those
// opportunities (LayoutInlineShaping::emergency), or this call finds them (LayoutInlineShaping::wrapsWords).
uint32_t breakWord(const LayoutStyles* styles, const LayoutInlineShaping& t, LayoutInlineFrame& f, uint32_t start,
                   float origin, float inlineSize, uint32_t end) {
    const PrTextEmergency* emergency = nullptr;
    uint32_t base = 0;
    if (t.emergency.count != 0) {
        emergency = t.emergency.data;
    } else if (t.wrapsWords) {
        layoutWordBreaks(styles, t, start, end, f.words);
        emergency = f.words.atByte.data;
        base = f.words.start;
    } else {
        return end;
    }
    uint32_t rescue = 0;
    LinePen pen{.t = t, .f = f, .origin = origin, .at = start};
    for (uint32_t b = start + 1; b < end; ++b) {
        if (emergency[b - base] == PrTextEmergency_None || t.breakAt.data[b] != PrUnicodeLineBreak_Prohibited)
            continue;
        pen.to(b);
        if (pen.width() > inlineSize + kFitTolerance)
            return rescue != 0 ? rescue : b;
        rescue = b;
    }
    return rescue != 0 ? rescue : end;
}

// Returns the end of the line that starts at `start`, `origin` from the starting content edge of the block. The line is
// broken greedily, no wider than `inlineSize` if possible, before the end of the segment of the frame. It breaks at the
// last opportunity that fits, or, if none fits, where breakWord says.
uint32_t breakLine(const LayoutStyles* styles, const LayoutInlineShaping& t, LayoutInlineFrame& f, uint32_t start,
                   float origin, float inlineSize) {
    uint32_t size = f.segment.byteEnd;
    uint32_t fits = 0;
    LinePen pen{.t = t, .f = f, .origin = origin, .at = start};
    for (uint32_t b = start + 1; b <= size; ++b) {
        if (t.breakAt.data[b] == PrUnicodeLineBreak_Prohibited)
            continue;
        pen.to(b);
        if (pen.width() > inlineSize + kFitTolerance)
            return fits != 0 ? fits : breakWord(styles, t, f, start, origin, inlineSize, b);
        fits = b;
        if (t.breakAt.data[b] == PrUnicodeLineBreak_Mandatory && b != size)
            return b;
    }
    return fits != 0 ? fits : size;
}

// ---------------------------------------------------------------------------------------------------------------------
// Line content

// Returns true if the line that ends at `end` ends in a forced break: a preserved segment break or a line break node.
bool endsForced(const LayoutInlineShaping& t, uint32_t end) {
    return end != 0 && t.text.data[end - 1] == '\n';
}

// Returns true if the line from `start` to `end` is the last line of segment `s`. It is the last if it reaches the end
// of the segment and no forced break ends it there. A forced break at the end puts the items after it on a line of
// their own, which is then the last.
bool isFinal(const LayoutInlineShaping& t, const LayoutInlineSegment& s, uint32_t start, uint32_t end) {
    return end == s.byteEnd && (start == end || !endsForced(t, end));
}

// Returns true if an item at `position` in the text is on the line from `start` to `end`, or is a float placed beside
// it. The item is on the line if it is before the end of the line, or at the end unless a forced break ends a non-empty
// line there.
bool isOnLine(const LayoutInlineShaping& t, uint32_t start, uint32_t end, uint32_t position) {
    return position < end || (position == end && !(endsForced(t, end) && start != end));
}

// Collects the content of the line from `at` to `end` of the segment of the frame into `line` of the frame: the boxes
// left open by the lines before, and each item on the line in order. The last line of the segment holds every remaining
// item. Returns where the next line starts.
LayoutLineStart collectLine(const LayoutInlineShaping& t, LayoutInlineFrame& f, LayoutLineStart at, uint32_t end) {
    if (f.lineCollected && f.line.from == at && f.line.end == end)
        return f.line.next;
    f.lineCollected = true;
    f.lineAligned = false;
    const LayoutInlineSegment& s = f.segment;
    LayoutLineContent& line = f.line;
    line.from = at;
    line.end = end;
    line.segmentEnd = s.byteEnd;
    line.carried.clear();
    for (uint32_t i = 0; i < f.open.count; ++i)
        line.carried.push(f.open.data[i]);
    line.items.clear();
    bool final = isFinal(t, s, at.byte, end);
    uint32_t drawn = end;
    while (drawn > at.byte && (t.spaces.data[drawn - 1] & LayoutSpaceFlag_Collapsible) != 0)
        --drawn;
    LayoutItemIndex item = at.item;
    uint32_t consumed = at.consumed;
    for (; item < s.itemEnd; ++item) {
        const LayoutInlineItem& it = t.items.data[item];
        if (it.kind != LayoutInlineItemKind_Text) {
            // The end of a box at the end of the line is on the line. Any other item there is on the next line.
            bool next = it.kind == LayoutInlineItemKind_Close ? it.start > end : it.start >= end;
            if (next && !final)
                break;
            line.items.push(LayoutLineItem{item, it.start, it.start});
            continue;
        }
        uint32_t from = consumed > it.start ? consumed : it.start;
        line.items.push(LayoutLineItem{item, from, it.end < drawn ? it.end : drawn});
        if (it.end > end) {
            consumed = end;
            break;
        }
        consumed = 0;
    }
    line.next = LayoutLineStart{end, item, consumed};
    return line.next;
}

// Collects the line from `at` to `end`, aligns it, and returns how far it reaches about its baseline (layoutAlignLine).
// Reuses the alignment if the line of the frame is that line and is already aligned.
LayoutLineExtent alignLine(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t, LayoutLineStart at,
                           uint32_t end, LayoutLineExtent strut) {
    collectLine(t, f, at, end);
    if (!f.lineAligned) {
        f.lineExtent = layoutAlignLine(state, f.aligner, t, f.styles.data, f.line, strut);
        f.lineAligned = true;
    }
    return f.lineExtent;
}

// ---------------------------------------------------------------------------------------------------------------------
// Building lines

// Returns how the glyphs of a run are turned on a line in `mode` (LayoutTextTurn). They are not turned if the line is
// horizontal or the run is upright. They are turned counter-clockwise on sideways-lr lines, which read from the bottom,
// and clockwise on other vertical lines.
LayoutTextTurn turnOf(LayoutMode mode, bool upright) {
    if (layoutIsHorizontal(mode) || upright)
        return LayoutTextTurn_None;
    return mode.writingMode == PrLayoutWritingMode_SidewaysLr ? LayoutTextTurn_CounterClockwise
                                                              : LayoutTextTurn_Clockwise;
}

// The justification of the line being placed: how many units each opportunity takes (css-text-3/#justification).
struct LineShares {
    const LayoutJustifyOpportunity* opportunities;  // in order, or none if the line is not justified
    uint32_t count;
    int32_t share;                  // the units each opportunity takes
    uint32_t extra;                 // the first `extra` opportunities take one unit more

    // Returns the units of opportunity `i`.
    int32_t at(uint32_t i) const { return share + (i < extra ? 1 : 0); }

    // Returns the first opportunity at or past byte `byte`.
    uint32_t from(uint32_t byte) const {
        return partitionPoint(0, count, [&](uint32_t i) { return opportunities[i].byte < byte; });
    }

    // Returns the units of the gap at byte `byte`, between two typographic character units. The gap goes on the side of
    // the content starting there from which the order of its text starts.
    int32_t gapAt(uint32_t byte) const {
        uint32_t i = from(byte);
        return i < count && opportunities[i].byte == byte && opportunities[i].gap ? at(i) : 0;
    }
};

// Returns the advance of the text or tab leaf `leaf` in units: the advance of its glyphs rounded up to a unit, measured
// from `*carry` px short of where the leaf starts, which is where the glyphs before it ended. Sets `*carry` to how far
// its own glyphs end short of its end.
PrLayoutUnit textAdvance(const LayoutLineLeaf& leaf, float* carry) {
    float end = leaf.glyphs - *carry;
    PrLayoutUnit advance = prLayoutUnitFromPxCeil(end);
    *carry = prLayoutUnitToPx(advance) - end;
    return advance;
}

// Returns the offset in the node's text of the end of the bytes [from, to) of the text item `item`: the offset of the
// byte at `to`, or past the character that the item's last byte comes from if `to` ends the item.
uint32_t sourceEnd(const LayoutState& state, const LayoutInlineShaping& t, const LayoutInlineItem& item, uint32_t to) {
    if (to < item.end)
        return t.sources.data[to];
    uint32_t last = t.sources.data[to - 1];
    PrStringView text = layoutNodeText(state.tree, item.node);
    uint32_t codePoint = 0;
    return last + uint32_t(prUtf8Decode(text, last, &codePoint));
}

// Places the preserved segment break `leaf`, of the item styled by `style`, as a text fragment at `x` along a line in
// `mode`, with no glyphs and no advance. It gives a caret a place at the end of its line, which is the only place on a
// line that holds nothing else. It spans the content area of the item's first face about the dominant baseline, which
// sits `baseline` below the line's baseline.
void placeBreakLeaf(LayoutState& state, const LayoutInlineShaping& t, const LayoutLineLeaf& leaf,
                    PrLayoutStyleIndex style, PrLayoutUnit x, PrLayoutUnit baseline, LayoutMode mode) {
    const LayoutInlineItem& item = t.items.data[leaf.item];
    const LayoutFontFaces& faces = layoutFontFaces(state.styles, style);
    PrLayoutUnit offset = layoutFaceBaseline(faces.first, layoutDominantBaseline(layoutFontStyle(state.styles, style)));
    PrLayoutUnit lo = -faces.first.ascent - offset;
    PrLayoutUnit hi = faces.first.descent - offset;
    LayoutFragment* text = layoutFragmentCreateText(item.node, prLayoutUnitToPx(faces.size), nullptr, 0, nullptr, 0, 0,
                                                    1);
    ++state.stats->textFragmentsMade;
    state.stats->textFragmentBytes += layoutFragmentTextBytes(0, 0, 0, 1);
    text->turn = turnOf(mode, false);
    text->flags |= LayoutFragmentFlag_Break;
    if (t.lined)
        text->flags |= LayoutFragmentFlag_FirstLine;
    if ((leaf.level & 1) != 0)
        text->flags |= LayoutFragmentFlag_Reversed;
    layoutFragmentTextStops(text)[0] = LayoutCaretStop{t.sources.data[leaf.from], 0};
    text->text.end = sourceEnd(state, t, item, leaf.to);
    text->extent = layoutPhysicalSize(mode, LayoutLogicalSize{kLayoutUnitZero, hi - lo});
    state.scratch->children.push(LayoutLogicalChild{text, LayoutLogicalOffset{x, baseline + lo}});
}

// Places the glyphs of the leaf `leaf`, of the item styled by `style`, as a text fragment at `x` along a line in
// `mode`. Returns how far it reaches along the line: its advance plus the justification it takes inside it.
//
// The block-start edge of the fragment, on the line-over side of the line, is relative to the baseline of the line, and
// the dominant baseline of the item sits `baseline` below it. Each glyph is set in the face of its shaping run, with
// the baseline of that face that has the dominant baseline's name aligned to the item's
// (css-inline-3/#dominant-baseline-property). The fragment spans the content areas of its faces across the line. Glyphs
// are placed as on a horizontal line and turned with a vertical one, or are upright, placed down the line from their
// vertical origins on the central baselines of their faces. A leaf at an odd level places its glyphs in visual order,
// the reverse of the order of the text (UAX9 L2). Justification widens the gaps between its glyphs, and a word
// separator at its end, on the side where the order of its text ends.
//
// `*carry` is how far, in px, the glyphs before the leaf end short of `x`, because advances round up to a unit. The
// glyphs of this leaf start there, so that dividing text into fragments moves no glyph. It is set to how far the glyphs
// of this leaf end short of the x that the function returns.
//
// The fragment's caret stops are where each cluster starts along the line, in the order its glyphs are placed.
PrLayoutUnit placeTextLeaf(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t,
                           const LayoutLineLeaf& leaf, PrLayoutStyleIndex style, PrLayoutUnit x, PrLayoutUnit baseline,
                           LayoutMode mode, const LineShares& shares, float* carry) {
    const LayoutInlineItem& item = t.items.data[leaf.item];
    float size = prLayoutUnitToPx(layoutFontFaces(state.styles, style).size);
    PrLayoutAlignmentBaseline dominant = layoutDominantBaseline(layoutFontStyle(state.styles, style));
    Array<LayoutLogicalChild>& children = state.scratch->children;
    uint32_t g = leaf.glyphStart;
    uint32_t glyphEnd = leaf.glyphEnd;
    ++state.stats->textLeavesPlaced;
    if (g == glyphEnd)
        return textAdvance(leaf, carry);

    // The faces of its glyphs, one piece for each shaping run they fall in, and the content area they span about the
    // dominant baseline, positive downward. The face of a run is among the faces of its own group, whose style the item
    // shares.
    PrLayoutUnit lo{kLayoutUnitMax};
    PrLayoutUnit hi{-kLayoutUnitMax};
    bool upright = t.runs.data[t.glyphs.data[g].run].upright;
    auto faceOf = [&](const LayoutShapingRun& run, uint32_t end) {
        PR_ASSERT(run.upright == upright);
        const LayoutFontFaces& group = layoutFontFacesOfGroup(state.styles, run.font);
        PR_ASSERT(run.face < group.metrics.count);
        const LayoutFaceMetrics& face = group.metrics.data[run.face];
        PrLayoutUnit offset = layoutFaceBaseline(face, dominant);
        lo = layoutUnitMin(lo, -face.ascent - offset);
        hi = layoutUnitMax(hi, face.descent - offset);
        return LayoutLeafFace{.glyphEnd = end, .font = layoutFaceFont(group, run.face), .metrics = &face,
                              .dominant = offset, .across = 0};
    };
    // A leaf of one run, which is the common case, has one face, one font and one font range, and needs no scratch.
    LayoutLeafFace single;
    LayoutLeafFace* faces = &single;
    uint32_t faceCount = 1;
    const PrFont* const* fonts = &single.font;
    uint32_t fontCount = 1;
    LayoutFragmentFontRange singleRange{0, glyphEnd - g};
    const LayoutFragmentFontRange* ranges = &singleRange;
    uint32_t rangeCount = 1;
    bool reversed = (leaf.level & 1) != 0;
    if (t.glyphs.data[glyphEnd - 1].run == t.glyphs.data[g].run) {
        single = faceOf(t.runs.data[t.glyphs.data[g].run], glyphEnd);
    } else {
        Array<LayoutLeafFace>& pieces = f.leafFaces;
        pieces.clear();
        for (uint32_t i = g; i < glyphEnd;) {
            const LayoutShapingRun& run = t.runs.data[t.glyphs.data[i].run];
            uint32_t end = run.glyphEnd < glyphEnd ? run.glyphEnd : glyphEnd;
            pieces.push(faceOf(run, end));
            i = end;
        }
        faces = pieces.data;
        faceCount = pieces.count;
        // Its distinct fonts, few, and its font ranges, in the order its glyphs are placed.
        Array<const PrFont*>& distinct = f.leafFonts;
        Array<LayoutFragmentFontRange>& placedRanges = f.leafRanges;
        distinct.clear();
        placedRanges.clear();
        uint32_t placed = 0;
        for (uint32_t k = 0; k < faceCount; ++k) {
            uint32_t j = reversed ? faceCount - 1 - k : k;
            uint32_t start = j != 0 ? faces[j - 1].glyphEnd : g;
            placed += faces[j].glyphEnd - start;
            uint32_t font = 0;
            while (font < distinct.count && distinct.data[font] != faces[j].font)
                ++font;
            if (font == distinct.count)
                distinct.push(faces[j].font);
            if (placedRanges.count != 0 && placedRanges.last().font == font)
                placedRanges.last().glyphEnd = placed;
            else
                placedRanges.push(LayoutFragmentFontRange{font, placed});
        }
        fonts = distinct.data;
        fontCount = distinct.count;
        ranges = placedRanges.data;
        rangeCount = placedRanges.count;
    }
    // Where the glyphs of each face sit across the fragment, in its frame. An upright glyph has its vertical origin on
    // the central baseline of its face, measured from the left, which is the line-under side. Any other glyph has its
    // alphabetic origin measured from the line-over side, which is the top of the frame that a turn rotates.
    for (uint32_t j = 0; j < faceCount; ++j) {
        PrLayoutUnit alphabetic = -faces[j].dominant - lo;
        faces[j].across = prLayoutUnitToPx(upright ? hi - lo - alphabetic - faces[j].metrics->central : alphabetic);
    }
    // Each glyph that starts a cluster has a stop, so a fragment has room for a stop for each glyph, which most text
    // fills, and records how many it took.
    LayoutFragment* text =
        layoutFragmentCreateText(item.node, size, fonts, fontCount, ranges, rangeCount, glyphEnd - g, glyphEnd - g);
    ++state.stats->textFragmentsMade;
    state.stats->textFragmentBytes += layoutFragmentTextBytes(fontCount, rangeCount, glyphEnd - g, glyphEnd - g);
    text->turn = turnOf(mode, upright);
    if (t.lined)
        text->flags |= LayoutFragmentFlag_FirstLine;
    if (reversed)
        text->flags |= LayoutFragmentFlag_Reversed;
    // The collapsible white space that ends the line, which no leaf holds, ends the bytes of the leaf before it.
    uint32_t to = leaf.to;
    uint32_t lineEnd = f.line.end < item.end ? f.line.end : item.end;
    bool trailing = to < lineEnd;
    for (uint32_t b = to; trailing && b < lineEnd; ++b)
        trailing = (t.spaces.data[b] & LayoutSpaceFlag_Collapsible) != 0;
    text->text.end = sourceEnd(state, t, item, trailing ? lineEnd : to);
    LayoutCaretStop* stops = layoutFragmentTextStops(text);
    const uint32_t* sources = t.sources.data;
    uint32_t stop = 0;

    // The opportunities inside the leaf: the gaps between its bytes, and the opportunity of a word separator at its
    // end.
    uint32_t first = shares.from(leaf.from + 1);
    uint32_t last = shares.from(leaf.to);
    if (last < shares.count && shares.opportunities[last].byte == leaf.to && !shares.opportunities[last].gap)
        ++last;
    int32_t inside = 0;
    float pen = -*carry;
    auto widen = [&](uint32_t i) {
        inside += shares.at(i);
    };
    // Visually, a left-to-right leaf takes each gap before the glyph it precedes, and a right-to-left leaf takes it
    // after. The share of a separator goes on the end side of the leaf, which is the right or the left.
    uint32_t next = first;          // left to right: the next opportunity to take
    uint32_t back = last;           // right to left: one past the next opportunity to take
    if (reversed && back > first && shares.opportunities[back - 1].byte == leaf.to)
        widen(--back);
    // The common case, left-to-right glyphs across a line with nothing to widen, has a loop of its own.
    bool plain = first == last && !upright && !reversed;
    uint32_t k = 0;
    PrFontGlyph* out = text->text.glyphs;
    const LayoutInlineGlyph* in = t.glyphs.data + g;
    for (uint32_t j = 0; plain && j < faceCount; ++j) {
        float across = faces[j].across;
        uint32_t end = faces[j].glyphEnd - g;
        for (; k < end; ++k) {
            if (k == 0 || in[k].cluster != in[k - 1].cluster)
                stops[stop++] = LayoutCaretStop{sources[in[k].cluster], pen};
            out[k] = PrFontGlyph{in[k].id, PrVec2{pen + in[k].offset.x, across + in[k].offset.y}};
            pen += in[k].advance;
        }
    }
    // The face of the glyph being placed, which moves through the faces in the order the glyphs are placed.
    uint32_t j = reversed ? faceCount - 1 : 0;
    for (k = 0; !plain && k < glyphEnd - g; ++k) {
        uint32_t i = reversed ? glyphEnd - 1 - k : g + k;
        const LayoutInlineGlyph& glyph = t.glyphs.data[i];
        if (!reversed) {
            while (i >= faces[j].glyphEnd)
                ++j;
            while (next < last && shares.opportunities[next].byte <= glyph.cluster
                   && shares.opportunities[next].byte < leaf.to)
                widen(next++);
        } else {
            while (j != 0 && i < faces[j - 1].glyphEnd)
                --j;
            while (back > first && shares.opportunities[back - 1].byte > glyph.cluster)
                widen(--back);
        }
        float along = pen + prLayoutUnitToPx(PrLayoutUnit{inside});
        // A cluster's line-left edge is at its first glyph in placement order, which is its last in the order of the
        // text if the leaf is reversed.
        uint32_t beside = reversed ? i + 1 : i - 1;
        if (k == 0 || t.glyphs.data[beside].cluster != glyph.cluster)
            stops[stop++] = LayoutCaretStop{sources[glyph.cluster], along};
        float across = faces[j].across;
        PrVec2 at = upright ? PrVec2{across + glyph.offset.x, along + glyph.offset.y}
                            : PrVec2{along + glyph.offset.x, across + glyph.offset.y};
        out[k] = PrFontGlyph{glyph.id, at};
        pen += glyph.advance;
    }
    while (!reversed && next < last)
        widen(next++);
    text->text.stopCount = stop;
    PrLayoutUnit advance = textAdvance(leaf, carry) + PrLayoutUnit{inside};
    text->extent = layoutPhysicalSize(mode, LayoutLogicalSize{advance, hi - lo});
    children.push(LayoutLogicalChild{text, LayoutLogicalOffset{x, baseline + lo}});
    return advance;
}

// Makes the fragment of the part `part` of an inline box on the line, from the left of its margin box at `part.x` to
// `right`, at its reserved index among the children being built. Its dominant baseline is at the aligned baseline of
// its box. Its extent is the content area of its first face around that baseline, with its padding and border on the
// line-over and line-under sides. On a phantom line, which is zero high, the part is zero high at the line-over edge of
// the line. A side that has no edge of the box is broken: the box continues past it, on another line or across a split.
LayoutFragment* makePart(LayoutState& state, const LayoutInlineFrame& f, const LayoutInlineShaping& t,
                         const LayoutLinePart& part, PrLayoutUnit right, const LayoutSpace& space, bool phantom) {
    const LayoutLineBox& box = f.lineBoxes.data[part.box];
    PrLayoutStyleIndex style = f.styles.data[box.open];
    LayoutLogicalBox logical = layoutLogicalBox(layoutBoxStyle(state.styles, style), space.mode);
    const LayoutFontFaces& faces = layoutFontFaces(state.styles, style);
    LayoutNodeIndex node = t.items.data[box.open].node;
    LayoutFragment* fragment = layoutFragmentCreate(LayoutFragmentKind_Inline, node, 0);
    if (t.lined)
        fragment->flags |= LayoutFragmentFlag_FirstLine;
    LayoutMode frame = layoutLineFrame(space.mode);
    LayoutSideFlags broken = 0;
    if (part.left == kNoItem)
        broken |= LayoutSideFlags(1u << layoutPhysicalSide(frame, LayoutSide_InlineStart));
    if (part.right == kNoItem)
        broken |= LayoutSideFlags(1u << layoutPhysicalSide(frame, LayoutSide_InlineEnd));
    fragment->broken = broken;
    PrLayoutUnit marginLeft = part.left != kNoItem ? f.margins.data[part.left] : kLayoutUnitZero;
    PrLayoutUnit marginRight = part.right != kNoItem ? f.margins.data[part.right] : kLayoutUnitZero;
    // The block extent of the border box is the content area with the padding and borders around it. The padding and
    // borders do not move the line (CSS2/#inline-non-replaced).
    LayoutSide over = layoutLineOverIsBlockStart(space.mode) ? LayoutSide_BlockStart : LayoutSide_BlockEnd;
    LayoutSide under = over == LayoutSide_BlockStart ? LayoutSide_BlockEnd : LayoutSide_BlockStart;
    PrLayoutUnit before = logical.border[over] + paddingOf(state, logical, over, space.containing.inlineSize);
    PrLayoutUnit after = logical.border[under] + paddingOf(state, logical, under, space.containing.inlineSize);
    fragment->extent = layoutPhysicalSize(
        space.mode, LayoutLogicalSize{layoutUnitMax(kLayoutUnitZero, right - part.x - marginLeft - marginRight),
                                      faces.first.ascent + faces.first.descent + before + after});
    LayoutLogicalChild& child = state.scratch->children.data[part.child];
    child.fragment = fragment;
    // A phantom line is not aligned, so nothing on it has a baseline. The part is zero high at the top of the line.
    if (phantom) {
        LayoutLogicalSize size = layoutLogicalSize(space.mode, fragment->extent);
        fragment->extent = layoutPhysicalSize(space.mode, LayoutLogicalSize{size.inlineSize, kLayoutUnitZero});
        child.offset = LayoutLogicalOffset{part.x + marginLeft, kLayoutUnitZero};
        return fragment;
    }
    PrLayoutAlignmentBaseline dominantName = layoutDominantBaseline(layoutFontStyle(state.styles, style));
    PrLayoutUnit dominant = layoutFaceBaseline(faces.first, dominantName);
    PrLayoutUnit baseline = layoutAlignedBaseline(f.aligner, box.open);
    child.offset = LayoutLogicalOffset{part.x + marginLeft, baseline - faces.first.ascent - dominant - before};
    return fragment;
}

// Returns the alignment of the lines in a container styled by `font` that are last, or that a forced break ends
// (css-text-3/#text-align-last-property). text-align-last applies. Its auto value takes text-align-all, or start if
// text-align-all is justify.
PrLayoutTextAlign lastAlignOf(const PrLayoutFontStyle& font) {
    if (font.textAlignLast != PrLayoutTextAlign_Auto)
        return font.textAlignLast;
    return font.textAlign == PrLayoutTextAlign_Justify ? PrLayoutTextAlign_Start : font.textAlign;
}

// Returns the offset from its start edge at which `align` places the content of a line within `free`, the room the
// content leaves, in a paragraph of `direction`. Centering rounds down to a unit.
PrLayoutUnit alignOffset(PrLayoutTextAlign align, PrTextDirection direction, PrLayoutUnit free) {
    bool ltr = direction == PrTextDirection_Ltr;
    switch (align) {
        case PrLayoutTextAlign_End: return free;
        case PrLayoutTextAlign_Left: return ltr ? kLayoutUnitZero : free;
        case PrLayoutTextAlign_Right: return ltr ? free : kLayoutUnitZero;
        case PrLayoutTextAlign_Center: return free / 2;
        default: return kLayoutUnitZero;
    }
}

// The output of building a line: its extent about its baseline, whether it is a phantom line, and the sides of the
// floats that a line break on it clears. A phantom line is zero high and has no baseline
// (css-inline-3/#invisible-line-boxes).
struct LineBuilt {
    LayoutLineExtent extent;
    bool phantom;
    LayoutLineSides clears;
};

// Where a line goes and how its content sits in it: its position from the corner of the context, its inline size, the
// indent its content starts at, and its alignment.
struct LinePlace {
    LayoutLogicalOffset at;
    PrLayoutUnit size;
    PrLayoutUnit indent;
    PrLayoutTextAlign align;
};

// Resolves the levels of the bytes [start, end) of the line, in a paragraph at level `paragraph`, into `lineLevels` of
// the frame, indexed by byte from `start`. The white space that ends the line, and the white space before a segment or
// paragraph separator, takes the level of the paragraph (UAX9 L1).
void lineLevels(LayoutInlineFrame& f, const LayoutInlineShaping& t, uint32_t start, uint32_t end, uint8_t paragraph) {
    PrStringView text{t.text.data, end};
    f.lineClasses.clear();
    f.lineCodeLevels.clear();
    for (uint32_t at = start; at < end;) {
        uint32_t codePoint = 0;
        uint32_t size = uint32_t(prUtf8Decode(text, at, &codePoint));
        f.lineClasses.push(prUnicodeBidiClass(codePoint));
        f.lineCodeLevels.push(t.levels.data[at]);
        at += size;
    }
    prUnicodeBidiLineLevels(f.lineClasses.data, f.lineClasses.count, paragraph, f.lineCodeLevels.data);
    f.lineLevels.resize(end - start);
    uint32_t k = 0;
    for (uint32_t at = start; at < end; ++k) {
        uint32_t codePoint = 0;
        uint32_t size = prUtf8Decode(text, at, &codePoint);
        for (uint32_t b = at; b < at + size; ++b)
            f.lineLevels.data[b - start] = f.lineCodeLevels.data[k];
        at += size;
    }
}

// Appends the leaves of the text of `on`, an item on the line from `lineStart`, to the leaves of the frame. Its bytes
// are split wherever their placement changes: where a level, an orientation or a preserved tab begins, and at `hang`,
// where the white space that hangs starts. A change of face or script alone, which begins another shaping run, does not
// split them.
//
// Each tab reaches its stop from `*logical`, which is how far the line reaches along itself in the order of the text
// from the start edge of the block, in px, as breaking measured it. Each leaf advances `*logical`.
void textLeaves(const LayoutInlineShaping& t, LayoutInlineFrame& f, const LayoutLineItem& on, uint32_t lineStart,
                uint32_t hang, float* logical) {
    const LayoutInlineItem& item = t.items.data[on.item];
    bool levels = t.bidi;
    // The run that holds the first byte of the item on the line, or the first run past it.
    uint32_t r = partitionPoint(item.runStart, item.runEnd, [&](uint32_t i) { return t.runs.data[i].end <= on.from; });
    auto levelAt = [&](uint32_t b) { return levels ? f.lineLevels.data[b - lineStart] : uint8_t(0); };
    auto tab = [&](uint32_t b) { return (t.spaces.data[b] & LayoutSpaceFlag_Tab) != 0; };
    for (uint32_t b = on.from; b < on.to;) {
        if (t.tabs && tab(b)) {
            float advance = tabAdvance(t, *logical);
            uint32_t g = t.glyphAt.data[b];
            f.leaves.push(LayoutLineLeaf{on.item, b, b + 1, prLayoutUnitFromPxCeil(advance), advance, g, g,
                                         LayoutLineLeafKind_Tab, levelAt(b), b >= hang, false});
            *logical += advance;
            ++b;
            continue;
        }
        if (t.text.data[b] == '\n') {
            uint32_t g = t.glyphAt.data[b];
            f.leaves.push(LayoutLineLeaf{on.item, b, b + 1, kLayoutUnitZero, 0, g, g, LayoutLineLeafKind_Break,
                                         levelAt(b), false, false});
            ++b;
            continue;
        }
        while (r < t.runs.count && t.runs.data[r].end <= b)
            ++r;
        // The leaf continues into the runs that follow its own with no byte between, as long as they have its
        // orientation and start at its level. A run holds no tab or line feed, so the end of the runs bounds the leaf.
        // A level changes only in bidi text, at the start of a run, or inside a run where L1 resets the white space
        // that ends the line.
        uint32_t limit = hang > b && hang < on.to ? hang : on.to;
        uint32_t e = b + 1;
        if (r < t.runs.count && t.runs.data[r].start <= b) {
            uint32_t k = r;
            e = t.runs.data[k].end;
            while (e < limit && k + 1 < t.runs.count && t.runs.data[k + 1].start == e
                   && t.runs.data[k + 1].upright == t.runs.data[r].upright && levelAt(e) == levelAt(b))
                e = t.runs.data[++k].end;
        }
        e = e < limit ? e : limit;
        if (levels) {
            uint32_t at = b + 1;
            while (at < e && levelAt(at) == levelAt(b))
                ++at;
            e = at;
        }
        // Its advance in units is found once the leaves of the line are in visual order.
        float pen = float(t.reach.data[e] - t.reach.data[b]);
        f.leaves.push(LayoutLineLeaf{on.item, b, e, kLayoutUnitZero, pen, t.glyphAt.data[b], t.glyphAt.data[e],
                                     LayoutLineLeafKind_Text, levelAt(b), b >= hang, false});
        *logical += pen;
        b = e;
    }
}

// Puts the children of the line, from `mark` among `children`, from visual order into tree order (CSS2/#painting-order,
// css-position-4/#paint-a-box-in-a-line-box). Each box contributes its parts on the line, in visual order from its
// start edge, then its content on the line in the order of the text, with each box inside it treated the same way. Each
// child keeps its position. Sets the opening and closing parts of each box, and how many children after its last part
// are its content.
void treeOrder(const LayoutStyles* styles, LayoutInlineFrame& f, uint32_t mark, Array<LayoutLogicalChild>& children) {
    uint32_t boxes = f.lineBoxes.count;
    uint32_t count = f.leaves.count;
    // The parts of each box, in the visual order that f.parts holds them, grouped by box.
    f.groupStart.resize(boxes + 1);
    for (uint32_t b = 0; b <= boxes; ++b)
        f.groupStart.data[b] = 0;
    for (uint32_t i = 0; i < f.parts.count; ++i)
        ++f.groupStart.data[f.parts.data[i].box + 1];
    for (uint32_t b = 0; b < boxes; ++b)
        f.groupStart.data[b + 1] += f.groupStart.data[b];
    f.groupParts.resize(f.parts.count);
    f.contentStart.resize(boxes);
    for (uint32_t b = 0; b < boxes; ++b)
        f.contentStart.data[b] = f.groupStart.data[b];
    for (uint32_t i = 0; i < f.parts.count; ++i)
        f.groupParts.data[f.contentStart.data[f.parts.data[i].box]++] = i;
    // The group of a box starts on the side of its start edge. That is the right for a right-to-left box, whose parts
    // are reversed.
    for (uint32_t b = 0; b < boxes; ++b) {
        uint32_t first = f.groupStart.data[b];
        uint32_t last = f.groupStart.data[b + 1];
        if (last - first < 2)
            continue;
        const PrLayoutFontStyle& font = layoutFontStyle(styles, f.styles.data[f.lineBoxes.data[b].open]);
        if (layoutUsedMode(font).direction != PrTextDirection_Rtl)
            continue;
        reverseItems(f.groupParts.data + first, last - first);
    }
    // The leaves in the order of the text, with the group of each box before the first leaf it holds.
    f.logical.clear();
    f.leafStart.resize(count + 1);
    uint32_t b = 0;
    for (uint32_t i = 0; i <= count; ++i) {
        f.leafStart.data[i] = f.logical.count;
        if (i == count)
            break;
        for (; b < boxes && f.lineBoxes.data[b].first == i; ++b) {
            for (uint32_t k = f.groupStart.data[b]; k < f.groupStart.data[b + 1]; ++k)
                f.logical.push(f.parts.data[f.groupParts.data[k]].child);
            f.contentStart.data[b] = f.logical.count;
        }
        if (f.leafChild.data[i] != kNoItem)
            f.logical.push(f.leafChild.data[i]);
    }
    PR_ASSERT(b == boxes && f.logical.count == children.count - mark);
    // A line that bidi does not reorder already places its children in tree order.
    bool ordered = true;
    for (uint32_t i = 0; i < f.logical.count && ordered; ++i)
        ordered = f.logical.data[i] == mark + i;
    if (!ordered) {
        f.reordered.clear();
        for (uint32_t i = 0; i < f.logical.count; ++i)
            f.reordered.push(children.data[f.logical.data[i]]);
        memCopy(children.data + mark, f.reordered.data, sizeof(LayoutLogicalChild) * f.reordered.count);
    }
    for (b = 0; b < boxes; ++b) {
        const LayoutLineBox& box = f.lineBoxes.data[b];
        uint32_t first = f.groupStart.data[b];
        uint32_t last = f.groupStart.data[b + 1] - 1;
        LayoutFragment* opening = f.parts.data[f.groupParts.data[first]].made;
        LayoutFragment* closing = f.parts.data[f.groupParts.data[last]].made;
        opening->part.opens = box.starts;
        closing->part.closes = box.close != kNoItem;
        closing->part.enclosed = f.leafStart.data[box.end] - f.contentStart.data[b];
    }
}

// Appends the line from `at` to `end` to the children being built, and advances `at` past it. The children of the line
// are the parts of its inline boxes, its text fragments and its atomic inlines, each at its aligned baseline. They are
// placed from the line-left edge of the line in visual order (css-writing-modes-4/#bidi-algo, UAX9 L1 and L2), and then
// `place` aligns the whole line in the direction of its paragraph.
//
// A justified line spreads its free space over its justification opportunities as it places them. A justified line with
// no opportunities aligns as its last line would, and is centered if that alignment is justify
// (css-text-3/#justify-limits). The line reaches at least `strut`, unless it is a phantom line. If the aligner aligns
// every item by its shift, the extent of the line is found during placement, in the same pass.
//
// The parts of an inline box are the runs of its leaves that are adjacent in visual order. Its start edge goes on the
// part that is start-most in its own direction, on the line that holds its start. Its end edge goes on the end-most
// part, on the line that holds its end (css-writing-modes-4/#bidi-box-model).
//
// A line that `phantom` marks as phantom, in a segment with nothing that keeps a line from being phantom, is not
// aligned. It is zero high, and nothing on it has a baseline.
LineBuilt buildLine(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t, LayoutLineStart& at,
                    uint32_t end, LayoutLineExtent strut, const LinePlace& place, const LayoutSpace& space,
                    bool phantom) {
    LayoutLineAligner& aligner = f.aligner;
    bool byShift = !phantom && layoutAlignsByShift(aligner);
    LayoutLineExtent extent = byShift || phantom ? strut : alignLine(state, f, t, at, end, strut);
    LayoutLineStart next = collectLine(t, f, at, end);
    const LayoutLineContent& line = f.line;
    auto take = [&](LayoutItemIndex item, LayoutLineExtent own) {
        if (!byShift)
            return;
        PrLayoutUnit shift = aligner.align.data[item].shift;
        layoutLineExtend(extent, LayoutLineExtent{own.above - shift, own.below + shift});
    };
    // The level of the paragraph, and the levels of the bytes of the line.
    uint8_t paragraph = 0;
    if (t.bidi) {
        paragraph = layoutBidiParagraphAt(t, line.from.byte).level;
        lineLevels(f, t, line.from.byte, end, paragraph);
    }
    PrTextDirection direction = (paragraph & 1) != 0 ? PrTextDirection_Rtl : PrTextDirection_Ltr;
    PrLayoutTextAlign align = place.align;
    // A line aligned to its start edge does not need to know the room it leaves. Otherwise the preserved white space
    // that ends the line hangs from `hang`, on the end side of the line (css-text-3/#white-space-phase-2).
    bool aligns = align != PrLayoutTextAlign_Start || direction == PrTextDirection_Rtl;
    uint32_t hang = UINT32_MAX;
    if (aligns) {
        uint32_t drawn = end;
        while (drawn > line.from.byte && (t.spaces.data[drawn - 1] & LayoutSpaceFlag_Collapsible) != 0)
            --drawn;
        hang = drawn;
        while (hang > line.from.byte && (t.spaces.data[hang - 1] & LayoutSpaceFlag_Hangs) != 0)
            --hang;
    }

    // Builds the leaves and boxes in the order of the text. The boxes still open from the previous line continue on
    // this one.
    LineBuilt built{};
    f.leaves.clear();
    f.lineBoxes.clear();
    f.boxStack.clear();
    f.closed.clear();
    // How far the line reaches along itself in the order of the text, from the start edge of the block, in px. Tabs
    // advance from this position.
    float logical = prLayoutUnitToPx(place.at.inlineOffset + place.indent);
    PrLayoutUnit edges = kLayoutUnitZero;   // the total of the edges of the inline boxes on the line
    for (uint32_t i = 0; i < f.open.count; ++i) {
        LayoutItemIndex item = f.open.data[i];
        f.boxStack.push(f.lineBoxes.count);
        f.lineBoxes.push(LayoutLineBox{item, kNoItem, 0, 0, i, false});
        take(item, aligner.align.data[item].extent);
    }
    // A box with no leaf on the line holds an empty leaf at its place.
    auto closeBox = [&](uint32_t index, uint32_t close) {
        LayoutLineBox& box = f.lineBoxes.data[index];
        if (box.first == f.leaves.count) {
            uint32_t byte = close != kNoItem ? t.items.data[close].start : end;
            uint8_t level = t.bidi && byte < end ? f.lineLevels.data[byte - line.from.byte] : paragraph;
            f.leaves.push(LayoutLineLeaf{box.open, byte, byte, kLayoutUnitZero, 0, 0, 0, LayoutLineLeafKind_Empty,
                                         level, false, false});
        }
        box.close = close;
        box.end = f.leaves.count;
        f.closed.push(index);
    };
    // The start and end of an isolating box are empty leaves at the levels of its isolate initiator and terminator.
    // They keep what is outside the box from joining what is inside it in reordering.
    auto isolateMark = [&](LayoutItemIndex item) {
        if (t.bidi && t.marks.data[item] != kLayoutMarkNone) {
            uint32_t byte = t.items.data[item].start;
            f.leaves.push(LayoutLineLeaf{item, byte, byte, kLayoutUnitZero, 0, 0, 0, LayoutLineLeafKind_Empty,
                                         t.marks.data[item], false, false});
        }
    };
    // A placeholder or an anchor resolves as a neutral with no text (UAX9 N1, N2). It takes the direction of the text
    // on both sides if they agree, and otherwise the direction of the embedding, which the lower of their levels gives.
    auto neutralLevel = [&](const LayoutInlineItem& it) {
        if (!t.bidi)
            return paragraph;
        uint32_t offset = it.start - line.from.byte;
        uint8_t before = offset > 0 ? f.lineLevels.data[offset - 1] : paragraph;
        uint8_t after = it.start < end ? f.lineLevels.data[offset] : paragraph;
        return before < after ? before : after;
    };
    bool content = false;           // true if the line holds content that keeps it from being a phantom line
    for (uint32_t i = 0; i < line.items.count; ++i) {
        const LayoutLineItem& on = line.items.data[i];
        LayoutItemIndex item = on.item;
        const LayoutInlineItem& it = t.items.data[item];
        switch (it.kind) {
            case LayoutInlineItemKind_Open:
                f.boxStack.push(f.lineBoxes.count);
                f.lineBoxes.push(LayoutLineBox{item, kNoItem, f.leaves.count, 0, f.boxStack.count - 1, true});
                f.open.push(item);
                isolateMark(item);
                logical += prLayoutUnitToPx(f.advances.data[item]);
                edges += f.advances.data[item];
                content = content || f.edges.data[item];
                take(item, aligner.align.data[item].extent);
                break;
            case LayoutInlineItemKind_Close:
                isolateMark(item);
                closeBox(f.boxStack.last(), item);
                f.boxStack.pop();
                f.open.pop();
                logical += prLayoutUnitToPx(f.advances.data[item]);
                edges += f.advances.data[item];
                content = content || f.edges.data[item];
                break;
            case LayoutInlineItemKind_Atomic: {
                uint8_t level = t.bidi ? f.lineLevels.data[it.start - line.from.byte] : uint8_t(0);
                f.leaves.push(LayoutLineLeaf{item, it.start, it.end, f.advances.data[item], 0, 0, 0,
                                             LayoutLineLeafKind_Atomic, level, false, false});
                logical += prLayoutUnitToPx(f.advances.data[item]);
                content = true;
                take(item, aligner.align.data[item].extent);
                break;
            }
            case LayoutInlineItemKind_OutOfFlow:
                f.leaves.push(LayoutLineLeaf{item, it.start, it.start, kLayoutUnitZero, 0, 0, 0,
                                             LayoutLineLeafKind_Placeholder, neutralLevel(it), false, content});
                break;
            case LayoutInlineItemKind_Float:
                f.leaves.push(LayoutLineLeaf{item, it.start, it.start, kLayoutUnitZero, 0, 0, 0,
                                             LayoutLineLeafKind_Anchor,
                                             neutralLevel(it), false, false});
                break;
            case LayoutInlineItemKind_Break: {
                content = true;
                const PrLayoutBoxStyle& cleared = layoutBoxStyle(state.styles, f.styles.data[item]);
                built.clears = LayoutLineSides(built.clears | layoutClearSides(cleared, space.mode.direction));
                break;
            }
            case LayoutInlineItemKind_Text:
                if (on.to > on.from) {
                    textLeaves(t, f, on, line.from.byte, hang, &logical);
                    content = true;
                    take(item, layoutTextOnLine(state, t, it, f.styles.data[item], on.from, on.to,
                                                aligner.align.data[item].extent));
                }
                break;
            default:
                break;
        }
    }
    // The boxes that are still open past the end of the line break here.
    for (uint32_t i = f.boxStack.count; i-- > 0;)
        closeBox(f.boxStack.data[i], kNoItem);

    // Relative positioning moves the parts of an inline box and everything in them, by its own offset plus the offsets
    // of the boxes around it (CSS2/#relative-positioning). It moves nothing else on the line. Each leaf moves with the
    // innermost box that holds it. The boxes are in the order they open, so an inner box follows the box around it.
    bool moves = false;
    f.boxMoves.resize(f.lineBoxes.count);
    f.depthMoves.clear();
    for (uint32_t b = 0; b < f.lineBoxes.count; ++b) {
        const LayoutLineBox& box = f.lineBoxes.data[b];
        LayoutLogicalOffset own = relativeOnLine(state, layoutBoxStyle(state.styles, f.styles.data[box.open]), space);
        LayoutLogicalOffset around = box.depth != 0 ? f.depthMoves.data[box.depth - 1] : kLayoutLogicalOffsetZero;
        f.boxMoves.data[b] = own + around;
        f.depthMoves.resize(box.depth + 1);
        f.depthMoves.data[box.depth] = f.boxMoves.data[b];
        moves = moves || f.boxMoves.data[b].inlineOffset != kLayoutUnitZero
                || f.boxMoves.data[b].blockOffset != kLayoutUnitZero;
    }
    if (moves) {
        f.leafMoves.resize(f.leaves.count);
        for (uint32_t i = 0; i < f.leaves.count; ++i)
            f.leafMoves.data[i] = kLayoutLogicalOffsetZero;
        for (uint32_t b = 0; b < f.lineBoxes.count; ++b) {
            const LayoutLineBox& box = f.lineBoxes.data[b];
            for (uint32_t i = box.first; i < box.end; ++i)
                f.leafMoves.data[i] = f.boxMoves.data[b];
        }
    }

    // Finds the visual order of the leaves, and the parts of each box in that order.
    uint32_t count = f.leaves.count;
    f.order.resize(count);
    f.visual.resize(count);
    if (t.bidi) {
        f.leafLevels.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            f.leafLevels.data[i] = f.leaves.data[i].level;
        prUnicodeBidiReorder(f.leafLevels.data, count, f.order.data);
    } else {
        for (uint32_t i = 0; i < count; ++i)
            f.order.data[i] = i;
    }
    for (uint32_t p = 0; p < count; ++p)
        f.visual.data[f.order.data[p]] = p;
    f.parts.clear();
    for (uint32_t b = 0; b < f.lineBoxes.count; ++b) {
        const LayoutLineBox& box = f.lineBoxes.data[b];
        uint32_t partsStart = f.parts.count;
        if (!t.bidi) {
            f.parts.push(
                LayoutLinePart{.box = b, .start = box.first, .end = box.end - 1, .left = kNoItem, .right = kNoItem});
        } else {
            f.positions.clear();
            for (uint32_t i = box.first; i < box.end; ++i)
                f.positions.push(f.visual.data[i]);
            sortItems(f.positions.data, f.positions.count, [](uint32_t x, uint32_t y) { return x < y; });
            // Adjacent leaves stay in one part if an unbroken line would keep them together.
            auto piece = [&](uint32_t p) {
                return layoutBidiSplitsBefore(t, box.open, f.leaves.data[f.order.data[p]].from);
            };
            for (uint32_t k = 0; k < f.positions.count; ++k) {
                uint32_t p = f.positions.data[k];
                if (k != 0 && p == f.parts.last().end + 1 && piece(p) == piece(p - 1))
                    f.parts.last().end = p;
                else
                    f.parts.push(LayoutLinePart{.box = b, .start = p, .end = p, .left = kNoItem, .right = kNoItem});
            }
        }
        // Puts the start edge of the box on its start-most part, and its end edge on its end-most part, by its own
        // direction.
        const PrLayoutFontStyle& font = layoutFontStyle(state.styles, f.styles.data[box.open]);
        bool rtl = layoutUsedMode(font).direction == PrTextDirection_Rtl;
        LayoutLinePart& leftmost = f.parts.data[partsStart];
        LayoutLinePart& rightmost = f.parts.last();
        uint32_t startEdge = box.starts ? box.open : kNoItem;
        if (rtl) {
            rightmost.right = startEdge;
            leftmost.left = box.close;
        } else {
            leftmost.left = startEdge;
            rightmost.right = box.close;
        }
    }
    // Text and tab leaves advance in visual order as far as they reach, each rounded up to a unit from where the
    // previous leaf ended. This way, dividing text moves no glyph, and a tab ends at its stop. An atomic inline, whose
    // box is in whole units, resets the carry. Placement takes the same steps.
    float measured = 0;
    for (uint32_t p = 0; p < count; ++p) {
        LayoutLineLeaf& leaf = f.leaves.data[f.order.data[p]];
        if (leaf.kind == LayoutLineLeafKind_Text || leaf.kind == LayoutLineLeafKind_Tab)
            leaf.advance = textAdvance(leaf, &measured);
        else if (leaf.kind == LayoutLineLeafKind_Atomic)
            measured = 0;
    }

    // The room that the content leaves: its width, less the white space that hangs. At a soft wrap all of that white
    // space hangs. At the end of a paragraph or before a forced break, only the part past the end of the line hangs.
    PrLayoutUnit width = edges;
    PrLayoutUnit hanging = kLayoutUnitZero;
    for (uint32_t i = 0; i < f.leaves.count; ++i) {
        width += f.leaves.data[i].advance;
        if (f.leaves.data[i].hangs)
            hanging += f.leaves.data[i].advance;
    }
    PrLayoutUnit room = place.size - place.indent;
    PrLayoutUnit used = width;
    if (hanging > kLayoutUnitZero) {
        PrLayoutUnit before = width - hanging;
        bool soft = !isFinal(t, f.segment, line.from.byte, end) && !endsForced(t, end);
        used = soft ? before : layoutUnitMax(before, layoutUnitMin(width, room));
    }
    PrLayoutUnit free = layoutUnitMax(kLayoutUnitZero, room - used);
    LineShares shares{};
    if (align == PrLayoutTextAlign_Justify) {
        layoutJustifyFind(state, f.justifier, t, f.styles.data, aligner.containerStyle, line);
        const Array<LayoutJustifyOpportunity>& found = f.justifier.opportunities;
        if (found.count != 0 && free > kLayoutUnitZero) {
            shares = LineShares{found.data, found.count, free.raw / int32_t(found.count),
                                uint32_t(free.raw % int32_t(found.count))};
            free = kLayoutUnitZero;
        }
        PrLayoutTextAlign last = lastAlignOf(layoutFontStyle(state.styles, aligner.containerStyle));
        align = free > kLayoutUnitZero ? (last == PrLayoutTextAlign_Justify ? PrLayoutTextAlign_Center : last)
                                       : PrLayoutTextAlign_Start;
    }

    // Parts open in visual order, outermost first if several start at one leaf, and close innermost first. Without
    // bidi, each box has one part, which is already in that order, and the boxes close in the order they ended.
    if (t.bidi) {
        auto depth = [&](const LayoutLinePart& part) { return f.lineBoxes.data[part.box].depth; };
        sortItems(f.parts.data, f.parts.count, [&](const LayoutLinePart& a, const LayoutLinePart& b) {
            return a.start != b.start ? a.start < b.start : depth(a) < depth(b);
        });
        f.closing.resize(f.parts.count);
        for (uint32_t i = 0; i < f.parts.count; ++i)
            f.closing.data[i] = i;
        sortItems(f.closing.data, f.closing.count, [&](uint32_t a, uint32_t b) {
            const LayoutLinePart& x = f.parts.data[a];
            const LayoutLinePart& y = f.parts.data[b];
            return x.end != y.end ? x.end < y.end : depth(x) > depth(y);
        });
    } else {
        f.closing.clear();
        f.closing.append(f.closed.data, f.closed.count);
    }

    // Places each leaf in visual order from line-left, and each part around its leaves with its edges.
    Array<LayoutLogicalChild>& children = state.scratch->children;
    uint32_t mark = children.count;
    LayoutMode frame = layoutLineFrame(space.mode);
    LayoutSide lineLeft = layoutLineLeft(space.mode);
    PrLayoutUnit x = kLayoutUnitZero;
    float carry = 0;                // how far the placed text ends short of x, since advances round up to a unit
    uint32_t opening = 0;
    uint32_t closing = 0;
    f.placeholders.clear();
    if (t.bidi)
        f.leafChild.resize(count);
    uint32_t anchorsBefore = f.floats.anchors.count;
    for (uint32_t p = 0; p < count; ++p) {
        const LayoutLineLeaf& leaf = f.leaves.data[f.order.data[p]];
        bool rightToLeft = (leaf.level & 1) != 0;
        // A gap at its start, on the side from which the order of its text starts.
        bool empty = leaf.kind == LayoutLineLeafKind_Empty || leaf.kind == LayoutLineLeafKind_Placeholder
                     || leaf.kind == LayoutLineLeafKind_Break;
        int32_t gap = empty ? 0 : shares.gapAt(leaf.from);
        if (!rightToLeft)
            x.raw += gap;
        for (; opening < f.parts.count && f.parts.data[opening].start == p; ++opening) {
            LayoutLinePart& part = f.parts.data[opening];
            part.child = children.count;
            children.push(LayoutLogicalChild{});
            part.x = x;
            if (part.left != kNoItem)
                x += f.advances.data[part.left];
        }
        PrLayoutStyleIndex style = f.styles.data[leaf.item];
        uint32_t placed = children.count;
        uint32_t made = children.count;
        switch (leaf.kind) {
            case LayoutLineLeafKind_Text:
                x += placeTextLeaf(state, f, t, leaf, style, x, layoutAlignedBaseline(aligner, leaf.item), space.mode,
                                   shares, &carry);
                break;
            case LayoutLineLeafKind_Atomic: {
                // Relative positioning moves an atomic inline from its place, and moves nothing else on the line.
                LayoutAtomic& atomic = f.atomics.data[leaf.item];
                LayoutLogicalOffset offset{x + atomic.margin[lineLeft],
                                           layoutAlignedBaseline(aligner, leaf.item) - atomic.baseline.offset};
                children.push(LayoutLogicalChild{atomic.fragment, offset + atomic.relative});
                atomic.fragment = nullptr;
                x += leaf.advance;
                carry = 0;
                break;
            }
            case LayoutLineLeafKind_Tab: x += textAdvance(leaf, &carry); break;
            case LayoutLineLeafKind_Break:
                placeBreakLeaf(state, t, leaf, style, x, layoutAlignedBaseline(aligner, leaf.item), space.mode);
                break;
            case LayoutLineLeafKind_Placeholder: {
                // Its position is set once the extent of the line is known.
                LayoutNodeIndex node = t.items.data[leaf.item].node;
                LayoutLogicalOffset move = moves ? f.leafMoves.data[f.order.data[p]] : kLayoutLogicalOffsetZero;
                f.placeholders.push(LayoutLinePlaceholder{children.count, move, leaf.contentBefore});
                LayoutFragment* placeholder =
                    layoutFragmentCreatePlaceholder(node, layoutStaticEdges(space.mode), layoutIsFixed(state.tree,
                                                                                                       node));
                children.push(LayoutLogicalChild{placeholder, LayoutLogicalOffset{x, kLayoutUnitZero}});
                placed = children.count;
                break;
            }
            case LayoutLineLeafKind_Anchor: {
                // The anchor carries the flags of the float that a walk looks for, taken from the fragment of the float
                // wherever that fragment currently is.
                LayoutItemIndex item = leaf.item;
                const LayoutFragment* floated = f.floats.child.data[item] != kNoItem
                                                    ? children.data[f.floats.child.data[item]].fragment
                                                    : f.atomics.data[item].fragment;
                LayoutFragmentFlags flags = floated != nullptr && !(f.floats.placed.data[item]
                                                                    && f.floats.child.data[item] == kNoItem)
                                                ? floated->flags
                                                : 0;
                LayoutFragment* anchor = layoutFragmentCreateAnchor(t.items.data[item].node, flags);
                f.floats.anchors.push(LayoutLineAnchor{anchor, item, kNoItem});
                children.push(LayoutLogicalChild{anchor, LayoutLogicalOffset{x, kLayoutUnitZero}});
                break;
            }
            default: break;
        }
        if (t.bidi)
            f.leafChild.data[f.order.data[p]] = children.count > made ? made : kNoItem;
        if (moves) {
            for (uint32_t i = placed; i < children.count; ++i)
                children.data[i].offset += f.leafMoves.data[f.order.data[p]];
        }
        if (rightToLeft)
            x.raw += gap;
        for (; closing < f.closing.count && f.parts.data[f.closing.data[closing]].end == p; ++closing) {
            LayoutLinePart& part = f.parts.data[f.closing.data[closing]];
            if (part.right != kNoItem)
                x += f.advances.data[part.right];
            part.made = makePart(state, f, t, part, x, space, !content);
            // Without bidi, the single part of a box precedes its content, which ends here.
            if (!t.bidi) {
                const LayoutLineBox& box = f.lineBoxes.data[part.box];
                part.made->part.opens = box.starts;
                part.made->part.closes = box.close != kNoItem;
                part.made->part.enclosed = children.count - part.child - 1;
            }
            if (moves)
                children.data[part.child].offset += f.boxMoves.data[part.box];
        }
    }

    // The content goes where the alignment puts it from its start edge, past the indent. The white space that hangs
    // goes past its end, which is on the right in an ltr paragraph and on the left in an rtl paragraph, where L1 put
    // it.
    PrLayoutUnit start = alignOffset(align, direction, free);
    PrLayoutUnit offset = direction == PrTextDirection_Ltr ? place.indent + start
                                                             : place.size - place.indent - start - x;
    built.phantom = !content;
    if (built.phantom)
        extent = LayoutLineExtent{};
    // The children were placed relative to the baseline, which is now known, and from the start of the line.
    for (uint32_t i = mark; i < children.count; ++i) {
        children.data[i].offset.blockOffset += extent.above;
        children.data[i].offset.inlineOffset += offset;
    }
    PrLayoutUnit blockSize = extent.above + extent.below;
    // If its box is inline-level, the static position of a placeholder is at the block-start edge of the line, at its
    // place along the line. If its box is block-level, the static position is at the inline start of the content box,
    // and on the next line if content of the line comes before it (css-position-3/#staticpos-rect).
    bool overIsStart = layoutLineOverIsBlockStart(space.mode);
    PrLayoutUnit lineStart = overIsStart ? kLayoutUnitZero : blockSize;
    PrLayoutUnit lineEnd = overIsStart ? blockSize : kLayoutUnitZero;
    PrLayoutUnit contentStart = space.mode.direction == PrTextDirection_Rtl ? place.at.inlineOffset + place.size
                                                                            : kLayoutUnitZero - place.at.inlineOffset;
    for (uint32_t i = 0; i < f.placeholders.count; ++i) {
        const LayoutLinePlaceholder& placeholder = f.placeholders.data[i];
        LayoutLogicalChild& child = children.data[placeholder.child];
        LayoutNodeIndex node = child.fragment->node;
        const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
        if (box.staticInline) {
            child.offset = LayoutLogicalOffset{child.offset.inlineOffset, lineStart} + placeholder.move;
            continue;
        }
        PrLayoutUnit block = placeholder.contentBefore ? lineEnd : lineStart;
        child.offset = LayoutLogicalOffset{contentStart, block} + placeholder.move;
    }
    // Without bidi, the children are already in tree order.
    if (t.bidi)
        treeOrder(state.styles, f, mark, children);
    LayoutLogicalSize size{place.size, blockSize};
    state.stats->lineChildrenMade += children.count - mark;
    LayoutFragment* fragment =
        layoutFragmentBuild(children, mark, LayoutFragmentKind_Line, kPrLayoutNodeNone, frame, size);
    layoutFragmentSetOverflow(state.tree, fragment);
    for (uint32_t i = anchorsBefore; i < f.floats.anchors.count; ++i)
        f.floats.anchors.data[i].line = children.count;
    children.push(LayoutLogicalChild{fragment, place.at});
    built.extent = extent;
    at = next;
    // The set of boxes open past the line changed, so the next line is collected afresh.
    f.lineCollected = false;
    return built;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lines beside floats

// Places the float of `item` at or below `top`, and returns its margin box.
LayoutExclusion placeFloat(const LayoutState& state, const LayoutInlineFrame& f, LayoutItemIndex item, PrLayoutUnit top,
                           const LayoutSpace& space, const LayoutInlineBfc& bfc) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, f.styles.data[item]);
    return layoutFloatPlace(box, f.atomics.data[item], bfc.containing, space.mode, top);
}

// Appends the fragment of the float of `item`, placed at `placed`, at its position from the corner of the context.
void pushFloat(LayoutState& state, LayoutInlineFrame& f, LayoutItemIndex item, const LayoutExclusion& placed,
               const LayoutSpace& space, const LayoutInlineBfc& bfc) {
    LayoutAtomic& atomic = f.atomics.data[item];
    LayoutLogicalOffset at = layoutFloatOffset(atomic, placed, bfc.containing, space.mode, bfc.top);
    f.floats.child.data[item] = state.scratch->children.count;
    state.scratch->children.push(LayoutLogicalChild{atomic.fragment, at});
    atomic.fragment = nullptr;
    f.floats.placed.data[item] = true;
}

// Returns the offset of the start of a line in `span` from the starting content edge of the block, in px. Tab stops are
// measured from that edge.
float originOf(const LayoutSpan& span, const LayoutSpace& space, const LayoutInlineBfc& bfc) {
    PrLayoutUnit start = layoutInlineStartFromLeft(bfc.containing, space.mode.direction, span.left,
                                                   span.right - span.left);
    return prLayoutUnitToPx(start);
}

// Breaks the line from `at`, whose content starts `indent` along it, in the space beside the floats over [y, y + h).
// Places on the line each float reached before its break, if the line has nothing before the float or the float fits in
// what the line has left. Placing a float narrows the space, and the line breaks again in it. The floats that do not
// fit, the forced ones, and the floats after them go after the line. Returns the end of the line, and sets `span` to
// the space it was broken in.
uint32_t breakBesideFloats(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t, LayoutLineStart at,
                           PrLayoutUnit indent, PrLayoutUnit y, PrLayoutUnit h, const LayoutSpace& space,
                           const LayoutInlineBfc& bfc, LayoutSpan& span) {
    LayoutInlineFloats& floats = f.floats;
    float indentPx = prLayoutUnitToPx(indent);
    span = layoutExclusionsAvailable(bfc.containing, y, h);
    float width = prLayoutUnitToPx(span.right - span.left) - indentPx;
    for (uint32_t next = floats.next.data[at.item];;) {
        float origin = originOf(span, space, bfc) + indentPx;
        uint32_t end = breakLine(state.styles, t, f, at.byte, origin, width);
        while (next < f.segment.itemEnd && floats.placed.data[next])
            next = floats.next.data[next + 1];
        if (next >= f.segment.itemEnd || !isOnLine(t, at.byte, end, t.items.data[next].start))
            return end;
        LayoutItemIndex item = next;
        next = floats.next.data[next + 1];
        uint32_t position = t.items.data[item].start;
        float before = position > at.byte ? widthOf(t, f, at.byte, position, origin) : 0;
        float inlineSize = prLayoutUnitToPx(layoutAtomicMarginBox(f.atomics.data[item], space.mode).inlineSize);
        if (floats.deferred.count == 0 && floats.forced.find(item) == kArrayMissing
            && (before <= 0 || before + inlineSize <= width + kFitTolerance)) {
            floats.onLine.push(LayoutInlinePlaced{item, placeFloat(state, f, item, y, space, bfc)});
            span = layoutExclusionsAvailable(bfc.containing, y, h);
            width = prLayoutUnitToPx(span.right - span.left) - indentPx;
        } else {
            floats.deferred.push(item);
        }
    }
}

// Where a line goes: its block-start edge, the space beside the floats that it was broken in, and its end.
struct LineFit {
    PrLayoutUnit y;
    LayoutSpan span;
    uint32_t end;
};

// Finds where the line from `at`, whose content starts `indent` along it, goes at or below `y`. The line breaks in the
// space beside the floats at its block offset, at first over the height of the strut. A float placed on the line
// narrows that space. A line that turns out taller, or that fits nowhere, breaks again. The floats of the frame record
// the floats that the line places beside it and after it.
LineFit fitLine(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t, LayoutLineStart at,
                PrLayoutUnit indent, PrLayoutUnit y, LayoutLineExtent strut, const LayoutSpace& space,
                const LayoutInlineBfc& bfc) {
    LayoutExclusions& exclusions = *bfc.containing.exclusions;
    LayoutExclusionMark mark = exclusions.mark;
    LayoutInlineFloats& floats = f.floats;
    floats.forced.clear();
    PrLayoutUnit h = strut.above + strut.below;
    LineFit fit{y, LayoutSpan{}, at.byte};
    // Every retry makes progress: the line moves down, turns out taller, or defers more of the floats it placed. None
    // of these is ever undone, and the floats that the line meets bound each of them.
    PrLayoutUnit triedY = fit.y;
    PrLayoutUnit triedH = h;
    uint32_t triedForced = 0;
    for (bool initial = true;; initial = false) {
        PR_ASSERT(initial || fit.y.raw > triedY.raw || h.raw > triedH.raw || floats.forced.count > triedForced);
        PR_ASSERT(fit.y.raw >= triedY.raw && h.raw >= triedH.raw && floats.forced.count >= triedForced);
        triedY = fit.y;
        triedH = h;
        triedForced = floats.forced.count;
        ++state.stats->linesBroken;
        layoutExclusionsCut(exclusions, mark);
        floats.onLine.clear();
        floats.deferred.clear();
        fit.end = breakBesideFloats(state, f, t, at, indent, fit.y, h, space, bfc, fit.span);
        // A float placed on the line that the final break of the line no longer reaches goes after the line.
        bool again = false;
        for (uint32_t i = 0; i < floats.onLine.count; ++i) {
            if (!isOnLine(t, at.byte, fit.end, t.items.data[floats.onLine.data[i].item].start)) {
                floats.forced.push(floats.onLine.data[i].item);
                again = true;
            }
        }
        if (again)
            continue;
        // If not even the first piece of the line fits beside the floats, the line moves down to the first offset at
        // which it fits. The piece is measured where it starts now, because the stops of its tabs depend on that, and
        // one search finds where it fits. The floats placed on the line stay where they are.
        float indentPx = prLayoutUnitToPx(indent);
        float width = prLayoutUnitToPx(fit.span.right - fit.span.left) - indentPx;
        float piece = fit.span.free ? 0 : widthOf(t, f, at.byte, fit.end, originOf(fit.span, space, bfc) + indentPx);
        if (!fit.span.free && piece > width + kFitTolerance) {
            for (uint32_t i = 0; i < floats.onLine.count; ++i)
                pushFloat(state, f, floats.onLine.data[i].item, floats.onLine.data[i].box, space, bfc);
            floats.onLine.clear();
            mark = exclusions.mark;
            float needed = maxF32(0, piece + indentPx - kFitTolerance);
            fit.y = layoutExclusionsFirstFit(bfc.containing, fit.y, h, prLayoutUnitFromPxCeil(needed)).y;
            continue;
        }
        // A line taller than the space it was broken in breaks again in the space over its full height, if that space
        // is narrower. Only floats make the space narrower.
        if (exclusions.floats.count == 0)
            return fit;
        LayoutLineExtent extent = alignLine(state, f, t, at, fit.end, strut);
        PrLayoutUnit height = extent.above + extent.below;
        if (height.raw > h.raw) {
            LayoutSpan taller = layoutExclusionsAvailable(bfc.containing, fit.y, height);
            if (taller.left.raw > fit.span.left.raw || taller.right.raw < fit.span.right.raw) {
                h = height;
                continue;
            }
        }
        return fit;
    }
}

// Places the floats of a segment with no line where the margins before it resolve, or records them as waiting for the
// margins. With no line, they are anchored on no line, and are children of their parent in tree order.
void placeLooseFloats(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t, const LayoutSpace& space,
                      const LayoutInlineBfc& bfc, LayoutInlineResult& result) {
    for (uint32_t i = f.segment.itemStart; i < f.segment.itemEnd; ++i) {
        if (t.items.data[i].kind != LayoutInlineItemKind_Float)
            continue;
        LayoutLineSides side = layoutFloatSide(layoutBoxStyle(state.styles, f.styles.data[i]), space.mode.direction);
        if (!bfc.topFinal) {
            result.unplaced = LayoutLineSides(result.unplaced | side);
            continue;
        }
        pushFloat(state, f, i, placeFloat(state, f, i, bfc.top, space, bfc), space, bfc);
        result.placed = LayoutLineSides(result.placed | side);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Segments

// Returns true if a line of a container styled by `font` is indented (css-text-3/#text-indent-property). The first
// formatted line of the container is indented, and with each-line, so is a line after a forced break. hanging inverts
// which lines are indented.
bool indents(const PrLayoutFontStyle& font, bool firstFormatted, bool afterForced) {
    bool each = (font.textIndentFlags & PrLayoutTextIndentFlag_EachLine) != 0;
    bool affected = firstFormatted || (each && afterForced);
    return (font.textIndentFlags & PrLayoutTextIndentFlag_Hanging) != 0 ? !affected : affected;
}

// Opens the boxes that the segment of the frame starts in, which are the boxes around the block-level box before it,
// outermost first. Each continues as a box left open by an earlier line does.
void openAcross(const LayoutInlineShaping& t, LayoutInlineFrame& f) {
    f.open.clear();
    if (f.segment.itemStart == 0)
        return;
    uint32_t block = f.segment.itemStart - 1;
    PR_ASSERT(t.items.data[block].kind == LayoutInlineItemKind_Block);
    const Array<LayoutInlineAlign>& align = f.aligner.align;
    for (uint32_t box = align.data[block].parent; box != kLayoutInlineRoot; box = align.data[box].parent)
        f.open.push(box);
    reverseItems(f.open.data, f.open.count);
}

// The strut of a line (CSS2/#line-height): the extent of its container's text about the dominant baseline, that extent
// with the outside markers that the first line that is not phantom holds, and the offset of the dominant baseline below
// the alphabetic baseline.
struct LineStrut {
    LayoutLineExtent extent;
    LayoutLineExtent marked;
    PrLayoutUnit dominant;
};

// Lays out the segment of the frame, as layoutInlineSegment describes.
LayoutInlineResult layoutLines(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t,
                               uint32_t container, const LayoutSpace& space, const LayoutInlineBfc& bfc) {
    LayoutInlineResult result{};
    const LayoutInlineSegment& segment = f.segment;
    PrLayoutStyleIndex containerStyle = layoutTreeNode(state.tree, container).style;
    // A segment with no text still makes lines if it holds the edge of an inline box or a box out of flow. These are
    // phantom lines, unless an edge has an inline-axis margin, border or padding. They place the parts of the boxes,
    // which every inline box has on the lines where it starts and ends (CSS2/#anonymous-block-level), the static
    // positions, and the anchors of the floats with them. Floats alone make no line.
    bool content = segment.byteEnd != segment.byteStart;
    bool placeholders = false;
    bool lined = content;
    for (uint32_t i = segment.itemStart; i < segment.itemEnd; ++i) {
        LayoutInlineItemKind kind = t.items.data[i].kind;
        content = content || f.edges.data[i] || kind == LayoutInlineItemKind_Atomic
                  || kind == LayoutInlineItemKind_Break;
        placeholders = placeholders || kind == LayoutInlineItemKind_OutOfFlow;
        lined = lined || (kind != LayoutInlineItemKind_Text && kind != LayoutInlineItemKind_Float);
    }
    f.floats.anchors.clear();
    if (!lined) {
        // The floats of the segment go where the margins before it resolve, once they resolve.
        placeLooseFloats(state, f, t, space, bfc, result);
        return result;
    }
    // If nothing on the lines resolves the margins before them, their floats go where the margins resolve, once they
    // resolve. Until then the anchors of the floats refer to nothing.
    if (!content && !bfc.topFinal) {
        for (uint32_t i = segment.itemStart; i < segment.itemEnd; ++i) {
            if (t.items.data[i].kind != LayoutInlineItemKind_Float)
                continue;
            const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, f.styles.data[i]);
            result.unplaced = LayoutLineSides(result.unplaced | layoutFloatSide(box, space.mode.direction));
            f.floats.placed.data[i] = true;
        }
    }
    if (!bfc.mayResolve && (content || placeholders)) {
        result.hasLines = true;
        result.stopped = true;
        return result;
    }
    measure(t, f, f.advances.data);
    // Each line holds at least the strut of the text of its container (CSS2/#line-height), about its dominant baseline.
    // The line exports its alphabetic baseline, measured from the block-start edge of the context. The first line that
    // is not phantom also holds the outside markers aligned on its alphabetic baseline, as the strut is held, which
    // keeps the line's place beside floats tall enough for them.
    auto strutOf = [&](PrLayoutStyleIndex style) {
        LayoutLineExtent extent = layoutTextExtent(state, style);
        LayoutLineExtent marked = extent;
        PrLayoutUnit dominant = layoutDominantOffset(state, style);
        if (!layoutLineExtentIsEmpty(bfc.marker))
            layoutLineExtend(marked, LayoutLineExtent{bfc.marker.above + dominant, bfc.marker.below - dominant});
        return LineStrut{extent, marked, dominant};
    };
    LineStrut strut = strutOf(containerStyle);
    bool overIsStart = layoutLineOverIsBlockStart(space.mode);
    // The first segment that makes lines aligns the items of the run. A segment of only white space makes no lines.
    if (!f.aligned) {
        layoutAlignItems(state, f.aligner, t, f.styles.data, f.atomics.data, containerStyle, strut.extent, space.mode);
        f.aligned = true;
    }
    const PrLayoutFontStyle& font = layoutFontStyle(state.styles, containerStyle);
    PrLayoutUnit indent = font.textIndent.bits == prLayoutLengthPx(0).bits
                              ? kLayoutUnitZero
                              : layoutResolve(state.styles, font.textIndent, space.containing.inlineSize);

    PrLayoutUnit y = bfc.top;
    LayoutLineStart at{segment.byteStart, segment.itemStart, 0};
    uint32_t lines = 0;
    bool afterForced = false;
    openAcross(t, f);
    f.lineCollected = false;
    f.words.end = 0;
    // Lays out the line from `at` with the frame `lf` and the shaping `lt`, beside the strut `ls`.
    auto layLine = [&](LayoutInlineFrame& lf, const LayoutInlineShaping& lt, const LineStrut& ls) {
        LayoutLineStart lineFrom = at;
        PrLayoutUnit lineIndent =
            indents(font, lines == 0 && bfc.firstFormatted, lines != 0 && afterForced) ? indent : kLayoutUnitZero;
        LayoutLineExtent lineStrut = result.hasLines ? ls.extent : ls.marked;
        LineFit fit = fitLine(state, lf, lt, at, lineIndent, y, lineStrut, space, bfc);
        PrLayoutUnit lineSize = fit.span.right - fit.span.left;
        LayoutLogicalOffset lineAt{
            layoutInlineStartFromLeft(bfc.containing, space.mode.direction, fit.span.left, lineSize), fit.y - bfc.top};
        bool lastLike = isFinal(lt, segment, at.byte, fit.end) || endsForced(lt, fit.end);
        LinePlace place{lineAt, lineSize, lineIndent, lastLike ? lastAlignOf(font) : font.textAlign};
        LineBuilt built = buildLine(state, lf, lt, at, fit.end, lineStrut, place, space, !content);
        PrLayoutUnit blockSize = built.extent.above + built.extent.below;
        if (!built.phantom) {
            PrLayoutUnit alphabetic = built.extent.above - ls.dominant;
            result.lastBaseline = fit.y - bfc.top + (overIsStart ? alphabetic : blockSize - alphabetic);
            if (!result.hasLines)
                result.firstBaseline = result.lastBaseline;
            result.hasLines = true;
        }
        for (uint32_t i = 0; i < lf.floats.onLine.count; ++i)
            pushFloat(state, lf, lf.floats.onLine.data[i].item, lf.floats.onLine.data[i].box, space, bfc);
        // Where the line goes depends on the floats over its whole height. The reads record that range whether or not a
        // float meets it now, because a float placed there later would move the line.
        layoutReadsRange(*bfc.containing.reads, fit.y, fit.y + blockSize);
        y = fit.y + blockSize;
        // The floats that did not fit go after the line, in order.
        for (uint32_t i = 0; i < lf.floats.deferred.count; ++i) {
            LayoutItemIndex item = lf.floats.deferred.data[i];
            pushFloat(state, lf, item, placeFloat(state, lf, item, y, space, bfc), space, bfc);
        }
        // A line break that clears puts what follows it below the floats on those sides, or at the end of the context
        // if nothing follows (CSS2/#flow-control). The floor is a read, which changes if a float before it moves.
        if (built.clears != 0) {
            PrLayoutUnit floor = layoutExclusionsClearance(bfc.containing, built.clears, y);
            if (floor != kLayoutOffsetNone)
                y = layoutUnitMax(y, floor);
        }
        afterForced = endsForced(lt, fit.end);
        ++lines;
        // Each line takes at least one byte or item, and the last line takes every remaining item.
        PR_ASSERT(at.byte > lineFrom.byte || at.item > lineFrom.item);
    };
    // The first formatted line of the container is laid out in the records of that line
    // (prLayoutNodeSetFirstLineStyle), from the context's shaping in them, with a frame of its own. Its fonts may break
    // it elsewhere than the container's own records would. The lines after it continue from where it ends, with the
    // boxes it leaves open and the floats it placed.
    if (bfc.firstFormatted && t.firstLine != nullptr && segment.itemStart == 0) {
        const LayoutInlineShaping& lt = *t.firstLine;
        PrLayoutStyleIndex lineStyle = layoutNodeLineStyle(state.tree, container);
        LayoutInlineFrame& lf = enterFrame(state.scratch);
        lf.words.end = 0;
        lf.segment = f.segment;
        resolveItems(state, lf, lt, space);
        startFloats(lf.floats, lt);
        lf.floats.anchors.clear();
        LineStrut lineStrut = strutOf(lineStyle);
        layoutAlignItems(state, lf.aligner, lt, lf.styles.data, lf.atomics.data, lineStyle, lineStrut.extent,
                         space.mode);
        lf.aligned = true;
        measure(lt, lf, lf.advances.data);
        lf.open.clear();
        lf.lineCollected = false;
        layLine(lf, lt, lineStrut);
        f.open.clear();
        f.open.append(lf.open.data, lf.open.count);
        for (uint32_t i = 0; i < lt.items.count; ++i) {
            if (!lf.floats.placed.data[i])
                continue;
            f.floats.placed.data[i] = true;
            f.floats.child.data[i] = lf.floats.child.data[i];
        }
        f.floats.anchors.append(lf.floats.anchors.data, lf.floats.anchors.count);
        releaseAtomics(lf);
        leaveFrame(state.scratch);
    }
    // Lines continue while items remain. The items after a forced break at the end of the text go on a line of their
    // own.
    while (lines == 0 || at.byte < segment.byteEnd || at.item < segment.itemEnd)
        layLine(f, t, strut);
    state.stats->linesLaidOut += lines;
    // Floats placed beside lines that are all phantom adjoin the margins around them, because nothing on the lines
    // resolved those margins.
    if (!result.hasLines) {
        for (uint32_t i = segment.itemStart; i < segment.itemEnd; ++i) {
            if (t.items.data[i].kind != LayoutInlineItemKind_Float || f.floats.child.data[i] == kNoItem)
                continue;
            const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, f.styles.data[i]);
            result.placed = LayoutLineSides(result.placed | layoutFloatSide(box, space.mode.direction));
        }
    }
    // Each anchor finds the fragment of its float, which its line or a later line placed.
    for (uint32_t i = 0; i < f.floats.anchors.count; ++i) {
        const LayoutLineAnchor& a = f.floats.anchors.data[i];
        uint32_t child = f.floats.child.data[a.item];
        if (child != kNoItem)
            a.anchor->anchor.sibling = int32_t(child) - int32_t(a.line);
    }
    result.blockSize = y - bfc.top;
    result.hasBaselines = result.hasLines;
    return result;
}

// Measures the segment of the frame, as layoutInlineSegmentIntrinsic describes.
LayoutIntrinsicSizes measureLines(LayoutState& state, LayoutInlineFrame& f, const LayoutInlineShaping& t,
                                  uint32_t container, const LayoutIntrinsicInput& input, bool firstFormatted) {
    const LayoutInlineSegment& segment = f.segment;
    // The indent, with a percentage taken as zero (css-text-3/#valdef-text-indent-percentage), widens the first piece
    // of each line that it indents.
    const PrLayoutFontStyle& font = layoutFontStyle(state.styles, layoutTreeNode(state.tree, container).style);
    float indent = prLayoutUnitToPx(layoutResolve(state.styles, font.textIndent, kLayoutUnitZero));
    auto indentAt = [&](uint32_t start) {
        bool first = start == segment.byteStart;
        bool afterForced = !first && t.breakAt.data[start] == PrUnicodeLineBreak_Mandatory;
        return indents(font, first && firstFormatted, afterForced) ? indent : 0;
    };

    // Resolves the edges of the inline boxes, with cyclic percentages against zero
    // (css-sizing-3/#cyclic-percentage-contribution), and takes the contributions of the atomic inlines and floats.
    // Min-content contributions go in `advances`, and max-content contributions in `maxAdvances`. A float adds nothing
    // to a line at min-content, because its min-content contribution stands alone.
    sizeItems(f, t);
    f.maxAdvances.resize(t.items.count);
    bool reads = false;
    PrLayoutUnit edges = kLayoutUnitZero;       // edges of every inline box: the width if there is no text
    PrLayoutUnit floatMin = kLayoutUnitZero;
    for (uint32_t i = segment.itemStart; i < segment.itemEnd; ++i) {
        const LayoutInlineItem& item = t.items.data[i];
        f.styles.data[i] = itemStyle(state, item);
        f.advances.data[i] = kLayoutUnitZero;
        f.margins.data[i] = kLayoutUnitZero;
        f.edges.data[i] = false;
        f.maxAdvances.data[i] = kLayoutUnitZero;
        if (item.kind == LayoutInlineItemKind_Atomic || item.kind == LayoutInlineItemKind_Float) {
            // An anonymous inline table has no edges, so its contribution is its intrinsic sizes.
            LayoutIntrinsicSizes contribution = item.wraps
                                                    ? layoutTableIntrinsic(state, item.node, item.runLast, input)
                                                    : layoutContribution(state, item.node, input);
            if (item.kind == LayoutInlineItemKind_Float)
                floatMin = layoutUnitMax(floatMin, contribution.minContent);
            else
                f.advances.data[i] = contribution.minContent;
            f.maxAdvances.data[i] = contribution.maxContent;
            reads = reads || contribution.readsPercentBlock;
        } else if (item.kind == LayoutInlineItemKind_Open || item.kind == LayoutInlineItemKind_Close) {
            resolveEdge(state, f, t, i, input.mode, kLayoutUnitZero);
            f.maxAdvances.data[i] = f.advances.data[i];
            edges += f.advances.data[i];
        }
    }
    LayoutMode mode = input.mode;
    auto clearsAt = [&](uint32_t i) {
        return t.items.data[i].kind == LayoutInlineItemKind_Float
               && layoutClearSides(layoutBoxStyle(state.styles, f.styles.data[i]), mode.direction) != 0;
    };
    // By byte of the segment: true if a float that clears comes before the byte.
    Array<bool>& clearing = f.clearing;
    clearing.resize(t.text.count + 1);
    for (uint32_t b = segment.byteStart; b <= segment.byteEnd; ++b)
        clearing.data[b] = false;
    for (uint32_t i = segment.itemStart; i < segment.itemEnd; ++i) {
        if (clearsAt(i))
            clearing.data[t.items.data[i].start] = true;
    }
    // Returns the widest piece between the breaks it takes, with `advances` by item. It breaks at every opportunity for
    // min-content (`soft`), and only at forced breaks for max-content. A trailing collapsible space hangs, because
    // widthOf leaves it out.
    auto widest = [&](bool soft, const PrLayoutUnit* advances) {
        if (segment.byteEnd == segment.byteStart) {
            PrLayoutUnit run = kLayoutUnitZero;
            PrLayoutUnit widest = kLayoutUnitZero;
            for (uint32_t i = segment.itemStart; i < segment.itemEnd; ++i) {
                if (t.items.data[i].kind != LayoutInlineItemKind_Float)
                    continue;
                if (clearsAt(i))
                    run = kLayoutUnitZero;
                run += advances[i];
                widest = layoutUnitMax(widest, run);
            }
            return edges + widest;
        }
        measure(t, f, advances);
        float width = 0;
        uint32_t start = segment.byteStart;
        for (uint32_t b = segment.byteStart + 1; b <= segment.byteEnd; ++b) {
            PrUnicodeLineBreak at = t.breakAt.data[b];
            // overflow-wrap: anywhere's opportunities count at min-content (css-text-4/#valdef-overflow-wrap-anywhere).
            if (soft && at == PrUnicodeLineBreak_Prohibited && t.emergency.count != 0
                && t.emergency.data[b] == PrTextEmergency_Anywhere)
                at = PrUnicodeLineBreak_Allowed;
            // A float that clears ends the run of floats beside the line before it.
            bool last = b == segment.byteEnd;
            bool cleared = !soft && !last && clearing.data[b];
            if (!last && !cleared
                && (at == PrUnicodeLineBreak_Prohibited || (!soft && at != PrUnicodeLineBreak_Mandatory)))
                continue;
            // Each piece is measured as a line of its own, as the narrowest container would break it.
            float lineIndent = indentAt(start);
            width = maxF32(width, lineIndent + widthOf(t, f, start, b, lineIndent));
            start = b;
        }
        // Rounded up, so that a layout at this size breaks no more often than the measurement did.
        return prLayoutUnitFromPxCeil(width);
    };
    LayoutIntrinsicSizes sizes{};
    sizes.minContent = layoutUnitMax(widest(true, f.advances.data), floatMin);
    sizes.maxContent = layoutUnitMax(widest(false, f.maxAdvances.data), sizes.minContent);
    sizes.readsPercentBlock = reads;
    // The first formatted line is measured in its own records (prLayoutNodeSetFirstLineStyle): its first piece at
    // min-content, and the line through its first forced break at max-content. The items' advances are the same in both
    // shapings, which differ only in their glyphs.
    const LayoutInlineShaping* lt = t.firstLine;
    if (firstFormatted && lt != nullptr && segment.itemStart == 0 && segment.byteEnd != segment.byteStart) {
        uint32_t piece = segment.byteStart + 1;
        while (piece < segment.byteEnd && lt->breakAt.data[piece] == PrUnicodeLineBreak_Prohibited)
            ++piece;
        uint32_t line = piece;
        while (line < segment.byteEnd && lt->breakAt.data[line] != PrUnicodeLineBreak_Mandatory)
            ++line;
        float lineIndent = indentAt(segment.byteStart);
        measure(*lt, f, f.advances.data);
        float pieceWidth = lineIndent + widthOf(*lt, f, segment.byteStart, piece, lineIndent);
        measure(*lt, f, f.maxAdvances.data);
        float lineWidth = lineIndent + widthOf(*lt, f, segment.byteStart, line, lineIndent);
        sizes.minContent = layoutUnitMax(sizes.minContent, prLayoutUnitFromPxCeil(pieceWidth));
        sizes.maxContent = layoutUnitMax(sizes.maxContent, layoutUnitMax(sizes.minContent,
                                                                         prLayoutUnitFromPxCeil(lineWidth)));
    }
    return sizes;
}

// Sets the segment of the frame to segment `index` of the run.
void enterSegment(LayoutInlineRun& run, uint32_t index) {
    PR_ASSERT(index < run.shaping->segments.count);
    run.frame->segment = run.shaping->segments.data[index];
}

// Returns true if the lines that `record` keeps are the lines that the run of the frame lays out in `space` beside
// `bfc`. They must be laid out from the same collection of its shaping, in the same space, with each atomic inline laid
// out as it is now. A record holds the fragments of the atomic inlines on its lines, so that a fragment it names never
// belongs to a different box.
bool linesHold(const LayoutLineRecord& record, const LayoutInlineFrame& f, const LayoutInlineShaping& t,
               const LayoutSpace& space, const LayoutInlineBfc& bfc) {
    if (record.collected == 0 || record.collected != t.collected || record.mode != space.mode
        || record.inlineSize != space.containing.inlineSize || record.available != space.available
        || record.lineLeft != bfc.containing.lineLeft || record.lineRight != bfc.containing.lineRight
        || record.top != bfc.top || record.firstFormatted != bfc.firstFormatted || !(record.marker == bfc.marker)
        || record.atomics.count != t.items.count)
        return false;
    for (uint32_t i = 0; i < t.items.count; ++i) {
        if (t.items.data[i].kind != LayoutInlineItemKind_Atomic)
            continue;
        const LayoutLineAtomic& kept = record.atomics.data[i];
        const LayoutAtomic& now = f.atomics.data[i];
        if (kept.fragment != now.fragment || kept.relative.inlineOffset != now.relative.inlineOffset
            || kept.relative.blockOffset != now.relative.blockOffset)
            return false;
    }
    return true;
}

// Returns true if the lines that the run of the frame laid out as `result`, the scratch children from `mark`, would be
// laid out the same at any block size of their container. This requires that its single segment placed no float, and
// that no inline box or atomic inline is moved by a percentage of the block size, which relativeOnLine resolves
// separately from the atomic inline's own layout. An atomic inline whose layout reads the block size gives a different
// result at a different size, which linesHold compares.
bool linesKeep(const LayoutState& state, const LayoutInlineFrame& f, const LayoutInlineShaping& t,
               const LayoutSpace& space, const LayoutInlineResult& result, uint32_t mark) {
    if (t.segments.count != 1 || result.stopped)
        return false;
    for (uint32_t i = 0; i < t.items.count; ++i) {
        LayoutInlineItemKind kind = t.items.data[i].kind;
        if (kind == LayoutInlineItemKind_Float)
            return false;
        if (kind != LayoutInlineItemKind_Open && kind != LayoutInlineItemKind_Atomic)
            continue;
        const PrLayoutBoxStyle& box = layoutInlineItemBox(state.styles, t.items.data[i], f.styles.data[i]);
        if (box.position != PrLayoutPosition_Relative)
            continue;
        LayoutLogicalBox logical = layoutLogicalBox(box, space.mode);
        if (layoutLengthReadsBase(logical.inset[LayoutSide_BlockStart])
            || layoutLengthReadsBase(logical.inset[LayoutSide_BlockEnd]))
            return false;
    }
    const Array<LayoutLogicalChild>& children = state.scratch->children;
    for (uint32_t i = mark; i < children.count; ++i) {
        if (children.data[i].fragment->kind != LayoutFragmentKind_Line)
            return false;
    }
    return true;
}

// Starts `record` over for the lines that the run of the frame lays out in `space` beside `bfc`. Records its atomic
// inlines as they are laid out, before a line takes them.
void startLines(LayoutLineRecord& record, const LayoutInlineFrame& f, const LayoutInlineShaping& t,
                const LayoutSpace& space, const LayoutInlineBfc& bfc) {
    layoutLineRecordClear(record);
    record.mode = space.mode;
    record.inlineSize = space.containing.inlineSize;
    record.available = space.available;
    record.lineLeft = bfc.containing.lineLeft;
    record.lineRight = bfc.containing.lineRight;
    record.top = bfc.top;
    record.firstFormatted = bfc.firstFormatted;
    record.marker = bfc.marker;
    for (uint32_t i = 0; i < t.items.count; ++i) {
        const LayoutAtomic& atomic = f.atomics.data[i];
        record.atomics.push(LayoutLineAtomic{atomic.fragment, atomic.relative});
    }
}

// Keeps the lines laid out as `result`, the scratch children from `mark`, in `record`, which startLines started. Sets
// the collection of the shaping in the record, which makes the record valid.
void keepLines(LayoutLineRecord& record, const LayoutInlineShaping& t, const LayoutInlineResult& result,
               const Array<LayoutLogicalChild>& children, uint32_t mark) {
    record.collected = t.collected;
    record.hasLines = result.hasLines;
    record.hasBaselines = result.hasBaselines;
    record.blockSize = result.blockSize;
    record.firstBaseline = result.firstBaseline;
    record.lastBaseline = result.lastBaseline;
    for (uint32_t i = mark; i < children.count; ++i) {
        layoutFragmentRetain(children.data[i].fragment);
        record.lines.push(children.data[i]);
    }
}

} // namespace

LayoutInlineRun layoutInlineBegin(LayoutState& state, uint32_t container, uint32_t first, uint32_t last,
                                  const LayoutSpace& space) {
    LayoutInlineFrame& f = enterFrame(state.scratch);
    const LayoutInlineShaping& t = layoutShaping(state, f.own, first, last);
    resolveItems(state, f, t, space);
    startFloats(f.floats, t);
    f.aligned = false;
    return LayoutInlineRun{&f, &t, container, space};
}

LayoutInlineRun layoutInlineMeasureBegin(LayoutState& state, uint32_t first, uint32_t last) {
    LayoutInlineFrame& f = enterFrame(state.scratch);
    const LayoutInlineShaping& t = layoutShaping(state, f.own, first, last);
    return LayoutInlineRun{&f, &t, layoutTreeNode(state.tree, first).parent, LayoutSpace{}};
}

void layoutInlineEnd(LayoutState& state, LayoutInlineRun& run) {
    PR_ASSERT(state.scratch->inlines->frames.data[state.scratch->inlines->depth - 1] == run.frame);
    releaseAtomics(*run.frame);
    leaveFrame(state.scratch);
}

uint32_t layoutInlineSegmentCount(const LayoutInlineRun& run) {
    return run.shaping->segments.count;
}

uint32_t layoutInlineBlockAfter(const LayoutInlineRun& run, uint32_t segment) {
    PR_ASSERT(segment + 1 < run.shaping->segments.count);
    const LayoutInlineItem& block = run.shaping->items.data[run.shaping->segments.data[segment].itemEnd];
    PR_ASSERT(block.kind == LayoutInlineItemKind_Block);
    return block.node;
}

LayoutInlineResult layoutInlineSegment(LayoutState& state, LayoutInlineRun& run, uint32_t segment,
                                       const LayoutInlineBfc& bfc) {
    enterSegment(run, segment);
    const LayoutInlineShaping& t = *run.shaping;
    LayoutInlineFrame& f = *run.frame;
    Array<LayoutLogicalChild>& children = state.scratch->children;
    // Lines in the records of the first formatted line are laid out every time.
    bool reuses = bfc.reuses && state.reuse && t.collected != 0 && t.firstLine == nullptr;
    PR_ASSERT(!bfc.reuses || (bfc.topFinal && bfc.mayResolve));
    LayoutNodeIndex first = t.nodes.data[0];
    if (const LayoutLineRecord* kept = reuses ? layoutCacheFindLines(state.tree->cache, first) : nullptr;
        kept != nullptr && linesHold(*kept, f, t, run.space, bfc)) {
        for (uint32_t i = 0; i < kept->lines.count; ++i) {
            layoutFragmentRetain(kept->lines.data[i].fragment);
            children.push(kept->lines.data[i]);
        }
        state.stats->linesReused += kept->lines.count;
        LayoutInlineResult result{};
        result.blockSize = kept->blockSize;
        result.firstBaseline = kept->firstBaseline;
        result.lastBaseline = kept->lastBaseline;
        result.hasBaselines = kept->hasBaselines;
        result.hasLines = kept->hasLines;
        return result;
    }
    // The atomic inlines are recorded before the lines take them. Every one of them is on a line, which the record
    // holds, because the lines that a record keeps place no float.
    LayoutLineRecord* record = nullptr;
    if (reuses) {
        record = &layoutCacheLines(state.tree->cache, first);
        startLines(*record, f, t, run.space, bfc);
    }
    uint32_t mark = children.count;
    LayoutInlineResult result = layoutLines(state, f, t, run.container, run.space, bfc);
    if (record != nullptr && linesKeep(state, f, t, run.space, result, mark))
        keepLines(*record, t, result, children, mark);
    else if (record != nullptr)
        layoutLineRecordClear(*record);
    return result;
}

LayoutIntrinsicSizes layoutInlineSegmentIntrinsic(LayoutState& state, LayoutInlineRun& run, uint32_t segment,
                                                  const LayoutIntrinsicInput& input, bool firstFormatted) {
    enterSegment(run, segment);
    return measureLines(state, *run.frame, *run.shaping, run.container, input, firstFormatted);
}

void layoutInlineScratchDestroy(LayoutInlineScratch* PR_NULLABLE scratch) {
    if (scratch == nullptr)
        return;
    PR_ASSERT(scratch->depth == 0);
    for (uint32_t i = 0; i < scratch->frames.count; ++i)
        destroyFrame(scratch->frames.data[i]);
    scratch->frames.reset();
    prMemoryFree(scratch);
}

} // namespace pr_impl
