#include "pr/text_edit_layout.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"

namespace pr_impl {

namespace {

// The geometry of what a layout tree shows.
struct LayoutLines {
    PrLayoutTree* tree;
    PrLayoutNode root;
    bool flat;                      // text nodes show `text`, and offsets count in it
    PrStringView text;
    const PrTextEditLayoutRun* PR_NULLABLE runs;    // the nodes of flat text, or `ownRun`
    uint32_t runCount;
    PrTextEditLayoutRun ownRun;
    Array<PrLayoutLineStop> found;
    Array<PrTextEditLineStop> stops;

    // The answer to the last question, which motion asks several times for each press. The text does not change while a
    // geometry is in use.
    bool answered;
    PrTextEditPosition answeredAt;
    PrTextEditLineStep answeredStep;
    PrTextEditLine answer;

    // The text's own lines, for text that the last layout does not show as it stands.
    bool hasOwnLines;
    PrTextEditGeometry ownLines;
};

// Returns true if the nodes show `text`, perhaps with more after it in the last.
bool showsText(const LayoutLines& lines) {
    for (uint32_t i = 0; i < lines.runCount; ++i) {
        uint32_t start = lines.runs[i].start;
        bool last = i + 1 == lines.runCount;
        uint32_t end = last ? uint32_t(lines.text.size) : lines.runs[i + 1].start;
        PrStringView shown = prLayoutNodeText(lines.tree, lines.runs[i].node);
        PrStringView part{lines.text.data + start, end - start};
        if (shown.size < part.size || (!last && shown.size != part.size))
            return false;
        if (PrStringView{shown.data, part.size} != part)
            return false;
    }
    return true;
}

// Returns the run that holds `at`. An offset where two runs meet is in the second, or in the first if it stands
// upstream.
uint32_t runHolding(const LayoutLines& lines, PrTextEditPosition at) {
    uint32_t run = 0;
    while (run + 1 < lines.runCount && lines.runs[run + 1].start <= at.offset)
        ++run;
    if (at.affinity == PrTextEditAffinity_Upstream && run > 0 && lines.runs[run].start == at.offset)
        --run;
    return run;
}

PrTextEditFlow flowFromWritingMode(PrLayoutWritingMode mode) {
    switch (mode) {
        case PrLayoutWritingMode_VerticalRl:
        case PrLayoutWritingMode_SidewaysRl: return PrTextEditFlow_VerticalRl;
        case PrLayoutWritingMode_VerticalLr: return PrTextEditFlow_VerticalLr;
        case PrLayoutWritingMode_SidewaysLr: return PrTextEditFlow_SidewaysLr;
        default: return PrTextEditFlow_HorizontalTb;
    }
}

// Answers from the text's own lines, in the direction and writing mode that its node's style gives them.
bool answerFromText(LayoutLines& lines, PrTextEditPosition at, PrTextEditLineStep step, PrTextEditLine* out) {
    PrLayoutLineInfo info{};
    uint32_t run = runHolding(lines, at);
    prLayoutNodeLineInfo(lines.tree, lines.runs[run].node, at.offset - lines.runs[run].start, &info);
    if (!lines.hasOwnLines) {
        lines.ownLines = prTextEditGeometryFromText(lines.text, info.direction);
        lines.hasOwnLines = true;
    }
    if (!lines.ownLines.line(lines.ownLines.context, at, step, out))
        return false;
    out->flow = flowFromWritingMode(info.writingMode);
    return true;
}

bool answerFromLayout(LayoutLines& lines, PrTextEditPosition at, PrTextEditLineStep step, PrTextEditLine* out) {
    PrLayoutTextPosition position{at.node, at.offset};
    if (lines.flat) {
        uint32_t run = runHolding(lines, at);
        position = PrLayoutTextPosition{lines.runs[run].node, at.offset - lines.runs[run].start};
    }
    PrLayoutLineInfo info{};
    PrLayoutLineStep layoutStep = PrLayoutLineStep_Same;
    if (step == PrTextEditLineStep_Next)
        layoutStep = PrLayoutLineStep_Next;
    else if (step == PrTextEditLineStep_Previous)
        layoutStep = PrLayoutLineStep_Previous;
    uint32_t capacity = lines.found.count;
    uint32_t count = prLayoutLineStops(lines.tree, lines.root, position, at.affinity, layoutStep, &info,
        lines.found.data, capacity);
    if (count == 0)
        return false;
    if (count > capacity) {
        lines.found.resize(count);
        prLayoutLineStops(lines.tree, lines.root, position, at.affinity, layoutStep, &info, lines.found.data, count);
    }
    lines.stops.clear();
    uint32_t startStop = 0;
    uint32_t endStop = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const PrLayoutLineStop& stop = lines.found.data[i];
        PrTextEditPosition place{stop.position.node, stop.position.offset, stop.affinity};
        if (lines.flat) {
            uint32_t run = 0;
            while (run < lines.runCount && lines.runs[run].node != stop.position.node)
                ++run;
            if (run == lines.runCount)
                continue;
            place.node = at.node;
            place.offset += lines.runs[run].start;
            // What a node shows after the flat text is not a place for a caret, so the text's last stop ends the line.
            if (place.offset > lines.text.size)
                continue;
        }
        // Where two runs meet, each has a stop at one place, which is one place for a caret, with the text after it.
        bool repeats = lines.stops.count != 0 && lines.stops.last().position.offset == place.offset &&
            lines.stops.last().position.node == place.node && lines.stops.last().along == stop.along;
        if (repeats) {
            lines.stops.last().position.affinity = PrTextEditAffinity_Downstream;
        } else {
            lines.stops.push(PrTextEditLineStop{place, stop.along});
        }
        uint32_t index = lines.stops.count - 1;
        if (i == info.startStop)
            startStop = index;
        if (i == info.endStop || (lines.flat && place.offset == lines.text.size))
            endStop = index;
    }
    if (lines.stops.count == 0)
        return false;
    *out = PrTextEditLine{
        .stops = lines.stops.data,
        .stopCount = lines.stops.count,
        .flow = flowFromWritingMode(info.writingMode),
        .direction = info.direction,
        .startStop = startStop,
        .endStop = endStop,
    };
    return true;
}

bool layoutLinesLine(void* PR_NULLABLE context, PrTextEditPosition at, PrTextEditLineStep step, PrTextEditLine* out) {
    LayoutLines& lines = *static_cast<LayoutLines*>(context);
    if (lines.answered && lines.answeredStep == step && lines.answeredAt.node == at.node &&
            lines.answeredAt.offset == at.offset && lines.answeredAt.affinity == at.affinity) {
        *out = lines.answer;
        return true;
    }
    lines.answered = false;
    bool found = false;
    if (lines.flat && !showsText(lines)) {
        found = answerFromText(lines, at, step, out);
    } else {
        found = answerFromLayout(lines, at, step, out);
        // A node whose layout is out of date has no line, not even the one that the position is on.
        if (!found && lines.flat && !answerFromLayout(lines, at, PrTextEditLineStep_Same, out))
            found = answerFromText(lines, at, step, out);
    }
    if (found) {
        lines.answered = true;
        lines.answeredAt = at;
        lines.answeredStep = step;
        lines.answer = *out;
    }
    return found;
}

PrStringView layoutLinesText(void* PR_NULLABLE context, uint64_t node) {
    LayoutLines& lines = *static_cast<LayoutLines*>(context);
    return lines.flat ? lines.text : prLayoutNodeText(lines.tree, node);
}

uint64_t layoutLinesNodeStep(void* PR_NULLABLE context, uint64_t node, PrTextEditLineStep step, bool* contiguous) {
    LayoutLines& lines = *static_cast<LayoutLines*>(context);
    *contiguous = false;
    if (lines.flat)
        return 0;
    if (step == PrTextEditLineStep_Next)
        return prLayoutNodeTextNext(lines.tree, lines.root, node, contiguous);
    return prLayoutNodeTextPrevious(lines.tree, lines.root, node, contiguous);
}

void layoutLinesDestroy(void* PR_NULLABLE context) {
    LayoutLines* lines = static_cast<LayoutLines*>(context);
    if (lines->hasOwnLines)
        prTextEditGeometryReset(&lines->ownLines);
    lines->found.reset();
    lines->stops.reset();
    prMemoryFree(lines);
}

PrTextEditGeometry geometryOf(LayoutLines* lines) {
    return PrTextEditGeometry{
        .context = lines,
        .line = layoutLinesLine,
        .nodeText = layoutLinesText,
        .nodeStep = layoutLinesNodeStep,
        .destroy = layoutLinesDestroy,
    };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTextEditGeometry prTextEditGeometryFromLayout(PrLayoutTree* tree, PrLayoutNode root) {
    LayoutLines* lines = allocZeroed<LayoutLines>();
    lines->tree = tree;
    lines->root = root;
    return geometryOf(lines);
}

PrTextEditGeometry prTextEditGeometryFromLayoutText(PrLayoutTree* tree, PrLayoutNode node, PrStringView text) {
    LayoutLines* lines = allocZeroed<LayoutLines>();
    lines->tree = tree;
    lines->root = node;
    lines->flat = true;
    lines->text = text;
    lines->ownRun = PrTextEditLayoutRun{node, 0};
    lines->runs = &lines->ownRun;
    lines->runCount = 1;
    return geometryOf(lines);
}

PrTextEditGeometry prTextEditGeometryFromLayoutRuns(PrLayoutTree* tree, PrLayoutNode root,
        const PrTextEditLayoutRun* runs, uint32_t count, PrStringView text) {
    PR_ASSERT(count != 0);
    LayoutLines* lines = allocZeroed<LayoutLines>();
    lines->tree = tree;
    lines->root = root;
    lines->flat = true;
    lines->text = text;
    lines->runs = runs;
    lines->runCount = count;
    return geometryOf(lines);
}
