// Studio Kit's timeline: blocks of time on named tracks, under a ruler and a playhead, in a model that the caller owns.

#include "pr/kit_timeline.h"

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/kit_text.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// The width of the gutter of track names, the heights of the ruler and of a track if the timeline names none, and the
// inset of a block from its lane's top and bottom, in px.
constexpr float kGutterWidth = 96;
constexpr float kRulerHeight = 20;
constexpr float kTrackHeight = 26;
constexpr float kBlockInset = 3;

// The width of the zone at each end of a block that a press resizes from, and the distance within which a time snaps,
// in px.
constexpr float kEdgeZone = 6;
constexpr float kSnapReach = 6;

// The side of a marker's diamond, the width of the playhead's head, and the least distance between the ruler's ticks,
// in px.
constexpr float kMarkerSize = 8;
constexpr float kHeadWidth = 9;
constexpr float kTickSpacing = 64;

// The factor by which each px of wheel zooms the view, and the least and most seconds that the view spans.
constexpr float kZoomPerPx = 0.01f;
constexpr float kMinSpan = 0.05f;
constexpr float kMaxSpan = 36000;

// The steps between the ruler's ticks, in seconds, from the finest.
constexpr float kTickSteps[] = {0.01f, 0.025f, 0.05f, 0.1f, 0.25f, 0.5f, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800};

// The timeline's frame: a column of its header and its tracks.
struct FrameBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
    }
};
struct FrameFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.direction = PrLayoutFlexDirection_Column;
        flex.shrink = 0;
    }
};

// The header or a track: a row `height` px tall, of its gutter and its lane, with a rule below it.
PR_NO_PADDING_BEGIN
struct BandBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float height;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPx(height);
        box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(1);
    }
};
PR_NO_PADDING_END

// The header's corner or a track's name: the gutter, a row that clips, with a rule at its end.
struct GutterBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.width = prLayoutLengthPx(kGutterWidth);
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
        kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace4));
        box.border[PrRectSide_Right] = prLayoutUnitFromPx(1);
    }
};
struct GutterFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.shrink = 0;
        flex.alignItems = PrLayoutAlignment_Center;
    }
};

// The ruler or a lane: the room the gutter leaves, which positions and clips what is placed on it.
struct LaneBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Relative;
        box.minWidth = prLayoutLengthPx(0);
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
    }
};

// A box placed over a lane or the ruler from `left` for `width`, as percentages of its width, and from `top` to
// `bottom` px of its edges, framed if `framed`: a block, a selection, a region. Its place moves with the view.
PR_NO_PADDING_BEGIN
struct SpanBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float left;
    float width;
    float top;
    float bottom;
    bool framed;
    uint8_t _pad[3];

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.inset[PrRectSide_Left] = prLayoutLengthPercent(left);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(top);
        box.inset[PrRectSide_Bottom] = prLayoutLengthPx(bottom);
        box.width = prLayoutLengthPercent(width);
        if (framed)
            kitSetBorderWidth(box, 1);
    }
};

// A line one px wide at `percent` of its parent's width, through its height.
struct LineBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float percent;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Left] = prLayoutLengthPercent(percent);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(0);
        box.inset[PrRectSide_Bottom] = prLayoutLengthPx(0);
        box.width = prLayoutLengthPx(1);
    }
};

// A square `size` px on a side, centered at `percent` of its parent's width and at `top` px: a marker or the playhead's
// head.
struct MarkBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float percent;
    float top;
    float size;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Left] = prLayoutLengthPercent(percent);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(top);
        box.margin[PrRectSide_Left] = prLayoutLengthPx(-size / 2);
        box.width = prLayoutLengthPx(size);
        box.height = prLayoutLengthPx(size);
    }
};

// A mark turned by a quarter of a right angle into a diamond.
struct DiamondEffects {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Effect;
    void make(const PrKit&, PrLayoutEffectStyle& effects) const {
        effects.transform = prLayoutTransformFromAffine2(prAffine2Rotate(kPiF32 / 4));
    }
};

