// Painting: the recordings of the boxes that have layers of their own, and the caches that hold them.

#include "layout/src/layout_paint.h"

#include "pr/assert.h"
#include "pr/draw_commands.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "layout/src/layout_query.h"
#include "layout/src/layout_table.h"
#include "layout/src/layout_text_position.h"
#include "layout/src/layout_walk.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Recording cache

// Returns the index of the cached recording of `fragment`'s subtree for `phase`, or kHashMissing. Every recording of a
// fragment has the fragment's hash, so one probe sequence finds them all.
uint32_t subFind(const LayoutPaintCache& cache, const LayoutFragment* fragment, LayoutPhase phase) {
    return cache.subIndex.find(prHashPointer(fragment, 0), [&](uint32_t i) {
        const LayoutPaintSub& sub = cache.subs.data[i];
        return sub.fragment == fragment && sub.phase == phase;
    });
}

// Returns the hash of a run's key.
uint32_t runHash(LayoutNodeIndex node, uint32_t phase, uint32_t index) {
    uint64_t key = uint64_t(node) << 32 | uint64_t(index) << 8 | phase;
    return uint32_t(prHashBytes(nullptr, 0, key));
}

// Returns the index of the cached run `index` of the children of `node` for `phase`, or kHashMissing.
uint32_t runFind(const LayoutPaintCache& cache, LayoutNodeIndex node, LayoutPhase phase, uint32_t index) {
    return cache.runIndex.find(runHash(node, phase, index), [&](uint32_t i) {
        const LayoutPaintRun& run = cache.runs.data[i];
        return run.node == node && run.phase == phase && run.index == index;
    });
}

// Releases what a recording holds and empties it, keeping its storage.
void clearRecording(LayoutPaintRecording& recording) {
    for (uint32_t i = 0; i < recording.segments.count; ++i)
        prDrawListRelease(recording.segments.data[i].list);
    recording.segments.clear();
    recording.scrolls.clear();
    recording.layers.clear();
    recording.slots.clear();
    recording.regions.clear();
    recording.lists.clear();
    recording.fixed.clear();
}

// Releases what a recording holds and frees its storage.
void resetRecording(LayoutPaintRecording& recording) {
    clearRecording(recording);
    recording.segments.reset();
    recording.scrolls.reset();
    recording.layers.reset();
    recording.slots.reset();
    recording.regions.reset();
    recording.lists.reset();
    recording.fixed.reset();
}

// The most emptied recordings that the cache keeps for parts to take their storage from (LayoutPaintCache::spare).
constexpr uint32_t kPaintSpareRecordings = 64;

// Empties a recording that the cache no longer holds, and keeps its storage for a part to take, unless the cache keeps
// enough already.
void spareRecording(LayoutPaintCache& cache, LayoutPaintRecording& recording) {
    clearRecording(recording);
    bool stored = recording.segments.capacity != 0 || recording.scrolls.capacity != 0 ||
        recording.layers.capacity != 0 || recording.slots.capacity != 0 || recording.regions.capacity != 0 ||
        recording.lists.capacity != 0 || recording.fixed.capacity != 0;
    if (!stored)
        return;
    if (cache.spare.count < kPaintSpareRecordings)
        cache.spare.push(recording);
    else
        resetRecording(recording);
    recording = LayoutPaintRecording{};
}

// Releases the fragments of the boxes that a recording placed from outside it, and frees the list.
void releaseOuter(RawArray<LayoutPaintPlaced>& outer) {
    for (uint32_t i = 0; i < outer.count; ++i)
        layoutFragmentRelease(outer.data[i].fragment);
    outer.reset();
}

// Frees what a run holds, and spares its recording's storage.
void runRelease(LayoutPaintCache& cache, LayoutPaintRun& run) {
    spareRecording(cache, run.recording);
    for (uint32_t i = 0; i < run.children.count; ++i)
        layoutFragmentRelease(run.children.data[i].fragment);
    run.children.reset();
    releaseOuter(run.entries);
}

// Drops the run at `at`.
void runDrop(LayoutPaintCache& cache, uint32_t at) {
    LayoutPaintRun& run = cache.runs.data[at];
    runRelease(cache, run);
    cache.runIndex.erase(runHash(run.node, run.phase, run.index), at);
    uint32_t last = cache.runs.count - 1;
    if (at != last) {
        const LayoutPaintRun& moved = cache.runs.data[last];
        cache.runIndex.relocate(runHash(moved.node, moved.phase, moved.index), last, at);
        cache.runs.data[at] = moved;
    }
    cache.runs.pop();
}

// Frees what a subtree's recording holds, and spares its storage.
void subRelease(LayoutPaintCache& cache, LayoutPaintSub& sub) {
    spareRecording(cache, sub.recording);
    layoutFragmentRelease(sub.fragment);
}

// Caches a subtree's recording, replacing one of the same fragment and phase.
void subStore(LayoutPaintCache& cache, LayoutPaintSub& sub) {
    uint32_t at = subFind(cache, sub.fragment, LayoutPhase(sub.phase));
    if (at != kHashMissing) {
        subRelease(cache, cache.subs.data[at]);
        cache.subs.data[at] = sub;
        return;
    }
    cache.subIndex.insert(prHashPointer(sub.fragment, 0), cache.subs.count);
    cache.subs.push(sub);
}

// Drops the cached recordings of `fragment`'s subtree, for every phase.
void subDrop(LayoutPaintCache& cache, const LayoutFragment* fragment) {
    uint32_t hash = prHashPointer(fragment, 0);
    for (;;) {
        uint32_t at = cache.subIndex.find(hash, [&](uint32_t i) { return cache.subs.data[i].fragment == fragment; });
        if (at == kHashMissing)
            return;
        subRelease(cache, cache.subs.data[at]);
        cache.subIndex.erase(hash, at);
        uint32_t last = cache.subs.count - 1;
        if (at != last) {
            const LayoutPaintSub& moved = cache.subs.data[last];
            cache.subIndex.relocate(prHashPointer(moved.fragment, 0), last, at);
            cache.subs.data[at] = moved;
        }
        cache.subs.pop();
    }
}

// Returns true if a change since the paint `recorded` reached the table parts from `node` up to its table: the tracks
// whose backgrounds a table part that paints whole records behind its cells (trackBackgrounds), and its table's
// columns, which mark the table when they change.
bool tableChangedSince(const LayoutTree* tree, LayoutNodeIndex node, uint32_t recorded) {
    for (LayoutNodeIndex at = layoutTreeNode(tree, node).parent; at != kPrLayoutNodeNone;
            at = layoutTreeNode(tree, at).parent) {
        const LayoutTreeNode& n = layoutTreeNode(tree, at);
        if (n.paintSubtree >= recorded)
            return true;
        if (layoutIsTable(layoutBoxStyle(tree->styles, n.style)))
            return false;
    }
    return false;
}

// Returns true if what a recording of `fragment` read from its nodes has changed since the paint `recorded` made it.
// The recording reads a box's subtree, as tracked by LayoutTreeNode::paintSubtree, the record of a text's parent, which
// the text of an anonymous block reads from outside the block, and for a table part that paints whole, the tracks
// around it.
bool changedSince(const LayoutTree* tree, const LayoutFragment* fragment, uint32_t recorded) {
    if (fragment->node != kPrLayoutNodeNone) {
        const LayoutTreeNode& n = layoutTreeNode(tree, fragment->node);
        if (n.paintSubtree >= recorded ||
                (n.kind == PrLayoutNodeKind_Text && layoutTreeNode(tree, n.parent).paintChanged >= recorded))
            return true;
        if (tree->styles->wholeTableParts == 0 || n.kind == PrLayoutNodeKind_Text ||
                fragment->kind != LayoutFragmentKind_Box)
            return false;
        bool part = layoutStyleTraits(tree->styles, n.style).wholeTablePart;
        return part && layoutFragmentPaintsWhole(tree, fragment) && tableChangedSince(tree, fragment->node, recorded);
    }
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        if (changedSince(tree, fragment->children[i].fragment, recorded))
            return true;
    }
    return false;
}

// Returns true if each list in `lists` still has the version that the recording found.
bool listsCurrent(const LayoutTree* tree, const RawArray<LayoutPaintListed>& lists) {
    for (uint32_t i = 0; i < lists.count; ++i) {
        if (!layoutStackingListIs(tree, lists.data[i].context, lists.data[i].version))
            return false;
    }
    return true;
}

// Returns true if a subtree's recording is what recording it again would give: the lists it painted from are the ones
// it found, and nothing it read has changed since.
bool subIsCurrent(const LayoutTree* tree, const LayoutPaintSub& sub) {
    return listsCurrent(tree, sub.recording.lists) && !changedSince(tree, sub.fragment, sub.recorded);
}

// Returns true if a run's recording is what recording `fragment`'s children from `start` to `end` would give: the
// children are the fragments it recorded, at the same offsets from one another, and nothing they read has changed
// since.
bool runIsCurrent(const LayoutTree* tree, const LayoutPaintRun& run, const LayoutFragment* fragment, uint32_t start,
        uint32_t end) {
    if (run.children.count != end - start)
        return false;
    PrLayoutVec first = fragment->children[start].offset;
    for (uint32_t i = 0; i < run.children.count; ++i) {
        const LayoutChild& child = fragment->children[start + i];
        const LayoutChild& kept = run.children.data[i];
        if (child.fragment != kept.fragment || child.offset - first != kept.offset ||
                changedSince(tree, child.fragment, run.recorded))
            return false;
    }
    return true;
}

// Returns the fragment that a box of a list paints from: its own fragment, or the fragment of an inline box's block
// container.
const LayoutFragment* PR_NULLABLE entryFragment(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex holder = layoutIsInlineBox(tree, node) ? layoutCachePlacement(tree->cache, node).parent : node;
    return holder != kPrLayoutNodeNone ? layoutCachePlaced(tree->cache, holder) : nullptr;
}

