// Scenes: a paint's recordings assembled into layers, each with its effects and the scroll containers it is in, and
// played into draw lists.
//
// A scene refers to nothing in its tree and is immutable, so it can be played on another thread while the tree changes.
// Playing a scene embeds each layer's recording at the point where its parent's recording names it. A player may give
// effects that replace the scene's own, and gives the scroll offsets, because the scene holds none.
//
// A scene's containers form one tree across its layers. Each segment, layer and slot names the container that holds it.
// That container may be in a recording around its layer's recording, if a box out of flow escapes the containers
// between its holder and its placeholder. A layer's effects form one group around everything it holds. Each piece is
// placed in the group by its own chain of containers, from the scene's space or from the nearest layer with a
// transform, which holds every box out of flow in it.

#pragma once

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/layout_scene.h"

#include "layout/src/layout_effects.h"
#include "layout/src/layout_paint.h"
#include "layout/src/layout_walk.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Scenes

// A layer of a scene, by its place among the scene's layers.
typedef uint32_t SceneLayerIndex;

// A layer of a scene: a box with a layer of its own, as its tree's last paint assembled it. A layer's subtree is the
// layers from it to `end`, with its children first among them. Each layer plays after the first `segmentsBefore`
// segments of its parent's recording.
struct LayoutSceneLayer {
    PrLayoutNode node;
    SceneLayerIndex end;            // past the last layer of its subtree
    SceneLayerIndex parent;         // the layer it is in, or kPaintScrollNone for the root's
    uint32_t segmentsBefore;
    uint32_t order;                 // its order among the layers and slots that its parent's recording names
    uint32_t placement;             // the container it is in, or kPaintScrollNone for none, past the viewport
    PrLayoutVec offset;             // its border box's top left, in its parent's space
    PrLayoutVec extent;             // the extent of its border box
    uint32_t transform;             // its entry in the scene's transforms, or kPaintScrollNone for the identity
    uint32_t effects;               // its entry in the scene's effects, or kPaintScrollNone for none
    float opacity;

    // Something in the layer blends with what lies beneath, so the layer's group is isolated
    // (compositing-1/#isolation).
    bool isolates;

    uint32_t segmentStart;          // the run of the scene's segments that are its recording
    uint32_t segmentCount;
    uint32_t scrollStart;           // the run of the scene's scroll containers that its recording holds
    uint32_t scrollCount;
    uint32_t slotStart;             // the run of the scene's slots that its recording names
    uint32_t slotCount;
    uint32_t regionStart;           // the run of the scene's wheel region that its recording holds
    uint32_t regionCount;
};

// The transform of a layer whose box has a transform, or whose effects a compositor may animate, resolved for its
// reference box and with its parent's perspective. A compositor's matrix replaces the resolved matrix, and keeps the
// origin and the perspective.
struct LayoutSceneTransform {
    LayoutResolvedTransform resolved;
    uint32_t perspectiveScroll;     // the container whose offset moves the perspective origin, or kPaintScrollNone
    bool transformed;               // the matrix is not the identity
};

// A slot of embedded content in a layer's recording: its id and node, its content box in its layer's space, the
// container that holds it, and the number of its layer's segments that play before it.
struct LayoutSceneSlot {
    uint64_t embed;
    PrLayoutNode node;
    SceneLayerIndex layer;
    uint32_t scroll;                // kPaintScrollNone for none, past the viewport
    uint32_t segmentsBefore;
    uint32_t order;                 // its order among the layers and slots that its layer's recording names
    PrLayoutRect box;
};

// A part of a scene's wheel region (LayoutPaintRegion): a rectangle in its layer's space, and the container that holds
// it.
struct LayoutSceneRegion {
    PrLayoutRect rect;
    uint32_t scroll;                // kPaintScrollNone for none, past the viewport
};

// A container of a scene: a scroll container, or a box that only clips. Its clip is in the space of the layer whose
// recording holds it, or in the scene's space for the viewport. A container's content is that space moved by the scroll
// offset that a player gives the container.
struct LayoutSceneContainer {
    PrLayoutNode node;              // kPrLayoutNodeNone for the viewport
    uint32_t parent;                // the container it is in, in its layer or around it, or kPaintScrollNone
    SceneLayerIndex layer;          // the layer whose recording holds it, or kPaintScrollNone for the viewport
    uint32_t segment;               // the first of its layer's segments that holds its content
    PrLayoutRect clip;
    PrLayoutRect range;
    PrLayoutVec portExtent;
    PrLayoutRect content;
    PrLayoutScrollFlags userAxes;
    PrLayoutScrollFlags chainAxes;
    bool scrolls;                   // false for a box that only clips, which no query names

    // If `curved`, the corners of the clip, by PrRectCorner, whose curves it clips to. Otherwise its corners are
    // square.
    bool curved;
    PrCorner corners[PrRectCorner_Count];
};

// A run of a layer's recording, in its layer's space, and the container it is in.
struct LayoutSceneSegment {
    PrDrawList* list;               // frozen; the scene holds a reference to it
    uint32_t scroll;                // kPaintScrollNone for none, past the viewport
    PrLayoutVec offset;             // position of the list's space in its layer's space
    uint32_t fixed;                 // index plus one of its clip in fixedClips if it is fixed to the viewport, or 0
};

// A run of the root's background layers over the canvas. A run either holds layers that scroll with the document, in
// the viewport's content space, or holds one layer fixed to the viewport, in the viewport's space.
struct LayoutSceneCanvasRun {
    PrDrawList* list;               // frozen; the scene holds a reference to it
    bool fixed;
};

