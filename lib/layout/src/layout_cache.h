// The layout cache: the result of the last layout of each box, and the intrinsic sizes from its last measurement. Both
// are reused while nothing in the subtree of the box has changed and the inputs are the same.
//
// A block-level box or an atomic inline keeps its last result with the space it was laid out in. A box in flow in a
// block formatting context that it does not establish also keeps what its subtree read of the floats of the context,
// and the floats it placed, measured from the block-start edge of its border box. What it read is the state when it
// began: the block range of its queries, the floors it read, and the floats in that range. It does not include the
// floats it placed, which are its output. A layout in the same space reuses the result wherever the box would read the
// same values, and places its floats again. If there were no floats in its range either time, a result does not depend
// on where the box is.

#pragma once

#include "pr/layout_tree.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

#include "layout/src/layout_exclusion.h"
#include "layout/src/layout_fragment.h"
#include "layout/src/layout_logical.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutAbsoluteBlock;
struct LayoutIntrinsicInput;
struct LayoutIntrinsicSizes;
struct LayoutRequests;
struct LayoutResult;
struct LayoutSpace;
struct LayoutState;
struct LayoutTree;

// ---------------------------------------------------------------------------------------------------------------------
// Entries

// The inputs that a table gives a cell beyond its size requests (LayoutSpace::cell).
//
// If the cell's row aligns it by its baseline (`aligned`), `baseline` is the offset from the cell's block-start border
// edge at which the cell's first baseline goes (css-tables-3/#row-layout). If the table collapses its borders
// (`collapsed`), `borders` are the cell's used borders by PrRectSide, which the table resolves among the cell and its
// neighbours (css-tables-3/#border-collapsing).
struct LayoutCellSpace {
    PrLayoutUnit baseline;
    PrLayoutUnit borders[PrRectSide_Count];
    bool aligned;
    bool collapsed;
};

// Returns true if two sets of cell inputs are the same in what each of them holds.
inline bool layoutCellSpacesEqual(const LayoutCellSpace& a, const LayoutCellSpace& b) {
    if (a.aligned != b.aligned || a.collapsed != b.collapsed || (a.aligned && a.baseline != b.baseline))
        return false;
    for (uint32_t side = 0; a.collapsed && side < PrRectSide_Count; ++side) {
        if (a.borders[side] != b.borders[side])
            return false;
    }
    return true;
}

// The axis along which a math parent asks an embellished operator to stretch (mathml-core/#box-model).
enum {
    LayoutMathStretchAxis_None,
    LayoutMathStretchAxis_Inline,       // to cover an inline size
    LayoutMathStretchAxis_Block,        // to cover an ink line-ascent and line-descent
};
typedef uint8_t LayoutMathStretchAxis;

// The stretch size constraint that a math parent lays out an embellished operator with (LayoutSpace::stretch). Along
// the inline axis, `size` is the inline size to cover. Along the block axis, `size` and `depth` are the ink line-ascent
// and line-descent to cover.
//
// An accent over a base taller than the font's FlattenedAccentBaseHeight is laid out `flattened`, and takes its glyph's
// flattened form (OpenType MATH, flac), along any axis or none.
//
// A number in an mstack is laid out with the inline offset of the origin of each of its glyphs from its inline-start
// edge, which puts each of its characters in its column (mathml4/#presm_mstack). The offsets are valid only during the
// layout that they are given to, and a cached result is keyed on their count and their hash.
//
// The fields before `columns` are the constraint's key, which is compared as bytes (layoutMathStretchesEqual). A field
// that the constraint does not use is zero, as `depth` is along the inline axis and both sizes are with no axis.
PR_NO_PADDING_BEGIN
struct LayoutMathStretch {
    PrLayoutUnit size;
    PrLayoutUnit depth;

    // The inline size that a row breaks its lines to fit (mathml4/#presm_linebreaking), or 0 for none. It is the width
    // of a formula's container, which a row that is a formula's only child takes from the formula.
    PrLayoutUnit lineWidth;