// Returns true if a run's recording is what recording the boxes `boxes` of a list again would give, with the first box
// at `first` in the viewport's content as laid out. Each box must also be in the container that it was in.
bool entriesCurrent(const LayoutTree* tree, const LayoutPaintRun& run, const LayoutStackedBox* boxes, uint32_t count,
        PrLayoutVec first) {
    if (run.entries.count != count || !listsCurrent(tree, run.recording.lists))
        return false;
    // If nothing was laid out or placed again since the recording, the boxes have the fragments, positions and
    // containers they had.
    bool placed = run.layouts == tree->layouts && run.placed == tree->cache.placed;
    // A box placed in the first box's parent is offset from it by their placements, in the first box's container.
    LayoutPlacement head = layoutCachePlacement(tree->cache, boxes[0].node);
    LayoutNodeIndex siblings = layoutIsInlineBox(tree, boxes[0].node) ? kPrLayoutNodeNone : head.parent;
    for (uint32_t i = 0; i < count; ++i) {
        const LayoutPaintPlaced& kept = run.entries.data[i];
        LayoutNodeIndex node = boxes[i].node;
        if (kept.node != node || layoutTreeNode(tree, node).paintSubtree >= run.recorded)
            return false;
        if (placed)
            continue;
        bool inlineBox = layoutIsInlineBox(tree, node);
        LayoutPlacement placement = layoutCachePlacement(tree->cache, node);
        LayoutNodeIndex holder = inlineBox ? placement.parent : node;
        if (holder == kPrLayoutNodeNone || layoutCachePlaced(tree->cache, holder) != kept.fragment)
            return false;
        if (!inlineBox && i != 0 && placement.parent == siblings && siblings != kPrLayoutNodeNone) {
            if (placement.offset - head.offset != kept.offset || kept.container != run.entries.data[0].container)
                return false;
            continue;
        }
        LayoutPositioned at = inlineBox ? layoutNodePositioned(tree, node) : layoutBoxPositioned(tree, node);
        if (at.container != kept.container || at.position - first != kept.offset)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Recording

// The minimum number of commands in a batch of a recording. A batch ends after the fragment that reaches this number,
// or among a text fragment's font ranges if that fragment's ranges alone reach it.
constexpr uint32_t kPaintBatch = 64;

// The minimum number of fragments that a subtree must visit for its recording to be cached if it draws nothing. A
// subtree that draws is always cached: it records into its own list, which is embedded in its parent's list, so caching
// costs one entry.
constexpr uint32_t kPaintSubVisits = 32;

// Returns the transform from the natural space of a replaced box's content, in pixels, to the space of its content box
// `box`, and sets `drawn` to the size that the natural space is drawn at, rounded to units. The transform fits the
// content to the box as `decoration` specifies (css-images-3/#the-object-fit, css-images-3/#the-object-position). The
// natural space has the content's natural size, and the box's size along an axis that has no natural size.
PrAffine2 fitContent(const LayoutStyles* styles, const PrLayoutDecorationStyle& decoration, PrLayoutNaturalSize natural,
        PrLayoutRect box, PrLayoutVec* drawn) {
    double boxWidth = double(layoutRectWidth(box).raw);
    double boxHeight = double(layoutRectHeight(box).raw);
    bool hasWidth = natural.width.raw >= 0;
    bool hasHeight = natural.height.raw >= 0;
    double width = hasWidth ? double(natural.width.raw) : boxWidth;
    double height = hasHeight ? double(natural.height.raw) : boxHeight;
    double ratio = hasWidth && hasHeight && height > 0 ? width / height : double(natural.ratio);
    // The size at which the content is drawn: the box's size, the largest size at the ratio that fits in the box, the
    // smallest size at the ratio that covers it, or the size of the natural space.
    auto atRatio = [&](bool cover, double* w, double* h) {
        *w = boxWidth;
        *h = boxHeight;
        if (ratio <= 0 || boxHeight <= 0)
            return;
        if ((boxWidth / boxHeight > ratio) != cover)
            *w = boxHeight * ratio;
        else
            *h = boxWidth / ratio;
    };
    double drawnWidth = boxWidth;
    double drawnHeight = boxHeight;
    switch (decoration.objectFit) {
        case PrLayoutObjectFit_Contain:
            atRatio(false, &drawnWidth, &drawnHeight);
            break;
        case PrLayoutObjectFit_Cover:
            atRatio(true, &drawnWidth, &drawnHeight);
            break;
        case PrLayoutObjectFit_None:
            drawnWidth = width;
            drawnHeight = height;
            break;
        case PrLayoutObjectFit_ScaleDown:
            atRatio(false, &drawnWidth, &drawnHeight);
            if (width <= drawnWidth && height <= drawnHeight) {
                drawnWidth = width;
                drawnHeight = height;
            }
            break;
        default:
            break;
    }
    // Position percentages refer to the room that the drawn size leaves.
    PrLayoutUnit roomX = prLayoutUnitClamped(int64_t(boxWidth - drawnWidth));
    PrLayoutUnit roomY = prLayoutUnitClamped(int64_t(boxHeight - drawnHeight));
    PrLayoutVec at{
        box.left + layoutResolve(styles, decoration.objectPositionX, roomX),
        box.top + layoutResolve(styles, decoration.objectPositionY, roomY),
    };
    float scaleX = width > 0 ? float(drawnWidth / width) : 1;
    float scaleY = height > 0 ? float(drawnHeight / height) : 1;
    PrVec2 offset = prLayoutVecToPx(at);
    *drawn = PrLayoutVec{
        prLayoutUnitClamped(int64_t(prF64Round(drawnWidth))),
        prLayoutUnitClamped(int64_t(prF64Round(drawnHeight))),
    };
    return PrAffine2{scaleX, 0, 0, scaleY, offset.x, offset.y};
}

// How a recording uses the cache of subtree recordings.
enum {
    RecordSubs_Cached,              // embeds the valid cached recordings and caches those it makes
    RecordSubs_Fresh,               // records every subtree and caches none. A subtree records in its own space either
                                    // way, so a fresh recording matches a cached one if the cache is correct.
};
typedef uint8_t RecordSubs;

// Records the body of a box that paints whole into a chunk: the commands in its own space, and the content of its
// containers, unscrolled, in segments of their own. A block subtree, a run of children and a run of a list each record
// separately, into a LayoutPaintRecording of their own, in their own space from their top-left corner. That recording
// is cached and appended to the recording around it, placed by a translation in doubles, so that a recording's
// coordinates stay as small as its parts' coordinates however far down the chunk they lie. The walk's offsets are in
// the chunk's space, in units, and each becomes a float only after the origin of the recording it records into is
// subtracted from it.
struct Painter : LayoutWalkVisitor {
    const LayoutTree* tree;
    const LayoutStyles* styles;
    LayoutPaintChunk* chunk;

    // The paint that last recorded the chunk if this paint follows no layout change, or 0 if that is not known.
    // Recording a box again whose paint inputs are older than that paint is wasted work.
    uint32_t previous;

    LayoutPaintCache* cache;        // whose recordings of parts it uses as `subs` says
    RecordSubs subs;
    PrLayoutTreeStats* stats;

    // Where it records. `list` is the current segment's list in the innermost open recording, created when a command is
    // first recorded into it. `current` is the container that the segment is in, as that recording names it. `origin`
    // is the recording's origin in the chunk's space.
    PrDrawList* PR_NULLABLE list;
    LayoutPaintContainerRef current;
    PrLayoutVec origin;

    // The containers that the walk is in, innermost last. The containers around the chunk's box come first, then the
    // chunk's own containers. Each is named by its node (PaintRefKind_Container), or is the chunk's own space, the
    // space of the layer around it, or none.
    RawArray<LayoutPaintContainerRef> stack;

    // The containers whose content the open recordings record. Each belongs to the innermost recording that first
    // entered it, and records how many parts were open around it there and its index in that recording's containers.
    struct Opened {
        LayoutNodeIndex node;
        uint32_t depth;
        uint32_t index;
    };
    RawArray<Opened> opened;
    HashIndex openedIndex;          // `opened` by node

    // The batch of commands being recorded. The list embeds the batch once it is full or the list ends, so that a
    // player can use the bounds of each batch to skip the batches outside what it shows.
    PrDrawList* PR_NULLABLE batch;
    uint32_t batched;
    bool held;                      // the batch is a box's decoration, which its clip rectangle clips whole
    PrLayoutRect heldClip;

    uint32_t commands;              // recorded or embedded so far
    uint32_t visits;                // boxes and text runs visited so far

    // The columns of the table whose backgrounds gridBackgrounds is recording, in order. Each entry is a column box
    // with the column group around it, or a column group with no column boxes, with their rectangles.
    struct GridColumn {
        LayoutNodeIndex node;
        LayoutNodeIndex group;
        PrLayoutRect rect;
        PrLayoutRect groupRect;
    };
    RawArray<GridColumn> columns;

    // A part being recorded separately, to be cached: a block subtree, a run of a box's children, or a run of a list's
    // boxes. A part that places a box through a box outside it records the placed box's position separately from the
    // part, and is not cached. A run of a list's boxes checks the positions of its boxes when it is replayed, and is
    // not cached if one of the boxes it places is for the chunk to check.
    struct Subtree {
        const LayoutFragment* PR_NULLABLE fragment; // null for a run of a list
        LayoutPhase phase;
        uint32_t run;                  // 0 for the fragment's subtree, or 1 + the index of a run
        PrLayoutVec offset;            // in the chunk's space, which is its recording's origin
        LayoutPaintContainerRef entry; // the container the walk was in when it began
        LayoutPaintRecording recording;
        PrDrawList* PR_NULLABLE outerList; // the list the enclosing recording used when it began
        LayoutPaintContainerRef outerCurrent;
        PrLayoutVec outerOrigin;
        uint32_t commands;             // `commands` when it started
        uint32_t visits;               // `visits` when it started
        bool placesOutside;

        // For a run of a list: its boxes, and the position of its first box in the viewport's content.
        const LayoutStackedBox* PR_NULLABLE boxes;
        uint32_t count;
        PrLayoutVec first;
    };
    RawArray<Subtree> subtrees;        // innermost last

    // The containers of a part being appended, as named by the recording it is appended to.
    RawArray<LayoutPaintContainerRef> mapped;

    // Scratch: the stretch along a text fragment's line that each of its highlights covers, in px, empty if it covers
    // none of it, and the cuts where a stretch starts or ends.
    struct HighlightStretch {
        float lo;
        float hi;
        uint32_t range;             // by index of the node's highlights
    };
    RawArray<HighlightStretch> highlightStretches;
    RawArray<uint32_t> highlightsReaching;
    RawArray<float> highlightCuts;

    // The text of the glyph runs of the fragment being recorded, for a paint that marks content: by glyph, the bytes of
    // the node's text that each glyph was shaped from, and the text and clusters of one run.
    RawArray<uint32_t> glyphStarts;
    RawArray<uint32_t> glyphEnds;
    RawArray<uint32_t> runClusters;
    RawArray<char> runText;
    RawArray<uint32_t> stopBytes;   // the bytes of the fragment's caret stops, in the text's order

    // By caret of the tree: the fragment that holds the caret, once `caretsSearched` is true.
    RawArray<const LayoutFragment*> caretFragments;
    bool caretsSearched;

    // Returns the reference by which the chunk names the content of its root's viewport: its own space in the root's
    // chunk, and the root's own space in any other chunk.
    LayoutPaintContainerRef viewportRef() const {
        LayoutNodeIndex root = layoutNodeRoot(tree, chunk->node);
        if (root == chunk->node)
            return layoutPaintOwn(kPaintScrollNone);
        return LayoutPaintContainerRef{root, PaintRefKind_Space};
    }

    // Returns the innermost open recording.
    LayoutPaintRecording& into() { return subtrees.count != 0 ? subtrees.last().recording : *chunk; }

    // Returns the container that the walk is in.
    LayoutPaintContainerRef top() const {
        return stack.count != 0 ? stack.last() : LayoutPaintContainerRef{0, PaintRefKind_Outside};
    }

    // Returns where an open recording records the content of the container `node`, or null.
    const Opened* PR_NULLABLE openedOf(LayoutNodeIndex node) const {
        uint32_t at = openedIndex.find(KeyIndex::hashOf(node), [&](uint32_t i) { return opened.data[i].node == node; });
        return at != kHashMissing ? &opened.data[at] : nullptr;
    }

    // Notes that the recording inside `depth` parts records the content of the container `node` as its `index`.
    void open(LayoutNodeIndex node, uint32_t depth, uint32_t index) {
        uint32_t at = openedIndex.find(KeyIndex::hashOf(node), [&](uint32_t i) { return opened.data[i].node == node; });
        if (at == kHashMissing) {
            openedIndex.insert(KeyIndex::hashOf(node), opened.count);
            opened.push(Opened{node, depth, index});
            return;
        }
        opened.data[at] = Opened{node, depth, index};
    }

    // Returns how the innermost open recording names the container that `ref` names by node: as one of its own
    // containers if it records the container's content, and otherwise by node.
    LayoutPaintContainerRef resolve(LayoutPaintContainerRef ref) const {
        if (ref.kind != PaintRefKind_Container)
            return ref;
        const Opened* found = openedOf(ref.value);
        return found != nullptr && found->depth == subtrees.count ? layoutPaintOwn(found->index) : ref;
    }

    // Returns true if a recording around the innermost one records the content of the container `node`.
    bool opensAround(LayoutNodeIndex node) const {
        const Opened* found = openedOf(node);
        return found != nullptr && found->depth < subtrees.count;
    }

    // Names the containers around the chunk's box, given by `chain` outermost first (layoutContainerChain). The last is
    // named as the chunk's own space, and the others as a recording around the chunk names them.
    void startIn(const RawArray<LayoutNodeIndex>& chain) {
        for (uint32_t i = 0; i + 1 < chain.count; ++i)
            if (chain.data[i] == kLayoutContainerViewport)
                stack.push(viewportRef());
        else
            stack.push(LayoutPaintContainerRef{chain.data[i], PaintRefKind_Container});
        if (chain.count != 0)
            stack.push(layoutPaintOwn(kPaintScrollNone));
        else
            stack.push(LayoutPaintContainerRef{0, PaintRefKind_Outside});
        segment(resolve(top()));
    }

    // Records into the container around the current one, or past the viewport if there is none.
    void leave() {
        stack.pop();
        segment(resolve(top()));
    }

    void leaveViewport() { leave(); }
    void enterViewport() {
        stack.push(viewportRef());
        segment(resolve(top()));
    }

    PrDrawList* out() {
        if (batch == nullptr)
            batch = prDrawListCreate();
        return batch;
    }

    // Counts a recorded command, and ends the batch if it holds enough.
    void recorded() {
        ++commands;
        ++stats->commandsRecorded;
        if (++batched >= kPaintBatch && !held)
            flush();
    }

    // Returns the current segment's list, and starts a segment of the innermost recording if there is none.
    PrDrawList* target() {
        if (list == nullptr) {
            list = prDrawListCreate();
            into().segments.push(LayoutPaintSegment{.list = list, .scroll = current, .offset = kLayoutVecZero});
        }
        return list;
    }

    void flush() {
        if (batch == nullptr)
            return;
        prDrawListEmbed(target(), batch, PrDVec2{0, 0});
        prDrawListRelease(batch);
        batch = nullptr;
        batched = 0;
    }

    // Records what follows into the container `scroll`, as the innermost recording names it. Starts a segment of its
    // own if `scroll` differs from the current segment's container.
    void segment(LayoutPaintContainerRef scroll) {
        if (scroll == current)
            return;
        flush();
        list = nullptr;
        current = scroll;
    }

    // Records `content` as a segment fixed to the viewport (LayoutPaintFixed). `content` is in the viewport's space,
    // and `clip` is in the recording's space. Takes the reference that the caller holds. What follows starts a segment
    // of its own.
    void fixed(PrDrawList* content, PrRect clip) {
        flush();
        LayoutPaintRecording& to = into();
        to.fixed.push(LayoutPaintFixed{to.segments.count, prLayoutRectFromPx(clip)});
        to.segments.push(LayoutPaintSegment{.list = content, .scroll = current, .offset = kLayoutVecZero});
        list = nullptr;
        recorded();
    }

    // Starts a new segment for what follows, so that the layer or slot named next plays after what is recorded so far.
    void nameNext() {
        flush();
        list = nullptr;
    }

    // Records what follows into the container that the walk is in, if the innermost recording records elsewhere.
    void follow() { segment(resolve(top())); }

    // Embeds a recording's list, moved from its own space into the current list's.
    void embedMoved(PrDrawList* recording, PrLayoutVec moved) {
        prDrawListEmbed(target(), recording, layoutOffset(moved));
    }

    // Returns true if a cached part that began in `entry` in the chunk of `node` is appended where it began.
    bool beganHere(LayoutNodeIndex node, LayoutPaintContainerRef entry) const {
        return node == chunk->node && entry == top();
    }

    // Appends the part `part`, whose space is at `at` in the chunk's space, to the innermost recording, with the same
    // result as recording it there. Its segments follow the recording's segments, and the first continues the current
    // segment if both are in the same container. Its containers are added, except those that the recording already has.
    // The layers and slots it names, and the lists it painted from, are added too. The cost is proportional to its
    // segments, containers and names.
    void append(const LayoutPaintRecording& part, PrLayoutVec at) {
        flush();
        PrLayoutVec moved = at - origin;
        // Most parts are in one container, the one that the walk is in, and name nothing.
        bool names = part.scrolls.count != 0 || part.layers.count != 0 || part.slots.count != 0 ||
            part.regions.count != 0 || part.fixed.count != 0;
        bool here = part.segments.count == 0 || (part.segments.count == 1 && part.segments.data[0].scroll == top());
        bool plain = !names && here;
        if (plain) {
            // The part's one segment is in the container that the walk is in, which need not be the container of the
            // segment being recorded, as when the last part appended ended elsewhere.
            if (part.segments.count != 0) {
                follow();
                embedMoved(part.segments.data[0].list, part.segments.data[0].offset + moved);
            }
            into().lists.append(part.lists.span());
            return;
        }
        LayoutPaintRecording& to = into();
        mapped.clear();
        auto map = [&](LayoutPaintContainerRef ref) {
            if (ref.kind == PaintRefKind_Own)
                return ref.value == kPaintScrollNone ? ref : mapped.data[ref.value];
            return resolve(ref);
        };
        uint32_t scrollAt = 0;
        uint32_t layerAt = 0;
        uint32_t slotAt = 0;
        // Adds the containers whose content begins before the part's segment `index`, and the names that play after the
        // part's first `index` segments.
        auto before = [&](uint32_t index) {
            for (; scrollAt < part.scrolls.count && part.scrolls.data[scrollAt].segment <= index; ++scrollAt) {
                // A part that was just recorded opened its own containers, and the recording takes them. A replayed
                // part may open containers that the recording, or a recording around it, has opened since.
                const LayoutPaintScroll& s = part.scrolls.data[scrollAt];
                const Opened* found = openedOf(s.node);
                LayoutPaintContainerRef ref{s.node, PaintRefKind_Container};
                if (found != nullptr && found->depth == subtrees.count) {
                    ref = layoutPaintOwn(found->index);
                } else if (found == nullptr || found->depth > subtrees.count) {
                    LayoutPaintScroll made = s;
                    made.parent = map(s.parent);
                    made.segment = to.segments.count;
                    made.clip = layoutClipMoved(s.clip, moved);
                    ref = layoutPaintOwn(to.scrolls.count);
                    open(s.node, subtrees.count, to.scrolls.count);
                    to.scrolls.push(made);
                }
                mapped.push(ref);
            }
            for (;;) {
                bool layerNext = layerAt < part.layers.count && part.layers.data[layerAt].segment == index;
                bool slotNext = slotAt < part.slots.count && part.slots.data[slotAt].segment == index;
                if (layerNext && slotNext)
                    layerNext = part.layers.data[layerAt].order < part.slots.data[slotAt].order;
                if (!layerNext && !slotNext)
                    break;
                nameNext();
                if (layerNext) {
                    LayoutPaintLayer l = part.layers.data[layerAt++];
                    l.offset = l.offset + moved;
                    l.scroll = map(l.scroll);
                    l.segment = to.segments.count;
                    l.order = named();
                    to.layers.push(l);
                } else {
                    LayoutPaintSlot s = part.slots.data[slotAt++];
                    s.box = layoutRectMoved(s.box, moved);
                    s.scroll = map(s.scroll);
                    s.segment = to.segments.count;
                    s.order = named();
                    to.slots.push(s);
                }
            }
        };
        // A segment that the part starts also becomes a segment of the recording, with its list taken as it is, and the
        // first segment continues the current one. A part therefore costs only its segments, however deeply the parts
        // around it are nested. A segment fixed to the viewport is taken as it is and joins no other.
        uint32_t fixedAt = 0;
        for (uint32_t i = 0; i < part.segments.count; ++i) {
            const LayoutPaintSegment& s = part.segments.data[i];
            before(i);
            segment(map(s.scroll));
            if (fixedAt < part.fixed.count && part.fixed.data[fixedAt].segment == i) {
                flush();
                prDrawListRetain(s.list);
                to.fixed.push(LayoutPaintFixed{to.segments.count, part.fixed.data[fixedAt++].clip});
                to.segments.push(LayoutPaintSegment{.list = s.list, .scroll = current, .offset = s.offset + moved});
                list = nullptr;
                continue;
            }
            if (list != nullptr || i == 0) {
                embedMoved(s.list, s.offset + moved);
                continue;
            }
            prDrawListRetain(s.list);
            to.segments.push(LayoutPaintSegment{.list = s.list, .scroll = current, .offset = s.offset + moved});
        }
        before(part.segments.count);
        for (uint32_t i = 0; i < part.regions.count; ++i)
            to.regions.push(LayoutPaintRegion{
                layoutRectMoved(part.regions.data[i].rect, moved),
                map(part.regions.data[i].scroll),
            });
        to.lists.append(part.lists.span());
        // A part ends where the walk is, which may be a different container from its last segment's. For example, a run
        // of a list ends in the container of its last box.
        follow();
    }

    // Starts recording a part into a recording of its own, at `offset` in the chunk's space.
    void enterPart(Subtree part) {
        flush();
        part.entry = top();
        part.outerList = list;
        part.outerCurrent = current;
        part.outerOrigin = origin;
        part.commands = commands;
        part.visits = visits;
        if (cache->spare.count != 0) {
            part.recording = cache->spare.last();
            cache->spare.pop();
        }
        subtrees.push(part);
        list = nullptr;
        current = resolve(top());
        origin = part.offset;
    }

    // Ends the innermost part, appends it to the recording around it, and returns it, with what it recorded.
    Subtree leavePart() {
        flush();
        Subtree s = subtrees.data[--subtrees.count];
        list = s.outerList;
        current = s.outerCurrent;
        origin = s.outerOrigin;
        append(s.recording, s.offset);
        return s;
    }

    // Returns true if a part recorded nothing: no commands, containers, names or wheel region.
    static bool isEmpty(const LayoutPaintRecording& recording) {
        return recording.segments.count == 0 && recording.scrolls.count == 0 && recording.layers.count == 0 &&
            recording.slots.count == 0 && recording.regions.count == 0;
    }

    // Embeds the cached recording of `fragment`'s subtree at `offset` for `phase` if it is valid. Returns false if
    // there is none to embed, so that the walk records the subtree.
    bool replay(const LayoutFragment* fragment, PrLayoutVec offset, LayoutPhase phase) {
        if (subs != RecordSubs_Cached)
            return false;
        uint32_t at = subFind(*cache, fragment, phase);
        if (at == kHashMissing)
            return false;
        const LayoutPaintSub& sub = cache->subs.data[at];
        if (!beganHere(sub.chunk, sub.entry) || !subIsCurrent(tree, sub))
            return false;
        append(sub.recording, offset);
        commands += sub.commands;
        ++stats->subtreesReused;
        return true;
    }

    // Starts recording `fragment`'s subtree into a recording of its own, whose origin is the subtree's top-left corner.
    // Returns the offset that the walk visits the subtree from, which is the subtree's position.
    PrLayoutVec enterSubtree(const LayoutFragment* fragment, PrLayoutVec offset, LayoutPhase phase) {
        enterPart(Subtree{.fragment = fragment, .phase = phase, .offset = offset});
        ++visits;
        return offset;
    }

    // Ends the innermost subtree or run and appends it to the recording around it. Caches a run's recording, and a
    // subtree's if it recorded anything, or if it recorded nothing but walking it again would visit at least
    // kPaintSubVisits fragments.
    void leaveSubtree() {
        Subtree s = leavePart();
        uint32_t recordedCommands = commands - s.commands;
        // A run is cached even if it records nothing, which saves examining its children again. A run of children holds
        // no box that paints whole, so it reads no list.
        bool worth = !s.placesOutside && (s.run != 0 || !isEmpty(s.recording) || visits - s.visits >= kPaintSubVisits);
        PR_ASSERT(s.run == 0 || layoutPhaseIsList(s.phase) || s.recording.lists.count == 0);
        if (subs != RecordSubs_Cached || !worth) {
            spareRecording(*cache, s.recording);
            return;
        }
        if (layoutPhaseIsList(s.phase)) {
            storeEntries(s, recordedCommands);
        } else if (s.run != 0) {
            storeRun(s, recordedCommands);
        } else {
            LayoutPaintSub sub{
                .fragment = s.fragment,
                .phase = s.phase,
                .chunk = chunk->node,
                .entry = s.entry,
                .recording = s.recording,
                .commands = recordedCommands,
                .recorded = cache->paints,
            };
            layoutFragmentRetain(s.fragment);
            subStore(*cache, sub);
            ++stats->subtreesRecorded;
        }
    }

    // Returns the end of the run of `fragment`'s children that starts at `start`. If the fragment has more than
    // kPaintRun children, they are split into runs of kPaintRun, and otherwise one run holds all of them. Runs are
    // recorded whether or not they are cached, so that a fresh recording has the same structure as a cached one.
    uint32_t runEnd(const LayoutFragment* fragment, uint32_t start) const {
        uint32_t count = fragment->childCount;
        return count > kPaintRun && start + kPaintRun < count ? start + kPaintRun : count;
    }

    // Returns true if `fragment` records its children in runs: it has more children than a run holds, and a node to key
    // the runs by.
    static bool hasRuns(const LayoutFragment* fragment) {
        return fragment->childCount > kPaintRun && fragment->node != kPrLayoutNodeNone;
    }

    // Embeds the cached recording of the run of `fragment`'s children from `start` to `end`, whose content space is at
    // `base`, if it is valid.
    bool replayRun(const LayoutFragment* fragment, uint32_t start, uint32_t end, PrLayoutVec base, LayoutPhase phase) {
        if (subs != RecordSubs_Cached || !hasRuns(fragment))
            return false;
        uint32_t at = runFind(*cache, fragment->node, phase, start / kPaintRun);
        if (at == kHashMissing)
            return false;
        const LayoutPaintRun& run = cache->runs.data[at];
        if (!beganHere(run.chunk, run.entry) || !runIsCurrent(tree, run, fragment, start, end))
            return false;
        append(run.recording, base + fragment->children[start].offset);
        commands += run.commands;
        ++stats->subtreesReused;
        return true;
    }

    // Starts recording the run of `fragment`'s children from `start` into a recording of its own, whose origin is the
    // top-left corner of its first child in the content space at `base`. Returns false if the children are not in runs,
    // in which case the walk records them in place.
    bool enterRun(const LayoutFragment* fragment, uint32_t start, uint32_t, PrLayoutVec base, LayoutPhase phase) {
        if (!hasRuns(fragment))
            return false;
        enterPart(Subtree{
            .fragment = fragment,
            .phase = phase,
            .run = start / kPaintRun + 1,
            .offset = base + fragment->children[start].offset,
        });
        return true;
    }

    void leaveRun() { leaveSubtree(); }

    // Embeds the cached recording of the run of a list whose boxes are `boxes`, keyed by the first box, if it is valid.
    bool replayEntries(LayoutNodeIndex, LayoutPhase phase, const LayoutStackedBox* boxes, uint32_t count,
            PrLayoutVec at, PrLayoutVec reference) {
        if (subs != RecordSubs_Cached)
            return false;
        uint32_t found = runFind(*cache, boxes[0].node, phase, 0);
        if (found == kHashMissing)
            return false;
        const LayoutPaintRun& run = cache->runs.data[found];
        PrLayoutVec first = layoutNodePosition(tree, boxes[0].node);
        if (!beganHere(run.chunk, run.entry) || !entriesCurrent(tree, run, boxes, count, first))
            return false;
        append(run.recording, at + first - reference);
        commands += run.commands;
        ++stats->subtreesReused;
        return true;
    }

    // Starts recording the run of a list whose boxes are `boxes` into a recording of its own. The recording's origin is
    // where the first box is placed, given that the context's reference box is at `at`.
    bool enterEntries(LayoutNodeIndex, LayoutPhase phase, const LayoutStackedBox* boxes, uint32_t count, PrLayoutVec at,
            PrLayoutVec reference) {
        PrLayoutVec first = layoutNodePosition(tree, boxes[0].node);
        enterPart(Subtree{
            .phase = phase,
            .run = 1,
            .offset = at + first - reference,
            .boxes = boxes,
            .count = count,
            .first = first,
        });
        return true;
    }

    void leaveEntries() { leaveSubtree(); }

    // Returns the cached run of `key`, emptied, and creates it if there is none.
    LayoutPaintRun& runFor(LayoutNodeIndex key, LayoutPhase phase, uint32_t index) {
        uint32_t at = runFind(*cache, key, phase, index);
        if (at == kHashMissing) {
            at = cache->runs.count;
            cache->runs.emplace() = LayoutPaintRun{};
            cache->runIndex.insert(runHash(key, phase, index), at);
        }
        LayoutPaintRun& run = cache->runs.data[at];
        runRelease(*cache, run);
        run.node = key;
        run.phase = phase;
        run.index = index;
        ++stats->subtreesRecorded;
        return run;
    }

    // Caches the recording of the run of a list that the part `s` recorded, replacing the one of the same key.
    void storeEntries(Subtree& s, uint32_t recordedCommands) {
        LayoutPaintRun& run = runFor(s.boxes[0].node, s.phase, 0);
        run.chunk = chunk->node;
        run.entry = s.entry;
        run.recording = s.recording;
        run.commands = recordedCommands;
        run.recorded = cache->paints;
        run.layouts = tree->layouts;
        run.placed = tree->cache.placed;
        for (uint32_t i = 0; i < s.count; ++i) {
            LayoutNodeIndex node = s.boxes[i].node;
            const LayoutFragment* fragment = entryFragment(tree, node);
            if (fragment != nullptr)
                layoutFragmentRetain(fragment);
            LayoutPositioned place = layoutNodePositioned(tree, node);
            run.entries.push(LayoutPaintPlaced{node, fragment, place.position - s.first, place.container});
        }
    }

    // Caches the recording of the run that the part `s` recorded, replacing the one of the same key.
    void storeRun(Subtree& s, uint32_t recordedCommands) {
        uint32_t index = s.run - 1;
        uint32_t start = index * kPaintRun;
        uint32_t end = runEnd(s.fragment, start);
        LayoutPaintRun& run = runFor(s.fragment->node, s.phase, index);
        run.chunk = chunk->node;
        run.entry = s.entry;
        run.recording = s.recording;
        run.commands = recordedCommands;
        run.recorded = cache->paints;
        PrLayoutVec first = s.fragment->children[start].offset;
        for (uint32_t i = start; i < end; ++i) {
            const LayoutChild& child = s.fragment->children[i];
            layoutFragmentRetain(child.fragment);
            run.children.push(LayoutChild{child.fragment, child.offset - first});
        }
    }

    PrLayoutVec pushScroll(const LayoutFragment* fragment, PrLayoutRect padding, PrLayoutVec) {
        container(fragment->node, fragment, padding, LayoutContainerKind_Scroll);
        localBackground(fragment, padding);
        return kLayoutVecZero;
    }

    // Records the background layers of the box `node`, from the bottom layer up. `boxes` are the box's background
    // boxes. If the box `scrolls`, the layers attached to its content are left to the content's recording. A layer
    // fixed to the viewport is recorded as a segment of its own. If a transform applies to the box, a fixed layer is
    // recorded as a scrolling layer instead (css-transforms-1/#transform-rendering), and so is a fixed layer of a
    // background that blends, which is one group.
    void backgroundLayers(LayoutNodeIndex node, const LayoutBackground& layers, const LayoutBackgroundBoxes& boxes,
            bool scrolls) {
        // A transform is looked for only if a layer is fixed, since that search walks the box's ancestors.
        int8_t viewportFixed = -1;
        for (uint32_t i = layers.count; i-- > 0;) {
            const PrLayoutBackgroundLayer& layer = layers.layers[i];
            if (layer.attachment == PrLayoutBackgroundAttachment_Local && scrolls)
                continue;
            bool fixed = layer.attachment == PrLayoutBackgroundAttachment_Fixed;
            if (fixed && viewportFixed < 0)
                viewportFixed = layers.blends || transformedAround(node) ? 0 : 1;
            if (fixed && viewportFixed == 1)
                fixedLayer(layer, boxes.boxes[layer.clip]);
            else if (layoutBackgroundPaintLayer(&styles->compounds, layer, boxes, out()))
                recorded();
        }
    }

    // Returns true if a transform applies to the box `node` or to a box around it.
    bool transformedAround(LayoutNodeIndex node) const {
        for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone; at = layoutTreeNode(tree, at).parent) {
            if (layoutIsTransformed(tree, at))
                return true;
        }
        return false;
    }

    // Records a background layer that is fixed to the viewport. The layer is sized and placed in the viewport
    // (css-backgrounds-3/#valdef-background-attachment-fixed), in the viewport's space. It paints within `clip`, which
    // is in the recording's space.
    void fixedLayer(const PrLayoutBackgroundLayer& layer, PrRect clip) {
        PrRect viewport{0, 0, prLayoutUnitToPx(tree->viewport.x), prLayoutUnitToPx(tree->viewport.y)};
        LayoutBackgroundBoxes boxes{.boxes = {viewport, viewport, viewport}, .area = viewport};
        PrDrawList* content = prDrawListCreate();
        if (layoutBackgroundPaintLayer(&styles->compounds, layer, boxes, content))
            fixed(content, clip);
        else
            prDrawListRelease(content);
    }

    // Opens the isolated group of a background whose layers blend (compositing-1/#background-blend-mode), so that they
    // blend with the layers below them and the background color, not with what lies beneath the box. The group covers
    // the border box and the painting area of `boxes`. The batch is held until the group closes, since a list that is
    // embedded must have no open pushes. Returns whether the batch was held before, which closeBlending takes.
    bool openBlending(const LayoutBackgroundBoxes& boxes) {
        bool wasHeld = held;
        held = true;
        PrDrawGroup group = prDrawGroupDefault();
        group.isolated = true;
        group.bounds = boxes.boxes[PrLayoutBackgroundBox_BorderBox];
        if (boxes.area.right > boxes.area.left && boxes.area.bottom > boxes.area.top)
            group.bounds = prRectUnion(group.bounds, boxes.area);
        prDrawListPushGroup(out(), &group);
        return wasHeld;
    }
    void closeBlending(bool wasHeld) {
        prDrawListPopGroup(out());
        held = wasHeld;
        recorded();
    }

    // Records the background color within the box `clip` of `boxes`, if it paints.
    void backgroundColor(PrColor16 color, const LayoutBackgroundBoxes& boxes, PrLayoutBackgroundBox clip) {
        PrRect rect = boxes.boxes[clip];
        if (!(prFloat16ToFloat(color.a) > 0) || !(rect.right > rect.left) || !(rect.bottom > rect.top))
            return;
        if (boxes.rounded)
            prDrawListCornerRect(out(), boxes.shapes[clip], {.color = color, .edges = PrDrawEdges_Snapped});
        else
            prDrawListRect(out(), rect, {.color = color, .edges = PrDrawEdges_Snapped});
        recorded();
    }

    // Records the background layers that are attached to the content of the scroll container `fragment`, whose padding
    // box is `padding`. They are recorded under the content, so they scroll with it. The color is recorded with them if
    // the bottom layer is attached to the content. The layers are placed and painted relative to the scrollable
    // overflow area rather than to the border (css-backgrounds-3/#valdef-background-attachment-local). The border box
    // and the padding box are that area, and the content box is the area inside the padding. The root's layers paint
    // the canvas instead.
    void localBackground(const LayoutFragment* fragment, PrLayoutRect padding) {
        const LayoutTreeNode& n = layoutTreeNode(tree, fragment->node);
        if (fragment->node == tree->root || layoutColorStyle(styles, n.style).visibility != PrLayoutVisibility_Visible)
            return;
        const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(styles, n.style);
        const LayoutBackground& layers = layoutBackgroundAt(&styles->backgrounds, decoration.backgroundLayers);
        bool color = decoration.backgroundAttachment == PrLayoutBackgroundAttachment_Local &&
            prFloat16ToFloat(decoration.background.a) > 0;
        if (layers.count == 0 && !color)
            return;
        const PrLayoutBoxStyle& box = layoutBoxStyle(styles, n.style);
        PrLayoutRect content = layoutScrollContent(fragment, box);
        PrLayoutVec start{padding.left, padding.top};
        PrRect area = prLayoutRectToPx(layoutRectMoved(content, start - origin));
        PrRect framed{
            area.left - prLayoutUnitToPx(box.border[PrRectSide_Left]),
            area.top - prLayoutUnitToPx(box.border[PrRectSide_Top]),
            area.right + prLayoutUnitToPx(box.border[PrRectSide_Right]),
            area.bottom + prLayoutUnitToPx(box.border[PrRectSide_Bottom]),
        };
        LayoutBackgroundBoxes boxes = layoutBackgroundBoxes(styles, fragment, box, framed);
        boxes.boxes[PrLayoutBackgroundBox_BorderBox] = boxes.boxes[PrLayoutBackgroundBox_PaddingBox];
        // The layers that scroll with the content blend among themselves, and with the color if it scrolls too.
        bool wasHeld = layers.blends && openBlending(boxes);
        if (color)
            backgroundColor(decoration.background, boxes, decoration.backgroundClip);
        for (uint32_t i = layers.count; i-- > 0;) {
            const PrLayoutBackgroundLayer& layer = layers.layers[i];
            if (layer.attachment == PrLayoutBackgroundAttachment_Local &&
                    layoutBackgroundPaintLayer(&styles->compounds, layer, boxes, out()))
                recorded();
        }
        if (layers.blends)
            closeBlending(wasHeld);
    }

    void pushClip(const LayoutFragment* fragment, PrLayoutRect rect) {
        container(fragment->node, fragment, rect, LayoutContainerKind_Clip);
    }

    // A sticky box's content is recorded where it was laid out, and clipped by nothing of its own.
    PrLayoutVec pushSticky(LayoutNodeIndex node) {
        PrLayoutRect unbounded{
            layoutUnit(-kLayoutClipUnbounded.raw), layoutUnit(-kLayoutClipUnbounded.raw), kLayoutClipUnbounded,
            kLayoutClipUnbounded,
        };
        container(node | kLayoutContainerSticky, nullptr, unbounded, LayoutContainerKind_Sticky);
        return kLayoutVecZero;
    }
    void popSticky() { leave(); }

    // Records the decoration that follows as a batch of its own, which popBoxClip embeds clipped to `rect`.
    void pushBoxClip(PrLayoutRect rect) {
        flush();
        held = true;
        heldClip = rect;
    }
    void popBoxClip() {
        held = false;
        if (batch == nullptr)
            return;
        PrDrawList* to = target();
        prDrawListPushClipRect(to, prLayoutRectToPx(layoutRectMoved(heldClip, kLayoutVecZero - origin)),
            PrDrawEdges_Snapped);
        prDrawListEmbed(to, batch, PrDVec2{0, 0});
        prDrawListPopClip(to);
        prDrawListRelease(batch);
        batch = nullptr;
        batched = 0;
    }

    // Returns true if the box `node` is the chunk's box or is inside it, so that what the chunk records of it is the
    // chunk's own.
    bool inChunk(LayoutNodeIndex node) const {
        for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone; at = layoutTreeNode(tree, at).parent) {
            if (at == chunk->node)
                return true;
        }
        return false;
    }

    // Records what follows into the content of the container `node` (layoutNodeContainer), which clips to `clip` and is
    // of `kind`. Its box's fragment is `fragment`, which a sticky box needs not give. A box that only clips, and a
    // sticky box, have no scroll range and no axes that a user scrolls. A container around the chunk's box, which a box
    // of a list escapes to, is recorded by a recording around the chunk's. A container's content is entered once in
    // each phase, and out of order for the boxes of lists in it. It is one container of the chunk, recorded by the
    // innermost recording that first enters it.
    void container(LayoutNodeIndex node, const LayoutFragment* PR_NULLABLE fragment, PrLayoutRect clip,
            LayoutContainerKind kind) {
        LayoutPaintContainerRef id{node, PaintRefKind_Container};
        LayoutPaintContainerRef ref = resolve(id);
        LayoutNodeIndex owner = layoutContainerBox(node);
        if (ref.kind == PaintRefKind_Container && inChunk(owner) && !opensAround(node)) {
            LayoutPaintRecording& to = into();
            PrLayoutVec port{layoutRectWidth(clip), layoutRectHeight(clip)};
            const PrLayoutBoxStyle& box = layoutBoxStyle(styles, layoutTreeNode(tree, owner).style);
            bool scrolls = kind == LayoutContainerKind_Scroll;
            LayoutPaintScroll made{
                .node = node,
                .parent = current,
                .segment = to.segments.count,
                .clip = layoutClipMoved(clip, kLayoutVecZero - origin),
                .kind = kind,
            };
            // A scroll container's clip rectangle cuts the clip of its scrollport, but the whole scrollport still
            // decides the scroll range.
            PrLayoutRect clipped = clip;
            if (scrolls && box.clipped) {
                PrLayoutVec corner{clip.left - box.border[PrRectSide_Left], clip.top - box.border[PrRectSide_Top]};
                PrLayoutRect rect = layoutClipRect(tree, node, layoutRectAt(corner, fragment->extent));
                clipped = layoutRectIntersect(clip, rect);
                made.clip = layoutClipMoved(clipped, kLayoutVecZero - origin);
            }
            if (kind != LayoutContainerKind_Sticky)
                made.curved = layoutClipCorners(tree, fragment, clipped, made.corners);
            if (scrolls) {
                made.content = layoutScrollContent(fragment, box);
                made.range = layoutScrollRange(port, made.content);
                made.portExtent = port;
                made.userAxes = layoutUserAxes(box);
                made.chainAxes = layoutChainAxes(box);
            }
            ref = layoutPaintOwn(to.scrolls.count);
            open(node, subtrees.count, to.scrolls.count);
            to.scrolls.push(made);
        }
        stack.push(id);
        segment(ref);
    }

    void popScroll() { leave(); }
    void popClip() { leave(); }

    // Names a box with a layer of its own at `offset`. The layer plays after what is recorded so far and before what
    // follows. Returns false for any other box, which the walk visits.
    bool takesLayer(const LayoutFragment* fragment, PrLayoutVec offset) {
        if (!layoutOwnsLayer(styles, layoutTreeNode(tree, fragment->node).style) &&
                !layoutIsolatesBlending(tree, fragment->node))
            return false;
        nameNext();
        into().layers.push(LayoutPaintLayer{
            fragment, offset - origin, current, into().segments.count, named(),
            fragment->node, LayoutPieceAt{kLayoutPieceNone, kLayoutPieceNone},
        });
        return true;
    }

    // Names the inline box `node`, which has a layer of its own and whose opening part is on the line at `place`, in
    // the same way that takesLayer names a box. Its layer is placed at its container's border box, which is where the
    // container's content is.
    bool takesInlineLayer(LayoutNodeIndex node, const LayoutWalkPlace& place) {
        if (!layoutInlineOwnsLayer(styles, layoutTreeNode(tree, node).style))
            return false;
        nameNext();
        into().layers.push(LayoutPaintLayer{
            place.container, place.base - origin, current, into().segments.count,
            named(), node, place.at,
        });
        return true;
    }

    // Notes the list of a stacking context that the walk paints from, as the cache found it.
    void listed(LayoutNodeIndex context, uint32_t version) { into().lists.push(LayoutPaintListed{context, version}); }

    // Notes where the box `node` of a list paints: its fragment, or the fragment of an inline box's block container, at
    // `at`. If the box is placed through the box of an open subtree, that subtree's fragment determines its position.
    // If it is placed through the chunk's box, the chunk's fragment determines it. The subtrees inside the one that it
    // is placed through record its position separately, and the chunk does the same if it is placed through neither.
    // The box that it is placed through can move it while the chunk's fragment stays the same, so the recording is
    // current only while the box is where it was.
    void placed(LayoutNodeIndex node, const LayoutFragment* fragment, PrLayoutVec at) {
        LayoutNodeIndex reference = chunk->fragment->node;
        uint32_t through = 0;
        bool inner = false;
        for (LayoutNodeIndex p = layoutCachePlacement(tree->cache, node).parent; p != kPrLayoutNodeNone && !inner;
                p = layoutCachePlacement(tree->cache, p).parent) {
            inner = p == reference;
            for (uint32_t i = subtrees.count; i-- > through;) {
                if (subtrees.data[i].run == 0 && subtrees.data[i].fragment->node == p) {
                    through = i + 1;
                    break;
                }
            }
        }
        // A run of a list checks the positions of its boxes from its context, so it is not marked if the chunk's box or
        // an open subtree places the box.
        for (uint32_t i = through; i < subtrees.count; ++i) {
            if (!layoutPhaseIsList(subtrees.data[i].phase) || (!inner && through == 0))
                subtrees.data[i].placesOutside = true;
        }
        if (inner || through != 0)
            return;
        layoutFragmentRetain(fragment);
        chunk->outer.push(LayoutPaintPlaced{node, fragment, at, layoutNodeContainer(tree, node)});
    }

    // Returns the order of the next layer or slot among those that the innermost recording names.
    uint32_t named() { return into().layers.count + into().slots.count; }

    // Names a slot of embedded content with its content box `box` in the chunk's space. Like a layer, the slot plays
    // after what is recorded so far and before what follows.
    void slot(uint64_t embed, LayoutNodeIndex node, PrLayoutRect box) {
        nameNext();
        into().slots.push(LayoutPaintSlot{
            .embed = embed,
            .node = node,
            .box = layoutRectMoved(box, kLayoutVecZero - origin),
            .scroll = current,
            .segment = into().segments.count,
            .order = named(),
        });
    }

    // Names each of the tree's carets that stands in `fragment` after it, placed at `at`. The text around a caret
    // records as it would without one, so what a caret costs does not grow with its text. The search for the carets'
    // fragments runs once, since it visits the fragments of their nodes.
    void caretSlots(const LayoutFragment* fragment, PrLayoutVec at) {
        const RawArray<PrLayoutCaret>& carets = tree->carets.carets;
        if (carets.count == 0)
            return;
        if (!caretsSearched) {
            caretsSearched = true;
            caretFragments.resize(carets.count);
            for (uint32_t i = 0; i < carets.count; ++i) {
                const PrLayoutCaret& caret = carets.data[i];
                caretFragments.data[i] = nullptr;
                if (prLayoutNodeIsValid(static_cast<const PrLayoutTree*>(tree), caret.position.node)) {
                    caretFragments.data[i] = layoutCaretFragment(tree, layoutNodeIndex(tree, caret.position.node),
                        caret.position.offset, caret.affinity);
                }
            }
        }
        for (uint32_t i = 0; i < carets.count; ++i) {
            if (caretFragments.data[i] != fragment)
                continue;
            float along = layoutTextCaretAlong(tree, fragment, carets.data[i].position.offset);
            nameNext();
            into().slots.push(LayoutPaintSlot{
                .node = fragment->node,
                .box = layoutTextSpanRect(tree, fragment, at - origin, along, along + 1),
                .scroll = current,
                .segment = into().segments.count,
                .order = named(),
                .caret = true,
                .color = carets.data[i].color,
            });
        }
    }

    // Adds the border box `at` of `fragment` to the wheel region, with the area that its descendants reach, unless the
    // box clips them as a scroll container does.
    void region(const LayoutFragment* fragment, PrLayoutRect at) {
        PrLayoutRect rect = at;
        if (fragment->kind == LayoutFragmentKind_Box && !layoutFragmentScrolls(tree, fragment))
            rect = layoutRectUnion(rect, layoutRectMoved(fragment->overflow, PrLayoutVec{at.left, at.top}));
        into().regions.push(LayoutPaintRegion{layoutRectMoved(rect, kLayoutVecZero - origin), current});
    }

    // Boxes with effects are layers of their own, which the walk does not enter.
    void pushTransform(const PrDMatrix3&) { PR_ASSERT(false); }
    void pushOpacity(float) { PR_ASSERT(false); }

    // Counts a box or text run of `node` that is recorded again although nothing it paints has changed since
    // `previous`.
    void repainted(LayoutNodeIndex node) {
        if (previous == 0 || node == kPrLayoutNodeNone)
            return;
        const LayoutTreeNode& n = layoutTreeNode(tree, node);
        bool changed = n.paintSubtree >= previous ||
            (n.kind == PrLayoutNodeKind_Text && layoutTreeNode(tree, n.parent).paintChanged >= previous);
        if (!changed)
            ++stats->boxesRepaintedUnchanged;
    }

    // Records the column rules of the multi-column container `fragment`, whose border box is `rect`: one in the middle
    // of each gap between two adjacent columns of a column set that both hold content, as tall as the set
    // (css-multicol-1/#column-gaps-and-rules). The rule's width is snapped as a border's.
    void columnRules(const LayoutFragment* fragment, PrRect rect, const PrLayoutDecorationStyle& decoration) {
        float width = prLayoutUnitToPx(layoutBorderWidth(styles, decoration.columnRuleWidth));
        const LayoutFragment* last = nullptr;
        PrRect before{};
        bool any = false;
        for (uint32_t i = 0; i < fragment->childCount; ++i) {
            const LayoutChild& child = fragment->children[i];
            if ((child.fragment->flags & LayoutFragmentFlag_Column) == 0) {
                last = nullptr;
                continue;
            }
            PrRect column = prLayoutRectToPx(layoutRectAt(child.offset, child.fragment->extent));
            column = PrRect{
                column.left + rect.left, column.top + rect.top, column.right + rect.left,
                column.bottom + rect.top,
            };
            bool holds = child.fragment->childCount != 0;
            bool sameSet = last != nullptr && (column.top == before.top || column.left == before.left);
            if (holds && sameSet) {
                // Columns side by side in x have a vertical rule between them, and columns stacked in y, as a vertical
                // writing mode stacks them, a horizontal one.
                PrDrawBorder rule{};
                PrRect line;
                PrRectSide side;
                if (column.top == before.top && column.left != before.left) {
                    float middle = (prF32Min(column.right, before.right) + prF32Max(column.left, before.left)) / 2;
                    line = PrRect{middle - width / 2, column.top, middle + width / 2, column.bottom};
                    side = PrRectSide_Left;
                } else {
                    float middle = (prF32Min(column.bottom, before.bottom) + prF32Max(column.top, before.top)) / 2;
                    line = PrRect{column.left, middle - width / 2, column.right, middle + width / 2};
                    side = PrRectSide_Top;
                }
                rule.shape = prCornerRectRound(line, 0);
                rule.sides[side] = PrDrawBorderSide{
                    .width = width,
                    .style = decoration.columnRuleStyle,
                    .color = decoration.columnRuleColor,
                };
                prDrawListBorder(out(), &rule, {.edges = PrDrawEdges_Snapped});
                any = true;
            }
            last = holds ? child.fragment : nullptr;
            before = column;
        }
        if (any)
            recorded();
    }

    // Records a box's background, unless the canvas took the background, and then its border and its outline. The
    // background is its color, within the box that its background-clip names, and its layers over it. A side at which
    // an inline box breaks across lines has no border (css-break-4/#break-decoration), and each of an inline box's
    // fragments places its background layers in its own boxes. The outline is painted with the box's decoration, so a
    // box painted later covers it, whereas CSS2/#elaborate-stacking-contexts paints outlines last in their stacking
    // context.
    //
    // A table's tracks and track groups paint only their outlines. Their cells paint their backgrounds
    // (gridBackgrounds). A table paints its background and border over its grid, and its outline around its whole box,
    // captions included. A table or cell whose borders collapse paints no border, because its grid draws the collapsed
    // borders (gridBorders). A cell that empty-cells hides paints nothing (css-tables-3/#empty-cell-rendering).
    void box(const LayoutFragment* fragment, PrLayoutRect at, bool visible, bool root) {
        ++visits;
        repainted(fragment->node);
        PrLayoutStyleIndex style = layoutFragmentStyle(tree, fragment);
        // A hidden box's descendants may show, and the wheel over them waits as well.
        if (layoutColorStyle(styles, style).wheelEvents == PrLayoutWheelEvents_Blocking)
            region(fragment, at);
        if (!visible)
            return;
        const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(styles, style);
        const PrLayoutBoxStyle& box = layoutBoxStyle(styles, style);
        bool borders = true;
        bool decorates = true;
        PrRect whole = prLayoutRectToPx(layoutRectMoved(at, kLayoutVecZero - origin));
        if (box.display >= PrLayoutDisplay_Table) {
            if (layoutDisplayIsTrack(box.display)) {
                decorates = false;
                borders = false;
            } else if (layoutIsTable(box)) {
                at = layoutRectMoved(layoutFragmentTableGrid(fragment).rect, PrLayoutVec{at.left, at.top});
                borders = layoutTableStyle(styles, style).borderCollapse != PrLayoutBorderCollapse_Collapse;
            } else if (box.display == PrLayoutDisplay_TableCell) {
                borders = !layoutTableCellCollapses(tree, fragment->node);
                if (borders && layoutColorStyle(styles, style).emptyCells == PrLayoutEmptyCells_Hide &&
                        layoutTableCellIsEmpty(fragment))
                    return;
            }
        }
        PrRect rect = prLayoutRectToPx(layoutRectMoved(at, kLayoutVecZero - origin));
        // Corners do not apply to a track, nor to a table or cell whose borders collapse
        // (css-backgrounds-3/#corner-clipping).
        bool rounded = decorates && borders && layoutHasCorners(decoration);
        PrCornerRect shape;
        if (rounded)
            shape = layoutBorderShape(styles, decoration, rect, fragment->broken);
        else
            shape = prCornerRectRound(rect, 0);
        PrColor16 background = decoration.background;
        const LayoutBackground& layers = layoutBackgroundAt(&styles->backgrounds, decoration.backgroundLayers);
        if (decorates && !root && (prFloat16ToFloat(background.a) > 0 || layers.count != 0)) {
            LayoutBackgroundBoxes boxes = layoutBackgroundBoxes(styles, fragment, box, rect);
            if (rounded) {
                uint32_t used = 1u << decoration.backgroundClip;
                for (uint32_t i = 0; i < layers.count; ++i)
                    used |= 1u << layers.layers[i].clip;
                layoutBackgroundRound(boxes, shape, used);
            }
            bool scrolls = layoutFragmentScrolls(tree, fragment);
            bool colorScrolls = scrolls && decoration.backgroundAttachment == PrLayoutBackgroundAttachment_Local;
            bool wasHeld = layers.blends && openBlending(boxes);
            if (!colorScrolls)
                backgroundColor(background, boxes, decoration.backgroundClip);
            backgroundLayers(fragment->node, layers, boxes, scrolls);
            if (layers.blends)
                closeBlending(wasHeld);
        }
        PrDrawBorder border{};
        border.shape = shape;
        bool any = false;
        for (uint32_t side = 0; borders && side < PrRectSide_Count; ++side) {
            bool broken = (fragment->broken & (1u << side)) != 0;
            PrDrawBorderSide& s = border.sides[side];
            s.width = broken ? 0 : prLayoutUnitToPx(box.border[side]);
            s.style = broken ? PrDrawBorderStyle_None : decoration.borderStyle[side];
            s.color = decoration.borderColor[side];
            any = any || (s.width > 0 && prDrawBorderStylePaints(s.style) && prFloat16ToFloat(s.color.a) > 0);
        }
        if (any) {
            prDrawListBorder(out(), &border, {.edges = PrDrawEdges_Snapped});
            recorded();
        }
        if (prDrawBorderStylePaints(decoration.columnRuleStyle) && decoration.columnRuleWidth > kLayoutUnitZero &&
                prFloat16ToFloat(decoration.columnRuleColor.a) > 0)
            columnRules(fragment, rect, decoration);
        PrLayoutUnit outlineWidth = layoutBorderWidth(styles, decoration.outlineWidth);
        if (decoration.outlineStyle != PrDrawBorderStyle_None && outlineWidth > kLayoutUnitZero &&
                prFloat16ToFloat(decoration.outlineColor.a) > 0) {
            // A negative offset shrinks the outline into the border box, but the outline's outer edge is never smaller
            // than twice its width in either dimension (css-ui-4/#negative-offset). The offset snaps as the width does
            // (w3c/csswg-drafts#12906). The outline follows the border box's corners
            // (css-borders-4/#corner-shape-rendering): its inner edge is the border box's shape moved by the offset,
            // grown as a shadow's spread grows it or shrunk, and its outer edge is that edge grown by the width, whose
            // curves the width then insets back to the inner edge.
            float width = prLayoutUnitToPx(outlineWidth);
            float outset = prLayoutUnitToPx(layoutSnapBorderWidth(styles, decoration.outlineOffset)) + width;
            float outsetX = prF32Max(outset, (2 * width - (whole.right - whole.left)) / 2);
            float outsetY = prF32Max(outset, (2 * width - (whole.bottom - whole.top)) / 2);
            PrDrawBorder outline{};
            if (rounded) {
                PrCornerRect corners = layoutBorderShape(styles, decoration, whole, fragment->broken);
                float offsetX = outsetX - width;
                float offsetY = outsetY - width;
                float grown[PrRectSide_Count] = {
                    prF32Max(offsetY, 0), prF32Max(offsetX, 0), prF32Max(offsetY, 0),
                    prF32Max(offsetX, 0),
                };
                float shrunk[PrRectSide_Count] = {
                    prF32Max(-offsetY, 0), prF32Max(-offsetX, 0), prF32Max(-offsetY, 0),
                    prF32Max(-offsetX, 0),
                };
                PrCornerRect inner = prCornerRectInset(prCornerRectOutset(corners, grown), shrunk);
                outline.shape = inner;
                outline.shape.rect = PrRect{
                    inner.rect.left - width, inner.rect.top - width, inner.rect.right + width,
                    inner.rect.bottom + width,
                };
                for (PrCorner& c : outline.shape.corners) {
                    if (c.radii.x > 0 && c.radii.y > 0)
                        c.radii = PrVec2{c.radii.x + width, c.radii.y + width};
                }
            } else {
                outline.shape = prCornerRectRound(
                    PrRect{whole.left - outsetX, whole.top - outsetY, whole.right + outsetX, whole.bottom + outsetY}, 0);
            }
            for (PrDrawBorderSide& s : outline.sides)
                s = PrDrawBorderSide{
                    .width = prLayoutUnitToPx(outlineWidth),
                    .style = decoration.outlineStyle,
                    .color = decoration.outlineColor,
                };
            prDrawListBorder(out(), &outline, {.edges = PrDrawEdges_Snapped});
            recorded();
        }
    }

    // Records the backgrounds of a table's column groups, columns, row groups and rows behind its cells, in that order
    // (css-tables-3/#drawing-cell-backgrounds). The table's fragment is `grid` at `at`, and holds its parts. A track's
    // background is positioned in the track's own box, and painted only within the border boxes of the cells that start
    // in the track. A cell starts in the first column, in column order, that its box overlaps. A cell that is hidden,
    // or that empty-cells hides, has none of these backgrounds. The backgrounds behind the cells of a track or cell
    // that paints whole paint with it instead, so that they move and stack with it.
    void gridBackgrounds(const LayoutFragment* grid, PrLayoutVec at) {
        tableColumns(grid, at);
        gridCells(grid, at, kPrLayoutNodeNone, PrLayoutRect{}, kPrLayoutNodeNone, PrLayoutRect{});
    }

    // Records the backgrounds of the tracks and track groups behind the cells of the row group, row or cell `part` at
    // `at`, which paints whole, so that they paint with it rather than with its table.
    void trackBackgrounds(const LayoutFragment* part, PrLayoutVec at) {
        LayoutNodeIndex node = part->node;
        LayoutNodeIndex table = kPrLayoutNodeNone;
        LayoutNodeIndex group = kPrLayoutNodeNone;
        LayoutNodeIndex row = kPrLayoutNodeNone;
        for (LayoutNodeIndex up = layoutTreeNode(tree, node).parent;
                up != kPrLayoutNodeNone && table == kPrLayoutNodeNone; up = layoutTreeNode(tree, up).parent) {
            const PrLayoutBoxStyle& box = layoutBoxStyle(styles, layoutTreeNode(tree, up).style);
            if (layoutIsTable(box))
                table = up;
            else if (box.display == PrLayoutDisplay_TableRow && row == kPrLayoutNodeNone)
                row = up;
            else if (layoutDisplayIsRowGroup(box.display) && group == kPrLayoutNodeNone)
                group = up;
        }
        const LayoutFragment* grid = table != kPrLayoutNodeNone ? layoutCachePlaced(tree->cache, table) : nullptr;
        if (grid == nullptr)
            return;
        PrLayoutVec position = layoutNodePosition(tree, node);
        auto rectOf = [&](LayoutNodeIndex box) {
            const LayoutFragment* f = box != kPrLayoutNodeNone ? layoutCachePlaced(tree->cache, box) : nullptr;
            if (f == nullptr)
                return PrLayoutRect{};
            return layoutRectAt(at + layoutNodePosition(tree, box) - position, f->extent);
        };
        tableColumns(grid, at + layoutNodePosition(tree, table) - position);
        PrLayoutRect rect = layoutRectAt(at, part->extent);
        PrLayoutDisplay display = layoutBoxStyle(styles, layoutTreeNode(tree, node).style).display;
        if (layoutDisplayIsRowGroup(display))
            gridCells(part, at, node, rect, kPrLayoutNodeNone, PrLayoutRect{});
        else if (display == PrLayoutDisplay_TableRow)
            gridCells(part, at, group, rectOf(group), node, rect);
        else
            cellBackgrounds(part, rect, group, rectOf(group), row, rectOf(row));
    }

    // Sets `columns` to the columns of the table `grid` at `at`, in order.
    void tableColumns(const LayoutFragment* grid, PrLayoutVec at) {
        columns.clear();
        for (uint32_t i = 0; i < grid->childCount; ++i) {
            const LayoutChild& child = grid->children[i];
            const LayoutFragment* f = child.fragment;
            if (f->node == kPrLayoutNodeNone)
                continue;
            PrLayoutDisplay display = layoutBoxStyle(styles, layoutTreeNode(tree, f->node).style).display;
            PrLayoutRect rect = layoutRectAt(at + child.offset, f->extent);
            if (display == PrLayoutDisplay_TableColumn) {
                columns.push(GridColumn{f->node, kPrLayoutNodeNone, rect, PrLayoutRect{}});
            } else if (display == PrLayoutDisplay_TableColumnGroup) {
                if (f->childCount == 0)
                    columns.push(GridColumn{kPrLayoutNodeNone, f->node, PrLayoutRect{}, rect});
                for (uint32_t k = 0; k < f->childCount; ++k) {
                    const LayoutChild& column = f->children[k];
                    columns.push(GridColumn{
                        column.fragment->node, f->node,
                        layoutRectAt(at + child.offset + column.offset, column.fragment->extent),
                        rect,
                    });
                }
            }
        }
    }

    // Records the track backgrounds behind each cell in `fragment` at `at`. The cells are in the row group `group` and
    // the row `row`, whose boxes are `groupRect` and `rowRect`, if those are not kPrLayoutNodeNone. A track or cell
    // that paints whole records its own (trackBackgrounds).
    void gridCells(const LayoutFragment* fragment, PrLayoutVec at, LayoutNodeIndex group, PrLayoutRect groupRect,
            LayoutNodeIndex row, PrLayoutRect rowRect) {
        for (uint32_t i = 0; i < fragment->childCount; ++i) {
            const LayoutChild& child = fragment->children[i];
            const LayoutFragment* f = child.fragment;
            if (f->kind != LayoutFragmentKind_Box)
                continue;
            if (styles->wholeTableParts != 0 && f->node != kPrLayoutNodeNone && layoutFragmentPaintsWhole(tree, f))
                continue;
            PrLayoutRect rect = layoutRectAt(at + child.offset, f->extent);
            PrLayoutDisplay display = PrLayoutDisplay_TableCell;
            if (f->node != kPrLayoutNodeNone)
                display = layoutBoxStyle(styles, layoutTreeNode(tree, f->node).style).display;
            if (layoutDisplayIsRowGroup(display))
                gridCells(f, at + child.offset, f->node, rect, kPrLayoutNodeNone, PrLayoutRect{});
            else if (display == PrLayoutDisplay_TableRow)
                gridCells(f, at + child.offset, group, groupRect, f->node, rect);
            else if (display == PrLayoutDisplay_TableCell)
                cellBackgrounds(f, rect, group, groupRect, row, rowRect);
        }
    }

    // Records the track backgrounds behind the cell `cell`, whose border box is `rect`, in the row group `group` and
    // the row `row`, whose boxes are `groupRect` and `rowRect`, if those are not kPrLayoutNodeNone.
    void cellBackgrounds(const LayoutFragment* cell, PrLayoutRect rect, LayoutNodeIndex group, PrLayoutRect groupRect,
            LayoutNodeIndex row, PrLayoutRect rowRect) {
        if (cell->node != kPrLayoutNodeNone) {
            PrLayoutStyleIndex style = layoutTreeNode(tree, cell->node).style;
            const PrLayoutColorStyle& color = layoutColorStyle(styles, style);
            if (color.visibility != PrLayoutVisibility_Visible ||
                    (color.emptyCells == PrLayoutEmptyCells_Hide && layoutTableCellIsEmpty(cell) &&
                        !layoutTableCellCollapses(tree, cell->node)))
                return;
        }
        for (uint32_t k = 0; k < columns.count; ++k) {
            const GridColumn& column = columns.data[k];
            PrLayoutRect area = column.node != kPrLayoutNodeNone ? column.rect : column.groupRect;
            if (!startsIn(area, rect))
                continue;
            trackBackground(column.group, column.groupRect, rect);
            trackBackground(column.node, column.rect, rect);
            break;
        }
        trackBackground(group, groupRect, rect);
        trackBackground(row, rowRect, rect);
    }

    // Returns true if the cell whose border box is `cell` starts in the column whose box is `area`: the boxes overlap,
    // or the column is collapsed and lies on the cell's start edge (css-tables-3/#visibility-collapse-track-rendering).
    static bool startsIn(PrLayoutRect area, PrLayoutRect cell) {
        if (area.right == area.left)
            return area.left >= cell.left && area.left < cell.right && area.top < cell.bottom && cell.top < area.bottom;
        PrLayoutRect meet = layoutRectIntersect(area, cell);
        return meet.right > meet.left && meet.bottom > meet.top;
    }

    // Records the background of the table track or track group `node`, if it is not kPrLayoutNodeNone and is visible.
    // The background is positioned in the box `area`, and painted over the whole of `cell`, which is its border box
    // (css-tables-3/#drawing-cell-backgrounds). A collapsed track has an empty box, and its background is positioned in
    // the cell (css-tables-3/#visibility-collapse-track-rendering).
    void trackBackground(LayoutNodeIndex node, PrLayoutRect area, PrLayoutRect cell) {
        if (node == kPrLayoutNodeNone)
            return;
        if (area.right == area.left || area.bottom == area.top)
            area = cell;
        PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
        if (layoutColorStyle(styles, style).visibility != PrLayoutVisibility_Visible)
            return;
        const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(styles, style);
        const LayoutBackground& layers = layoutBackgroundAt(&styles->backgrounds, decoration.backgroundLayers);
        if (prFloat16ToFloat(decoration.background.a) <= 0 && layers.count == 0)
            return;
        PrRect box = prLayoutRectToPx(layoutRectMoved(area, kLayoutVecZero - origin));
        PrRect clip = prLayoutRectToPx(layoutRectMoved(cell, kLayoutVecZero - origin));
        LayoutBackgroundBoxes boxes{.boxes = {box, box, box}, .area = clip};
        bool wasHeld = layers.blends && openBlending(boxes);
        LayoutBackgroundBoxes cellBoxes{.boxes = {clip, clip, clip}};
        backgroundColor(decoration.background, cellBoxes, PrLayoutBackgroundBox_BorderBox);
        if (layers.count != 0) {
            prDrawListPushClipRect(out(), clip, PrDrawEdges_Aliased);
            backgroundLayers(node, layers, boxes, false);
            prDrawListPopClip(out());
        }
        if (layers.blends)
            closeBlending(wasHeld);
    }

    // Records the collapsed borders of the grid of a table, whose fragment is `table` at `at`, after its cells'
    // backgrounds (css-tables-3/#drawing-collapsed-borders-2). A solid segment is a rectangle, and any other style is
    // drawn as one side of its rectangle.
    void gridBorders(const LayoutFragment* table, PrLayoutVec at) {
        LayoutTableGrid grid = layoutFragmentTableGrid(table);
        for (uint32_t i = 0; i < grid.borderCount; ++i) {
            const LayoutGridBorder& segment = grid.borders[i];
            PrRect rect = prLayoutRectToPx(layoutRectMoved(segment.rect, at - origin));
            if (prFloat16ToFloat(segment.color.a) <= 0)
                continue;
            if (segment.style == PrDrawBorderStyle_Solid) {
                prDrawListRect(out(), rect, {.color = segment.color});
            } else {
                PrDrawBorder border{};
                border.shape = prCornerRectRound(rect, 0);
                bool across = segment.side == PrRectSide_Top || segment.side == PrRectSide_Bottom;
                border.sides[segment.side] = PrDrawBorderSide{
                    .width = across ? rect.bottom - rect.top : rect.right - rect.left,
                    .style = segment.style,
                    .color = segment.color,
                };
                prDrawListBorder(out(), &border, {});
            }
            recorded();
        }
    }

    // Returns the drawing of a replaced box's image over its natural size, or over `box` in an axis that has none, at
    // the content's limit, with a reference the caller holds. The node caches the drawing for one size. A fresh
    // recording draws again and bypasses the cache.
    PrDrawList* imaged(const LayoutReplaced& n, PrLayoutRect box) {
        bool cached = subs != RecordSubs_Fresh;
        PrLayoutVec extent{
            n.natural.width.raw > 0 ? n.natural.width : layoutRectWidth(box),
            n.natural.height.raw > 0 ? n.natural.height : layoutRectHeight(box),
        };
        if (cached && n.content != nullptr && n.painted == extent) {
            prDrawListRetain(n.content);
            return n.content;
        }
        PrUVec2 texels = prDrawImageExtent(n.image);
        PrRect covered{0, 0, prLayoutUnitToPx(extent.x), prLayoutUnitToPx(extent.y)};
        PrDrawBrush brush = prDrawBrushImage(prDrawSourceImage(n.image, PrRect{0, 0, float(texels.x), float(texels.y)}), covered,
            n.rendering == PrLayoutImageRendering_Auto ? PrDrawSampling_Linear : PrDrawSampling_Nearest);
        brush.image.limit = n.limit;
        PrDrawList* drawing = prDrawListCreate();
        prDrawListRect(drawing, covered, {.brush = &brush});
        prDrawListFreeze(drawing);
        if (cached) {
            prDrawListRelease(n.content);
            prDrawListRetain(drawing);
            n.content = drawing;
            n.painted = extent;
        }
        return drawing;
    }

    // Returns what the painter of the replaced box `node` draws at `extent`, with a reference the caller holds. The
    // node caches the drawing for one extent. A fresh recording draws again and bypasses the cache. The roots that the
    // painter draws are noted as the node's (LayoutTree::rootReaders).
    PrDrawList* painted(LayoutNodeIndex node, const LayoutReplaced& n, PrLayoutVec extent) {
        bool cached = subs != RecordSubs_Fresh;
        if (cached && n.content != nullptr && n.painted == extent) {
            prDrawListRetain(n.content);
            return n.content;
        }
        PrDrawList* drawing = prDrawListCreate();
        RawArray<LayoutRootReader>& readers = tree->rootReaders;
        for (uint32_t i = readers.count; i-- > 0;) {
            if (readers.data[i].replaced == node)
                readers.erase(i);
        }
        tree->painting = node;
        n.painter.paint(n.painter.context, prLayoutVecToPx(extent), drawing);
        tree->painting = kPrLayoutNodeNone;
        prDrawListFreeze(drawing);
        if (cached) {
            prDrawListRelease(n.content);
            prDrawListRetain(drawing);
            n.content = drawing;
            n.painted = extent;
        }
        return drawing;
    }

    // Records a replaced box's content, clipped to its content box: a list or an image fitted to the box, or what its
    // painter draws at the size that fitting gives it. The clip's edges snap as the content box's do. An image paints
    // as an image brush over the rectangle it is fitted to, whose edges snap too, so that an image that fills the box
    // covers whole target pixels. A list or a painter's drawing is placed with its origin on a target pixel.
    void replaced(const LayoutFragment* fragment, PrLayoutVec at, bool visible) {
        const LayoutReplaced& n = layoutReplaced(tree, fragment->node);
        if (!visible || n.contentKind == PrLayoutContentKind_None)
            return;
        if (n.contentKind == PrLayoutContentKind_Embed) {
            slot(n.embed, fragment->node, layoutRectMoved(fragment->replaced.content, at));
            return;
        }
        PrLayoutStyleIndex style = layoutTreeNode(tree, fragment->node).style;
        PrLayoutRect box = layoutRectMoved(fragment->replaced.content, at - origin);
        PrLayoutVec fitted{};
        PrAffine2 placed = fitContent(styles, layoutDecorationStyle(styles, style), n.natural, box, &fitted);
        // The natural space, the box's size along an axis that has no natural size.
        PrLayoutVec space{
            n.natural.width.raw >= 0 ? n.natural.width : layoutRectWidth(box),
            n.natural.height.raw >= 0 ? n.natural.height : layoutRectHeight(box),
        };
        PrDrawList* into = out();
        uint64_t key = markKey(fragment->node);
        if (key != 0)
            prDrawListPushMark(into, key);
        // The content is clipped to the curves of the content box's corners (css-backgrounds-3/#corner-clipping).
        const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(styles, style);
        if (layoutHasCorners(decoration)) {
            PrRect border = prLayoutRectToPx(layoutRectAt(at - origin, fragment->extent));
            LayoutBackgroundBoxes boxes{.boxes = {border, border, prLayoutRectToPx(box)}};
            layoutBackgroundRound(boxes, layoutBorderShape(styles, decoration, border),
                1u << PrLayoutBackgroundBox_ContentBox);
            prDrawListPushClipCornerRect(into, boxes.shapes[PrLayoutBackgroundBox_ContentBox], PrDrawEdges_Snapped);
        } else {
            prDrawListPushClipRect(into, prLayoutRectToPx(box), PrDrawEdges_Snapped);
        }
        if (n.contentKind == PrLayoutContentKind_Image) {
            PrVec2 extent = prLayoutVecToPx(space);
            PrRect drawn{placed.e, placed.f, placed.e + extent.x * placed.a, placed.f + extent.y * placed.d};
            PrUVec2 texels = prDrawImageExtent(n.image);
            PrDrawBrush brush = prDrawBrushImage(
                prDrawSourceImage(n.image, PrRect{0, 0, float(texels.x), float(texels.y)}), drawn,
                n.rendering == PrLayoutImageRendering_Auto ? PrDrawSampling_Linear : PrDrawSampling_Nearest);
            brush.image.limit = n.limit;
            prDrawListRect(into, drawn, {.brush = &brush, .edges = PrDrawEdges_Snapped});
        } else {
            PrDrawList* content = nullptr;
            if (n.contentKind == PrLayoutContentKind_DrawList) {
                content = n.content;
                prDrawListRetain(content);
            } else {
                // A painter draws at the size that its natural space is fitted to, where a list would be scaled.
                content = painted(fragment->node, n, fitted);
                placed = prAffine2Translate(PrVec2{placed.e, placed.f});
            }
            // The content's extent, which fills the rectangle it is fitted to and snaps with it.
            PrVec2 extent;
            if (n.contentKind == PrLayoutContentKind_DrawList)
                extent = prLayoutVecToPx(space);
            else
                extent = prLayoutVecToPx(fitted);
            prDrawListPushTransformSnappedRect(into, prDAffine2FromAffine2(placed), extent);
            prDrawListEmbed(into, content, PrDVec2{0, 0});
            prDrawListPopTransform(into);
            prDrawListRelease(content);
        }
        prDrawListPopClip(into);
        if (key != 0)
            prDrawListPopMark(into);
        recorded();
    }

    // Returns the key that marks the content of `node`: its key if the tree marks content, and zero otherwise.
    uint64_t markKey(LayoutNodeIndex node) const {
        return tree->marking == PrLayoutMarking_Content ? layoutTreeNode(tree, node).key : 0;
    }

    // Finds the bytes of the node's text that each glyph of the text fragment was shaped from. A glyph that starts a
    // cluster lies where the cluster's caret stop does along the line, and the glyphs after it up to the next stop
    // belong to the same cluster. A cluster runs to the next cluster in the order of the text.
    void glyphText(const LayoutFragment* fragment) {
        const LayoutFragmentText& text = fragment->text;
        const LayoutCaretStop* stops = layoutFragmentTextStops(fragment);
        bool vertical = layoutStyleTraits(styles, layoutFragmentStyle(tree, fragment)).vertical;
        bool upright = vertical && fragment->turn == LayoutTextTurn_None;
        glyphStarts.resize(text.glyphCount);
        glyphEnds.resize(text.glyphCount);
        uint32_t stop = 0;
        for (uint32_t i = 0; i < text.glyphCount; ++i) {
            float along = upright ? text.glyphs[i].position.y : text.glyphs[i].position.x;
            while (stop + 1 < text.stopCount && stops[stop + 1].along <= along + 1e-3f)
                ++stop;
            glyphStarts.data[i] = stops[stop].byte;
        }
        // A glyph's characters end at the first stop in the text's order after their start, or at the end of the text.
        stopBytes.clear();
        for (uint32_t k = 0; k < text.stopCount; ++k)
            stopBytes.push(stops[k].byte);
        sortItems(stopBytes.span(), [](uint32_t a, uint32_t b) { return a < b; });
        for (uint32_t i = 0; i < text.glyphCount; ++i) {
            uint32_t lo = 0;
            uint32_t hi = stopBytes.count;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2;
                if (stopBytes.data[mid] <= glyphStarts.data[i])
                    lo = mid + 1;
                else
                    hi = mid;
            }
            glyphEnds.data[i] = lo < stopBytes.count && stopBytes.data[lo] < text.end ? stopBytes.data[lo] : text.end;
        }
    }

    // Gives `run`, the glyphs from `first` of the fragment, its text and clusters, from the bytes that glyphText found.
    // White space that breaks lines reads as a space. White space at the end of the run that no glyph draws, as a space
    // that hangs or is removed at the end of a line, is left out, so that each glyph's characters are the ones that it
    // draws.
    void runTextOf(const LayoutFragment* fragment, uint32_t first, PrFontGlyphRun* run) {
        uint32_t begin = UINT32_MAX;
        uint32_t end = 0;
        uint32_t last = 0;
        for (uint32_t i = first; i < first + run->glyphCount; ++i) {
            begin = glyphStarts.data[i] < begin ? glyphStarts.data[i] : begin;
            end = glyphEnds.data[i] > end ? glyphEnds.data[i] : end;
            last = glyphStarts.data[i] > last ? glyphStarts.data[i] : last;
        }
        PrStringView source = layoutNodeText(tree, fragment->node);
        if (begin >= end || end > source.size)
            return;
        auto white = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
        while (end > last + 1 && white(source.data[end - 1]))
            --end;
        runText.clear();
        for (uint32_t b = begin; b < end; ++b) {
            char c = source.data[b];
            runText.push(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
        }
        runClusters.resize(run->glyphCount);
        for (uint32_t i = 0; i < run->glyphCount; ++i)
            runClusters.data[i] = glyphStarts.data[first + i] - begin;
        run->text = PrStringView{runText.data, runText.count};
        run->clusters = runClusters.data;
    }

    // Records a text fragment's glyphs, one command for each of its font ranges. A fragment normally stays whole in its
    // batch, and the batch ends after the fragment if it holds enough commands. The exception is a fragment whose own
    // commands fill a batch, as a long line with many fonts may: its batch ends among its ranges. The ranges are in
    // placement order, so each batch holds a part of the line that playback can skip if it is outside the clip. A turn
    // is closed at the end of a batch and opened again in the next.
    void text(const LayoutFragment* fragment, PrLayoutVec at, bool visible) {
        ++visits;
        repainted(fragment->node);
        const LayoutFragmentText& text = fragment->text;
        if (!visible)
            return;
        const LayoutHighlights* highlights = layoutTextHighlights(tree, fragment->node);
        if (highlights != nullptr) {
            marked(fragment, at, highlights);
            caretSlots(fragment, at);
            return;
        }
        if (text.glyphCount == 0) {
            caretSlots(fragment, at);
            return;
        }
        PrVec2 offset = prLayoutVecToPx(at - origin);
        PrColor16 color = layoutColorStyle(styles, layoutFragmentStyle(tree, fragment)).color;
        // The glyphs are placed from the fragment's corner, in the frame of a horizontal line that a turn rotates by a
        // quarter turn about that corner, or by the run's origin if they are not turned. A pushed translation carries
        // the fragment's position in the block axis and snaps it to a target pixel, which puts the baseline there too,
        // since metrics are whole target pixels. Along the line, glyphs stay where layout placed them, by the run's
        // origin.
        bool turned = fragment->turn != LayoutTextTurn_None;
        bool vertical = layoutStyleTraits(styles, layoutFragmentStyle(tree, fragment)).vertical;
        PrAffine2 placed = vertical ? prAffine2Translate(PrVec2{offset.x, 0}) : prAffine2Translate(PrVec2{0, offset.y});
        PrVec2 pen = vertical ? PrVec2{0, offset.y} : PrVec2{offset.x, 0};
        PrDrawSnapFlags blockAxis = vertical ? PrDrawSnapFlag_X : PrDrawSnapFlag_Y;
        if (turned) {
            PrVec2 extent = prLayoutVecToPx(fragment->extent);
            if (fragment->turn == LayoutTextTurn_Clockwise)
                placed = PrAffine2{0, 1, -1, 0, offset.x + extent.x, offset.y};
            else
                placed = PrAffine2{0, -1, 1, 0, offset.x, offset.y + extent.y};
            pen = PrVec2{};
            blockAxis = PrDrawSnapFlag_X;
        }
        const PrFont* const* fonts = layoutFragmentTextFonts(fragment);
        const LayoutFragmentFontRange* ranges = layoutFragmentTextRanges(fragment);
        uint64_t key = markKey(fragment->node);
        if (key != 0)
            glyphText(fragment);
        PrDrawList* into = nullptr;     // the batch being recorded into, with the frame pushed
        uint32_t own = 0;               // the commands of the fragment in that batch
        uint32_t start = 0;
        for (uint32_t i = 0; i < text.rangeCount; ++i) {
            if (into == nullptr) {
                into = out();
                own = 0;
                prDrawListPushTransformSnapped(into, prDAffine2FromAffine2(placed), blockAxis);
                if (key != 0)
                    prDrawListPushMark(into, key);
            }
            PrFontGlyphRun run{};
            run.font = fonts[ranges[i].font];
            run.size = text.size * ranges[i].scale;
            run.glyphs = text.glyphs + start;
            run.glyphCount = ranges[i].glyphEnd - start;
            if (key != 0)
                runTextOf(fragment, start, &run);
            prDrawListGlyphs(into, &run, pen, {.color = color});
            start = ranges[i].glyphEnd;
            ++commands;
            ++stats->commandsRecorded;
            ++batched;
            ++own;
            if (batched >= kPaintBatch && (i + 1 == text.rangeCount || own >= kPaintBatch)) {
                if (key != 0)
                    prDrawListPopMark(into);
                prDrawListPopTransform(into);
                flush();
                into = nullptr;
            }
        }
        if (into != nullptr) {
            if (key != 0)
                prDrawListPopMark(into);
            prDrawListPopTransform(into);
        }
        ++stats->textFragmentsRecorded;
        stats->glyphCommandsRecorded += text.rangeCount;
        caretSlots(fragment, at);
    }

    // Records a text fragment that has highlights, in one batch: the backgrounds of its highlights, then its glyphs,
    // clipped to each stretch along its line that one color paints (css-pseudo-4/#highlight-painting).
    void marked(const LayoutFragment* fragment, PrLayoutVec at, const LayoutHighlights* highlights) {
        const LayoutFragmentText& text = fragment->text;
        PrLayoutVec offset = at - origin;
        PrColor16 base = layoutColorStyle(styles, layoutFragmentStyle(tree, fragment)).color;
        float length = layoutTextLength(tree, fragment);
        uint32_t start = layoutTextStart(fragment);
        PrDrawList* into = out();
        uint32_t before = commands;
        auto record = [&]() {
            ++commands;
            ++stats->commandsRecorded;
            ++batched;
        };
        RawArray<HighlightStretch>& stretches = highlightStretches;
        stretches.clear();
        // Only the ranges that reach the fragment matter, which keeps the cost of a long text's many lines in
        // proportion to its ranges.
        layoutHighlightsReaching(highlights, start, text.end, highlightsReaching);
        for (uint32_t r = 0; r < highlightsReaching.count; ++r) {
            uint32_t i = highlightsReaching.data[r];
            const PrLayoutHighlight& h = highlights->ranges.data[i];
            uint32_t from = h.start > start ? h.start : start;
            uint32_t to = h.end < text.end ? h.end : text.end;
            if (from >= to)
                continue;
            float a = layoutTextCaretAlong(tree, fragment, from);
            float b = layoutTextCaretAlong(tree, fragment, to);
            stretches.push(HighlightStretch{a < b ? a : b, a < b ? b : a, i});
            if (h.background.a > 0) {
                PrRect rect = prLayoutRectToPx(layoutTextSpanRect(tree, fragment, offset, a, b));
                prDrawListRect(into, rect, {.color = h.background});
                record();
            }
        }
        // The cuts along the line where a highlight starts or ends divide it into stretches of one color each. A
        // stretch is painted in the color of the last highlight that covers it, or in the text's own.
        RawArray<float>& cuts = highlightCuts;
        cuts.clear();
        cuts.push(-kPrF32Infinity);
        for (uint32_t i = 0; i < stretches.count; ++i) {
            if (stretches.data[i].lo < stretches.data[i].hi && highlights->ranges.data[stretches.data[i].range].color.a > 0) {
                cuts.push(stretches.data[i].lo);
                cuts.push(stretches.data[i].hi);
            }
        }
        cuts.push(kPrF32Infinity);
        sortItems(cuts.span(), [](float a, float b) { return a < b; });
        for (uint32_t c = 0; text.glyphCount != 0 && c + 1 < cuts.count; ++c) {
            float lo = cuts.data[c];
            float hi = cuts.data[c + 1];
            if (lo >= hi)
                continue;
            float middle = lo == -kPrF32Infinity ? hi - 1 : hi == kPrF32Infinity ? lo + 1 : (lo + hi) / 2;
            PrColor16 color = base;
            for (uint32_t i = 0; i < stretches.count; ++i) {
                const HighlightStretch& stretch = stretches.data[i];
                const PrLayoutHighlight& h = highlights->ranges.data[stretch.range];
                if (h.color.a > 0 && stretch.lo <= middle && middle < stretch.hi)
                    color = h.color;
            }
            // Glyphs reach past the fragment's content area, so the clip reaches an em past it across the line.
            bool whole = cuts.count == 2;
            if (!whole) {
                PrRect clip = prLayoutRectToPx(layoutTextSpanRect(tree, fragment, offset,
                    lo == -kPrF32Infinity ? -text.size : lo, hi == kPrF32Infinity ? length + text.size : hi));
                bool vertical = layoutStyleTraits(styles, layoutFragmentStyle(tree, fragment)).vertical;
                if (vertical) {
                    clip.left -= text.size;
                    clip.right += text.size;
                } else {
                    clip.top -= text.size;
                    clip.bottom += text.size;
                }
                prDrawListPushClipRect(into, clip, PrDrawEdges_Aliased);
                record();
            }
            glyphs(into, fragment, offset, color);
            if (!whole) {
                prDrawListPopClip(into);
                record();
            }
        }
        // The lines under the text paint in front of its glyphs, in the order of their highlights. A line sits on the
        // block-end side of the content area, and its wave is in phase along the whole line, so that the lines of
        // neighboring stretches meet.
        bool vertical = layoutStyleTraits(styles, layoutFragmentStyle(tree, fragment)).vertical;
        float thickness = text.size / 14 > 1 ? text.size / 14 : 1;
        for (uint32_t i = 0; i < stretches.count; ++i) {
            const HighlightStretch& stretch = stretches.data[i];
            const PrLayoutHighlight& h = highlights->ranges.data[stretch.range];
            if (h.underline.a <= 0 || stretch.lo >= stretch.hi)
                continue;
            PrRect span = prLayoutRectToPx(layoutTextSpanRect(tree, fragment, offset, stretch.lo, stretch.hi));
            PrDrawDecoration line{
                .length = stretch.hi - stretch.lo,
                .thickness = thickness,
                .style = h.underlineStyle,
                .wavelength = thickness * 4,
                .amplitude = thickness,
                .phase = stretch.lo,
            };
            float inset = thickness / 2 + (h.underlineStyle == PrDrawDecorationStyle_Wavy ? thickness : 0);
            if (vertical) {
                prDrawListPushTransformAffine2(into, PrAffine2{0, 1, -1, 0, span.left + inset, span.top});
                prDrawListDecoration(into, &line, {.color = h.underline});
                prDrawListPopTransform(into);
                record();
                record();
            } else {
                line.origin = PrVec2{span.left, span.bottom - inset};
                prDrawListDecoration(into, &line, {.color = h.underline});
            }
            record();
        }
        ++stats->textFragmentsRecorded;
        if (commands != before && batched >= kPaintBatch)
            flush();
    }

    // Records the glyphs of a text fragment at `offset` from the origin in `color`, one command for each of its font
    // ranges, as text does.
    void glyphs(PrDrawList* into, const LayoutFragment* fragment, PrLayoutVec offset, PrColor16 color) {
        const LayoutFragmentText& text = fragment->text;
        PrVec2 at = prLayoutVecToPx(offset);
        bool turned = fragment->turn != LayoutTextTurn_None;
        bool vertical = layoutStyleTraits(styles, layoutFragmentStyle(tree, fragment)).vertical;
        PrAffine2 placed = vertical ? prAffine2Translate(PrVec2{at.x, 0}) : prAffine2Translate(PrVec2{0, at.y});
        PrVec2 pen = vertical ? PrVec2{0, at.y} : PrVec2{at.x, 0};
        PrDrawSnapFlags blockAxis = vertical ? PrDrawSnapFlag_X : PrDrawSnapFlag_Y;
        if (turned) {
            PrVec2 extent = prLayoutVecToPx(fragment->extent);
            if (fragment->turn == LayoutTextTurn_Clockwise)
                placed = PrAffine2{0, 1, -1, 0, at.x + extent.x, at.y};
            else
                placed = PrAffine2{0, -1, 1, 0, at.x, at.y + extent.y};
            pen = PrVec2{};
            blockAxis = PrDrawSnapFlag_X;
        }
        prDrawListPushTransformSnapped(into, prDAffine2FromAffine2(placed), blockAxis);
        const PrFont* const* fonts = layoutFragmentTextFonts(fragment);
        const LayoutFragmentFontRange* ranges = layoutFragmentTextRanges(fragment);
        uint64_t key = markKey(fragment->node);
        if (key != 0) {
            glyphText(fragment);
            prDrawListPushMark(into, key);
        }
        uint32_t start = 0;
        for (uint32_t i = 0; i < text.rangeCount; ++i) {
            PrFontGlyphRun run{};
            run.font = fonts[ranges[i].font];
            run.size = text.size * ranges[i].scale;
            run.glyphs = text.glyphs + start;
            run.glyphCount = ranges[i].glyphEnd - start;
            if (key != 0)
                runTextOf(fragment, start, &run);
            prDrawListGlyphs(into, &run, pen, {.color = color});
            start = ranges[i].glyphEnd;
        }
        if (key != 0)
            prDrawListPopMark(into);
        prDrawListPopTransform(into);
        commands += text.rangeCount + 2;
        stats->commandsRecorded += text.rangeCount + 2;
        batched += text.rangeCount + 2;
        stats->glyphCommandsRecorded += text.rangeCount;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Chunks

// Frees what a chunk holds except its node, keeping its storage.
void resetChunk(LayoutPaintChunk& chunk) {
    layoutFragmentRelease(chunk.fragment);
    chunk.fragment = nullptr;
    clearRecording(chunk);
    for (uint32_t i = 0; i < chunk.outer.count; ++i)
        layoutFragmentRelease(chunk.outer.data[i].fragment);
    chunk.outer.clear();
}

// Frees what a chunk holds and its storage.
void destroyChunk(LayoutPaintChunk& chunk) {
    resetChunk(chunk);
    resetRecording(chunk);
    releaseOuter(chunk.outer);
}

// Returns true if the lists that a chunk painted from are the ones it found, and the boxes it placed from outside its
// box are where they were (LayoutPaintChunk::outer): the same fragments, at the same positions relative to the chunk's
// box, in the same containers.
bool outerCurrent(const LayoutTree* tree, const LayoutPaintChunk& chunk) {
    for (uint32_t i = 0; i < chunk.lists.count; ++i) {
        if (!layoutStackingListIs(tree, chunk.lists.data[i].context, chunk.lists.data[i].version))
            return false;
    }
    if (chunk.outer.count == 0)
        return true;
    PrLayoutVec reference = layoutNodePosition(tree, chunk.fragment->node);
    for (uint32_t i = 0; i < chunk.outer.count; ++i) {
        const LayoutPaintPlaced& placed = chunk.outer.data[i];
        LayoutNodeIndex node = placed.node;
        if (node >= tree->nodes.count || !tree->nodes.data[node].live)
            return false;
        bool inlineBox = layoutIsInlineBox(tree, node);
        LayoutNodeIndex holder = inlineBox ? layoutCachePlacement(tree->cache, node).parent : node;
        if (holder == kPrLayoutNodeNone || layoutCachePlaced(tree->cache, holder) != placed.fragment ||
                layoutNodePosition(tree, node) - reference != placed.offset ||
                layoutNodeContainer(tree, node) != placed.container)
            return false;
    }
    return true;
}

// Returns true if a chunk's recording is what recording the box `fragment` would give: the fragment is the one it
// recorded, the boxes it placed from outside it are where they were, and nothing it read from its nodes has changed
// since. A change to the box's own effects does not count, because effects apply around the recording.
bool isCurrent(const LayoutTree* tree, const LayoutPaintChunk& chunk, const LayoutFragment* fragment,
        LayoutPieceAt opening, bool root) {
    if (chunk.fragment != fragment || chunk.opening.child != opening.child || chunk.opening.line != opening.line ||
            chunk.root != root || !outerCurrent(tree, chunk))
        return false;
    // An inline box's recording reads only its pieces, which are fragments of its descendants. The descendants mark the
    // box when they change.
    bool changed;
    if (chunk.node != fragment->node)
        changed = layoutTreeNode(tree, chunk.node).paintSubtree >= chunk.recorded;
    else
        changed = changedSince(tree, fragment, chunk.recorded);
    if (changed)
        return false;
    // A box named as a layer must still have a layer of its own. An inline box is checked by its node, separately from
    // its container's fragment.
    for (uint32_t i = 0; i < chunk.layers.count; ++i) {
        const LayoutPaintLayer& layer = chunk.layers.data[i];
        PrLayoutStyleIndex style = layoutTreeNode(tree, layer.node).style;
        bool owns;
        if (layer.node != layer.fragment->node)
            owns = layoutInlineOwnsLayer(tree->styles, style);
        else
            owns = layoutOwnsLayer(tree->styles, style) || layoutIsolatesBlending(tree, layer.node);
        if (!owns)
            return false;
    }
    return true;
}

// Records the body of the box `fragment` into the chunk, in the box's own space, in the containers it paints in.
void record(LayoutTree* tree, LayoutPaintChunk& chunk, const LayoutFragment* fragment, LayoutPieceAt opening, bool root,
        RecordSubs subs) {
    bool reused = subs == RecordSubs_Cached && chunk.fragment == fragment && (tree->paintedFor & LayoutInputFlag_Layout) == 0;
    uint32_t previous = reused ? chunk.recorded : 0;
    resetChunk(chunk);
    layoutFragmentRetain(fragment);
    chunk.fragment = fragment;
    chunk.opening = opening;
    chunk.root = root;
    chunk.recorded = tree->paint.paints;
    Painter painter{
        .tree = tree,
        .styles = tree->styles,
        .chunk = &chunk,
        .previous = previous,
        .cache = &tree->paint,
        .subs = subs,
        .stats = &tree->stats,
        .current = layoutPaintOwn(kPaintScrollNone),
    };
    LayoutWalk<Painter> w{.tree = tree, .styles = tree->styles, .visitor = painter};
    RawArray<LayoutNodeIndex> around;
    layoutContainerChain(tree, layoutNodeContainer(tree, chunk.node), around);
    painter.startIn(around);
    around.reset();
    w.layer(chunk.node, fragment, opening, root);
    w.release();
    painter.flush();
    painter.subtrees.reset();
    painter.stack.reset();
    painter.opened.reset();
    painter.openedIndex.reset();
    painter.columns.reset();
    painter.mapped.reset();
    painter.highlightStretches.reset();
    painter.highlightsReaching.reset();
    painter.caretFragments.reset();
    painter.highlightCuts.reset();
    painter.glyphStarts.reset();
    painter.glyphEnds.reset();
    painter.runClusters.reset();
    painter.runText.reset();
    painter.stopBytes.reset();
    // Scenes embed the recording on whatever thread plays them.
    for (uint32_t i = 0; i < chunk.segments.count; ++i)
        prDrawListFreeze(chunk.segments.data[i].list);
}

// Returns true if two recordings of a box are the same: the commands of their segments, the containers they are in, the
// boxes they name, and their wheel regions.
bool sameRecording(const LayoutPaintChunk& a, const LayoutPaintChunk& b) {
    if (a.segments.count != b.segments.count || a.scrolls.count != b.scrolls.count ||
            a.layers.count != b.layers.count || a.slots.count != b.slots.count)
        return false;
    for (uint32_t i = 0; i < a.layers.count; ++i) {
        const LayoutPaintLayer& x = a.layers.data[i];
        const LayoutPaintLayer& y = b.layers.data[i];
        if (x.fragment != y.fragment || x.node != y.node || x.offset.x != y.offset.x || x.offset.y != y.offset.y ||
                x.scroll != y.scroll || x.segment != y.segment || x.order != y.order)
            return false;
    }
    if (a.outer.count != b.outer.count || a.fixed.count != b.fixed.count || a.regions.count != b.regions.count)
        return false;
    for (uint32_t i = 0; i < a.regions.count; ++i) {
        if (a.regions.data[i].rect != b.regions.data[i].rect || a.regions.data[i].scroll != b.regions.data[i].scroll)
            return false;
    }
    for (uint32_t i = 0; i < a.fixed.count; ++i) {
        if (a.fixed.data[i].segment != b.fixed.data[i].segment || a.fixed.data[i].clip != b.fixed.data[i].clip)
            return false;
    }
    for (uint32_t i = 0; i < a.outer.count; ++i) {
        const LayoutPaintPlaced& x = a.outer.data[i];
        const LayoutPaintPlaced& y = b.outer.data[i];
        if (x.node != y.node || x.fragment != y.fragment || x.offset != y.offset || x.container != y.container)
            return false;
    }
    for (uint32_t i = 0; i < a.slots.count; ++i) {
        const LayoutPaintSlot& x = a.slots.data[i];
        const LayoutPaintSlot& y = b.slots.data[i];
        if (x.embed != y.embed || x.node != y.node || prMemCompare(&x.box, &y.box, sizeof(PrLayoutRect)) != 0 ||
                x.scroll != y.scroll || x.segment != y.segment || x.order != y.order || x.caret != y.caret ||
                !prMemEquals(&x.color, &y.color, sizeof(PrColor16)))
            return false;
    }
    for (uint32_t i = 0; i < a.scrolls.count; ++i) {
        const LayoutPaintScroll& x = a.scrolls.data[i];
        const LayoutPaintScroll& y = b.scrolls.data[i];
        if (x.node != y.node || x.parent != y.parent || prMemCompare(&x.clip, &y.clip, sizeof(PrLayoutRect)) != 0 ||
                x.range != y.range || x.userAxes != y.userAxes || x.chainAxes != y.chainAxes || x.kind != y.kind)
            return false;
    }
    bool same = true;
    for (uint32_t i = 0; i < a.segments.count && same; ++i) {
        PrRawString x = prDrawListDump(a.segments.data[i].list);
        PrRawString y = prDrawListDump(b.segments.data[i].list);
        same = a.segments.data[i].scroll == b.segments.data[i].scroll &&
            a.segments.data[i].offset == b.segments.data[i].offset && prRawStringView(&x) == prRawStringView(&y);
        prRawStringReset(&x);
        prRawStringReset(&y);
    }
    return same;
}

} // namespace

void layoutPaintCacheReset(LayoutPaintCache& cache) {
    for (uint32_t i = 0; i < cache.chunks.count; ++i)
        destroyChunk(cache.chunks.data[i]);
    cache.chunks.reset();
    cache.byNode.reset();
    for (uint32_t i = 0; i < cache.subs.count; ++i)
        subRelease(cache, cache.subs.data[i]);
    cache.subs.reset();
    cache.subIndex.reset();
    for (uint32_t i = 0; i < cache.runs.count; ++i)
        runRelease(cache, cache.runs.data[i]);
    cache.runs.reset();
    cache.runIndex.reset();
    for (uint32_t i = 0; i < cache.spare.count; ++i)
        resetRecording(cache.spare.data[i]);
    cache.spare.reset();
}

LayoutBackgroundBoxes layoutBackgroundBoxes(const LayoutStyles* styles, const LayoutFragment* fragment,
        const PrLayoutBoxStyle& box, PrRect border) {
    LayoutBackgroundBoxes boxes{};
    PrLayoutUnit base = layoutUnitMax(fragment->box.containingInline, kLayoutUnitZero);
    PrRect padding = border;
    // A side at which the fragment breaks has no border or padding.
    float inset[2][PrRectSide_Count];
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        bool broken = (fragment->broken & (1u << side)) != 0;
        inset[0][side] = broken ? 0 : prLayoutUnitToPx(box.border[side]);
        inset[1][side] = broken ? 0 : prLayoutUnitToPx(layoutResolve(styles, box.padding[side], base));
    }
    padding.left += inset[0][PrRectSide_Left];
    padding.top += inset[0][PrRectSide_Top];
    padding.right -= inset[0][PrRectSide_Right];
    padding.bottom -= inset[0][PrRectSide_Bottom];
    PrRect content = padding;
    content.left += inset[1][PrRectSide_Left];
    content.top += inset[1][PrRectSide_Top];
    content.right -= inset[1][PrRectSide_Right];
    content.bottom -= inset[1][PrRectSide_Bottom];
    boxes.boxes[PrLayoutBackgroundBox_BorderBox] = border;
    boxes.boxes[PrLayoutBackgroundBox_PaddingBox] = padding;
    boxes.boxes[PrLayoutBackgroundBox_ContentBox] = content;
    return boxes;
}