// A scene. The viewport's scroll is its first scroll container, which holds the root's layer.
struct LayoutScene {
    mutable Atomic<uint32_t> references; // changes behind const, as an immutable scene's holders come and go
    Array<LayoutSceneLayer> layers;      // the root's first, each layer before its subtree
    Array<LayoutSceneTransform> transforms;
    Array<LayoutResolvedEffects> effects; // the resolved clip paths, masks, filters and blending of layers
    Array<LayoutSceneContainer> scrolls;
    Array<LayoutSceneSegment> segments;
    Array<PrLayoutRect> fixedClips;       // the clips of the segments fixed to the viewport (LayoutPaintFixed)
    Array<LayoutSceneSlot> slots;         // each layer's run in painting order
    Array<LayoutSceneRegion> regions;     // each layer's run

    // Whether a wheel anywhere over the viewport waits, as the tree's value was set (prLayoutTreeViewportWheelEvents).
    PrLayoutWheelEvents viewportWheelEvents;

    PrLayoutRect canvas;            // in the viewport's content space
    PrColor canvasColor;            // the color that fills the canvas (prLayoutSceneCanvasColor)
    float pixelRatio;               // the tree's target pixels per px, to which playing rounds scroll offsets

    // The moves that scroll anchoring made to the tree's offsets since the scene before this one
    // (prLayoutSceneScrollAdjustments).
    Array<PrLayoutScrollAdjustment> adjustments;

    // The root's background layers over the canvas, from the bottom up, and what they were drawn from: the root's
    // decoration, the root's border box `canvasBorder`, and the viewport's extent `canvasViewport`. A new scene reuses
    // the runs of the previous scene if all of these are unchanged. The decoration names its background layers by an
    // index, and a later entry may reuse the index, so the layers' hash is compared as well.
    Array<LayoutSceneCanvasRun> canvasRuns;
    PrLayoutDecorationStyle canvasDecoration;
    uint32_t canvasLayersHash;
    PrLayoutRect canvasBorder;
    PrLayoutVec canvasViewport;
};

// ---------------------------------------------------------------------------------------------------------------------
// Making and playing

// Returns true if layoutScene would return the previous paint's scene (prLayoutTreeSceneIsCurrent).
bool layoutSceneIsCurrent(const LayoutTree* tree);

// Returns a scene of the tree's last layout, with a reference for the caller. Returns the previous paint's scene again
// if nothing that a scene reads has changed since, and otherwise assembles a new scene, recording again what its paint
// cache cannot keep.
const PrLayoutScene* layoutScene(LayoutTree* tree);

// Plays a scene into `list`, as `options` say.
void layoutScenePlay(const LayoutScene* scene, PrDrawList* list, const PrLayoutScenePlayOptions& options);

// Frees the scene and releases its recordings.
void layoutSceneDestroy(LayoutScene* scene);

// Appends to `out` the effects that `tracks` give the scene's layers at `time`, one for each layer that a track sets at
// that time, and returns the number of tracks that set a value. A transform is sampled with the extent of its layer's
// reference box.
uint32_t layoutSceneSampleTracks(const LayoutScene* scene, const PrLayoutTrack* PR_NULLABLE tracks, uint32_t count,
    double time, Array<PrLayoutSceneEffect>& out);

// Returns the scroll container of the scene that presents `node`, or kPaintScrollNone.
uint32_t layoutSceneNodeScroll(const LayoutScene* scene, PrLayoutNode node);

// Sets `out`, indexed by the scene's containers, to the offset at which each container is played, taken from `scrolls`
// and clamped to its range, or zero if `scrolls` gives none. The cost is proportional to the number of containers if
// `scrolls` follows the scene's order, as a tree's offsets do (prLayoutSceneScrolls), and to their product otherwise.
void layoutSceneScrollOffsets(const LayoutScene* scene, const PrLayoutSceneScroll* PR_NULLABLE scrolls, uint32_t count,
    Array<PrLayoutVec>& out);

// What a user's scroll at a point of a scene meets first: the innermost scroll container of the topmost layer,
// container or slot at the point. If the scroll meets a slot, it also holds the slot and the point in the space of the
// slot's content box. Scroll that the slot's content cannot take goes to the container, and scroll that the container
// cannot take goes along the container's chain (layoutSceneScrollNext). `blocks` is true if the point is in the scene's
// wheel region, whatever is topmost there, or if the wheel waits over the whole viewport.
struct LayoutScrollHit {
    uint32_t scroll;                // kPaintScrollNone if the point is outside the scene
    uint32_t slot;                  // kPaintScrollNone if it meets no slot
    PrLayoutVec point;              // in the slot's content box's space
    bool blocks;
};

// Returns what a user's scroll at `point` meets first. `point` is in the scene's space as played with `effects` and at
// `offsets` (layoutSceneScrollOffsets).
LayoutScrollHit layoutSceneScrollAt(const LayoutScene* scene, PrLayoutVec point,
    const PrLayoutSceneEffect* PR_NULLABLE effects, uint32_t count, const PrLayoutVec* offsets);

// Returns the scroll container around `scroll` in the scene, across its layers, or kPaintScrollNone past the viewport.
uint32_t layoutSceneScrollNext(const LayoutScene* scene, uint32_t scroll);

} // namespace pr_impl

// C opaque types
struct PrLayoutScene : pr_impl::LayoutScene {};

PR_ASSUME_NONNULL_END