    uint32_t columnCount;
    uint64_t columnsHash;
    LayoutMathStretchAxis axis;
    bool flattened;
    uint8_t _pad[6];

    const PrLayoutUnit* PR_NULLABLE columns;
};
PR_NO_PADDING_END

// Returns true if the constraint asks the box that it lays out to stretch, to take another form, to set its characters
// in columns, or to break its lines.
inline bool layoutMathStretchIsActive(const LayoutMathStretch& stretch) {
    return stretch.axis != LayoutMathStretchAxis_None || stretch.flattened || stretch.columnCount != 0 ||
        stretch.lineWidth != PrLayoutUnit{0};
}

// Returns true if two stretch size constraints have equal keys.
inline bool layoutMathStretchesEqual(const LayoutMathStretch& a, const LayoutMathStretch& b) {
    return prMemEquals(&a, &b, offsetof(LayoutMathStretch, columns));
}

// What a cached result read beyond what its entry holds. If the result is not plain (LayoutCacheFlag_Plain), this is
// what it read of its block formatting context beyond its range, and the floats it placed there. If it read the
// fallback of its space (LayoutResultFlag_ReadsFallback), this holds that fallback. Offsets are from the block-start
// edge of its border box and the line-left edge of its containing block.
struct LayoutCacheContext {
    LayoutFloorRead clearanceRead[2];   // the floors it read
    LayoutFloorRead lastTopRead;
    bool clearance;                   // true if its border box was put at a clearance floor
    RawArray<LayoutExclusion> view;   // the floats that existed when it began, in the range it read, clipped to it
    RawArray<LayoutExclusion> placed; // the floats that its subtree placed, in placement order
    PrLayoutVec fallback;             // the fallback of its space (LayoutSpace::fallback)
    LayoutLineExtent marker;          // the outside markers of its space (LayoutSpace::marker)

    LayoutCellSpace cell;           // the inputs from the cell's table (LayoutSpace::cell)
    LayoutMathStretch stretch;      // the stretch size constraint of an embellished operator (LayoutSpace::stretch)
};

// Marks no context record (LayoutCacheEntry::context). The first record of the cache is never used.
constexpr uint32_t kLayoutCacheContextNone = 0;

// Marks no placement record (LayoutCacheEntry::absolute). The first record of the cache is never used.
constexpr uint32_t kLayoutCacheAbsoluteNone = 0;

// What the layout of a box found beyond its sizes (LayoutResult::flags). Its cache keeps these flags unchanged.
enum {
    // It has a first or a last baseline (css-align-3/#baseline-set). These are the baselines of its first and last
    // in-flow lines, at any depth of its block-level descendants, clamped to its border box if it is a scroll
    // container. A box laid out in a space of an orthogonal writing mode has none. Its fragment records them and their
    // sets (layoutFragmentBaseline).
    LayoutResultFlag_FirstBaseline = 1 << 0,
    LayoutResultFlag_LastBaseline = 1 << 1,

    // It is a scroll container that is not orthogonal to the space. Its parent takes its block-end margin edge as a
    // last baseline.
    LayoutResultFlag_Scrolls = 1 << 2,

    LayoutResultFlag_CollapsedThrough = 1 << 3,     // its block-start and block-end margins are adjoining

    // It, or a box in it, read the fallback of the space (LayoutSpace::fallback), which its cache then compares.
    LayoutResultFlag_ReadsFallback = 1 << 4,

    // It read the block size of its containing block or the size it stretches to (LayoutSpace::containing,
    // LayoutSpace::stretchBlock), which its cache then compares. The boxes in it read its own content box instead.
    LayoutResultFlag_ReadsBlockBase = 1 << 5,

    // Its inline size is what the available size leaves it, as an auto or stretched size that no limit clamps. Any
    // other available size gives it a different inline size (LayoutResolvedInline).
    LayoutResultFlag_FillsAvailable = 1 << 6,

    LayoutResultFlag_All = (1 << 7) - 1,
};
typedef uint8_t LayoutResultFlags;

