// Painting: the recordings of the boxes that have layers of their own, and the caches that hold them.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-backgrounds-3 - https://drafts.csswg.org/css-backgrounds-3/
//
// A paint records each box that has a layer of its own separately, in its own space and without its effects, in the
// walk's painting order (layout_walk.h). The content of a scroll container is recorded unscrolled in segments of its
// own, so that a recording does not depend on scroll offsets. A recording names the boxes in it that have layers of
// their own at the points where they paint, and a scene plays them at those points (layout_scene.h).
//
// Recordings are cached at three granularities, each keyed so that a change re-records only what it reaches. A layer's
// recording is keyed by its node, and is reused while its fragment is the same and nothing it read has changed. A block
// subtree's recording for a phase is keyed by its fragment. A run of a wide box's children for a phase, or a run of a
// stacking context's list, is keyed by a node. Each is a LayoutPaintRecording, which may span containers and name
// layers. A recording embeds a cached recording by appending it.

#pragma once

#include "pr/array.h"
#include "pr/draw.h"
#include "pr/hash_index.h"
#include "pr/layout_query.h"

#include "layout/src/layout_background.h"
#include "layout/src/layout_fragment.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutStyles;
struct LayoutTree;

// ---------------------------------------------------------------------------------------------------------------------
// Recordings

// The index that names no scroll container of a recording or a scene.
constexpr uint32_t kPaintScrollNone = 0xffffffffu;

// How a recording names the container that holds something it records (LayoutPaintContainerRef). A box of a list paints
// in its own containers, which may enclose the recording that paints it.
enum {
    PaintRefKind_Own,               // a container of the recording, by index, or kPaintScrollNone for its layer's space
    PaintRefKind_Container,         // a container around the recording, by its box's node
    PaintRefKind_Space,             // the own space of a layer around it, by its box's node, in no container of its own
    PaintRefKind_Outside,           // no container: the scene's space, past the viewport
};
typedef uint8_t PaintRefKind;

// A reference to the container that holds something a recording records, in one of the forms of PaintRefKind.
struct LayoutPaintContainerRef {
    uint32_t value;
    PaintRefKind kind;
};

// Returns true if two references name the same container.
inline bool operator==(LayoutPaintContainerRef a, LayoutPaintContainerRef b) {
    return a.kind == b.kind && a.value == b.value;
}
inline bool operator!=(LayoutPaintContainerRef a, LayoutPaintContainerRef b) {
    return !(a == b);
}

// Returns a reference to the recording's own container `index`, or its own space for kPaintScrollNone.
inline LayoutPaintContainerRef layoutPaintOwn(uint32_t index) {
    return LayoutPaintContainerRef{index, PaintRefKind_Own};
}

// A box of a list that a recording placed through a box outside the recording (LayoutPlacement). It holds the box's
// fragment, or for an inline box the fragment of its block container, and the recording holds a reference to that
// fragment. It also holds the box's offset in the recording's space and the container that the box paints in
// (layoutNodeContainer). The recording is current only while the box stays at that offset.
struct LayoutPaintPlaced {
    LayoutNodeIndex node;
    const LayoutFragment* fragment;
    PrLayoutVec offset;
    LayoutNodeIndex container;
};

// A stacking context's list that a recording painted from, with the version the cache found
// (LayoutStackingList::version). The recording is current only while the list keeps that version.
struct LayoutPaintListed {
    LayoutNodeIndex context;
    uint32_t version;
};

// A box with a layer of its own that a recording names at the point where its stacking context paints it. It holds the
// box's fragment, its offset in the recording's space, the container that holds it, the number of the recording's
// segments that play before it, and its order among the layers and slots that the recording names. For an inline box,
// the fragment is its container's fragment, and its opening part is on the line `opening`.
struct LayoutPaintLayer {
    const LayoutFragment* fragment;
    PrLayoutVec offset;
    LayoutPaintContainerRef scroll;
    uint32_t segment;
    uint32_t order;
    LayoutNodeIndex node;
    LayoutPieceAt opening;
};

// A slot of embedded content that a recording names at the point where its replaced box paints it. A slot is named in
// the same way as a layer, and also holds its embed id, its node, and its content box in the recording's space. The
// tree's caret is a slot too, named after its text fragment, so that the scene can show or hide it without recording. A
// caret's box is its bar, and it has no embed id.
struct LayoutPaintSlot {
    uint64_t embed;
    LayoutNodeIndex node;
    PrLayoutRect box;
    LayoutPaintContainerRef scroll;
    uint32_t segment;
    uint32_t order;
    bool caret;
    PrColor16 color;                // the bar of a caret
};

// A part of a recording's wheel region: the border box of a box whose wheel waits for its document
// (PrLayoutWheelEvents_Blocking), with the area that the box's descendants reach, in the recording's space, and the
// container that holds the box. A wheel over a region scrolls only once the document has had the wheel event.
struct LayoutPaintRegion {
    PrLayoutRect rect;
    LayoutPaintContainerRef scroll;
};

