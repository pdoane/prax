// Text editing: the gap buffer, the line index, transactions, and what they carry through a change.
//
// The text is a gap buffer, so an edit moves only the bytes between it and the last edit. The line index is a gap array
// over the same idea. A line start before the gap is an offset from the start of the text. A line start after the gap
// is a distance from the end of the text, so an edit before it does not change it.

#include "pr/text_edit.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/search.h"
#include "pr/sort.h"

namespace pr_impl {

namespace {

struct TextEditView;
struct TextEditStream;
struct TextEditHistory;

// An anchor's storage. Its value is its slot plus one.
struct AnchorSlot {
    uint32_t offset;
    PrTextEditBias bias;
    bool live;
    bool used;
};

struct TextEditBuffer {
    char* PR_NULLABLE text;
    uint32_t capacity;
    uint32_t gapStart;
    uint32_t gapEnd;
    uint64_t version;

    // The line starts, by line outside the gap. Line zero always starts at zero.
    uint32_t* PR_NULLABLE lines;
    uint32_t lineCapacity;
    uint32_t lineGapStart;
    uint32_t lineGapEnd;

    Array<AnchorSlot> anchors;
    Array<uint32_t> freeAnchors;
    Array<TextEditView*> views;
    Array<TextEditStream*> streams;
    TextEditHistory* PR_NULLABLE history;

    // The style runs, none if all the text is of style zero, and scratch memory for rebuilding them.
    Array<PrTextEditStyleRun> runs;
    Array<PrTextEditStyleRun> runsScratch;

    PrTextEditProposeFn PR_NULLABLE propose;
    void* PR_NULLABLE proposeContext;

    // The records that some stream has not read, of which the first has the sequence number `recordBase`.
    Array<PrTextEditRecord> records;
    uint64_t recordBase;

    // The new text of those records. A stored record's text holds no pointer, because the bytes can move.
    Array<char> recordBytes;
    Array<uint32_t> recordTexts;    // the offset in `recordBytes` of the text of each of `records`

    // Scratch memory of a transaction.
    Array<PrTextEditChange> changes;
    Array<int64_t> shifts;          // the change in length before each change, and in total
};

struct TextEditView {
    TextEditBuffer* PR_NULLABLE buffer;
    Array<PrTextEditSelection> selections;

