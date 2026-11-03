// Studio Kit's timeline: blocks of time on named tracks, under a ruler and a playhead, in a model that the caller owns.

#pragma once

#include "pr/color.h"
#include "pr/kit.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A span of time, in seconds, from `start` to `end`.
typedef struct PrKitTimeSpan {
    float start;
    float end;
} PrKitTimeSpan;

// A track of a timeline, named in its gutter.
typedef struct PrKitTimelineTrack {
    PrStringView name;
} PrKitTimelineTrack;

// A block on track `track` from `start` for `duration` seconds, in `color`, with `label` in it. `id` names the block
// from round to round, and no other block has it. `selected` marks the block as selected, and the timeline writes it.
typedef struct PrKitTimelineBlock {
    uint64_t id;
    uint32_t track;
    float start;
    float duration;
    PrColor color;
    PrStringView label;
    bool selected;
} PrKitTimelineBlock;

// A moment on track `track` at `time` seconds, drawn as a diamond in `color`. The timeline shows markers and does not
// edit them.
typedef struct PrKitTimelineMarker {
    uint32_t track;
    float time;
    PrColor color;
} PrKitTimelineMarker;

// What a time that a drag edits snaps to: the nearest of the enabled times within a few px of it, or else the nearest
// multiple of `step` if `step` is above zero. A scrub of the playhead never snaps to the playhead.
typedef struct PrKitTimelineSnap {
    float step;
    bool edges;                     // the starts and ends of the blocks that the drag does not move
    bool markers;
    bool playhead;
} PrKitTimelineSnap;

// The content of a timeline: the caller's model of tracks, blocks, which the timeline moves, resizes and selects, and
// markers. `view` is the span that the lanes show, which the wheel pans and, with Command or Control, zooms about the
// pointer. The playhead and the loop are shown if they are given, and a drag on the ruler scrubs the playhead. A read
// only timeline shows its model and edits none of it.
//
// A drag on a block moves it and the other selected blocks, and a drag on either end of a block resizes it. A press on
// a block selects it alone; with Shift it joins the selection, and with Command or Control it joins or leaves it. A
// drag on an empty part of a track selects the blocks that its rectangle touches, and a click there clears the
// selection. Each drag is one gesture.
typedef struct PrKitTimeline {
    PrUiKey key;
    const PrKitTimelineTrack* PR_NULLABLE tracks;
    uint32_t trackCount;
    PrKitTimelineBlock* PR_NULLABLE blocks;
    uint32_t blockCount;
    const PrKitTimelineMarker* PR_NULLABLE markers;
    uint32_t markerCount;
    PrKitTimeSpan* view;            // its end is after its start
    float* PR_NULLABLE playhead;
    const PrKitTimeSpan* PR_NULLABLE loop;
    PrKitTimelineSnap snap;
    bool readOnly;
} PrKitTimeline;

// The style of a timeline: the height of its tracks, and its placement in its parent.
typedef struct PrKitTimelineStyle {
    float trackHeight;              // px, or zero for the default
    PrKitPlace place;
} PrKitTimelineStyle;

// Creates a timeline under `parent`, applies the round's gestures to its model, and returns its node. Delete or
// Backspace asks to remove the selected blocks while the timeline has focus (prKitRemoved).
PrLayoutNode prKitTimeline(PrKit* kit, PrLayoutNode parent, PrKitTimeline timeline, PrKitTimelineStyle style);

// Returns true if a double press on an empty part of a track asked to add a block in the current round, and writes the
// track and the time in seconds where it asked. The request is one gesture.
bool prKitAdded(const PrKit* kit, PrLayoutNode timeline, uint32_t* track, float* time);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