// A tick's label on the ruler, just past its tick at `percent` of the ruler's width.
struct TickLabelBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float percent;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Left] = prLayoutLengthPercent(percent);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(kPrKitSpace2);
        box.margin[PrRectSide_Left] = prLayoutLengthPx(kPrKitSpace2);
    }
};
PR_NO_PADDING_END

// What a drag on the timeline does.
enum {
    DragMode_None,
    DragMode_Scrub,                     // the ruler moves the playhead
    DragMode_Move,                      // a block moves, with the other selected blocks
    DragMode_ResizeStart,               // a block's start moves, and its end stays
    DragMode_ResizeEnd,                 // a block's end moves, and its start stays
    DragMode_Marquee,                   // a rectangle over the lanes selects the blocks it touches
};
typedef uint8_t DragMode;

// What a timeline keeps between rounds: the drag in progress.
struct TimelineState {
    uint64_t block;                 // the block that the drag holds
    float grab;                     // the time of the press, less the block's start
    float start;                    // the block's start and duration at the press
    float duration;
    float applied;                  // the shift that the move has applied to the selected blocks
    float marqueeTime;              // the time and track of the press that started the marquee
    uint32_t marqueeTrack;
    float reachTime;                // the time and track that the marquee reaches
    uint32_t reachTrack;
    DragMode mode;
    bool live;                   // a gesture is open
    bool extend;                 // the marquee adds to the selection
};

// The geometry of the view in one round: the seconds that it spans, and the width of the lanes at the last layout.
struct Span {
    float start;
    float seconds;
    float width;

    float percent(float time) const {
        return (time - start) / seconds * 100;
    }
    float timeAt(float x) const {
        return width > 0 ? start + x / width * seconds : start;
    }
    float secondsPerPx() const {
        return width > 0 ? seconds / width : 0;
    }
};

// A time that a drag snapped, and whether it snapped to a source: an edge, a marker or the playhead, rather than the
// grid's step or nothing.
struct Snapped {
    float time;
    bool guided;
};

// Returns `time` snapped to the timeline's snap sources, other than the blocks that `moving` names, or to the playhead
// if `scrub` is true.
Snapped snapTime(const PrKitTimeline& t, const Span& span, float time, bool scrub, bool moving) {
    const PrKitTimelineSnap& snap = t.snap;
    float nearest = kSnapReach * span.secondsPerPx();
    float best = time;
    bool found = false;
    auto consider = [&](float candidate) {
        float distance = absF32(candidate - time);
        if (distance <= nearest) {
            nearest = distance;
            best = candidate;
            found = true;
        }
    };
    if (snap.edges) {
        for (uint32_t i = 0; i < t.blockCount; ++i) {
            const PrKitTimelineBlock& b = t.blocks[i];
            if (moving && b.selected)
                continue;
            consider(b.start);
            consider(b.start + b.duration);
        }
    }
    if (snap.markers) {
        for (uint32_t i = 0; i < t.markerCount; ++i)
            consider(t.markers[i].time);
    }
    if (snap.playhead && !scrub && t.playhead != nullptr)
        consider(*t.playhead);
    if (found)
        return Snapped{best, true};
    return Snapped{snap.step > 0 ? roundF32(time / snap.step) * snap.step : time, false};
}

// Creates a span in `parent` (SpanBox) with `fill` and, if `edge` is not clear, a 1 px frame of it.
PrLayoutNode spanBox(PrKit& kit, PrLayoutNode parent, float left, float width, float top, float bottom, PrColor fill,
        PrColor edge) {
    SpanBox box{.left = left, .width = width, .top = top, .bottom = bottom, .framed = edge.a > 0};
    return prLayoutTreeFrameElement(kit.tree, parent, 0, kitStyle(kit, box, KitColors{.background = fill, .border = edge}));
}