    // The style that the view's next local insertion takes, if `hasTyping`.
    PrTextEditStyle typing;
    bool hasTyping;
};

struct TextEditStream {
    TextEditBuffer* PR_NULLABLE buffer;
    uint64_t next;                  // the sequence number of the next record to read
};

// A replacement that reverts a change: the bytes from `start` to `end` of the text that the change left become a range
// of its step's bytes.
struct StepChange {
    uint32_t start;
    uint32_t end;
    uint32_t textStart;
    uint32_t textSize;
};

// A style that a step restores to a range of the text that it brings back.
struct StepStyle {
    uint32_t start;
    uint32_t end;
    PrTextEditStyle style;
};

// One step of a history: how to replace the text that the step left with the text that it found, the styles to give
// that text, and the selections on each side of it. A step with no changes changes only styles.
struct HistoryStep {
    Array<StepChange> changes;
    Array<char> bytes;
    Array<StepStyle> styles;
    Array<PrTextEditSelection> before;
    Array<PrTextEditSelection> after;
    PrTextEditMerge merge;
    bool sealed;                    // nothing may join the step
};

struct TextEditHistory {
    TextEditBuffer* PR_NULLABLE buffer;
    Array<HistoryStep> undo;
    Array<HistoryStep> redo;
    bool applying;                  // the history is applying a step, and records nothing
};

uint32_t sizeOf(const TextEditBuffer* b) {
    return b->capacity - (b->gapEnd - b->gapStart);
}

uint32_t lineCountOf(const TextEditBuffer* b) {
    return b->lineCapacity - (b->lineGapEnd - b->lineGapStart);
}

uint32_t lineStartOf(const TextEditBuffer* b, uint32_t line) {
    if (line < b->lineGapStart)
        return b->lines[line];
    return sizeOf(b) - b->lines[line + (b->lineGapEnd - b->lineGapStart)];
}

// Returns the last line that starts at or before `offset`.
uint32_t lineFromOffset(const TextEditBuffer* b, uint32_t offset) {
    uint32_t after = partitionPoint(1, lineCountOf(b), [&](uint32_t line) { return lineStartOf(b, line) <= offset; });
    return after - 1;
}

PrTextEditPoint pointFromOffset(const TextEditBuffer* b, uint32_t offset) {
    uint32_t line = lineFromOffset(b, offset);
    return PrTextEditPoint{line, uint32_t(offset - lineStartOf(b, line))};
}

// Returns the contiguous bytes that start at `offset`, up to the gap or the end of the text.
PrStringView chunkAt(const TextEditBuffer* b, uint32_t offset) {
    uint32_t size = sizeOf(b);
    if (offset == size)
        return "";
    if (offset < b->gapStart)
        return PrStringView{b->text + offset, size_t(b->gapStart - offset)};
    return PrStringView{b->text + offset + (b->gapEnd - b->gapStart), size_t(size - offset)};
}

void copyBytes(const TextEditBuffer* b, uint32_t start, uint32_t end, char* PR_NULLABLE out) {
    for (uint32_t at = start; at < end;) {
        PrStringView chunk = chunkAt(b, at);
        uint32_t count = chunk.size < end - at ? uint32_t(chunk.size) : end - at;
        memCopy(out + (at - start), chunk.data, count);
        at += count;
    }
}

// Moves the gap of the text to `at`.
void moveGap(TextEditBuffer* b, uint32_t at) {
    if (at < b->gapStart) {
        uint32_t count = b->gapStart - at;
        memMove(b->text + b->gapEnd - count, b->text + at, count);
        b->gapStart -= count;
        b->gapEnd -= count;
    } else if (at > b->gapStart) {
        uint32_t count = at - b->gapStart;
        memMove(b->text + b->gapStart, b->text + b->gapEnd, count);
        b->gapStart += count;
        b->gapEnd += count;
    }
}

// Makes room for `needed` bytes in the gap, with the gap at `gapStart` as it stands.
void reserveGap(TextEditBuffer* b, uint32_t needed) {
    uint32_t gap = b->gapEnd - b->gapStart;
    if (gap >= needed)
        return;
    uint32_t size = sizeOf(b);
    uint32_t capacity = size + needed;
    capacity += capacity / 2 + 64;
    uint32_t tail = b->capacity - b->gapEnd;
    b->text = static_cast<char*>(prMemoryRealloc(b->text, capacity));
    memMove(b->text + capacity - tail, b->text + b->gapEnd, tail);
    b->gapEnd = capacity - tail;
    b->capacity = capacity;
}

// Moves the gap of the line index so that `line` is the first line after it.
void moveLineGap(TextEditBuffer* b, uint32_t line) {
    // Locals keep the fields out of the loops, since a write to a line start could otherwise alias them.
    uint32_t size = sizeOf(b);
    uint32_t* lines = b->lines;
    uint32_t start = b->lineGapStart;
    uint32_t end = b->lineGapEnd;
    while (start > line)
        lines[--end] = size - lines[--start];
    while (start < line)
        lines[start++] = size - lines[end++];
    b->lineGapStart = start;
    b->lineGapEnd = end;
}

// Makes room for `needed` more lines in the gap of the line index.
void reserveLines(TextEditBuffer* b, uint32_t needed) {
    uint32_t gap = b->lineGapEnd - b->lineGapStart;
    if (gap >= needed)
        return;
    uint32_t capacity = lineCountOf(b) + needed;
    capacity += capacity / 2 + 8;
    uint32_t tail = b->lineCapacity - b->lineGapEnd;
    b->lines = static_cast<uint32_t*>(prMemoryRealloc(b->lines, sizeof(uint32_t) * capacity));
    memMove(b->lines + capacity - tail, b->lines + b->lineGapEnd, sizeof(uint32_t) * tail);
    b->lineGapEnd = capacity - tail;
    b->lineCapacity = capacity;
}

// Replaces the bytes from `start` to `end` with `text`, in the text and in the line index.
void replaceBytes(TextEditBuffer* b, uint32_t start, uint32_t end, PrStringView text) {
    // The lines that start inside the range go. Lines after the gap are measured from the end of the text, so they stay
    // correct as the text before them changes size.
    uint32_t first = lineFromOffset(b, start);
    uint32_t last = lineFromOffset(b, end);
    moveLineGap(b, first + 1);
    b->lineGapEnd += last - first;

    moveGap(b, end);
    b->gapStart = start;
    reserveGap(b, uint32_t(text.size));
    if (text.size != 0)
        memCopy(b->text + b->gapStart, text.data, text.size);
    b->gapStart += uint32_t(text.size);

    uint32_t added = 0;
    for (size_t i = 0; i < text.size; ++i)
        added += text.data[i] == '\n' ? 1 : 0;
    if (added == 0)
        return;
    reserveLines(b, added);
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] == '\n')
            b->lines[b->lineGapStart++] = start + uint32_t(i) + 1;
    }
}

// Returns the style at `offset`, which at the end of the text is the style of its last run.
PrTextEditStyle styleAt(const TextEditBuffer* b, uint32_t offset) {
    if (b->runs.count == 0)
        return 0;
    const Array<PrTextEditStyleRun>& runs = b->runs;
    return runs.data[partitionPoint(1, runs.count, [&](uint32_t i) { return runs.data[i].start <= offset; }) - 1].style;
}

// Appends a run, unless the last run has its style. A run that would hold nothing gives way to it.
void pushRun(Array<PrTextEditStyleRun>& runs, uint32_t start, PrTextEditStyle style) {
    if (runs.count != 0 && runs.last().start == start)
        runs.pop();
    if (runs.count == 0 || runs.last().style != style)
        runs.push(PrTextEditStyleRun{start, style});
}

// Replaces the runs of the bytes from `start` to `end`, of the text as it stands, with one run of `style` that holds
// `size` bytes.
void spliceRuns(TextEditBuffer* b, uint32_t start, uint32_t end, uint32_t size, PrTextEditStyle style) {
    if (b->runs.count == 0 && style == 0)
        return;
    uint32_t total = sizeOf(b);
    PrTextEditStyle after = styleAt(b, end);
    Array<PrTextEditStyleRun>& out = b->runsScratch;
    out.clear();
    if (b->runs.count == 0 && start != 0)
        out.push(PrTextEditStyleRun{0, 0});
    uint32_t i = 0;
    for (; i < b->runs.count && b->runs.data[i].start < start; ++i)
        out.push(b->runs.data[i]);
    if (size != 0 || out.count == 0)
        pushRun(out, start, style);
    if (end < total)
        pushRun(out, start + size, after);
    for (; i < b->runs.count; ++i) {
        const PrTextEditStyleRun& run = b->runs.data[i];
        if (run.start > end)
            pushRun(out, run.start - end + start + size, run.style);
    }
    Array<PrTextEditStyleRun> kept = b->runs;
    b->runs = out;
    b->runsScratch = kept;
    if (b->runs.count == 1 && b->runs.data[0].style == 0)
        b->runs.clear();
}