// What the last result of a box was beyond its sizes (LayoutCacheEntry::flags). Holds its LayoutResultFlags unchanged,
// and what reusing it in its context reads.
enum {
    LayoutCacheFlag_InContext = 1 << 7,         // it was laid out in flow in a context it does not establish
    LayoutCacheFlag_Plain = 1 << 8,             // it saw and placed no floats, read no floor and had no clearance

    // What the requests of its space asked (LayoutRequests), which reuse compares, and what its measurement records
    // hold.
    LayoutCacheFlag_InlineFixed = 1 << 9,
    LayoutCacheFlag_BlockFixed = 1 << 10,
    LayoutCacheFlag_BlockContent = 1 << 11,
    LayoutCacheFlag_BlockDefinite = 1 << 12,
    LayoutCacheFlag_Measured = 1 << 13,     // a measurement record holds an answer (LayoutMeasureRecord)
    LayoutCacheFlag_SizeOnly = 1 << 14,     // a measurement record's answer has no baselines (LayoutMeasureKind_Size)

    // The space held outside markers, a table cell's baseline or borders, or a stretch size constraint. The context
    // record holds them (LayoutCacheContext::marker, LayoutCacheContext::cell, LayoutCacheContext::stretch).
    LayoutCacheFlag_SpaceExtras = 1 << 15,
    LayoutCacheFlag_Requests = LayoutCacheFlag_InlineFixed | LayoutCacheFlag_BlockFixed | LayoutCacheFlag_BlockContent |
        LayoutCacheFlag_BlockDefinite,
};
typedef uint16_t LayoutCacheFlags;
static_assert((LayoutResultFlag_All & (LayoutCacheFlag_InContext | LayoutCacheFlag_Plain)) == 0);

// The last result of a box and the space it was laid out in.
//
// Reuse reads every field, with these exceptions. It reads the inline size of the containing block only if the style of
// the box reads it. It reads the block size and the size the box stretches to only if the result read them
// (LayoutResultFlag_ReadsBlockBase). It reads the context record only if the result is not plain, read the fallback or
// was laid out with outside markers. Reuse compares either the available size or the inline size that the box resolves
// to from it (LayoutResolvedInline).
//
// The percentage bases of the space follow from the sizes of the containing block and its available size, together with
// the style of the box. For a box out of flow, they follow from the sizes of its containing block, from its automatic
// sizes as its insets determine them (a change to which marks the box), and from the size it stretches to, which a
// static position decides if both insets of its axis are auto.
PR_NO_PADDING_BEGIN
struct LayoutCacheEntry {
    // The space it was laid out in. A fixed inline size takes the place of the available size, because a box whose
    // container decides its inline size does not read the available size. A fixed block size takes the place of the
    // size it stretches to, which only a box out of flow reads (LayoutCacheFlag_Requests).
    PrLayoutUnit available;
    PrLayoutUnit containingInline;
    PrLayoutUnit containingBlock;

    PrLayoutUnit readLo;            // the block range its queries read; empty if readLo >= readHi
    PrLayoutUnit readHi;

    // If the result is not plain or read the fallback, what it read and placed of its context: a record of
    // LayoutCache::contexts, or kLayoutCacheContextNone. If the box is plain, the entry retains the record for the next
    // layout of the box.
    uint32_t context;

    const LayoutFragment* PR_NULLABLE fragment; // the entry holds a reference to it; null if there is none
    PrLayoutUnit startPositive;                 // its margin struts and placement (LayoutResult)
    PrLayoutUnit startNegative;
    PrLayoutUnit endPositive;
    PrLayoutUnit endNegative;
    PrLayoutUnit marginInlineStart;
    PrLayoutUnit stretchBlock;      // of the space it was laid out in, which a static position may decide
    LayoutMode mode;                // of the space it was laid out in
    LayoutCacheFlags flags;

    // For a box out of flow that its holder placed, the placement: a record of LayoutCache::absolutes, or
    // kLayoutCacheAbsoluteNone.
    uint32_t absolute;
};
PR_NO_PADDING_END