// What a container of a recording or a scene does to its content.
enum {
    LayoutContainerKind_Scroll,     // a scroll container, which clips its content and moves it by its scroll offset
    LayoutContainerKind_Clip,       // a box that only clips its content
    LayoutContainerKind_Sticky,     // a sticky box, which moves itself and its content by its sticky offset
};
typedef uint8_t LayoutContainerKind;

// A container in a recording: a scroll container, a box that only clips, or a sticky box. Its clip is in the
// recording's space, and the content of every container is also in that space, unscrolled. A box that only clips has no
// range, extents or user axes. A sticky box clips nothing, and a scene finds what moves it from the tree.
struct LayoutPaintScroll {
    LayoutNodeIndex node;           // as layoutNodeContainer names it
    LayoutPaintContainerRef parent; // the container it is in
    uint32_t segment;               // the recording's segments before the first that may hold its content
    PrLayoutRect clip;
    PrLayoutRect range;
    PrLayoutVec portExtent;
    PrLayoutRect content;
    PrLayoutScrollFlags userAxes;
    PrLayoutScrollFlags chainAxes;
    LayoutContainerKind kind;

    // If `curved`, the corners of the clip, by PrRectCorner, whose curves it clips to (layoutClipCorners). Otherwise
    // its corners are square.
    bool curved;
    PrCorner corners[PrRectCorner_Count];
};

// A run of a recording's commands in one container. A segment holds at least one command. Its list is in its own space,
// at `offset` in the recording's space, so that a recording can take a cached part's list as a segment of its own if
// the part starts a segment.
struct LayoutPaintSegment {
    PrDrawList* list;               // frozen once recorded
    LayoutPaintContainerRef scroll;
    PrLayoutVec offset;
};

// A recording's segment that holds a background layer fixed to the viewport (PrLayoutBackgroundAttachment_Fixed).
// `segment` is the segment's index in LayoutPaintRecording::segments. The segment's list is in the viewport's space, so
// the layer stays in place while its box scrolls. The layer paints only within `clip`, which is in the segment's own
// space, so the clip moves with the box. No other commands are recorded into the segment.
//
// A recording lists these separately instead of marking each LayoutPaintSegment. Few recordings have a fixed layer, and
// a mark on every segment would make all of them larger.
struct LayoutPaintFixed {
    uint32_t segment;
    PrLayoutRect clip;
};

// A recording holds, in painting order and in its own space, its segments, the containers whose content it records, the
// layers and slots it names, its wheel region, and the stacking contexts' lists it painted from. Each container comes
// after the container that holds it. The recording is current only while those lists are unchanged. A layer's recording
// is a chunk. A cached part of a recording, such as a subtree or a run, is in its own space from its top-left corner,
// and names the containers around it by node. Appending the part to another recording at its place there therefore
// gives the same result as recording it in place.
struct LayoutPaintRecording {
    Array<LayoutPaintSegment> segments;
    Array<LayoutPaintScroll> scrolls;
    Array<LayoutPaintLayer> layers;       // in painting order
    Array<LayoutPaintSlot> slots;         // in painting order
    Array<LayoutPaintRegion> regions;
    Array<LayoutPaintListed> lists;
    Array<LayoutPaintFixed> fixed;        // in the order of their segments
};

// The recording of a box with a layer of its own (the root, or a stacking context with effects). It holds the box's
// decoration and content in painting order, in its own space from the top-left corner of its border box. It omits the
// box's effects and the boxes in it that have layers of their own, which it names instead. The recording of an inline
// box holds its pieces, in its container's space from the container's border box. The content of a scroll container in
// the chunk is in segments of its own, unscrolled. A chunk is reused while the fragment is the same, the lists it
// painted from are the same, the boxes it placed through boxes outside it are where they were, and nothing it read from
// its nodes has changed.
struct LayoutPaintChunk : LayoutPaintRecording {
    LayoutNodeIndex node;

    // The fragment of the box, or of an inline box's container, which the chunk holds a reference to.
    const LayoutFragment* fragment;

    LayoutPieceAt opening;          // the line of an inline box's opening part
    Array<LayoutPaintPlaced> outer; // boxes placed through boxes outside it; each holds a fragment reference
    uint32_t recorded;              // the paint that recorded it, as LayoutPaintCache::paints counts it
    uint32_t painted;               // the paint that last used it
    bool root;                      // true for the root's chunk, whose background the canvas takes
};

// ---------------------------------------------------------------------------------------------------------------------
// Caches

// The chunk index that names no chunk.
constexpr uint32_t kPaintChunkNone = 0xffffffffu;

