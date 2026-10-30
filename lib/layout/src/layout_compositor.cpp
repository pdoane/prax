// Layout compositors: presenting scenes, and running the animations of the effects of their layers independently of the
// client.

#include "pr/layout_compositor.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/draw_list.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_scene.h"

namespace pr_impl {

namespace {

// What was committed for the presented scene, or for an embedded content.
struct CompositorContent {
    uint64_t embed;                 // zero for the presented scene
    const PrLayoutScene* scene;
    PrAffine2 transform;
    Array<PrLayoutTrack> tracks;    // each naming a run of `keyframes`
    Array<PrLayoutKeyframe> keyframes;
};

// The offset of a scroll container, which frames show. It is either the client's offset or the compositor's own, set by
// a user's scroll.
struct CompositorScroll {
    uint64_t embed;
    PrLayoutNode node;
    PrLayoutVec offset;
    uint32_t sequence;              // of the compositor's last scroll of it, or zero if the client set it last
};

// What a frame plays of one content: its scene, to which the frame holds a reference, its transform, and its runs of
// the frame's effects and offsets.
struct FramePart {
    uint64_t embed;
    const PrLayoutScene* scene;
    PrAffine2 transform;
    uint32_t effectStart;
    uint32_t effectCount;
    uint32_t scrollStart;
    uint32_t scrollCount;
};

// The greatest number of embedded contents, nested one inside another, that a frame plays. A slot nested deeper shows
// nothing.
constexpr uint32_t kMaxEmbedDepth = 16;

// A compositor. The fields after `mutex` are guarded by it. A frame takes what it needs of them under the mutex, then
// plays and presents outside it.
struct LayoutCompositor {
    PrLayoutCompositorDesc desc;
    PrThread thread;
    bool threaded;
    PrDrawList* list;                   // the list each frame records into, used by one frame at a time
    Array<FramePart> parts;             // scratch: the parts a frame plays
    Array<PrLayoutSceneEffect> effects; // scratch: the values the tracks give a frame, by part
    Array<PrLayoutSceneScroll> offsets; // scratch: the offsets a frame plays, by part
    uint32_t depth;                     // scratch: how many embedded contents the frame is playing inside
    mutable PrMutex mutex;
    Array<CompositorContent> contents;
    Array<CompositorScroll> scrolls;
    uint32_t sequence;              // incremented by each scroll
    uint32_t taken;                 // the sequence as of the last take
    PrLayoutCompositorStats stats;
    double lastStart;               // the monotonic time at which the last frame started, or zero before the first
    PrLayoutVec shown;              // the viewport offset that the last frame showed
    double lastMoved;               // when a frame last showed the viewport moved; zero before any did
    bool unshown;                   // something was committed, removed or scrolled since the last frame

    // The current wheel sequence, once a wheel has come: when and where its last wheel came, on the clock of the
    // description, and whether the sequence's wheels wait for the client.
    bool wheeled;
    double lastWheel;
    PrLayoutVec wheelAt;
    bool wheelWaits;