// Reuse reads one entry for each box that a layout reaches, so an entry fits in one cache line. Its size where a
// pointer is 64 bits:
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(LayoutCacheEntry) == 64);
#endif

// The intrinsic sizes that a measurement of a box gave (css-sizing-3/#intrinsic-sizes), and the inputs it was measured
// with. An inline formatting context keeps no entry of its own, because the entry of its container holds its
// measurement.
struct LayoutIntrinsicEntry {
    PrLayoutUnit minContent;        // border-box inline sizes
    PrLayoutUnit maxContent;
    PrLayoutUnit percentBlock;      // the block-axis percentage base it was measured with
    PrLayoutVec fallback;           // the fallback it was measured with (LayoutIntrinsicInput::fallback)
    LayoutMode mode;                // the mode it was measured in
    bool valid;
    bool measured;                  // true if its measurement records hold answers (LayoutCache::measureAt)
    bool readsPercentBlock;         // true if the sizes read percentBlock, so that another base may change them
    bool readsFallback;             // true if the sizes read the fallback
    bool fillsAvailable;            // same as LayoutIntrinsicSizes::fillsAvailable
};

// What a measurement asks of a box (layoutMeasure): its block size alone, or its baselines as well. A flex container
// solved for its size alone finds no baselines. Its baselines need measurements of their own at the used sizes of its
// items.
enum {
    LayoutMeasureKind_Size,
    LayoutMeasureKind_Baselines,
};
typedef uint8_t LayoutMeasureKind;

// The answer of a box to a measurement (layoutMeasure), and the space it was measured in, kept in the same form as a
// LayoutCacheEntry keeps its space. A record without LayoutCacheFlag_Measured holds no answer.
struct LayoutMeasureRecord {
    PrLayoutUnit available;         // the size an auto inline size filled, or the fixed inline size in its place
    PrLayoutUnit containingInline;
    PrLayoutUnit containingBlock;
    PrLayoutUnit stretchBlock;
    PrLayoutVec fallback;           // compared if its result read it
    LayoutMode mode;
    LayoutCacheFlags flags;         // the flags of its result and its requests, and LayoutCacheFlag_Measured

    PrLayoutUnit blockSize;         // the block size of its border box, in its own block axis
    LayoutBaselines baselines;      // as the box exports them in the space; the record owns their clamps
    LayoutCellSpace cell;           // the inputs that a table gave the measured cell
};

// The number of measurements a box keeps, newest first.
constexpr uint32_t kLayoutMeasureSlots = 2;

// Marks a box with no measurement records (LayoutCache::measureAt).
constexpr uint32_t kLayoutMeasureNone = 0xffffffffu;

// The layout of an atomic inline on the lines that a record keeps. The record can be reused only if the layout is the
// same. Holds the fragment of the atomic inline, which the lines of the record hold, and its relative offset.
struct LayoutLineAtomic {
    const LayoutFragment* PR_NULLABLE fragment;
    LayoutLogicalOffset relative;
};

// The lines that an inline formatting context last laid out, if its block container establishes the formatting context
// and holds nothing else (layoutInlineSegment). A layout of the container at another block size takes the lines again
// if nothing on them read that size, its shaping is the same collection, its space is the same, and each atomic inline
// has the same result. The first node of the context keeps the record.
struct LayoutLineRecord {
    uint32_t collected;             // the collection of the shaping (LayoutInlineShaping::collected)
    LayoutMode mode;                // mode of the content box the lines were laid out in
    PrLayoutUnit inlineSize;        // inline size of that content box
    PrLayoutUnit available;         // available size of that content box
    PrLayoutUnit lineLeft;          // line-left edge of the context among its floats
    PrLayoutUnit lineRight;         // line-right edge of the context among its floats
    PrLayoutUnit top;               // block-start edge of the context
    bool firstFormatted;
    LayoutLineExtent marker;        // the outside markers of the context (LayoutInlineBfc::marker)

    // What the layout gave (LayoutInlineResult).
    bool hasLines;
    bool hasBaselines;
    PrLayoutUnit blockSize;
    PrLayoutUnit firstBaseline;
    PrLayoutUnit lastBaseline;