void layoutPaintBegin(LayoutTree* tree) {
    layoutPaintReused(tree, 0);
    ++tree->paint.paints;
}

void layoutPaintReused(LayoutTree* tree, uint32_t layers) {
    tree->stacking.listsFound = 0;
    tree->stats.layersRecorded = 0;
    tree->stats.layersReused = layers;
    tree->stats.subtreesRecorded = 0;
    tree->stats.subtreesReused = 0;
    tree->stats.commandsRecorded = 0;
    tree->stats.textFragmentsRecorded = 0;
    tree->stats.glyphCommandsRecorded = 0;
    tree->stats.boxesRepaintedUnchanged = 0;
}

uint32_t layoutPaintChunk(LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment, LayoutPieceAt opening,
        bool root) {
    LayoutPaintCache& cache = tree->paint;
    while (cache.byNode.count <= node)
        cache.byNode.push(kPaintChunkNone);
    uint32_t at = cache.byNode.data[node];
    if (at == kPaintChunkNone) {
        at = cache.chunks.count;
        cache.chunks.emplace() = LayoutPaintChunk{};
        cache.chunks.data[at].node = node;
        cache.byNode.data[node] = at;
    }
    LayoutPaintChunk& chunk = cache.chunks.data[at];
    chunk.painted = cache.paints;
    if (isCurrent(tree, chunk, fragment, opening, root)) {
        ++tree->stats.layersReused;
    } else {
        record(tree, chunk, fragment, opening, root, RecordSubs_Cached);
        ++tree->stats.layersRecorded;
    }
    return at;
}

