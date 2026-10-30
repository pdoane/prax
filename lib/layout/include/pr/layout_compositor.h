// Layout compositors: presenting scenes, and running the animations of their layers' effects and their scrolling
// separately from the client.
//
// A client commits a scene and the tracks that animate its layers' opacity and transform. At each frame the compositor
// plays the scene with each track's value at the frame's time, and hands the list to the host to present. A compositor
// with a thread of its own presents on that thread, so composited animations run while the client's thread is busy.
//
// A compositor holds the offsets of the scenes' scroll containers, because a scene holds none. It holds the client's
// offsets, given by the client's commits and scrolls, and its own offsets, set as a user scrolls the containers in the
// compositor. It hands its own offsets back to the client to apply to its tree. If the compositor scrolled a container
// since the client last took its offsets, the client's offset for that container gives way to the compositor's, so a
// scroll on either side is never undone by the other.
//
// A scene's slots of embedded content (PrLayoutContentKind_Embed) show the scenes committed for their ids, each with
// its own tracks and scroll containers, so an embedded document updates without the scene around it. A scroll at a
// point over a slot scrolls the embedded scene first, and what that scene cannot take goes on to the containers around
// the slot.
//
// A user's wheel scrolls in the compositor without the client, except over a scene's wheel region, where the client may
// cancel it. The compositor answers which wheels wait for the client, one gesture at a time.

#pragma once

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/geom.h"
#include "pr/layout_animation.h"
#include "pr/layout_scene.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What presents scenes.
typedef struct PrLayoutCompositor PrLayoutCompositor;

// Returns the time of a frame on the clock that the tracks' timings use, in milliseconds.
typedef double (*PrLayoutClockFn)(void* PR_NULLABLE context);

// Presents `list`, which the compositor made for a frame and changes again at the next one. `canvas` is the frame's
// canvas color: that of the scene committed with no embed (prLayoutSceneCanvasColor). The list fills the canvas with it
// unless the host does (PrLayoutCompositorDesc::canvasFilled). Called on the thread that makes the frame.
typedef void (*PrLayoutPresentFn)(void* PR_NULLABLE context, PrDrawList* list, PrColor16 canvas);

// Called when the compositor comes to want a frame (prLayoutCompositorWantsFrame), on the thread whose call to the
// compositor caused it, which may be any thread. A host that makes frames only as they are needed requests one.
typedef void (*PrLayoutWantFrameFn)(void* PR_NULLABLE context);

// How a compositor presents. Its callbacks are called with `context`.
typedef struct PrLayoutCompositorDesc {
    void* PR_NULLABLE context;
    PrLayoutClockFn now;
    PrLayoutPresentFn present;
    PrLayoutWantFrameFn PR_NULLABLE wantFrame;  // null for a host that makes frames whether they are wanted or not
    double interval;                            // the milliseconds between frames its thread makes, or zero for none

    // True if the host fills what lies beneath each list that it presents with the frame's canvas color, as its render
    // pass's clear does, so that the list records no fill for the canvas (PrLayoutScenePlayOptions::canvasFilled).
    bool canvasFilled;
} PrLayoutCompositorDesc;

// Creates a compositor with nothing committed, and starts its thread if it has one.
PrLayoutCompositor* prLayoutCompositorCreate(const PrLayoutCompositorDesc* desc);

// Ends the compositor's thread, if it has one, and frees the compositor and what it holds.
void prLayoutCompositorDestroy(PrLayoutCompositor* PR_NULLABLE compositor);

// What a client commits.
typedef struct PrLayoutCommit {
    // The embedded content that the scene fills, which the slots of committed scenes name (PrLayoutContentKind_Embed),
    // or zero for the scene that the compositor presents.
    uint64_t embed;

    const PrLayoutScene* scene;
    PrAffine2 transform;                     // from the scene's pixels to the presented list's, or to its slot's
    const PrLayoutTrack* PR_NULLABLE tracks; // animating the scene's layers
    uint32_t trackCount;

    // The client's offsets for the scene's scroll containers (prLayoutSceneScrolls). `scrollsApplied` is what
    // prLayoutCompositorTakeScrolls returned for the last offsets that the client applied before it read these offsets,
    // or zero if it applied none.
    const PrLayoutSceneScroll* PR_NULLABLE scrolls;
    uint32_t scrollCount;
    uint32_t scrollsApplied;
} PrLayoutCommit;

// Hands the compositor a scene to present, or to show in the slots of an embedded content, replacing what was committed
// for it before, along with the client's scroll offsets. The compositor holds a reference to the scene and copies the
// tracks and offsets. It drops the offsets of containers that the scene does not have.
void prLayoutCompositorCommit(PrLayoutCompositor* compositor, const PrLayoutCommit* commit);

// Sets the offsets of scroll containers of what is committed for `embed`, as the client's own scrolls moved them, for
// the frames after the call, with no new scene. `scrollsApplied` has the same meaning as for a commit. Callable from
// any thread.
void prLayoutCompositorSetScrolls(PrLayoutCompositor* compositor, uint64_t embed,
    const PrLayoutSceneScroll* PR_NULLABLE scrolls, uint32_t count, uint32_t scrollsApplied);