// Creates a line at `percent` of `parent`'s width (LineBox).
void line(PrKit& kit, PrLayoutNode parent, float percent, PrColor color) {
    prLayoutTreeFrameElement(kit.tree, parent, 0, kitStyle(kit, LineBox{.percent = percent}, KitColors{.background = color}));
}

// Creates a mark at `percent` of `parent`'s width (MarkBox), a diamond if `diamond` is true.
void mark(PrKit& kit, PrLayoutNode parent, float percent, float top, float size, PrColor color, bool diamond) {
    MarkBox box{.percent = percent, .top = top, .size = size};
    KitColors fill{.background = color};
    PrLayoutStyleIndex record = diamond ? kitStyle(kit, box, fill, DiamondEffects{}) : kitStyle(kit, box, fill);
    prLayoutTreeFrameElement(kit.tree, parent, 0, record);
}

// Creates the ruler's ticks and their labels, at the finest step that leaves room between them.
void ticks(PrKit& kit, PrLayoutNode ruler, const Span& span) {
    const PrKitPalette& p = kit.theme.palette;
    float step = kTickSteps[countOf(kTickSteps) - 1];
    for (float s : kTickSteps) {
        if (span.width <= 0 || s / span.secondsPerPx() >= kTickSpacing) {
            step = s;
            break;
        }
    }
    float first = ceilF32(span.start / step);
    float last = floorF32((span.start + span.seconds) / step);
    for (float i = first; i <= last; ++i) {
        float time = i * step;
        float percent = span.percent(time);
        line(kit, ruler, percent, p.line2);
        PrLayoutStyleIndex record = kitStyle(kit, TickLabelBox{.percent = percent},
            KitFont{.role = PrKitTextRole_Meta}, KitInk{.color = p.text3});
        PrLayoutNode label = prLayoutTreeFrameElement(kit.tree, ruler, 0, record);
        char text[kF64TextMaxBytes + 1];
        size_t size = formatF64Significant(double(time), 6, text);
        text[size++] = 's';
        prLayoutTreeFrameText(kit.tree, label, 0, PrStringView{text, size});
    }
}

