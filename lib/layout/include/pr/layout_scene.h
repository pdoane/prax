// Scenes: a tree's last layout painted into layers for a compositor, and played into draw lists.

#pragma once

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/geom.h"
#include "pr/layout_animation.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Scenes

// The output of a paint, as layers for a compositor. Each layer has its effects (opacity, transform), its recording,
// and the scroll containers between it and its parent layer. Scroll container content is recorded unscrolled in its own
// segment, and a scene holds no scroll offsets: a player is given them, so a scroll records and assembles nothing. A
// scene is immutable, atomically reference counted and holds no references into its tree, so it can be played on
// another thread while the tree changes.
typedef struct PrLayoutScene PrLayoutScene;

// Paints the last layout into a scene, reusing cached recordings that are still valid. The caller owns the returned
// reference.
const PrLayoutScene* prLayoutTreeScene(PrLayoutTree* tree);

// Returns true if the tree's last scene shows its last layout: nothing that a scene reads changed since that scene was
// painted, so prLayoutTreeScene would return it as it is. Returns false if no scene was painted yet.
bool prLayoutTreeSceneIsCurrent(const PrLayoutTree* tree);

// Adds or releases a reference. The last release frees the scene.
void prLayoutSceneRetain(const PrLayoutScene* scene);
void prLayoutSceneRelease(const PrLayoutScene* PR_NULLABLE scene);

// ---------------------------------------------------------------------------------------------------------------------
// Playing

// Overrides a player applies to a layer's effects when playing a scene, as for a compositor-driven animation.
enum {
    PrLayoutSceneEffectFlag_Opacity = 1 << 0,
    PrLayoutSceneEffectFlag_Transform = 1 << 1,
};
typedef uint8_t PrLayoutSceneEffectFlags;

// An override for one layer, identified by its node.
typedef struct PrLayoutSceneEffect {
    PrLayoutNode node;
    PrLayoutSceneEffectFlags flags;
    float opacity;                  // with PrLayoutSceneEffectFlag_Opacity

    // With PrLayoutSceneEffectFlag_Transform: the layer's transform, resolved for its reference box and relative to its
    // origin (PrLayoutEffectStyle).
    PrMatrix4 transform;
} PrLayoutSceneEffect;

// A scroll container's offset: an element's, or the viewport's for kPrLayoutNodeNone.
typedef struct PrLayoutSceneScroll {
    PrLayoutNode node;
    PrLayoutVec offset;
} PrLayoutSceneScroll;

// Draws the content for embed slot `embed` into `list` while a scene plays, for a content box of `extent`, in
// coordinates relative to the box's top left. The player clips the drawing to the content box.
typedef void (*PrLayoutEmbedFn)(void* PR_NULLABLE context, uint64_t embed, PrLayoutVec extent, PrDrawList* list);

// Options for prLayoutScenePlay.
typedef struct PrLayoutScenePlayOptions {
    const PrLayoutSceneEffect* PR_NULLABLE effects;   // overrides of the scene's own effects
    uint32_t effectCount;

    // The offsets of the scene's scroll containers, which the player clamps to their ranges. A container given none
    // plays unscrolled.
    const PrLayoutSceneScroll* PR_NULLABLE scrolls;
    uint32_t scrollCount;

    // Fills each embed slot, called with `embedContext`. Null leaves the slots empty.
    PrLayoutEmbedFn PR_NULLABLE embed;
    void* PR_NULLABLE embedContext;

    // True if the application filled what lies beneath the scene with the scene's canvas color
    // (prLayoutSceneCanvasColor), as its render pass's clear does, so that the player records no fill for the canvas.
    // The root's background images play over the canvas either way.
    bool canvasFilled;
} PrLayoutScenePlayOptions;

// Returns options with no overrides, no scroll offsets and empty embed slots.
PrLayoutScenePlayOptions prLayoutScenePlayOptionsDefault(void);

// Writes to `out`, up to `capacity`, the overrides that `tracks` give the scene's layers at `time`, one for each layer
// that a track sets at that time, and returns the number of such layers. The tracks' times are on `time`'s timeline. A
// transform is sampled with the extent of its layer's reference box, as a compositor samples it, so that playing the
// scene with the overrides shows what a compositor's frame at `time` shows.
uint32_t prLayoutSceneSampleTracks(const PrLayoutScene* scene, const PrLayoutTrack* PR_NULLABLE tracks,
    uint32_t trackCount, double time, PrLayoutSceneEffect* PR_NULLABLE out, uint32_t capacity);

