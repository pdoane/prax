#include "pr/text_edit_motion.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/unicode_grapheme.h"

namespace pr_impl {

namespace {

// The geometry of text with no layout: its bytes' lines, in the paragraph's direction.
struct TextLines {
    PrStringView text;
    PrTextDirection direction;
    Array<PrTextEditLineStop> stops;
};

bool textLinesLine(void* PR_NULLABLE context, PrTextEditPosition at, PrTextEditLineStep step, PrTextEditLine* out) {
    TextLines& lines = *static_cast<TextLines*>(context);
    PrStringView text = lines.text;
    uint32_t start = prTextEditLineStart(text, at.offset);
    if (step == PrTextEditLineStep_Next) {
        uint32_t end = prTextEditLineEnd(text, at.offset);
        if (end >= text.size)
            return false;
        start = end + 1;
    } else if (step == PrTextEditLineStep_Previous) {
        if (start == 0)
            return false;
        start = prTextEditLineStart(text, start - 1);
    }
    uint32_t end = prTextEditLineEnd(text, start);
    PrStringView view{text.data, end};
    lines.stops.clear();
    bool reversed = lines.direction == PrTextDirection_Rtl;
    float along = 0;
    for (uint32_t b = start;; b = uint32_t(prUnicodeGraphemeNext(view, b))) {
        lines.stops.push(PrTextEditLineStop{{at.node, b, PrTextEditAffinity_Downstream}, reversed ? -along : along});
        along += 1;
        if (b >= end)
            break;
    }
    for (uint32_t i = 0, j = lines.stops.count; reversed && i + 1 < j; ++i) {
        --j;
        PrTextEditLineStop kept = lines.stops.data[i];
        lines.stops.data[i] = lines.stops.data[j];
        lines.stops.data[j] = kept;
    }
    *out = PrTextEditLine{
        .stops = lines.stops.data,
        .stopCount = lines.stops.count,
        .flow = PrTextEditFlow_HorizontalTb,
        .direction = lines.direction,
        .startStop = reversed ? lines.stops.count - 1 : 0,
        .endStop = reversed ? 0 : lines.stops.count - 1,
    };
    return true;
}

PrStringView textLinesText(void* PR_NULLABLE context, uint64_t) {
    return static_cast<TextLines*>(context)->text;
}

void textLinesDestroy(void* PR_NULLABLE context) {
    TextLines* lines = static_cast<TextLines*>(context);
    lines->stops.reset();
    prMemoryFree(lines);
}

// Returns true if the position is the same caret offset in the same node, whichever side it stands on.
bool isSameOffset(PrTextEditPosition a, PrTextEditPosition b) {
    return a.node == b.node && a.offset == b.offset;
}

// Returns the index of the stop of `at` in `line`: the one with its offset and affinity, or else its offset, or else
// the nearest offset in its node, or else the first stop.
uint32_t stopIndex(const PrTextEditLine& line, PrTextEditPosition at) {
    uint32_t found = UINT32_MAX;
    uint32_t nearest = UINT32_MAX;
    for (uint32_t i = 0; i < line.stopCount; ++i) {
        PrTextEditPosition p = line.stops[i].position;
        if (isSameOffset(p, at) && p.affinity == at.affinity)
            return i;
        if (p.node != at.node)
            continue;
        uint32_t distance = p.offset < at.offset ? at.offset - p.offset : p.offset - at.offset;
        if (distance < nearest) {
            nearest = distance;
            found = i;
        }
    }
    return found != UINT32_MAX ? found : 0;
}

// The text around a node that a word or a cluster can span: the nodes that no block boundary separates, joined. Flat
// text is its own storage.
struct Paragraph {
    // A node of the paragraph, and where its text starts in the paragraph's.
    struct Piece {
        uint64_t node;
        uint32_t start;
        uint32_t size;
    };

    Array<Piece> pieces;
    Array<char> joined;
    PrStringView text;