// The context that a timeline provides for the current round: the block that a double press on a track asked to add.
struct TimelineAdd {
    uint32_t track;
    float time;
    bool added;
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitTimeline(PrKit* kit, PrLayoutNode parent, PrKitTimeline timeline, PrKitTimelineStyle style) {
    const PrKitTimeline& t = timeline;
    PR_ASSERT(t.view != nullptr && t.view->end > t.view->start);
    PR_ASSERT(t.blocks != nullptr || t.blockCount == 0);
    PrKit& k = *kit;
    PrUi* ui = k.ui;
    const PrKitPalette& p = k.theme.palette;
    float trackHeight = style.trackHeight > 0 ? style.trackHeight : kTrackHeight;
    bool editable = !t.readOnly && !prUiIsDisabled(ui, parent);

    PrLayoutNode frame = prLayoutTreeFrameElement(k.tree, parent, t.key,
        kitStyle(k, FrameBox{}, FrameFlex{}, KitColors{.background = p.bg1}));
    TimelineAdd& add = kitProvide<TimelineAdd>(k, frame, KitContext_Timeline);
    PrUiFlags flags = PrUiFlag_Wheel;
    if (editable)
        flags = flags | PrUiFlag_Focusable;
    prUiSetFlags(ui, frame, flags);
    prKitPlace(kit, frame, &style.place);
    TimelineState* state = static_cast<TimelineState*>(prUiState(ui, frame, &kKitOwner, KitStateKey_Timeline,
        sizeof(TimelineState)));

    KitColors gutterRule{.border = p.line1};
    PrLayoutNode header = prLayoutTreeFrameElement(k.tree, frame, 0,
        kitStyle(k, BandBox{.height = kRulerHeight}, KitFixed{}, KitColors{.background = p.bg2, .border = p.line1}));
    prLayoutTreeFrameElement(k.tree, header, 0, kitStyle(k, GutterBox{}, GutterFlex{}, gutterRule));
    PrLayoutNode ruler = prLayoutTreeFrameElement(k.tree, header, 0, kitStyle(k, LaneBox{}, KitGrow{}));
    if (editable && t.playhead != nullptr)
        prUiSetFlags(ui, ruler, PrUiFlag_Pressable);
    Span span{.start = t.view->start, .seconds = t.view->end - t.view->start, .width = kitExtent(k, ruler).x};

    // The wheel pans the view, and with Command or Control zooms it about the pointer.
    PrVec2 wheel{};
    if (prUiWheel(ui, frame, &wheel) && span.width > 0) {
        PrUiEvents events = prUiEvents(ui, frame);
        bool zoom = false;
        for (uint32_t i = 0; i < events.count; ++i) {
            if (events.events[i].kind == PrAppEventKind_Scroll)
                zoom = (events.events[i].modifiers & (PrAppModifierFlag_Super | PrAppModifierFlag_Control)) != 0;
        }
        PrVec2 pointer{};
        PrVec2 at{};
        if (zoom && prUiPointer(ui, &pointer) && prUiLocalPoint(ui, ruler, pointer, &at)) {
            float pivot = span.timeAt(at.x);
            float seconds = span.seconds * expF32(wheel.y * kZoomPerPx);
            seconds = seconds < kMinSpan ? kMinSpan : seconds > kMaxSpan ? kMaxSpan : seconds;
            float start = pivot - (pivot - span.start) * (seconds / span.seconds);
            *t.view = PrKitTimeSpan{start, start + seconds};
        } else if (!zoom) {
            float shift = (wheel.x != 0 ? wheel.x : wheel.y) * span.secondsPerPx();
            *t.view = PrKitTimeSpan{t.view->start + shift, t.view->end + shift};
        }
        span.start = t.view->start;
        span.seconds = t.view->end - t.view->start;
    }

    // The ruler scrubs the playhead. Whether a drag is in progress, and whether this round ended it, after which the
    // next press starts another.
    PrUiDrag drag{};
    bool dragging = false;
    bool ended = false;
    // The time that the round's drag snapped to a source, which a guide marks across the lanes.
    Snapped guide{};
    Array<PrLayoutNode> lanes;
    if (editable && t.playhead != nullptr && prUiDrag(ui, ruler, 0, 0, &drag) && (drag.active || drag.released)) {
        PrVec2 at{};
        if (span.width > 0 && prUiLocalPoint(ui, ruler, drag.position, &at)) {
            dragging = true;
            ended = ended || drag.released;
            state->mode = DragMode_Scrub;
            if (!state->live) {
                state->live = true;
                prUiEdit(k.ui, frame, PrUiEditPhase_Begin);
            }
            Snapped snapped = snapTime(t, span, span.timeAt(at.x), true, false);
            float time = snapped.time;
            if (snapped.guided)
                guide = Snapped{time, true};
            if (time != *t.playhead) {
                *t.playhead = time;
                prUiEdit(k.ui, frame, PrUiEditPhase_Change);
            }
        }
    }

    // The lanes, and the gestures that start on them and on their blocks.
    for (uint32_t track = 0; track < t.trackCount; ++track) {
        PrLayoutNode row = prLayoutTreeFrameElement(
            k.tree, frame, 0, kitStyle(k, BandBox{.height = trackHeight}, KitFixed{}, gutterRule));
        PrLayoutNode name =
            prLayoutTreeFrameElement(k.tree, row, 0, kitStyle(k, GutterBox{}, GutterFlex{}, gutterRule));
        PrKitTextStyle nameStyle{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Secondary, .wrap = PrKitWrap_Clip};
        prKitLabel(kit, name, {.text = t.tracks[track].name}, nameStyle);
        PrLayoutNode lane = prLayoutTreeFrameElement(k.tree, row, 0, kitStyle(k, LaneBox{}, KitGrow{}));
        lanes.push(lane);
        if (editable)
            prUiSetFlags(ui, lane, PrUiFlag_Pressable);

        // A press on the lane's empty part starts a marquee, and a second press in quick succession asks for a block.
        if (editable && prUiDrag(ui, lane, 0, 0, &drag) && (drag.active || drag.released)) {
            PrVec2 at{};
            if (span.width > 0 && prUiLocalPoint(ui, lane, drag.position, &at)) {
                dragging = true;
                ended = ended || drag.released;
                if (state->mode != DragMode_Marquee) {
                    PrVec2 origin{};
                    prUiLocalPoint(ui, lane, drag.origin, &origin);
                    state->mode = DragMode_Marquee;
                    state->marqueeTime = span.timeAt(origin.x);
                    state->marqueeTrack = track;
                    state->extend = (prUiPressModifiers(ui, 0) & PrAppModifierFlag_Shift) != 0;
                    if (prUiPressCount(ui, 0) == 2) {
                        add.added = true;
                        add.track = track;
                        add.time = snapTime(t, span, state->marqueeTime, false, false).time;
                        kitEditOnce(k, frame, [] {});
                    }
                }
                float rows = at.y / trackHeight;
                int32_t reached = int32_t(track) + int32_t(floorF32(rows));
                reached = reached < 0 ? 0 : reached >= int32_t(t.trackCount) ? int32_t(t.trackCount) - 1 : reached;
                state->reachTime = span.timeAt(at.x);
                state->reachTrack = uint32_t(reached);
                if (drag.moved) {
                    if (!state->live) {
                        state->live = true;
                        prUiEdit(k.ui, frame, PrUiEditPhase_Begin);
                    }
                    float low = minF32(state->marqueeTime, state->reachTime);
                    float high = maxF32(state->marqueeTime, state->reachTime);
                    uint32_t top = state->marqueeTrack < state->reachTrack ? state->marqueeTrack : state->reachTrack;
                    uint32_t bottom = state->marqueeTrack < state->reachTrack ? state->reachTrack : state->marqueeTrack;
                    bool changed = false;
                    for (uint32_t i = 0; i < t.blockCount; ++i) {
                        PrKitTimelineBlock& b = t.blocks[i];
                        bool inside = b.track >= top && b.track <= bottom && b.start < high && b.start + b.duration > low;
                        bool selected = inside || (state->extend && b.selected);
                        changed = changed || selected != b.selected;
                        b.selected = selected;
                    }
                    if (changed)
                        prUiEdit(k.ui, frame, PrUiEditPhase_Change);
                } else if (drag.released && !state->extend) {
                    // A click clears the selection.
                    for (uint32_t i = 0; i < t.blockCount; ++i)
                        t.blocks[i].selected = false;
                }
            }
        }

        // The loop band and the playhead under the blocks, and the marquee's part of the lane over them.
        if (t.loop != nullptr && t.loop->end > t.loop->start) {
            PrColor wash = p.accent;
            wash.a = 0.06f;
            spanBox(k, lane, span.percent(t.loop->start), (t.loop->end - t.loop->start) / span.seconds * 100, 0, 0,
                wash, PrColor{});
        }

        for (uint32_t i = 0; i < t.blockCount; ++i) {
            PrKitTimelineBlock& b = t.blocks[i];
            if (b.track != track)
                continue;
            PrColor edge = b.selected ? p.text1 : PrColor{0, 0, 0, 0};
            PrColor fill = b.color;
            fill.a = b.selected ? 1 : 0.8f;
            PrLayoutNode node = spanBox(k, lane, span.percent(b.start), b.duration / span.seconds * 100,
                kBlockInset, kBlockInset, fill, edge);
            if (b.label.size != 0) {
                PrKitTextStyle labelStyle{
                    .role = PrKitTextRole_BodySmall,
                    .tone = PrKitTone_Primary,
                    .wrap = PrKitWrap_Clip,
                };
                PrKitBoxStyle inner{
                    .direction = PrKitDirection_Row,
                    .align = PrKitAlign_Center,
                    .overflow = PrKitOverflow_Clip,
                    .padding = prKitEdgesXY(0, kPrKitSpace3),
                };
                inner.place.width = prKitPercent(100);
                inner.place.height = prKitPercent(100);
                prKitLabel(kit, prKitBox(kit, node, {}, inner), {.text = b.label}, labelStyle);
            }
            if (!editable)
                continue;
            prUiSetFlags(ui, node, PrUiFlag_Pressable);
            float width = kitExtent(k, node).x;
            PrVec2 pointer{};
            PrVec2 local{};
            bool near = prUiPointer(ui, &pointer) && prUiLocalPoint(ui, node, pointer, &local) && width > 0 &&
                (local.x < kEdgeZone || local.x > width - kEdgeZone);
            // A block that a resize holds keeps the resize cursor wherever the drag takes the pointer.
            bool resizing = state->block == b.id &&
                (state->mode == DragMode_ResizeStart || state->mode == DragMode_ResizeEnd);
            prUiSetCursor(ui, node, near || resizing ? PrAppCursor_ResizeX : PrAppCursor_Hand);
            if (!prUiDrag(ui, node, 0, 0, &drag) || !(drag.active || drag.released))
                continue;
            PrVec2 at{};
            if (span.width <= 0 || !prUiLocalPoint(ui, lane, drag.position, &at))
                continue;
            dragging = true;
            ended = ended || drag.released;
            float time = span.timeAt(at.x);
            if (state->mode < DragMode_Move || state->block != b.id) {
                // The press: what it does depends on where on the block it lands, and on the modifiers.
                PrVec2 origin{};
                PrVec2 pressed{};
                prUiLocalPoint(ui, node, drag.origin, &origin);
                prUiLocalPoint(ui, lane, drag.origin, &pressed);
                PrAppModifierFlags modifiers = prUiPressModifiers(ui, 0);
                bool toggle = (modifiers & (PrAppModifierFlag_Super | PrAppModifierFlag_Control)) != 0;
                bool extend = (modifiers & PrAppModifierFlag_Shift) != 0;
                if (toggle) {
                    b.selected = !b.selected;
                } else if (!b.selected) {
                    for (uint32_t j = 0; j < t.blockCount; ++j)
                        t.blocks[j].selected = extend && t.blocks[j].selected;
                    b.selected = true;
                }
                if (origin.x < kEdgeZone)
                    state->mode = DragMode_ResizeStart;
                else if (origin.x > width - kEdgeZone)
                    state->mode = DragMode_ResizeEnd;
                else
                    state->mode = DragMode_Move;
                state->block = b.id;
                state->grab = span.timeAt(pressed.x) - b.start;
                state->start = b.start;
                state->duration = b.duration;
                state->applied = 0;
            }
            if (!drag.moved || !b.selected)
                continue;
            if (!state->live) {
                state->live = true;
                prUiEdit(k.ui, frame, PrUiEditPhase_Begin);
            }
            float least = t.snap.step > 0 ? t.snap.step : kEdgeZone * span.secondsPerPx();
            bool changed = false;
            if (state->mode == DragMode_Move) {
                // The block's start or its end snaps, whichever moves it less.
                float start = time - state->grab;
                Snapped atStart = snapTime(t, span, start, false, true);
                Snapped atEnd = snapTime(t, span, start + state->duration, false, true);
                float byStart = atStart.time - start;
                float byEnd = atEnd.time - (start + state->duration);
                bool end = byEnd != 0 && (byStart == 0 || absF32(byEnd) < absF32(byStart));
                float snapped = start + (end ? byEnd : byStart);
                if (end ? atEnd.guided : atStart.guided)
                    guide = end ? atEnd : atStart;
                float shift = snapped - state->start;
                float by = shift - state->applied;
                if (by != 0) {
                    for (uint32_t j = 0; j < t.blockCount; ++j) {
                        if (t.blocks[j].selected)
                            t.blocks[j].start += by;
                    }
                    state->applied = shift;
                    changed = true;
                }
            } else if (state->mode == DragMode_ResizeStart) {
                float end = state->start + state->duration;
                Snapped snapped = snapTime(t, span, time, false, true);
                if (snapped.guided)
                    guide = snapped;
                float start = minF32(snapped.time, end - least);
                changed = start != b.start;
                b.start = start;
                b.duration = end - start;
            } else {
                Snapped snapped = snapTime(t, span, time, false, true);
                if (snapped.guided)
                    guide = snapped;
                float end = maxF32(snapped.time, state->start + least);
                changed = end != b.start + b.duration;
                b.duration = end - b.start;
            }
            if (changed)
                prUiEdit(k.ui, frame, PrUiEditPhase_Change);
        }

        for (uint32_t i = 0; i < t.markerCount; ++i) {
            const PrKitTimelineMarker& m = t.markers[i];
            if (m.track == track)
                mark(k, lane, span.percent(m.time), (trackHeight - kMarkerSize) / 2, kMarkerSize, m.color, true);
        }
        if (t.playhead != nullptr)
            line(k, lane, span.percent(*t.playhead), p.accent);
        if (state->mode == DragMode_Marquee && state->live) {
            uint32_t top = state->marqueeTrack < state->reachTrack ? state->marqueeTrack : state->reachTrack;
            uint32_t bottom = state->marqueeTrack < state->reachTrack ? state->reachTrack : state->marqueeTrack;
            if (track >= top && track <= bottom) {
                float low = minF32(state->marqueeTime, state->reachTime);
                float high = maxF32(state->marqueeTime, state->reachTime);
                PrColor wash = p.accent;
                wash.a = 0.12f;
                spanBox(k, lane, span.percent(low), (high - low) / span.seconds * 100, 0, 0, wash, p.accent);
            }
        }
    }

    // A drag that snapped to an edge, a marker or the playhead shows where, over the lanes.
    if (guide.guided && dragging && !ended) {
        for (uint32_t i = 0; i < lanes.count; ++i)
            line(k, lanes.data[i], span.percent(guide.time), p.warning);
        line(k, ruler, span.percent(guide.time), p.warning);
    }
    lanes.reset();

    // The ruler's ticks, the loop band, and the playhead's line and head.
    ticks(k, ruler, span);
    if (t.loop != nullptr && t.loop->end > t.loop->start) {
        PrColor wash = p.accent;
        wash.a = 0.25f;
        spanBox(k, ruler, span.percent(t.loop->start), (t.loop->end - t.loop->start) / span.seconds * 100,
            kRulerHeight - kPrKitSpace2 - 1, 0, wash, PrColor{});
    }
    if (t.playhead != nullptr) {
        float percent = span.percent(*t.playhead);
        line(k, ruler, percent, p.accent);
        mark(k, ruler, percent, 0, kHeadWidth, p.accent, false);
    }

    if (!dragging || ended) {
        if (state->live) {
            state->live = false;
            prUiEdit(k.ui, frame, PrUiEditPhase_Commit);
        }
        state->mode = DragMode_None;
    }

    // Delete and Backspace ask to remove the selection.
    if (editable) {
        bool any = false;
        for (uint32_t i = 0; i < t.blockCount; ++i)
            any = any || t.blocks[i].selected;
        bool pressed = prUiKeyPressed(ui, frame, PrAppKey_Delete, 0);
        pressed = prUiKeyPressed(ui, frame, PrAppKey_Backspace, 0) || pressed;
        if (pressed && any) {
            kitSignal(k, frame, KitSignal_Removed);
            kitEditOnce(k, frame, [] {});
        }
    }
    return frame;
}

bool prKitAdded(const PrKit* kit, PrLayoutNode timeline, uint32_t* track, float* time) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    const TimelineAdd* add = kitContext<TimelineAdd>(*kit, timeline, KitContext_Timeline, &provider);
    if (add == nullptr || provider != timeline || !add->added)
        return false;
    *track = add->track;
    *time = add->time;
    return true;
}