// Returns the style that text replacing the bytes from `start` to `end` takes.
PrTextEditStyle insertedStyle(const TextEditBuffer* b, uint32_t start, uint32_t end) {
    return styleAt(b, end > start || start == 0 ? start : start - 1);
}

// Appends the styles of the bytes from `start` to `end`. A buffer with no runs has none to restore, since the text at
// the time of a restore has the same runs as now.
void captureStyles(const TextEditBuffer* b, uint32_t start, uint32_t end, Array<StepStyle>& out) {
    const Array<PrTextEditStyleRun>& runs = b->runs;
    for (uint32_t i = 0; i < runs.count; ++i) {
        uint32_t from = runs.data[i].start;
        uint32_t to = i + 1 < runs.count ? runs.data[i + 1].start : sizeOf(b);
        from = from > start ? from : start;
        to = to < end ? to : end;
        if (from < to)
            out.push(StepStyle{from, to, runs.data[i].style});
    }
}

// Fills `b->shifts` for `count` changes.
void findShifts(TextEditBuffer* b, const PrTextEditChange* changes, uint32_t count) {
    b->shifts.resize(count + 1);
    b->shifts.data[0] = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const PrTextEditChange& c = changes[i];
        b->shifts.data[i + 1] = b->shifts.data[i] + int64_t(c.text.size) - int64_t(c.end - c.start);
    }
}

// Returns the offset after the transaction of the start of change `i`.
uint32_t newStart(const TextEditBuffer* b, const PrTextEditChange* changes, uint32_t i) {
    return uint32_t(int64_t(changes[i].start) + b->shifts.data[i]);
}

// Moves `offset` as an anchor with `bias` moves through a transaction. Writes false to `live` if a change held the
// offset strictly inside it.
uint32_t remapAnchor(const TextEditBuffer* b, const PrTextEditChange* changes, uint32_t count, uint32_t offset,
        PrTextEditBias bias, bool* live) {
    uint32_t before = partitionPoint(0, count, [&](uint32_t i) { return changes[i].start < offset; });
    if (before != 0 && offset < changes[before - 1].end) {
        *live = false;
        return newStart(b, changes, before - 1);
    }
    if (bias == PrTextEditBias_Left)
        return uint32_t(int64_t(offset) + b->shifts.data[before]);
    // A right anchor passes every change that starts at the offset.
    uint32_t after = before;
    while (after < count && changes[after].start == offset)
        ++after;
    if (after == before)
        return uint32_t(int64_t(offset) + b->shifts.data[before]);
    return newStart(b, changes, after - 1) + uint32_t(changes[after - 1].text.size);
}

// Moves `offset`, a caret or anchor of the view that made the transaction. An offset that touches a change lands at the
// end of the change's new text.
uint32_t remapEdited(const TextEditBuffer* b, const PrTextEditChange* changes, uint32_t count, uint32_t offset, bool* touched) {
    uint32_t after = partitionPoint(0, count, [&](uint32_t i) { return changes[i].start <= offset; });
    if (after != 0 && offset <= changes[after - 1].end) {
        *touched = true;
        return newStart(b, changes, after - 1) + uint32_t(changes[after - 1].text.size);
    }
    return uint32_t(int64_t(offset) + b->shifts.data[after]);
}

uint32_t lowOf(const PrTextEditSelection& s) {
    return s.anchor < s.caret ? s.anchor : s.caret;
}

uint32_t highOf(const PrTextEditSelection& s) {
    return s.anchor < s.caret ? s.caret : s.anchor;
}

// Returns true if `next`, which starts at or after `was` starts, joins `was`. Two ranges that only touch stay apart, so
// that each keeps its own edit, and a caret that touches another selection joins it.
bool selectionsJoin(const PrTextEditSelection& was, const PrTextEditSelection& next) {
    if (lowOf(next) != highOf(was))
        return lowOf(next) < highOf(was);
    return lowOf(next) == highOf(next) || lowOf(was) == highOf(was);
}

// Sorts `selections` by position and joins those that overlap, with the primary selection last.
void normalizeSelections(Array<PrTextEditSelection>& selections) {
    if (selections.count < 2)
        return;
    PrTextEditSelection primary = selections.last();
    sortItems(selections.data, selections.count, [](const PrTextEditSelection& a, const PrTextEditSelection& b) {
        return lowOf(a) < lowOf(b);
    });
    uint32_t kept = 0;
    uint32_t primaryAt = selections.count;
    for (uint32_t i = 0; i < selections.count; ++i) {
        const PrTextEditSelection s = selections.data[i];
        bool isPrimary = s.anchor == primary.anchor && s.caret == primary.caret;
        if (kept == 0 || !selectionsJoin(selections.data[kept - 1], s)) {
            if (isPrimary)
                primaryAt = kept;
            selections.data[kept++] = s;
            continue;
        }
        PrTextEditSelection& joined = selections.data[kept - 1];
        uint32_t low = lowOf(joined);
        uint32_t high = highOf(s) > highOf(joined) ? highOf(s) : highOf(joined);
        joined.anchor = low;
        joined.caret = high;
        if (isPrimary || primaryAt == kept - 1) {
            primaryAt = kept - 1;
            joined.goal = primary.goal;
            joined.hasGoal = primary.hasGoal;
            joined.affinity = primary.affinity;
            if (primary.caret < primary.anchor) {
                joined.anchor = high;
                joined.caret = low;
            }
        }
    }
    selections.count = kept;
    PrTextEditSelection last = selections.data[primaryAt];
    selections.erase(primaryAt);
    selections.push(last);
}