    RawArray<LayoutLogicalChild> lines;    // the record holds a reference to each line
    RawArray<LayoutLineAtomic> atomics;    // by LayoutItemIndex
};

// The last placement of a box out of flow by its holder: the containing block and static position it was placed
// against, and the resulting offset of its border box. The next layout of the holder reuses the placement while these
// inputs and the box's own result stay the same.
struct LayoutAbsoluteEntry {
    LayoutNodeIndex node;
    PrLayoutRect containing;
    PrLayoutVec at;
    PrLayoutVec offset;
    LayoutMode mode;
    LayoutStaticEdges edges;
};

// Where the last layout put a node, in the fragment of its parent. The parent is the box whose fragment holds the node,
// through any anonymous boxes, rows and lines in between. It is the holder of a box out of flow, the nearest box around
// a box in flow, and the viewport for the root and the boxes the viewport holds.
//
// A box with a fragment of its own records the offset of its border box from the border box of its parent. An inline
// box records the line of its opening part, and a text records the line of its first fragment, among the children of
// its parent (LayoutPieceAt). The layout of the parent records the placement as it makes the fragment that holds the
// node.
//
// Placements are the published geometry. Queries and painting read the fragment of a box from its placement rather than
// from its layout entry, which may hold a result that is not in the final tree.
struct LayoutPlacement {
    LayoutNodeIndex parent;         // kPrLayoutNodeNone for the viewport
    union {
        PrLayoutVec offset;         // offset of a box's border box from its parent's border box
        LayoutPieceAt at;           // the line an inline box or a text starts on
    };

    // The fragment of a box within the fragment of its parent. The placement holds a reference to it. Null for an
    // inline box or a text, and for a node that no layout placed.
    const LayoutFragment* PR_NULLABLE fragment;
};

// The position of a box in the content of the viewport along its placements (layoutNodePosition), and its container
// (layoutNodeContainer), memoized as of LayoutCache::placed.
struct LayoutPositioned {
    PrLayoutVec position;
    LayoutNodeIndex container;
    uint32_t version;               // the LayoutCache::placed it is current at
};

// The layout cache of a tree.
struct LayoutCache {
    RawArray<LayoutCacheEntry> entries;        // by LayoutNodeIndex
    RawArray<LayoutCacheContext> contexts;     // context records of the entries; the first is unused
    RawArray<uint32_t> freeContexts;           // records that no entry holds, each with its storage retained
    RawArray<LayoutIntrinsicEntry> intrinsics; // by LayoutNodeIndex, grown as measurement first reaches a node
    RawArray<LayoutAbsoluteEntry> absolutes;   // placement records of the entries; the first is unused
    RawArray<LayoutPlacement> placements;      // by LayoutNodeIndex, grown as layout first places a node

    // The measurements of the boxes that were measured. `measureAt` holds, by LayoutNodeIndex, the first of the
    // kLayoutMeasureSlots records of each box, or kLayoutMeasureNone, and grows as measurement first reaches a node.
    // `measures` holds the records, and `freeMeasures` the free runs of them. The records of a box hold answers only
    // while its intrinsic entry is `measured`. Invalidation clears that flag in the same entry as the sizes, and the
    // next store for the box clears the records that the change left behind.
    RawArray<uint32_t> measureAt;
    RawArray<LayoutMeasureRecord> measures;
    RawArray<uint32_t> freeMeasures;

    // `lines` holds, by the first node of each inline formatting context, the lines it last laid out, or null.
    // `collections` counts the shapings collected so far, and so names each collection
    // (LayoutInlineShaping::collected).
    RawArray<LayoutLineRecord* PR_NULLABLE> lines;
    uint32_t collections;

    // The fragments that left the final tree during the current layout, with the anonymous boxes and line content in
    // them. A fragment leaves when its placement is replaced or its node is forgotten. The caches keyed by fragment
    // drop their entries for these fragments when the layout ends.
    RawArray<const LayoutFragment*> retired;