// Drops what was committed for the embedded content `embed`, whose slots then show nothing, and its scroll offsets.
void prLayoutCompositorRemove(PrLayoutCompositor* compositor, uint64_t embed);

// Scrolls the scroll container `node` of the scene committed for `embed` (zero for the presented scene), or its
// viewport for kPrLayoutNodeNone, by `delta` in the scene's units, within its range. Returns how far it moved, or zero
// if nothing is committed or the scene has no such container. Callable from any thread. The frames made after the call
// show the offset.
PrLayoutVec prLayoutCompositorScroll(PrLayoutCompositor* compositor, uint64_t embed, PrLayoutNode node, PrLayoutVec delta);

// Scrolls in the same way as a user's scroll by `delta` at `point`, both in the presented scene's units. The innermost
// scroll container at that point that the user may scroll along an axis takes what it can, and passes the rest on to
// the containers around it (css-overscroll-1/#scroll-chaining), from an embedded scene's containers to those around its
// slot. Returns true if any container moved. Callable from any thread.
bool prLayoutCompositorScrollAt(PrLayoutCompositor* compositor, PrLayoutVec point, PrLayoutVec delta);

// The pause between a user's wheels, in milliseconds, after which the next wheel starts a new sequence. A wheel at
// another point also starts one. A sequence is one gesture, and its first wheel decides whether the rest of it waits.
static const double kPrLayoutWheelSequenceGap = 500.0;

// Returns true if a user's wheel at `point`, in the presented scene's units, waits for the client, which may cancel it,
// before anything scrolls. The first wheel of a sequence waits if the point is in the wheel region of the presented
// scene or of a scene embedded in a slot there (PrLayoutWheelEvents_Blocking). The rest of the sequence waits as the
// first did, until the client releases it. The compositor times the sequence by its clock
// (PrLayoutCompositorDesc::now). A wheel that does not wait is the host's to scroll (prLayoutCompositorScrollAt).
// Callable from any thread.
bool prLayoutCompositorWheelBlocks(PrLayoutCompositor* compositor, PrLayoutVec point);

// Lets the rest of the current wheel sequence scroll without waiting, as the client does once no listener canceled the
// sequence's first wheel. Callable from any thread.
void prLayoutCompositorReleaseWheel(PrLayoutCompositor* compositor);

// One scroll container's offset in the scene committed for `embed`, which is zero for the presented scene. The
// container is an element, or the viewport for kPrLayoutNodeNone.
typedef struct PrLayoutScroll {
    uint64_t embed;
    PrLayoutNode node;
    PrLayoutVec offset;
} PrLayoutScroll;

// Called with each offset a compositor hands back.
typedef void (*PrLayoutScrollFn)(void* PR_NULLABLE context, const PrLayoutScroll* scroll);

// Calls `fn` with the offset of each scroll container that the compositor scrolled since the last call, for the client
// to apply to its tree (prLayoutNodeSetScroll). Returns the value that the client commits as
// PrLayoutCommit::scrollsApplied with the first scenes it records after applying the offsets.
uint32_t prLayoutCompositorTakeScrolls(PrLayoutCompositor* compositor, PrLayoutScrollFn fn, void* PR_NULLABLE context);

// Makes and presents a frame at the clock's time, on the caller's thread, for a compositor with no thread of its own.
// Does nothing before the first commit.
void prLayoutCompositorFrame(PrLayoutCompositor* compositor);

// Returns true if a frame would show what the last frame did not: a commit, a removal or a scroll since then, or a
// composited animation that changes as the clock advances. A host that makes frames only as they are needed makes one
// while this is true: as the compositor's wantFrame callback asks, and after each frame while an animation runs.
// Returns false before the first commit.
bool prLayoutCompositorWantsFrame(const PrLayoutCompositor* compositor);

// What a compositor has done.
typedef struct PrLayoutCompositorStats {
    uint32_t frames;                // number of frames presented
    uint32_t commits;               // number of scenes committed
    uint32_t sampled;               // number of track values that the last frame sampled
    uint32_t scrolled;              // offsets set by the compositor's own scrolls that the last frame showed

    // The duration of the last frame and of the longest frame so far, in milliseconds, the index of that longest frame,
    // and the part of it that playing its scene took, including releasing the scene it held before. The rest of the
    // frame is presenting.
    double frameMillis;
    double longestMillis;
    uint32_t longestFrame;
    double longestPlayMillis;

    // The longest time between the starts of two frames, in milliseconds, and the frame that ended it. A long gap means
    // that a frame could not start on time, because the thread waited for its mutex or a frame before it ran long.
    double longestGapMillis;
    uint32_t longestGapFrame;

    // While the viewport scrolls, the longest time between two frames that each showed it moving, in milliseconds, and
    // the frame that ended it. A user who scrolls throughout sees this as a stall.
    double longestScrollGapMillis;
    uint32_t longestScrollGapFrame;
} PrLayoutCompositorStats;

// Returns what the compositor has done since it was created.
PrLayoutCompositorStats prLayoutCompositorStats(const PrLayoutCompositor* compositor);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