    bool moving;                    // a track of the last frame's scenes changes as the clock advances
    bool ending;
};

// Returns what was committed for `embed`, or null.
const CompositorContent* PR_NULLABLE contentOf(const LayoutCompositor* c, uint64_t embed) {
    for (uint32_t i = 0; i < c->contents.count; ++i) {
        if (c->contents.data[i].embed == embed)
            return &c->contents.data[i];
    }
    return nullptr;
}

CompositorContent* PR_NULLABLE contentOf(LayoutCompositor* c, uint64_t embed) {
    return const_cast<CompositorContent*>(contentOf(static_cast<const LayoutCompositor*>(c), embed));
}

// Returns the offset of `node` of `embed`, or null if it has none. A node with no offset shows unscrolled.
CompositorScroll* PR_NULLABLE scrollOf(LayoutCompositor* c, uint64_t embed, PrLayoutNode node) {
    for (uint32_t i = 0; i < c->scrolls.count; ++i) {
        if (c->scrolls.data[i].embed == embed && c->scrolls.data[i].node == node)
            return &c->scrolls.data[i];
    }
    return nullptr;
}

// Appends the offsets of `embed` to `out`.
void addScrolls(const LayoutCompositor* c, uint64_t embed, Array<PrLayoutSceneScroll>& out) {
    for (uint32_t i = 0; i < c->scrolls.count; ++i) {
        if (c->scrolls.data[i].embed == embed)
            out.push(PrLayoutSceneScroll{c->scrolls.data[i].node, c->scrolls.data[i].offset});
    }
}

// Sets the client's offsets of `embed`, except for containers that the compositor scrolled since `applied`. Drops the
// offsets of containers that `scene` does not have. The mutex is held.
void setScrolls(LayoutCompositor* c, uint64_t embed, const LayoutScene* PR_NULLABLE scene,
        const PrLayoutSceneScroll* PR_NULLABLE scrolls, uint32_t count, uint32_t applied) {
    for (uint32_t i = 0; i < count; ++i) {
        CompositorScroll* own = scrollOf(c, embed, scrolls[i].node);
        if (own != nullptr && own->sequence > applied)
            continue;
        if (own == nullptr) {
            own = &c->scrolls.emplace();
            own->embed = embed;
            own->node = scrolls[i].node;
        }
        own->offset = scrolls[i].offset;
        own->sequence = 0;
    }
    for (uint32_t i = c->scrolls.count; i-- != 0;) {
        const CompositorScroll& own = c->scrolls.data[i];
        if (own.embed == embed && (scene == nullptr || layoutSceneNodeScroll(scene, own.node) == kPaintScrollNone))
            c->scrolls.erase(i);
    }
}

// Moves the scroll container `scroll` of the scene committed for `embed` by as much of `delta` as it can, along the
// axes in `axes`. Returns how far it moved. The mutex is held.
PrLayoutVec scrollBy(LayoutCompositor* c, uint64_t embed, const LayoutScene* scene, uint32_t scroll, PrLayoutVec delta,
        PrLayoutScrollFlags axes) {
    const LayoutSceneContainer& s = scene->scrolls.data[scroll];
    CompositorScroll* own = scrollOf(c, embed, s.node);
    PrLayoutVec from = own != nullptr ? layoutScrollClamp(own->offset, s.range) : kLayoutVecZero;
    PrLayoutVec wanted{
        (axes & PrLayoutScrollFlag_X) != 0 ? from.x + delta.x : from.x,
        (axes & PrLayoutScrollFlag_Y) != 0 ? from.y + delta.y : from.y,
    };
    PrLayoutVec moved = layoutScrollClamp(wanted, s.range) - from;
    if (moved == kLayoutVecZero)
        return moved;
    PrLayoutVec to = from + moved;
    ++c->sequence;
    if (own == nullptr) {
        own = &c->scrolls.emplace();
        own->embed = embed;
        own->node = s.node;
    }
    own->offset = to;
    own->sequence = c->sequence;
    return moved;
}

// Scrolls by `delta` at `point` in the scene of `content`, as a user's scroll does. The content embedded at the point
// scrolls first. Returns the part of `delta` that none of the containers took. The mutex is held.
PrLayoutVec scrollAt(LayoutCompositor* c, const CompositorContent& content, PrLayoutVec point, PrLayoutVec delta,
        uint32_t depth) {
    const LayoutScene* scene = content.scene;
    Array<PrLayoutSceneScroll> scrolls;
    addScrolls(c, content.embed, scrolls);
    Array<PrLayoutVec> offsets;
    layoutSceneScrollOffsets(scene, scrolls.data, scrolls.count, 0, offsets);
    LayoutScrollHit hit = layoutSceneScrollAt(scene, point, nullptr, 0, offsets.data);
    offsets.reset();
    scrolls.reset();
    PrLayoutVec rest = delta;
    if (hit.slot != kPaintScrollNone && depth < kMaxEmbedDepth) {
        uint64_t embed = scene->slots.data[hit.slot].embed;
        const CompositorContent* inner = contentOf(c, embed);
        PrDAffine2 inverse{};
        if (inner != nullptr && embed != content.embed && prDAffine2Invert(prDAffine2FromAffine2(inner->transform), &inverse))
            rest = scrollAt(c, *inner, layoutVecMapped(inverse, hit.point), rest, depth + 1);
    }
    for (uint32_t scroll = hit.scroll; scroll != kPaintScrollNone && rest != kLayoutVecZero;
            scroll = layoutSceneScrollNext(scene, scroll)) {
        const LayoutSceneContainer& s = scene->scrolls.data[scroll];
        rest -= scrollBy(c, content.embed, scene, scroll, rest, s.userAxes);
        // A container whose overscroll behavior contains the scroll keeps the rest from the containers around it.
        if ((s.chainAxes & PrLayoutScrollFlag_X) == 0)
            rest.x = kLayoutUnitZero;
        if ((s.chainAxes & PrLayoutScrollFlag_Y) == 0)
            rest.y = kLayoutUnitZero;
    }
    return rest;
}

// Returns true if `point` in the scene of `content` is in its wheel region, or in the region of the content embedded at
// the point. The mutex is held.
bool regionHolds(LayoutCompositor* c, const CompositorContent& content, PrLayoutVec point, uint32_t depth) {
    const LayoutScene* scene = content.scene;
    Array<PrLayoutSceneScroll> scrolls;
    addScrolls(c, content.embed, scrolls);
    Array<PrLayoutVec> offsets;
    layoutSceneScrollOffsets(scene, scrolls.data, scrolls.count, 0, offsets);
    LayoutScrollHit hit = layoutSceneScrollAt(scene, point, nullptr, 0, offsets.data);
    offsets.reset();
    scrolls.reset();
    if (hit.blocks)
        return true;
    if (hit.slot == kPaintScrollNone || depth == kMaxEmbedDepth)
        return false;
    uint64_t embed = scene->slots.data[hit.slot].embed;
    const CompositorContent* inner = contentOf(c, embed);
    PrDAffine2 inverse{};
    return inner != nullptr && embed != content.embed &&
        prDAffine2Invert(prDAffine2FromAffine2(inner->transform), &inverse) &&
        regionHolds(c, *inner, layoutVecMapped(inverse, hit.point), depth + 1);
}

// Plays the content that fills a slot, in the same way that the scene of a frame plays.
void playEmbedded(void* PR_NULLABLE context, uint64_t embed, PrLayoutVec, PrDrawList* list);

// Plays a part of the frame into `list`, without a fill for its canvas if `canvasFilled` is true.
void playPart(LayoutCompositor* c, const FramePart& part, PrDrawList* list, bool canvasFilled) {
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.canvasFilled = canvasFilled;
    options.effects = c->effects.data + part.effectStart;
    options.effectCount = part.effectCount;
    options.scrolls = c->offsets.data + part.scrollStart;
    options.scrollCount = part.scrollCount;
    options.embed = playEmbedded;
    options.embedContext = c;
    prDrawListPushTransformAffine2(list, part.transform);
    prLayoutScenePlay(part.scene, list, &options);
    prDrawListPopTransform(list);
}

void playEmbedded(void* PR_NULLABLE context, uint64_t embed, PrLayoutVec, PrDrawList* list) {
    LayoutCompositor* c = static_cast<LayoutCompositor*>(context);
    if (c->depth == kMaxEmbedDepth)
        return;
    for (uint32_t i = 0; i < c->parts.count; ++i) {
        if (c->parts.data[i].embed != embed || embed == 0)
            continue;
        ++c->depth;
        playPart(c, c->parts.data[i], list, false);
        --c->depth;
        return;
    }
}

// A pause in scrolling longer than this is a pause by the user, not a stall.
constexpr double kScrollIdleMillis = 2000;

void frame(LayoutCompositor* c) {
    double started = prTimeMonotonicMilliseconds();
    double time = c->desc.now(c->desc.context);
    prMutexLock(&c->mutex);
    if (contentOf(c, 0) == nullptr) {
        prMutexUnlock(&c->mutex);
        return;
    }
    // Gathers, for each content, its scene, its transform, the values of its tracks at the time of the frame, and its
    // scroll offsets.
    c->parts.clear();
    c->effects.clear();
    c->offsets.clear();
    uint32_t sampled = 0;
    c->unshown = false;
    c->moving = false;
    for (uint32_t k = 0; k < c->contents.count; ++k) {
        const CompositorContent& content = c->contents.data[k];
        // A track moves until its active interval is over, unless it is paused. One whose delay has not passed moves
        // once it starts.
        for (uint32_t i = 0; i < content.tracks.count && !c->moving; ++i) {
            bool finished = false;
            prLayoutTimingProgress(&content.tracks.data[i].timing, time, &finished);
            c->moving = !content.tracks.data[i].timing.paused && !finished;
        }
        uint32_t start = c->effects.count;
        sampled += layoutSceneSampleTracks(content.scene, content.tracks.data, content.tracks.count, time, c->effects);
        uint32_t scrollStart = c->offsets.count;
        addScrolls(c, content.embed, c->offsets);
        prLayoutSceneRetain(content.scene);
        c->parts.push(FramePart{
            content.embed, content.scene, content.transform, start, c->effects.count - start,
            scrollStart, c->offsets.count - scrollStart,
        });
    }
    uint32_t scrolled = 0;
    for (uint32_t i = 0; i < c->scrolls.count; ++i)
        scrolled += c->scrolls.data[i].sequence != 0 ? 1 : 0;
    prMutexUnlock(&c->mutex);
    const FramePart* root = nullptr;
    for (uint32_t i = 0; i < c->parts.count; ++i) {
        if (c->parts.data[i].embed == 0)
            root = &c->parts.data[i];
    }
    // The offset of the viewport, clamped to the range of the first container of the scene, which is the viewport.
    const LayoutScene* data = root->scene;
    PrLayoutVec shown = kLayoutVecZero;
    for (uint32_t i = 0; i < root->scrollCount && data->scrolls.count != 0; ++i) {
        const PrLayoutSceneScroll& scroll = c->offsets.data[root->scrollStart + i];
        if (scroll.node == kPrLayoutNodeNone)
            shown = layoutScrollClamp(scroll.offset, data->scrolls.data[0].range);
    }
    prDrawListClear(c->list);
    c->depth = 0;
    playPart(c, *root, c->list, c->desc.canvasFilled);
    PrColor16 canvas = prLayoutSceneCanvasColor(root->scene);
    for (uint32_t i = 0; i < c->parts.count; ++i)
        prLayoutSceneRelease(c->parts.data[i].scene);
    double played = prTimeMonotonicMilliseconds();
    c->desc.present(c->desc.context, c->list, canvas);
    double ended = prTimeMonotonicMilliseconds();
    prMutexLock(&c->mutex);
    ++c->stats.frames;
    c->stats.sampled = sampled;
    c->stats.scrolled = scrolled;
    c->stats.frameMillis = ended - started;
    if (c->lastStart != 0 && started - c->lastStart > c->stats.longestGapMillis) {
        c->stats.longestGapMillis = started - c->lastStart;
        c->stats.longestGapFrame = c->stats.frames;
    }
    c->lastStart = started;
    // A frame that shows the viewport moved ends a gap. The gap counts only if an earlier frame also moved the
    // viewport.
    if (shown.x != c->shown.x || shown.y != c->shown.y) {
        if (c->lastMoved != 0 && started - c->lastMoved > c->stats.longestScrollGapMillis &&
                started - c->lastMoved < kScrollIdleMillis) {
            c->stats.longestScrollGapMillis = started - c->lastMoved;
            c->stats.longestScrollGapFrame = c->stats.frames;
        }
        c->lastMoved = started;
        c->shown = shown;
    }
    if (c->stats.frameMillis > c->stats.longestMillis) {
        c->stats.longestMillis = c->stats.frameMillis;
        c->stats.longestFrame = c->stats.frames;
        c->stats.longestPlayMillis = played - started;
    }
    prMutexUnlock(&c->mutex);
}

void serve(void* PR_NULLABLE context) {
    LayoutCompositor* c = static_cast<LayoutCompositor*>(context);
    for (;;) {
        prMutexLock(&c->mutex);
        bool ending = c->ending;
        prMutexUnlock(&c->mutex);
        if (ending)
            break;
        frame(c);
        prThreadSleep(c->desc.interval);
    }
}

// Frees what a content holds.
void resetContent(CompositorContent& content) {
    prLayoutSceneRelease(content.scene);
    content.tracks.reset();
    content.keyframes.reset();
}

} // namespace

// Returns true if the compositor wants a frame. The caller holds the mutex.
bool wantsFrame(const LayoutCompositor* c) {
    return (c->unshown || c->moving) && contentOf(c, 0) != nullptr;
}

// Marks that a frame would show what the last frame did not, if `changed`. Returns true if the compositor did not want
// a frame and now does, in which case the caller calls notifyWanted once it releases the mutex. The caller holds the
// mutex.
bool markUnshown(LayoutCompositor* c, bool changed) {
    bool wanted = wantsFrame(c);
    c->unshown = c->unshown || changed;
    return !wanted && wantsFrame(c);
}

// Tells the host that the compositor wants a frame, if `wanted`. The caller does not hold the mutex, so that the host
// may call the compositor.
void notifyWanted(const LayoutCompositor* c, bool wanted) {
    if (wanted && c->desc.wantFrame != nullptr)
        c->desc.wantFrame(c->desc.context);
}

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrLayoutCompositor : LayoutCompositor {};

PrLayoutCompositor* prLayoutCompositorCreate(const PrLayoutCompositorDesc* desc) {
    PrLayoutCompositor* c = allocZeroed<PrLayoutCompositor>();
    c->desc = *desc;
    c->list = prDrawListCreate();
    prMutexInit(&c->mutex);
    c->threaded = desc->interval > 0;
    if (c->threaded)
        prThreadStart(&c->thread, serve, c);
    return c;
}

void prLayoutCompositorDestroy(PrLayoutCompositor* PR_NULLABLE compositor) {
    if (compositor == nullptr)
        return;
    if (compositor->threaded) {
        prMutexLock(&compositor->mutex);
        compositor->ending = true;
        prMutexUnlock(&compositor->mutex);
        prThreadJoin(&compositor->thread);
    }
    for (uint32_t i = 0; i < compositor->contents.count; ++i)
        resetContent(compositor->contents.data[i]);
    compositor->contents.reset();
    compositor->parts.reset();
    compositor->effects.reset();
    compositor->offsets.reset();
    compositor->scrolls.reset();
    prDrawListRelease(compositor->list);
    prMutexReset(&compositor->mutex);
    prMemoryFree(compositor);
}

void prLayoutCompositorCommit(PrLayoutCompositor* compositor, const PrLayoutCommit* commit) {
    LayoutCompositor* c = compositor;
    prLayoutSceneRetain(commit->scene);
    prMutexLock(&c->mutex);
    CompositorContent* content = contentOf(c, commit->embed);
    if (content == nullptr) {
        content = &c->contents.emplace();
        *content = CompositorContent{};
        content->embed = commit->embed;
    }
    const PrLayoutScene* last = content->scene;
    content->scene = commit->scene;
    content->transform = commit->transform;
    content->tracks.clear();
    content->keyframes.clear();
    for (uint32_t i = 0; i < commit->trackCount; ++i) {
        content->keyframes.append(commit->tracks[i].keyframes, commit->tracks[i].keyframeCount);
        content->tracks.push(commit->tracks[i]);
    }
    // The copied tracks point at the keyframes of the content, which no longer move.
    uint32_t at = 0;
    for (uint32_t i = 0; i < content->tracks.count; ++i) {
        content->tracks.data[i].keyframes = content->keyframes.data + at;
        at += content->tracks.data[i].keyframeCount;
    }
    setScrolls(c, commit->embed, commit->scene, commit->scrolls, commit->scrollCount, commit->scrollsApplied);
    // A new scene's boxes are where scroll anchoring moved them. An offset that the compositor's own scroll set, which
    // the client's offset did not replace, moves with them, so the scene and its offset arrive in the same frame.
    if (commit->scene != last) {
        const Array<PrLayoutScrollAdjustment>& adjustments = commit->scene->adjustments;
        for (uint32_t i = 0; i < adjustments.count; ++i) {
            CompositorScroll* own = scrollOf(c, commit->embed, adjustments.data[i].node);
            if (own != nullptr && own->sequence > commit->scrollsApplied)
                own->offset = own->offset + adjustments.data[i].delta;
        }
    }
    ++c->stats.commits;
    bool wanted = markUnshown(c, true);
    prMutexUnlock(&c->mutex);
    prLayoutSceneRelease(last);
    notifyWanted(c, wanted);
}

void prLayoutCompositorSetScrolls(PrLayoutCompositor* compositor, uint64_t embed,
        const PrLayoutSceneScroll* PR_NULLABLE scrolls, uint32_t count, uint32_t scrollsApplied) {
    prMutexLock(&compositor->mutex);
    const CompositorContent* content = contentOf(compositor, embed);
    setScrolls(compositor, embed, content != nullptr ? content->scene : nullptr, scrolls, count, scrollsApplied);
    bool wanted = markUnshown(compositor, true);
    prMutexUnlock(&compositor->mutex);
    notifyWanted(compositor, wanted);
}

void prLayoutCompositorRemove(PrLayoutCompositor* compositor, uint64_t embed) {
    const PrLayoutScene* last = nullptr;
    prMutexLock(&compositor->mutex);
    for (uint32_t i = 0; i < compositor->contents.count; ++i) {
        if (compositor->contents.data[i].embed != embed)
            continue;
        last = compositor->contents.data[i].scene;
        compositor->contents.data[i].scene = nullptr;
        resetContent(compositor->contents.data[i]);
        compositor->contents.erase(i);
        break;
    }
    for (uint32_t i = compositor->scrolls.count; i-- != 0;) {
        if (compositor->scrolls.data[i].embed == embed)
            compositor->scrolls.erase(i);
    }
    bool wanted = markUnshown(compositor, true);
    prMutexUnlock(&compositor->mutex);
    prLayoutSceneRelease(last);
    notifyWanted(compositor, wanted);
}

PrLayoutVec prLayoutCompositorScroll(PrLayoutCompositor* compositor, uint64_t embed, PrLayoutNode node, PrLayoutVec delta) {
    prMutexLock(&compositor->mutex);
    PrLayoutVec moved = kLayoutVecZero;
    const CompositorContent* content = contentOf(compositor, embed);
    const LayoutScene* scene = content != nullptr ? content->scene : nullptr;
    uint32_t scroll = scene != nullptr ? layoutSceneNodeScroll(scene, node) : kPaintScrollNone;
    if (scroll != kPaintScrollNone)
        moved = scrollBy(compositor, embed, scene, scroll, delta, PrLayoutScrollFlag_X | PrLayoutScrollFlag_Y);
    bool wanted = markUnshown(compositor, moved != kLayoutVecZero);
    prMutexUnlock(&compositor->mutex);
    notifyWanted(compositor, wanted);
    return moved;
}

bool prLayoutCompositorScrollAt(PrLayoutCompositor* compositor, PrLayoutVec point, PrLayoutVec delta) {
    prMutexLock(&compositor->mutex);
    const CompositorContent* root = contentOf(compositor, 0);
    bool any = root != nullptr && scrollAt(compositor, *root, point, delta, 0) != delta;
    bool wanted = markUnshown(compositor, any);
    prMutexUnlock(&compositor->mutex);
    notifyWanted(compositor, wanted);
    return any;
}

bool prLayoutCompositorWheelBlocks(PrLayoutCompositor* compositor, PrLayoutVec point) {
    double now = compositor->desc.now(compositor->desc.context);
    prMutexLock(&compositor->mutex);
    if (!compositor->wheeled || now - compositor->lastWheel > kPrLayoutWheelSequenceGap || point != compositor->wheelAt) {
        const CompositorContent* root = contentOf(compositor, 0);
        compositor->wheelWaits = root != nullptr && regionHolds(compositor, *root, point, 0);
    }
    compositor->wheeled = true;
    compositor->lastWheel = now;
    compositor->wheelAt = point;
    bool waits = compositor->wheelWaits;
    prMutexUnlock(&compositor->mutex);
    return waits;
}

void prLayoutCompositorReleaseWheel(PrLayoutCompositor* compositor) {
    prMutexLock(&compositor->mutex);
    compositor->wheelWaits = false;
    prMutexUnlock(&compositor->mutex);
}

uint32_t prLayoutCompositorTakeScrolls(PrLayoutCompositor* compositor, PrLayoutScrollFn fn, void* PR_NULLABLE context) {
    Array<PrLayoutScroll> taking;
    prMutexLock(&compositor->mutex);
    for (uint32_t i = 0; i < compositor->scrolls.count; ++i) {
        const CompositorScroll& own = compositor->scrolls.data[i];
        if (own.sequence > compositor->taken)
            taking.push(PrLayoutScroll{own.embed, own.node, own.offset});
    }
    compositor->taken = compositor->sequence;
    uint32_t taken = compositor->taken;
    prMutexUnlock(&compositor->mutex);
    // The callback runs outside the mutex, so it may scroll.
    for (uint32_t i = 0; i < taking.count; ++i)
        fn(context, &taking.data[i]);
    taking.reset();
    return taken;
}

void prLayoutCompositorFrame(PrLayoutCompositor* compositor) {
    PR_ASSERT(!compositor->threaded);
    frame(compositor);
}

bool prLayoutCompositorWantsFrame(const PrLayoutCompositor* compositor) {
    prMutexLock(&compositor->mutex);
    bool wants = wantsFrame(compositor);
    prMutexUnlock(&compositor->mutex);
    return wants;
}

PrLayoutCompositorStats prLayoutCompositorStats(const PrLayoutCompositor* compositor) {
    prMutexLock(&compositor->mutex);
    PrLayoutCompositorStats stats = compositor->stats;
    prMutexUnlock(&compositor->mutex);
    return stats;
}