// Moves the selections of a view through a transaction. `edited` is true for the view that made it.
void remapSelections(TextEditView* view, const TextEditBuffer* b, const PrTextEditChange* changes, uint32_t count, bool edited) {
    for (uint32_t i = 0; i < view->selections.count; ++i) {
        PrTextEditSelection& s = view->selections.data[i];
        if (edited) {
            bool touched = false;
            s.anchor = remapEdited(b, changes, count, s.anchor, &touched);
            s.caret = remapEdited(b, changes, count, s.caret, &touched);
            if (touched) {
                s.hasGoal = false;
                s.affinity = PrTextEditAffinity_Downstream;
            }
            continue;
        }
        // The two ends of a selection keep text inserted at either boundary outside it.
        bool live = true;
        bool ranged = s.anchor != s.caret;
        PrTextEditBias anchorBias = ranged && s.anchor < s.caret ? PrTextEditBias_Right : PrTextEditBias_Left;
        PrTextEditBias caretBias = ranged && s.caret < s.anchor ? PrTextEditBias_Right : PrTextEditBias_Left;
        s.anchor = remapAnchor(b, changes, count, s.anchor, anchorBias, &live);
        s.caret = remapAnchor(b, changes, count, s.caret, caretBias, &live);
    }
    normalizeSelections(view->selections);
}

void resetStep(HistoryStep& step) {
    step.changes.reset();
    step.bytes.reset();
    step.styles.reset();
    step.before.reset();
    step.after.reset();
}

void clearSteps(Array<HistoryStep>& steps) {
    for (uint32_t i = 0; i < steps.count; ++i)
        resetStep(steps.data[i]);
    steps.clear();
}

// Fills `step` with the changes that revert `changes`, which have not applied yet. `b->shifts` describes `changes`.
void captureInverse(const TextEditBuffer* b, const PrTextEditChange* changes, uint32_t count, HistoryStep& step) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrTextEditChange& c = changes[i];
        uint32_t start = newStart(b, changes, i);
        uint32_t at = step.bytes.count;
        uint32_t removed = uint32_t(c.end - c.start);
        step.bytes.resize(at + removed);
        copyBytes(b, c.start, c.end, step.bytes.data + at);
        step.changes.push(StepChange{.start = start, .end = start + uint32_t(c.text.size), .textStart = at, .textSize = removed});
        // The styles are in the coordinates of the text before `changes`, which is the text that reverting them
        // restores.
        captureStyles(b, c.start, c.end, step.styles);
    }
}

// Returns true if `next`, the step of the following transaction, joined the single change of `step`.
bool mergeStep(HistoryStep& step, const HistoryStep& next, PrTextEditMerge merge) {
    if (merge == PrTextEditMerge_None || step.sealed || step.merge != merge || step.changes.count != 1 ||
            next.changes.count != 1 || step.after.count != next.before.count)
        return false;
    for (uint32_t i = 0; i < step.after.count; ++i) {
        if (step.after.data[i].anchor != next.before.data[i].anchor ||
                step.after.data[i].caret != next.before.data[i].caret)
            return false;
    }
    StepChange& was = step.changes.data[0];
    const StepChange& now = next.changes.data[0];
    if (merge == PrTextEditMerge_Typing) {
        // A new insertion continues at the end of what the step inserted, and it removed nothing.
        if (now.start != was.end || now.textSize != 0)
            return false;
        was.end = now.end;
        return true;
    }
    // Each side of a deletion only restores text, and inserts nothing that the other would have to remove.
    if (now.end != now.start || was.end != was.start)
        return false;
    if (now.start == was.start) {
        // A forward deletion removed what follows what the step removed.
        for (uint32_t i = 0; i < next.styles.count; ++i) {
            const StepStyle& style = next.styles.data[i];
            step.styles.push(StepStyle{style.start + step.bytes.count, style.end + step.bytes.count, style.style});
        }
        step.bytes.append(next.bytes.data, next.bytes.count);
    } else if (now.start + now.textSize == was.start) {
        // A backward deletion removed what comes before what the step removed.
        step.styles.append(next.styles.data, next.styles.count);
        uint32_t size = step.bytes.count;
        step.bytes.resize(size + next.bytes.count);
        memMove(step.bytes.data + next.bytes.count, step.bytes.data, size);
        memCopy(step.bytes.data, next.bytes.data, next.bytes.count);
        was.start = now.start;
        was.end = now.start;
    } else {
        return false;
    }
    was.textSize = step.bytes.count;
    return true;
}