    // The memoized positions and containers of the boxes, which reads fill. A memo is current while `placed` equals the
    // version it records. Layout increments `placed` when the placement of a box changes, and the tree increments it
    // when a style that a memo reads changes.
    mutable RawArray<LayoutPositioned> positioned;  // by LayoutNodeIndex
    mutable RawArray<LayoutNodeIndex> unpositioned; // the boxes whose memos a read is filling, innermost first
    uint32_t placed;

    uint32_t published;             // the placements recorded since the count was last cleared
};

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

// Initializes an empty cache, and frees a cache.
void layoutCacheInit(LayoutCache& cache);
void layoutCacheReset(LayoutCache& cache);

// Empties the entries of the node `node`, which is new, is being freed, or has changed metrics. Retires the fragment it
// was placed with.
void layoutCacheForget(LayoutCache& cache, LayoutNodeIndex node);

// Notes that `fragment` leaves the final tree in the running layout (LayoutCache::retired).
void layoutCacheRetire(LayoutCache& cache, const LayoutFragment* PR_NULLABLE fragment);

// Drops the intrinsic sizes and measurements of the node, which a change to its subtree may change.
void layoutCacheInvalidateIntrinsic(LayoutCache& cache, LayoutNodeIndex node);

// Drops the result of the node as well as its intrinsic sizes and measurements. Used for an anonymous flex item, whose
// caches the first node of its run keeps, and which a change to any node of the run changes, whatever the marks of the
// first node.
void layoutCacheInvalidate(LayoutCache& cache, LayoutNodeIndex node);

// If the box out of flow `node` was last placed against the containing block `containing` at the static position `at`
// with `edges`, and its own result is still valid in the fallback that `containing` gives, sets `*out` to the box as
// its holder placed it and returns true. Otherwise returns false.
bool layoutCacheFindAbsolute(LayoutState& state, LayoutNodeIndex node, const LayoutAbsoluteBlock& containing,
    PrLayoutVec at, LayoutStaticEdges edges, LayoutChild* out);

// Caches where the holder placed the box out of flow `node`, against the containing block `containing` at the static
// position `at` with `edges`.
void layoutCacheStoreAbsolute(LayoutState& state, LayoutNodeIndex node, const LayoutAbsoluteBlock& containing,
    PrLayoutVec at, LayoutStaticEdges edges, PrLayoutVec offset);

// ---------------------------------------------------------------------------------------------------------------------
// Placements

// Publishes the placements of the boxes in the box fragment `fragment` of `node`. These are the boxes with fragments of
// their own among its children, through any anonymous boxes, rows and lines in between, and the inline boxes and texts
// on its lines. The fragment of the viewport, for kPrLayoutNodeNone, holds the root and the boxes that the viewport
// holds.
//
// A box placed with a new fragment retires its old one, and the boxes in its new fragment are placed in turn. Placing
// the fragment of the viewport after a layout publishes the final tree. The cost is the number of children of the
// fragments that are new to the tree. Layout itself places nothing, so a fragment that a layout builds and then drops
// never reaches the placements.
void layoutCachePlace(LayoutCache& cache, LayoutNodeIndex node, const LayoutFragment* fragment);

// Returns the placement of `node` that the last layout recorded. A node that no layout placed has the viewport as its
// parent, at the origin. A box has a placement from the last layout only if it and its ancestors generate boxes.
inline LayoutPlacement layoutCachePlacement(const LayoutCache& cache, LayoutNodeIndex node) {
    if (node < cache.placements.count)
        return cache.placements.data[node];
    return LayoutPlacement{.parent = kPrLayoutNodeNone, .offset = PrLayoutVec{}};
}