// The recording of a block subtree of a box with a layer of its own, for one phase of its painting. The chunk that
// holds the box embeds it. It is cached while the tree holds the subtree's fragment, and is valid while what it read is
// unchanged, under the same rule as a chunk, so that recording a chunk again records only the subtrees that changed. It
// is in the subtree's own space, from its top-left corner, wherever the subtree lies. It is appended where it began: in
// the same chunk and the same container.
struct LayoutPaintSub {
    const LayoutFragment* fragment; // which the recording holds a reference to
    uint8_t phase;
    LayoutNodeIndex chunk;          // the node of the chunk it was recorded in
    LayoutPaintContainerRef entry;  // the container it began in, by node
    LayoutPaintRecording recording;
    uint32_t commands;              // number of commands it records, including embedded ones
    uint32_t recorded;              // the paint that recorded it
};

// The recording of a run of a box's children for one phase of its painting, or of a run of the boxes of a stacking
// context's list (layoutPhaseIsList). A box with many children, or a context with many boxes in its list, records them
// in runs, so that a change re-records only the run that holds it. The box's recording embeds one recording for each
// run, so its cost grows with the number of runs, not the number of children. A run is keyed by the box's node rather
// than its fragment, so a layout that replaces the box's fragment reuses the runs whose children it reused. A run of a
// list is keyed by its first box, so inserting or removing a box changes only the runs around it. A run is in the space
// whose origin is the top-left corner of its first child or box, wherever the run lies. It is appended where it began,
// in the same way as a subtree.
struct LayoutPaintRun {
    LayoutNodeIndex node;           // the box, or the first box of a run of a list
    uint8_t phase;
    uint32_t index;                 // which run of the box's children, or 0
    LayoutNodeIndex chunk;          // the node of the chunk it was recorded in
    LayoutPaintContainerRef entry;  // the container it began in, by node
    LayoutPaintRecording recording;
    uint32_t commands;              // number of commands it records, including embedded ones
    uint32_t recorded;              // the paint that recorded it

    // The children it recorded, each holding a reference to its fragment, and each offset from the first child. The run
    // is current while the box holds the same fragments at the same offsets from one another, and nothing they read has
    // changed.
    Array<LayoutChild> children;

    // The boxes of a list it recorded, each placed relative to the first and holding a reference to its fragment. The
    // run is current while all of the following hold: the list holds the same boxes with the same fragments, at the
    // same offsets from one another, in the container that the walk is in; the lists it painted from are the ones it
    // found; and nothing the boxes read has changed.
    Array<LayoutPaintPlaced> entries;
    uint32_t layouts;               // LayoutTree::layouts when it recorded the boxes
    uint32_t placed;                // LayoutCache::placed when it recorded them
};

// The recordings that paints reuse.
struct LayoutPaintCache {
    Array<LayoutPaintChunk> chunks;
    Array<uint32_t> byNode;         // the chunk of each node, or kPaintChunkNone
    Array<LayoutPaintSub> subs;
    HashIndex subIndex;             // `subs` by fragment
    Array<LayoutPaintRun> runs;
    HashIndex runIndex;             // `runs` by node, phase and index
    uint32_t paints;                // number of paints made, including the one in progress
};

// Frees the memory that the cache holds.
void layoutPaintCacheReset(LayoutPaintCache& cache);

// Returns the border, padding and content boxes of `fragment`, styled by `box`, whose border box is `border` in the
// space that it paints in.
LayoutBackgroundBoxes layoutBackgroundBoxes(const LayoutStyles* styles, const LayoutFragment* fragment,
    const PrLayoutBoxStyle& box, PrRect border);

// Starts a paint: counts it, and resets the paint's work counts.
void layoutPaintBegin(LayoutTree* tree);

// Counts a paint that returned the previous paint's scene of `layers` layers, and so records nothing.
void layoutPaintReused(LayoutTree* tree, uint32_t layers);

// Returns the index of the chunk of the box `node`, which has a layer of its own and whose fragment is `fragment`. For
// an inline box, the chunk is its container's chunk, with the box's opening part on the line `opening`. The chunk is
// recorded again if it is not current. Marks the chunk as used by the running paint.
uint32_t layoutPaintChunk(LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment, LayoutPieceAt opening,
    bool root);

// Frees the chunks that the running paint did not use.
void layoutPaintSweep(LayoutTree* tree);

// Drops the cache entries of the fragments that left the final tree in the tree's most recent layout
// (LayoutCache::retired).
void layoutPaintRetire(LayoutTree* tree);

// Drops the recordings of runs of the children of `node`, which is being freed.
void layoutPaintForget(LayoutTree* tree, LayoutNodeIndex node);

// Checks that each recording the cache would reuse matches a fresh recording. Returns the node of the first layer whose
// recording differs, or kPrLayoutNodeNone.
uint32_t layoutPaintVerify(LayoutTree* tree);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