// Trims the records that every stream has read.
void trimRecords(TextEditBuffer* b) {
    if (b->streams.count == 0) {
        b->recordBase += b->records.count;
        b->records.clear();
        b->recordBytes.clear();
        b->recordTexts.clear();
        return;
    }
    uint64_t oldest = b->streams.data[0]->next;
    for (uint32_t i = 1; i < b->streams.count; ++i)
        oldest = b->streams.data[i]->next < oldest ? b->streams.data[i]->next : oldest;
    uint32_t read = uint32_t(oldest - b->recordBase);
    if (read == 0)
        return;
    uint32_t bytes = read < b->records.count ? b->recordTexts.data[read] : b->recordBytes.count;
    b->records.erase(0, read);
    b->recordTexts.erase(0, read);
    b->recordBytes.erase(0, bytes);
    for (uint32_t i = 0; i < b->recordTexts.count; ++i)
        b->recordTexts.data[i] -= bytes;
    b->recordBase = oldest;
}

// Applies a transaction through the view that makes it, if any.
bool applyTransaction(TextEditBuffer* b, TextEditView* PR_NULLABLE view, const PrTextEditChange* PR_NULLABLE changes,
        uint32_t count, PrTextEditOrigin origin, PrTextEditMerge merge) {
    b->changes.clear();
    if (count != 0)
        b->changes.append(changes, count);
    bool proposed = origin == PrTextEditOrigin_Local || origin == PrTextEditOrigin_Composition;
    if (proposed && b->propose != nullptr && !b->propose(b->proposeContext, origin, b->changes.data, count))
        return false;

    // A change that does nothing makes no version, record or step.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const PrTextEditChange& c = b->changes.data[i];
        PR_ASSERT(c.start <= c.end && c.end <= sizeOf(b));
        PR_ASSERT(i == 0 || b->changes.data[i - 1].end <= c.start);
        if (c.start != c.end || c.text.size != 0)
            b->changes.data[kept++] = c;
    }
    if (kept == 0)
        return true;
    b->changes.count = kept;
    const PrTextEditChange* list = b->changes.data;
    findShifts(b, list, kept);
    if (int64_t(sizeOf(b)) + b->shifts.data[kept] > kPrTextEditBufferMaxBytes)
        return false;

    TextEditHistory* history = b->history;
    HistoryStep step{};
    bool recording = history != nullptr && !history->applying && origin == PrTextEditOrigin_Local;
    if (recording) {
        captureInverse(b, list, kept, step);
        if (view != nullptr)
            step.before.append(view->selections.data, view->selections.count);
        step.merge = merge;
    }

    bool typing = view != nullptr && view->hasTyping && proposed;
    bool streamed = b->streams.count != 0;
    if (streamed)
        trimRecords(b);
    for (uint32_t i = kept; i-- > 0;) {
        const PrTextEditChange& c = list[i];
        PrTextEditRecord record{};
        if (streamed) {
            record.version = b->version + 1;
            record.start = c.start;
            record.oldEnd = c.end;
            record.newEnd = c.start + uint32_t(c.text.size);
            record.startPoint = pointFromOffset(b, c.start);
            record.oldEndPoint = pointFromOffset(b, c.end);
            record.origin = origin;
            record.text = PrStringView{nullptr, c.text.size};
            b->recordTexts.push(b->recordBytes.count);
            if (c.text.size != 0)
                b->recordBytes.append(c.text.data, uint32_t(c.text.size));
        }
        PrTextEditStyle style = typing ? view->typing : insertedStyle(b, c.start, c.end);
        spliceRuns(b, c.start, c.end, uint32_t(c.text.size), style);
        replaceBytes(b, c.start, c.end, c.text);
        if (streamed) {
            record.newEndPoint = pointFromOffset(b, record.newEnd);
            b->records.push(record);
        }
    }
    if (typing && origin == PrTextEditOrigin_Local)
        view->hasTyping = false;
    ++b->version;

    for (uint32_t i = 0; i < b->anchors.count; ++i) {
        AnchorSlot& a = b->anchors.data[i];
        if (a.used)
            a.offset = remapAnchor(b, list, kept, a.offset, a.bias, &a.live);
    }
    for (uint32_t i = 0; i < b->views.count; ++i)
        remapSelections(b->views.data[i], b, list, kept, b->views.data[i] == view);

    if (recording) {
        if (view != nullptr)
            step.after.append(view->selections.data, view->selections.count);
        clearSteps(history->redo);
        if (history->undo.count != 0 && mergeStep(history->undo.last(), step, merge)) {
            HistoryStep& last = history->undo.last();
            last.after.clear();
            last.after.append(step.after.data, step.after.count);
            resetStep(step);
        } else {
            history->undo.push(step);
        }
    } else if (history != nullptr && !history->applying &&
            (origin == PrTextEditOrigin_Remote || origin == PrTextEditOrigin_Program)) {
        clearSteps(history->undo);
        clearSteps(history->redo);
    }
    return true;
}