    void gather(const PrTextEditGeometry& g, uint64_t node) {
        uint64_t first = node;
        bool contiguous = false;
        while (g.nodeStep != nullptr) {
            uint64_t before = g.nodeStep(g.context, first, PrTextEditLineStep_Previous, &contiguous);
            if (before == 0 || !contiguous)
                break;
            first = before;
        }
        uint32_t size = 0;
        for (uint64_t at = first; at != 0;) {
            PrStringView t = g.nodeText(g.context, at);
            pieces.push(Piece{at, size, uint32_t(t.size)});
            size += uint32_t(t.size);
            uint64_t after = g.nodeStep != nullptr ? g.nodeStep(g.context, at, PrTextEditLineStep_Next, &contiguous) : 0;
            at = after != 0 && contiguous ? after : 0;
        }
        if (pieces.count == 1) {
            text = g.nodeText(g.context, first);
            return;
        }
        for (uint32_t i = 0; i < pieces.count; ++i) {
            PrStringView t = g.nodeText(g.context, pieces.data[i].node);
            joined.append(t.data, uint32_t(t.size));
        }
        text = PrStringView{joined.data, joined.count};
    }

    uint32_t flatten(PrTextEditPosition at) const {
        for (uint32_t i = 0; i < pieces.count; ++i) {
            if (pieces.data[i].node == at.node) {
                uint32_t offset = at.offset < pieces.data[i].size ? at.offset : pieces.data[i].size;
                return pieces.data[i].start + offset;
            }
        }
        return 0;
    }

    // Returns the position of `flat`. A position where two nodes meet is at the end of the first if the motion went
    // forward, and at the start of the second if it went back, as a visual motion leaves it.
    PrTextEditPosition position(uint32_t flat, bool forward) const {
        uint32_t chosen = 0;
        for (uint32_t i = 0; i < pieces.count; ++i) {
            const Piece& p = pieces.data[i];
            if (forward ? flat <= p.start + p.size : flat >= p.start) {
                chosen = i;
                if (forward)
                    break;
            }
        }
        const Piece& p = pieces.data[chosen];
        uint32_t offset = flat - p.start;
        bool ends = forward && offset == p.size && offset != 0 && chosen + 1 < pieces.count;
        PrTextEditAffinity affinity = ends ? PrTextEditAffinity_Upstream : PrTextEditAffinity_Downstream;
        return PrTextEditPosition{p.node, offset, affinity};
    }