void layoutPaintSweep(LayoutTree* tree) {
    LayoutPaintCache& cache = tree->paint;
    uint32_t i = 0;
    while (i < cache.chunks.count) {
        LayoutPaintChunk& chunk = cache.chunks.data[i];
        if (chunk.painted == cache.paints) {
            ++i;
            continue;
        }
        destroyChunk(chunk);
        cache.byNode.data[chunk.node] = kPaintChunkNone;
        uint32_t last = cache.chunks.count - 1;
        if (i != last) {
            chunk = cache.chunks.data[last];
            cache.byNode.data[chunk.node] = i;
        }
        cache.chunks.pop();
    }
}

void layoutPaintRetire(LayoutTree* tree) {
    for (uint32_t i = 0; i < tree->cache.retired.count; ++i)
        subDrop(tree->paint, tree->cache.retired.data[i]);
}

void layoutPaintForget(LayoutTree* tree, LayoutNodeIndex node) {
    LayoutPaintCache& cache = tree->paint;
    for (uint32_t phase = 0; phase < LayoutPhase_Count; ++phase) {
        for (uint32_t index = 0;; ++index) {
            uint32_t at = runFind(cache, node, LayoutPhase(phase), index);
            if (at == kHashMissing)
                break;
            runDrop(cache, at);
        }
    }
}