// Applies the last step of `from` and pushes the step that reverts it onto `to`. Gives the view the selections that the
// step restores.
bool applyStep(TextEditHistory* history, TextEditView* PR_NULLABLE view, Array<HistoryStep>& from, Array<HistoryStep>& to) {
    TextEditBuffer* b = history->buffer;
    if (b == nullptr || from.count == 0)
        return false;
    HistoryStep step = from.last();
    from.pop();

    Array<PrTextEditChange> changes;
    for (uint32_t i = 0; i < step.changes.count; ++i) {
        const StepChange& c = step.changes.data[i];
        changes.push(PrTextEditChange{
            .start = c.start,
            .end = c.end,
            .text = PrStringView{step.bytes.data + c.textStart, c.textSize},
        });
    }

    findShifts(b, changes.data, changes.count);
    HistoryStep inverse{};
    captureInverse(b, changes.data, changes.count, inverse);
    inverse.before.append(step.after.data, step.after.count);
    inverse.after.append(step.before.data, step.before.count);
    inverse.sealed = true;
    if (step.changes.count == 0) {
        for (uint32_t i = 0; i < step.styles.count; ++i)
            captureStyles(b, step.styles.data[i].start, step.styles.data[i].end, inverse.styles);
    }

    history->applying = true;
    applyTransaction(b, nullptr, changes.data, changes.count, PrTextEditOrigin_Undo, PrTextEditMerge_None);
    history->applying = false;
    changes.reset();
    for (uint32_t i = 0; i < step.styles.count; ++i) {
        const StepStyle& style = step.styles.data[i];
        spliceRuns(b, style.start, style.end, style.end - style.start, style.style);
    }

    if (view != nullptr && view->buffer == b && step.before.count != 0) {
        view->selections.clear();
        view->selections.append(step.before.data, step.before.count);
    }
    to.push(inverse);
    resetStep(step);
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextEditBuffer : TextEditBuffer {};
struct PrTextEditStream : TextEditStream {};
struct PrTextEditView : TextEditView {};
struct PrTextEditHistory : TextEditHistory {};

PrTextEditBuffer* prTextEditBufferCreate(PrStringView text) {
    PR_ASSERT(text.size <= kPrTextEditBufferMaxBytes);
    PrTextEditBuffer* buffer = allocZeroed<PrTextEditBuffer>();
    reserveLines(buffer, 1);
    buffer->lines[buffer->lineGapStart++] = 0;
    if (text.size != 0)
        replaceBytes(buffer, 0, 0, text);
    return buffer;
}

void prTextEditBufferDestroy(PrTextEditBuffer* PR_NULLABLE buffer) {
    if (buffer == nullptr)
        return;
    for (uint32_t i = 0; i < buffer->views.count; ++i)
        buffer->views.data[i]->buffer = nullptr;
    for (uint32_t i = 0; i < buffer->streams.count; ++i)
        buffer->streams.data[i]->buffer = nullptr;
    if (buffer->history != nullptr)
        buffer->history->buffer = nullptr;
    prMemoryFree(buffer->text);
    prMemoryFree(buffer->lines);
    buffer->anchors.reset();
    buffer->freeAnchors.reset();
    buffer->views.reset();
    buffer->streams.reset();
    buffer->runs.reset();
    buffer->runsScratch.reset();
    buffer->records.reset();
    buffer->recordBytes.reset();
    buffer->recordTexts.reset();
    buffer->changes.reset();
    buffer->shifts.reset();
    prMemoryFree(buffer);
}

uint32_t prTextEditBufferSize(const PrTextEditBuffer* buffer) {
    return sizeOf(buffer);
}

uint64_t prTextEditBufferVersion(const PrTextEditBuffer* buffer) {
    return buffer->version;
}

PrStringView prTextEditBufferChunk(const PrTextEditBuffer* buffer, uint32_t offset) {
    PR_ASSERT(offset <= sizeOf(buffer));
    return chunkAt(buffer, offset);
}

void prTextEditBufferCopy(const PrTextEditBuffer* buffer, uint32_t start, uint32_t end, char* PR_NULLABLE out) {
    PR_ASSERT(start <= end && end <= sizeOf(buffer));
    copyBytes(buffer, start, end, out);
}

PrStringView prTextEditBufferText(PrTextEditBuffer* buffer) {
    uint32_t size = sizeOf(buffer);
    if (size == 0)
        return "";
    moveGap(buffer, size);
    return PrStringView{buffer->text, size_t(size)};
}

uint32_t prTextEditBufferLineCount(const PrTextEditBuffer* buffer) {
    return lineCountOf(buffer);
}

uint32_t prTextEditBufferLineStart(const PrTextEditBuffer* buffer, uint32_t line) {
    PR_ASSERT(line < lineCountOf(buffer));
    return lineStartOf(buffer, line);
}

uint32_t prTextEditBufferLineEnd(const PrTextEditBuffer* buffer, uint32_t line) {
    PR_ASSERT(line < lineCountOf(buffer));
    return line + 1 < lineCountOf(buffer) ? lineStartOf(buffer, line + 1) - 1 : sizeOf(buffer);
}

PrTextEditPoint prTextEditBufferPointFromOffset(const PrTextEditBuffer* buffer, uint32_t offset) {
    PR_ASSERT(offset <= sizeOf(buffer));
    return pointFromOffset(buffer, offset);
}

uint32_t prTextEditBufferOffsetFromPoint(const PrTextEditBuffer* buffer, PrTextEditPoint point) {
    uint32_t offset = prTextEditBufferLineStart(buffer, point.line) + point.column;
    uint32_t end = prTextEditBufferLineEnd(buffer, point.line);
    return offset < end ? offset : end;
}

void prTextEditBufferSetPropose(PrTextEditBuffer* buffer, PrTextEditProposeFn PR_NULLABLE fn, void* PR_NULLABLE context) {
    buffer->propose = fn;
    buffer->proposeContext = context;
}

bool prTextEditBufferApply(PrTextEditBuffer* buffer, const PrTextEditChange* PR_NULLABLE changes, uint32_t count,
        PrTextEditOrigin origin) {
    return applyTransaction(buffer, nullptr, changes, count, origin, PrTextEditMerge_None);
}

void prTextEditBufferSetText(PrTextEditBuffer* buffer, PrStringView text) {
    PrTextEditChange change{.start = 0, .end = sizeOf(buffer), .text = text};
    applyTransaction(buffer, nullptr, &change, 1, PrTextEditOrigin_Program, PrTextEditMerge_None);
}

PrTextEditStream* prTextEditStreamCreate(PrTextEditBuffer* buffer) {
    PrTextEditStream* stream = allocZeroed<PrTextEditStream>();
    stream->buffer = buffer;
    stream->next = buffer->recordBase + buffer->records.count;
    buffer->streams.push(stream);
    return stream;
}

void prTextEditStreamDestroy(PrTextEditStream* PR_NULLABLE stream) {
    if (stream == nullptr)
        return;
    if (stream->buffer != nullptr) {
        stream->buffer->streams.removeFirst(stream);
        trimRecords(stream->buffer);
    }
    prMemoryFree(stream);
}

uint32_t prTextEditStreamTake(PrTextEditStream* stream, PrTextEditRecord* out, uint32_t capacity) {
    TextEditBuffer* b = stream->buffer;
    if (b == nullptr)
        return 0;
    // Trimming comes first, because it moves the text that the last take returned.
    trimRecords(b);
    uint64_t available = b->recordBase + b->records.count - stream->next;
    uint32_t count = available < capacity ? uint32_t(available) : capacity;
    uint32_t first = uint32_t(stream->next - b->recordBase);
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = b->records.data[first + i];
        out[i].text.data = b->recordBytes.data + b->recordTexts.data[first + i];
    }
    stream->next += count;
    return count;
}