// Plays the scene into `list`, in pixels. A null `options` means the defaults.
void prLayoutScenePlay(const PrLayoutScene* scene, PrDrawList* list, const PrLayoutScenePlayOptions* PR_NULLABLE options);

// Returns the color that fills the scene's canvas beneath its content, straight alpha: the root's background color over
// the tree's canvas color (prLayoutTreeSetCanvasColor), source-over on encoded values as a target blends. The fill
// covers the viewport. An application that owns the render pass may fill it with this color more cheaply than a draw
// can, as the pass's clear, and tells the player that it did (PrLayoutScenePlayOptions::canvasFilled).
PrColor16 prLayoutSceneCanvasColor(const PrLayoutScene* scene);

// ---------------------------------------------------------------------------------------------------------------------
// Contents

// An embed slot in a scene: a replaced element with PrLayoutContentKind_Embed content, and its content box size.
typedef struct PrLayoutSceneEmbed {
    uint64_t embed;
    PrLayoutNode node;
    PrLayoutVec extent;
} PrLayoutSceneEmbed;

// Writes up to `capacity` of the scene's embed slots to `out`, in paint order, and returns the total count.
uint32_t prLayoutSceneEmbeds(const PrLayoutScene* scene, PrLayoutSceneEmbed* PR_NULLABLE out, uint32_t capacity);

// prLayoutSceneLayerCount returns the number of layers in the scene, and prLayoutSceneHasLayer returns true if `node`
// has a layer.
uint32_t prLayoutSceneLayerCount(const PrLayoutScene* scene);
bool prLayoutSceneHasLayer(const PrLayoutScene* scene, PrLayoutNode node);

// Returns true if the scene has scroll container `node`, or the viewport for kPrLayoutNodeNone.
bool prLayoutSceneHasScroll(const PrLayoutScene* scene, PrLayoutNode node);

// Writes to `out` up to `capacity` of the moves that scroll anchoring made to the tree's offsets since the scene before
// this one, one for each container, and returns the total count. The scene's boxes are where those moves place them, so
// a compositor that scrolled a container itself moves its own offset by as much when the scene arrives.
uint32_t prLayoutSceneScrollAdjustments(const PrLayoutScene* scene, PrLayoutScrollAdjustment* PR_NULLABLE out, uint32_t capacity);

// Writes up to `capacity` of the tree's scroll offsets for the scroll containers of `scene`, a scene of the tree, to
// `out`, each clamped to its container's range, and returns the total count. A container whose node the tree no longer
// has is unscrolled.
uint32_t prLayoutSceneScrolls(const PrLayoutTree* tree, const PrLayoutScene* scene,
    PrLayoutSceneScroll* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Painting a tree directly

// Paints the last layout into `list`, in pixels, in CSS painting order (CSS2/#elaborate-stacking-contexts). Equivalent
// to playing prLayoutTreeScene's scene with the tree's scroll offsets (prLayoutSceneScrolls).
void prLayoutTreePaint(PrLayoutTree* tree, PrDrawList* list);

// Draws `root`, a root of the last layout other than the viewport's, into `list`, in pixels from the top left of its
// containing block. A replaced element's painter calls it while the tree paints, to draw the root in its content. The
// root is drawn whole, in CSS painting order: the layers in it are drawn in place, with their own effects, and its
// scroll containers at the tree's offsets. Its embed slots are empty. The tree draws the replaced element again when
// the root's drawing changes. A paint draws each root before the replaced elements that may draw it: a root may draw
// only roots that come after it in the layout's roots.
void prLayoutNodeDraw(PrLayoutTree* tree, PrLayoutNode root, PrDrawList* list);

// Returns a number that changes each time a paint draws `root` again, a root of the last layout other than the
// viewport's, so that a client that shows the root's drawing (prLayoutNodeDraw) on its own draws it again only then.
// Returns 0 for a node that is no such root, or that no paint has drawn.
uint64_t prLayoutNodeDrawingRevision(const PrLayoutTree* tree, PrLayoutNode root);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