uint32_t layoutPaintVerify(LayoutTree* tree) {
    // Recording again for the comparison does not count as the previous paint's work.
    PrLayoutTreeStats stats = tree->stats;
    uint32_t differs = kPrLayoutNodeNone;
    LayoutPaintCache& cache = tree->paint;
    for (uint32_t i = 0; i < cache.chunks.count && differs == kPrLayoutNodeNone; ++i) {
        const LayoutPaintChunk& chunk = cache.chunks.data[i];
        // A chunk of a box laid out since, or of a node erased, is recorded again before it is used.
        LayoutNodeIndex node = chunk.node;
        LayoutNodeIndex laid = chunk.fragment->node;
        bool laidOut = node < tree->nodes.count && tree->nodes.data[node].live && laid < tree->nodes.count &&
            tree->nodes.data[laid].live && layoutCachePlaced(tree->cache, laid) == chunk.fragment;
        if (!laidOut || !isCurrent(tree, chunk, chunk.fragment, chunk.opening, chunk.root))
            continue;
        LayoutPaintChunk fresh{};
        fresh.node = node;
        record(tree, fresh, chunk.fragment, chunk.opening, chunk.root, RecordSubs_Fresh);
        if (!sameRecording(chunk, fresh))
            differs = node;
        destroyChunk(fresh);
    }
    tree->stats = stats;
    return differs;
}

} // namespace pr_impl