PrTextEditAnchor prTextEditAnchorCreate(PrTextEditBuffer* buffer, uint32_t offset, PrTextEditBias bias) {
    PR_ASSERT(offset <= sizeOf(buffer));
    uint32_t slot = buffer->anchors.count;
    if (buffer->freeAnchors.count != 0) {
        slot = buffer->freeAnchors.last();
        buffer->freeAnchors.pop();
    } else {
        buffer->anchors.emplace();
    }
    buffer->anchors.data[slot] = AnchorSlot{.offset = offset, .bias = bias, .live = true, .used = true};
    return slot + 1;
}

void prTextEditAnchorRemove(PrTextEditBuffer* buffer, PrTextEditAnchor anchor) {
    PR_ASSERT(anchor != kPrTextEditAnchorNone && buffer->anchors.data[anchor - 1].used);
    buffer->anchors.data[anchor - 1].used = false;
    buffer->freeAnchors.push(anchor - 1);
}

uint32_t prTextEditAnchorOffset(const PrTextEditBuffer* buffer, PrTextEditAnchor anchor) {
    PR_ASSERT(anchor != kPrTextEditAnchorNone && buffer->anchors.data[anchor - 1].used);
    return buffer->anchors.data[anchor - 1].offset;
}

bool prTextEditAnchorIsLive(const PrTextEditBuffer* buffer, PrTextEditAnchor anchor) {
    PR_ASSERT(anchor != kPrTextEditAnchorNone && buffer->anchors.data[anchor - 1].used);
    return buffer->anchors.data[anchor - 1].live;
}

PrTextEditView* prTextEditViewCreate(PrTextEditBuffer* buffer) {
    PrTextEditView* view = allocZeroed<PrTextEditView>();
    view->buffer = buffer;
    view->selections.push(PrTextEditSelection{});
    buffer->views.push(view);
    return view;
}

void prTextEditViewDestroy(PrTextEditView* PR_NULLABLE view) {
    if (view == nullptr)
        return;
    if (view->buffer != nullptr)
        view->buffer->views.removeFirst(view);
    view->selections.reset();
    prMemoryFree(view);
}

PrTextEditBuffer* PR_NULLABLE prTextEditViewBuffer(const PrTextEditView* view) {
    return static_cast<PrTextEditBuffer*>(view->buffer);
}

uint32_t prTextEditViewSelectionCount(const PrTextEditView* view) {
    return view->selections.count;
}

const PrTextEditSelection* prTextEditViewSelections(const PrTextEditView* view) {
    return view->selections.data;
}

void prTextEditViewSetSelections(PrTextEditView* view, const PrTextEditSelection* selections, uint32_t count) {
    PR_ASSERT(count != 0);
    // A style waits for the next insertion only where the selections stand when it is set.
    bool moved = count != view->selections.count;
    for (uint32_t i = 0; !moved && i < count; ++i) {
        moved = selections[i].anchor != view->selections.data[i].anchor ||
            selections[i].caret != view->selections.data[i].caret;
    }
    if (moved)
        view->hasTyping = false;
    view->selections.clear();
    view->selections.append(selections, count);
    uint32_t size = view->buffer != nullptr ? sizeOf(view->buffer) : 0;
    for (uint32_t i = 0; i < count; ++i) {
        PrTextEditSelection& s = view->selections.data[i];
        s.anchor = s.anchor < size ? s.anchor : size;
        s.caret = s.caret < size ? s.caret : size;
    }
    normalizeSelections(view->selections);
}

