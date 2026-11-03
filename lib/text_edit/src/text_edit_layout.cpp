#include "pr/text_edit_layout.h"

#include "pr/alloc.h"
#include "pr/array.h"

namespace pr_impl {

namespace {

// The geometry of what a layout tree shows.
struct LayoutLines {
    PrLayoutTree* tree;
    PrLayoutNode root;
    bool flat;                      // the root is one text node that shows `text`
    PrStringView text;
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

// Returns true if the node shows `text`, perhaps with more after it.
bool showsText(const LayoutLines& lines) {
    PrStringView shown = prLayoutNodeText(lines.tree, lines.root);
    if (shown.size < lines.text.size)
        return false;
    return PrStringView{shown.data, lines.text.size} == lines.text;
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
    prLayoutNodeLineInfo(lines.tree, lines.root, at.offset, &info);
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
        // What a node shows after the flat text is not a place for a caret, so the text's last stop ends the line.
        if (lines.flat && stop.position.offset > lines.text.size)
            continue;
        if (i == info.startStop)
            startStop = lines.stops.count;
        if (i == info.endStop || (lines.flat && stop.position.offset == lines.text.size))
            endStop = lines.stops.count;
        lines.stops.push(PrTextEditLineStop{{stop.position.node, stop.position.offset, stop.affinity}, stop.along});
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
    return geometryOf(lines);
}