    void reset() {
        pieces.reset();
        joined.reset();
    }
};

// Returns where a motion in the order of the text takes `at`: by cluster, or by word if `word` is true. Past the end of
// a paragraph, a cluster motion stands at the start of the next, as a word motion continues into it.
PrTextEditPosition moveLogical(const PrTextEditGeometry& g, PrTextEditPosition at, bool forward, bool word) {
    for (;;) {
        Paragraph paragraph{};
        paragraph.gather(g, at.node);
        PrStringView text = paragraph.text;
        uint32_t flat = paragraph.flatten(at);
        bool atEnd = forward ? flat >= text.size : flat == 0;
        if (!atEnd) {
            uint32_t to;
            if (word)
                to = forward ? prTextEditWordAfter(text, flat) : prTextEditWordBefore(text, flat);
            else
                to = forward ? prTextEditClusterAfter(text, flat) : prTextEditClusterBefore(text, flat);
            PrTextEditPosition moved = paragraph.position(to, forward);
            paragraph.reset();
            return moved;
        }
        uint64_t edge = paragraph.pieces.data[forward ? paragraph.pieces.count - 1 : 0].node;
        paragraph.reset();
        bool contiguous = false;
        PrTextEditLineStep step = forward ? PrTextEditLineStep_Next : PrTextEditLineStep_Previous;
        uint64_t node = g.nodeStep != nullptr ? g.nodeStep(g.context, edge, step, &contiguous) : 0;
        if (node == 0)
            return at;
        uint32_t offset = forward ? 0 : uint32_t(g.nodeText(g.context, node).size);
        at = PrTextEditPosition{node, offset, PrTextEditAffinity_Downstream};
        if (!word)
            return at;
    }
}

// Writes which sides of its offset `at` has a place on, on its line. An offset has both at a change of direction.
void sidesOf(const PrTextEditLine& line, PrTextEditPosition at, bool* upstream, bool* downstream) {
    *upstream = false;
    *downstream = false;
    for (uint32_t i = 0; i < line.stopCount; ++i) {
        PrTextEditPosition p = line.stops[i].position;
        if (isSameOffset(p, at)) {
            *upstream = *upstream || p.affinity == PrTextEditAffinity_Upstream;
            *downstream = *downstream || p.affinity == PrTextEditAffinity_Downstream;
        }
    }
}

// Returns `at` standing on `preferred` if its line gives it a place on both sides, or on the side that it has.
PrTextEditPosition settled(const PrTextEditGeometry& g, PrTextEditPosition at, PrTextEditAffinity preferred) {
    PrTextEditLine line{};
    at.affinity = PrTextEditAffinity_Downstream;
    if (!g.line(g.context, at, PrTextEditLineStep_Same, &line))
        return at;
    bool upstream = false;
    bool downstream = false;
    sidesOf(line, at, &upstream, &downstream);
    if (upstream && downstream)
        at.affinity = preferred;
    else if (upstream)
        at.affinity = PrTextEditAffinity_Upstream;
    return at;
}

// Returns where a logical motion that stops at a change of direction takes `at`. It arrives on the side of the text
// that it came through, and the next press crosses to the other side.
PrTextEditPosition moveStops(const PrTextEditGeometry& g, PrTextEditPosition at, bool forward) {
    PrTextEditAffinity arriving = forward ? PrTextEditAffinity_Upstream : PrTextEditAffinity_Downstream;
    PrTextEditLine line{};
    if (at.affinity == arriving && g.line(g.context, at, PrTextEditLineStep_Same, &line)) {
        bool upstream = false;
        bool downstream = false;
        sidesOf(line, at, &upstream, &downstream);
        if (upstream && downstream) {
            at.affinity = forward ? PrTextEditAffinity_Downstream : PrTextEditAffinity_Upstream;
            return at;
        }
    }
    return settled(g, moveLogical(g, at, forward, false), arriving);
}

// Returns the first or last position of all the text.
PrTextEditPosition textEdge(const PrTextEditGeometry& g, PrTextEditPosition at, bool end) {
    uint64_t node = at.node;
    bool contiguous = false;
    PrTextEditLineStep step = end ? PrTextEditLineStep_Next : PrTextEditLineStep_Previous;
    while (g.nodeStep != nullptr) {
        uint64_t adjacent = g.nodeStep(g.context, node, step, &contiguous);
        if (adjacent == 0)
            break;
        node = adjacent;
    }
    uint32_t offset = end ? uint32_t(g.nodeText(g.context, node).size) : 0;
    return PrTextEditPosition{node, offset, PrTextEditAffinity_Downstream};
}

// Returns where a visual motion takes `at`: to the next place along the line that differs from its own, or onto the
// line that continues the text past the end of the line.
PrTextEditPosition moveVisual(const PrTextEditGeometry& g, PrTextEditPosition at, bool right) {
    PrTextEditLine line{};
    if (!g.line(g.context, at, PrTextEditLineStep_Same, &line) || line.stopCount == 0)
        return at;
    uint32_t i = stopIndex(line, at);
    float along = line.stops[i].along;
    int32_t step = right ? 1 : -1;
    for (int32_t j = int32_t(i) + step; j >= 0 && j < int32_t(line.stopCount); j += step) {
        if (line.stops[j].along != along)
            return line.stops[j].position;
    }
    bool ltr = line.direction != PrTextDirection_Rtl;
    bool forward = right == ltr;
    PrTextEditLine onto{};
    PrTextEditLineStep adjacent = forward ? PrTextEditLineStep_Next : PrTextEditLineStep_Previous;
    if (!g.line(g.context, at, adjacent, &onto) || onto.stopCount == 0)
        return at;
    // A line of a paragraph is entered at the end where its text continues the line left behind.
    bool fromLeft = forward == (onto.direction != PrTextDirection_Rtl);
    int32_t inward = fromLeft ? 1 : -1;
    int32_t j = fromLeft ? 0 : int32_t(onto.stopCount) - 1;
    // A soft wrap gives one offset a place on each line, and a press moves past the offset.
    if (isSameOffset(onto.stops[j].position, at)) {
        float edge = onto.stops[j].along;
        for (int32_t k = j + inward; k >= 0 && k < int32_t(onto.stopCount); k += inward) {
            if (onto.stops[k].along != edge)
                return onto.stops[k].position;
        }
    }
    return onto.stops[j].position;
}

// Returns where a motion between lines takes `at`, keeping the goal, or the edge of the text if there is no such line.
PrTextEditPosition moveBetweenLines(const PrTextEditGeometry& g, PrTextEditPosition at, bool next, float* goal, bool* hasGoal) {
    PrTextEditLine line{};
    if (!g.line(g.context, at, PrTextEditLineStep_Same, &line) || line.stopCount == 0)
        return textEdge(g, at, next);
    if (!*hasGoal) {
        *goal = line.stops[stopIndex(line, at)].along;
        *hasGoal = true;
    }
    PrTextEditLine onto{};
    PrTextEditLineStep step = next ? PrTextEditLineStep_Next : PrTextEditLineStep_Previous;
    if (!g.line(g.context, at, step, &onto) || onto.stopCount == 0)
        return textEdge(g, at, next);
    uint32_t best = 0;
    float nearest = kInfinityF32;
    for (uint32_t i = 0; i < onto.stopCount; ++i) {
        float distance = onto.stops[i].along - *goal;
        distance = distance < 0 ? -distance : distance;
        if (distance < nearest) {
            nearest = distance;
            best = i;
        }
    }
    return onto.stops[best].position;
}

// A motion in terms of the line: along it or across the lines.
enum {
    LineAxis_Left,
    LineAxis_Right,
    LineAxis_Previous,
    LineAxis_Next,
};
typedef uint8_t LineAxis;

// The motion along or across the line of each arrow, by PrTextEditFlow and the order of the arrows in
// PrTextEditCommand.
constexpr LineAxis kArrowAxes[4][4] = {
    {LineAxis_Left, LineAxis_Right, LineAxis_Previous, LineAxis_Next},
    {LineAxis_Next, LineAxis_Previous, LineAxis_Left, LineAxis_Right},
    {LineAxis_Previous, LineAxis_Next, LineAxis_Left, LineAxis_Right},
    {LineAxis_Previous, LineAxis_Next, LineAxis_Right, LineAxis_Left},
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTextEditGeometry prTextEditGeometryFromText(PrStringView text, PrTextDirection direction) {
    TextLines* lines = allocZeroed<TextLines>();
    lines->text = text;
    lines->direction = direction;
    return PrTextEditGeometry{
        .context = lines,
        .line = textLinesLine,
        .nodeText = textLinesText,
        .destroy = textLinesDestroy,
    };
}

void prTextEditGeometryReset(PrTextEditGeometry* geometry) {
    if (geometry->destroy != nullptr)
        geometry->destroy(geometry->context);
    *geometry = PrTextEditGeometry{};
}

PrTextEditMotion prTextEditMotionFromCommand(const PrTextEditGeometry* geometry, PrTextEditPosition at,
        PrTextEditCommand command, PrTextEditArrowMotion arrowMotion) {
    switch (command) {
        case PrTextEditCommand_LineStart: return PrTextEditMotion_LineStart;
        case PrTextEditCommand_LineEnd: return PrTextEditMotion_LineEnd;
        case PrTextEditCommand_TextStart: return PrTextEditMotion_TextStart;
        case PrTextEditCommand_TextEnd: return PrTextEditMotion_TextEnd;
        default: break;
    }
    if (command < PrTextEditCommand_Left || command > PrTextEditCommand_EdgeDown)
        return PrTextEditMotion_None;
    PrTextEditLine line{};
    PrTextEditFlow flow = PrTextEditFlow_HorizontalTb;
    bool rtl = false;
    if (geometry->line(geometry->context, at, PrTextEditLineStep_Same, &line)) {
        flow = line.flow;
        rtl = line.direction == PrTextDirection_Rtl;
    }
    uint32_t arrows = command - PrTextEditCommand_Left;
    LineAxis axis = kArrowAxes[flow][arrows % 4];
    if (axis == LineAxis_Previous || axis == LineAxis_Next) {
        if (arrows / 4 == 2)
            return axis == LineAxis_Previous ? PrTextEditMotion_TextStart : PrTextEditMotion_TextEnd;
        return axis == LineAxis_Previous ? PrTextEditMotion_LinePrevious : PrTextEditMotion_LineNext;
    }
    bool left = axis == LineAxis_Left;
    switch (arrows / 4) {
        case 0: {
            bool forward = left == rtl;
            switch (arrowMotion) {
                case PrTextEditArrowMotion_Visual: return left ? PrTextEditMotion_Left : PrTextEditMotion_Right;
                case PrTextEditArrowMotion_LogicalStops:
                    return forward ? PrTextEditMotion_StopAfter : PrTextEditMotion_StopBefore;
                default: return forward ? PrTextEditMotion_ClusterAfter : PrTextEditMotion_ClusterBefore;
            }
        }
        case 1: return left ? PrTextEditMotion_WordLeft : PrTextEditMotion_WordRight;
        default: return left ? PrTextEditMotion_LineLeft : PrTextEditMotion_LineRight;
    }
}

PrTextEditPosition prTextEditMotionMove(const PrTextEditGeometry* geometry, PrTextEditPosition at,
        PrTextEditMotion motion, float* goal, bool* hasGoal) {
    const PrTextEditGeometry& g = *geometry;
    PrTextEditLine line{};
    if (!g.line(g.context, at, PrTextEditLineStep_Same, &line) || line.stopCount == 0) {
        // The geometry has no line here, because the text is not shown as it stands. The text's own lines still move
        // the caret. Its node has text, so these lines always answer.
        PrTextEditGeometry lines = prTextEditGeometryFromText(g.nodeText(g.context, at.node), PrTextDirection_Ltr);
        PrTextEditPosition moved = prTextEditMotionMove(&lines, at, motion, goal, hasGoal);
        prTextEditGeometryReset(&lines);
        return moved;
    }
    bool rtl = line.direction == PrTextDirection_Rtl;
    switch (motion) {
        case PrTextEditMotion_Left: return moveVisual(g, at, false);
        case PrTextEditMotion_Right: return moveVisual(g, at, true);
        // A logical motion leaves a caret at a change of direction on the side of the text before it, wherever it came
        // from, so that a caret stands in one place for each offset.
        case PrTextEditMotion_ClusterBefore:
            return settled(g, moveLogical(g, at, false, false), PrTextEditAffinity_Upstream);
        case PrTextEditMotion_ClusterAfter:
            return settled(g, moveLogical(g, at, true, false), PrTextEditAffinity_Upstream);
        case PrTextEditMotion_StopBefore: return moveStops(g, at, false);
        case PrTextEditMotion_StopAfter: return moveStops(g, at, true);
        case PrTextEditMotion_WordLeft:
            return settled(g, moveLogical(g, at, rtl, true), PrTextEditAffinity_Upstream);
        case PrTextEditMotion_WordRight:
            return settled(g, moveLogical(g, at, !rtl, true), PrTextEditAffinity_Upstream);
        case PrTextEditMotion_WordBefore:
            return settled(g, moveLogical(g, at, false, true), PrTextEditAffinity_Upstream);
        case PrTextEditMotion_WordAfter:
            return settled(g, moveLogical(g, at, true, true), PrTextEditAffinity_Upstream);
        case PrTextEditMotion_LineLeft:
        case PrTextEditMotion_LineRight:
        case PrTextEditMotion_LineStart:
        case PrTextEditMotion_LineEnd: {
            bool end = motion == PrTextEditMotion_LineEnd || (motion == PrTextEditMotion_LineRight && !rtl) ||
                (motion == PrTextEditMotion_LineLeft && rtl);
            return line.stops[end ? line.endStop : line.startStop].position;
        }
        case PrTextEditMotion_LinePrevious: return moveBetweenLines(g, at, false, goal, hasGoal);
        case PrTextEditMotion_LineNext: return moveBetweenLines(g, at, true, goal, hasGoal);
        case PrTextEditMotion_TextStart: return textEdge(g, at, false);
        case PrTextEditMotion_TextEnd: return textEdge(g, at, true);
        default: return at;
    }
}

PrTextEditSelectionEnd prTextEditMotionCollapseEnd(PrTextEditMotion motion, PrTextDirection direction) {
    bool rtl = direction == PrTextDirection_Rtl;
    switch (motion) {
        case PrTextEditMotion_Left: return rtl ? PrTextEditSelectionEnd_High : PrTextEditSelectionEnd_Low;
        case PrTextEditMotion_Right: return rtl ? PrTextEditSelectionEnd_Low : PrTextEditSelectionEnd_High;
        case PrTextEditMotion_ClusterBefore:
        case PrTextEditMotion_StopBefore:
        case PrTextEditMotion_LinePrevious: return PrTextEditSelectionEnd_Low;
        case PrTextEditMotion_ClusterAfter:
        case PrTextEditMotion_StopAfter:
        case PrTextEditMotion_LineNext: return PrTextEditSelectionEnd_High;
        default: return PrTextEditSelectionEnd_None;
    }
}

PrTextEditSelection prTextEditSelectionMove(const PrTextEditGeometry* geometry, uint64_t node,
        PrTextEditSelection selection, PrTextEditMotion motion, PrTextEditSelectionMove kind) {
    PrTextEditPosition at{node, selection.caret, selection.affinity};
    bool extend = kind == PrTextEditSelectionMove_Extend;
    uint32_t low = selection.anchor < selection.caret ? selection.anchor : selection.caret;
    uint32_t high = selection.anchor < selection.caret ? selection.caret : selection.anchor;
    PrTextEditLine line{};
    PrTextDirection direction = PrTextDirection_Ltr;
    if (geometry->line(geometry->context, at, PrTextEditLineStep_Same, &line))
        direction = line.direction;
    PrTextEditSelectionEnd end = prTextEditMotionCollapseEnd(motion, direction);
    bool collapses = !extend && low != high && end != PrTextEditSelectionEnd_None;
    uint32_t collapsed = end == PrTextEditSelectionEnd_Low ? low : high;
    bool between = motion == PrTextEditMotion_LinePrevious || motion == PrTextEditMotion_LineNext;
    if (collapses && !between)
        return PrTextEditSelection{.anchor = collapsed, .caret = collapsed};
    if (collapses)
        at = PrTextEditPosition{node, collapsed, PrTextEditAffinity_Downstream};
    float goal = selection.goal;
    bool hasGoal = between && selection.hasGoal && !collapses;
    PrTextEditPosition moved = prTextEditMotionMove(geometry, at, motion, &goal, &hasGoal);
    return PrTextEditSelection{
        .anchor = extend ? selection.anchor : moved.offset,
        .caret = moved.offset,
        .goal = hasGoal ? goal : 0,
        .hasGoal = hasGoal,
        .affinity = moved.affinity,
    };
}