bool prTextEditViewApply(PrTextEditView* view, const PrTextEditChange* PR_NULLABLE changes, uint32_t count,
        PrTextEditOrigin origin, PrTextEditMerge merge) {
    if (view->buffer == nullptr)
        return false;
    return applyTransaction(view->buffer, view, changes, count, origin, merge);
}

bool prTextEditViewInsert(PrTextEditView* view, PrStringView text, PrTextEditOrigin origin, PrTextEditMerge merge) {
    TextEditBuffer* b = view->buffer;
    if (b == nullptr)
        return false;
    Array<PrTextEditChange> changes;
    for (uint32_t i = 0; i < view->selections.count; ++i) {
        const PrTextEditSelection& s = view->selections.data[i];
        changes.push(PrTextEditChange{.start = lowOf(s), .end = highOf(s), .text = text});
    }
    sortItems(changes.data, changes.count, [](const PrTextEditChange& x, const PrTextEditChange& y) {
        return x.start < y.start;
    });
    bool applied = applyTransaction(b, view, changes.data, changes.count, origin, merge);
    changes.reset();
    return applied;
}

uint32_t prTextEditBufferStyleRunCount(const PrTextEditBuffer* buffer) {
    return buffer->runs.count;
}

const PrTextEditStyleRun* PR_NULLABLE prTextEditBufferStyleRuns(const PrTextEditBuffer* buffer) {
    return buffer->runs.data;
}

PrTextEditStyle prTextEditBufferStyleAt(const PrTextEditBuffer* buffer, uint32_t offset) {
    PR_ASSERT(offset <= sizeOf(buffer));
    return styleAt(buffer, offset);
}

void prTextEditBufferSetStyle(PrTextEditBuffer* buffer, uint32_t start, uint32_t end, PrTextEditStyle style) {
    PR_ASSERT(start <= end && end <= sizeOf(buffer));
    if (start != end)
        spliceRuns(buffer, start, end, end - start, style);
}

void prTextEditViewSetStyle(PrTextEditView* view, PrTextEditStyle style) {
    TextEditBuffer* b = view->buffer;
    if (b == nullptr)
        return;
    bool holds = false;
    for (uint32_t i = 0; i < view->selections.count; ++i)
        holds = holds || lowOf(view->selections.data[i]) != highOf(view->selections.data[i]);
    if (!holds) {
        view->typing = style;
        view->hasTyping = true;
        return;
    }
    TextEditHistory* history = b->history;
    HistoryStep step{};
    for (uint32_t i = 0; i < view->selections.count; ++i) {
        const PrTextEditSelection& s = view->selections.data[i];
        if (history != nullptr)
            captureStyles(b, lowOf(s), highOf(s), step.styles);
        // Text of style zero in a buffer with no runs has none to capture, and it reverts to zero.
        if (history != nullptr && b->runs.count == 0 && lowOf(s) != highOf(s))
            step.styles.push(StepStyle{lowOf(s), highOf(s), 0});
        if (lowOf(s) != highOf(s))
            spliceRuns(b, lowOf(s), highOf(s), highOf(s) - lowOf(s), style);
    }
    if (history == nullptr)
        return;
    step.before.append(view->selections.data, view->selections.count);
    step.after.append(view->selections.data, view->selections.count);
    step.sealed = true;
    clearSteps(history->redo);
    history->undo.push(step);
}

PrTextEditHistory* prTextEditHistoryCreate(PrTextEditBuffer* buffer) {
    PR_ASSERT(buffer->history == nullptr);
    PrTextEditHistory* history = allocZeroed<PrTextEditHistory>();
    history->buffer = buffer;
    buffer->history = history;
    return history;
}

void prTextEditHistoryDestroy(PrTextEditHistory* PR_NULLABLE history) {
    if (history == nullptr)
        return;
    if (history->buffer != nullptr)
        history->buffer->history = nullptr;
    clearSteps(history->undo);
    clearSteps(history->redo);
    history->undo.reset();
    history->redo.reset();
    prMemoryFree(history);
}

bool prTextEditHistoryCanUndo(const PrTextEditHistory* history) {
    return history->undo.count != 0;
}

bool prTextEditHistoryCanRedo(const PrTextEditHistory* history) {
    return history->redo.count != 0;
}

bool prTextEditHistoryUndo(PrTextEditHistory* history, PrTextEditView* PR_NULLABLE view) {
    return applyStep(history, view, history->undo, history->redo);
}

bool prTextEditHistoryRedo(PrTextEditHistory* history, PrTextEditView* PR_NULLABLE view) {
    return applyStep(history, view, history->redo, history->undo);
}

void prTextEditHistoryBreak(PrTextEditHistory* history) {
    if (history->undo.count != 0)
        history->undo.last().sealed = true;
}

void prTextEditHistoryClear(PrTextEditHistory* history) {
    clearSteps(history->undo);
    clearSteps(history->redo);
}

PrTextEditCaretBlink prTextEditCaretBlinkAt(double activity, double time) {
    // A caret shows for half a second, then hides for half a second.
    constexpr double kPartSeconds = 0.5;
    if (time < activity)
        return PrTextEditCaretBlink{.on = true, .next = activity + kPartSeconds};
    double part = floorF64((time - activity) / kPartSeconds);
    return PrTextEditCaretBlink{.on = part - 2 * floorF64(part / 2) == 0, .next = activity + (part + 1) * kPartSeconds};
}