// Returns the fragment that the last layout placed the box `node` with, or null if it placed none. If the box is in the
// final tree, this is its fragment there (laidOut).
inline const LayoutFragment* PR_NULLABLE layoutCachePlaced(const LayoutCache& cache, LayoutNodeIndex node) {
    return node < cache.placements.count ? cache.placements.data[node].fragment : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Results

// The inline size and inline-start margin that a box resolves to in a space by its own sizing, as its layout resolves
// them. A block container in flow that saw no floats depends on the available size of the space only through these
// values.
struct LayoutResolvedInline {
    PrLayoutUnit inlineSize;        // of its border box
    PrLayoutUnit marginInlineStart;
};

// Takes the result that the cache holds for the box of `node` in `space` into `out`, which must be empty. `requests` is
// what its flex container asks of it, or null for a box that is not a flex item. The result is taken only if nothing in
// the subtree of the box has changed since and, in a context, the box would read the same values of the context at its
// current position. Its floats are then placed again, and what it read is added to the reads of its parent. If floats
// wait for its margins and no resolution is given, the result stops where the margins resolve, as its layout would.
// Returns false if there is no such result.
//
// A result laid out when a different size was available differs only if the box resolves to a different inline size,
// and is not taken here (layoutCacheFindResized).
bool layoutCacheFind(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests* PR_NULLABLE requests, LayoutResult* out);

// Returns true if the box of `node` holds a result, laid out when a different size was available, that may still be
// valid in `space` at `requests`. The box must be an in-flow block container that is not an orthogonal flow root and
// whose inline size did not fill the available size. If it is in a context, it must have seen no floats.
bool layoutCacheMayResize(const LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests* PR_NULLABLE requests);

// Takes the result that layoutCacheFind takes, or a result laid out when a different size was available if `resolved`,
// the sizing of the box in `space`, gives the same inline size. The taken result uses the new inline-start margin.
bool layoutCacheFindResized(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests* PR_NULLABLE requests, const LayoutResolvedInline& resolved, LayoutResult* out);

// Keeps the result of laying out the box of `node` in `space` at `requests` for the next layout in the same space. In a
// context, also keeps `reads`, what its subtree read of the context, and the floats it placed.
void layoutCacheStore(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests* PR_NULLABLE requests, const LayoutResult& result, const LayoutReads* PR_NULLABLE reads);

// ---------------------------------------------------------------------------------------------------------------------
// Measurements

// Sets `*blockSize` and `*baselines` to the answer that the box of `node` gave a measurement of `kind` or more in
// `space` at `requests`, and returns true. The answer must come from a space that is the same in everything its result
// read, and nothing may have changed since. Otherwise returns false. The record keeps ownership of the clamps of the
// baselines.
bool layoutCacheFindMeasure(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests& requests, LayoutMeasureKind kind, PrLayoutUnit* blockSize, LayoutBaselines* baselines);

// Keeps the answer that the box of `node` gave a measurement of `kind` in `space` at `requests`, whose result had
// `flags`. The answer replaces a record of the same request that holds less, and otherwise the oldest record.
void layoutCacheStoreMeasure(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
    const LayoutRequests& requests, LayoutMeasureKind kind, LayoutResultFlags flags,
    PrLayoutUnit blockSize, const LayoutBaseline& first, const LayoutBaseline& last);

// ---------------------------------------------------------------------------------------------------------------------
// Lines

// Returns the record of the lines of the inline formatting context whose first node is `first`, and makes an empty one
// if there is none.
LayoutLineRecord& layoutCacheLines(LayoutCache& cache, LayoutNodeIndex first);

// Returns the record of the lines of the inline formatting context whose first node is `first`, or null.
inline const LayoutLineRecord* PR_NULLABLE layoutCacheFindLines(const LayoutCache& cache, LayoutNodeIndex first) {
    return first < cache.lines.count ? cache.lines.data[first] : nullptr;
}

// Releases the lines that a record holds, and empties it.
void layoutLineRecordClear(LayoutLineRecord& record);

// ---------------------------------------------------------------------------------------------------------------------
// Intrinsic sizes

// Takes the intrinsic sizes that the cache holds for the box of `node` measured in `input` into `out`. Returns false if
// there are none.
bool layoutCacheFindIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input,
    LayoutIntrinsicSizes* out);

// Keeps the intrinsic sizes of the box of `node` measured in `input`.
void layoutCacheStoreIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input,
    const LayoutIntrinsicSizes& sizes);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
