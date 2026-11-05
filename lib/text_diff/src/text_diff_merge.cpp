// Three-way merges (text_diff_merge.h): the base compared with each side between sync points, the two edit scripts
// walked along the base into chunks, and the conflict markers of a merge's text.

#include "pr/text_diff_merge.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/raw_array.h"

#include "text_diff/src/text_diff_sequence.h"

namespace pr_impl {

namespace {

// The texts of a merge.
enum {
    TextDiffMergeText_Base,
    TextDiffMergeText_Left,
    TextDiffMergeText_Right,
    TextDiffMergeText_Count,
};
typedef uint8_t TextDiffMergeText;

struct TextDiffMerge {
    TextDiffSequence sequence;
    RawArray<PrTextDiffMergeChunk> chunks;
    uint32_t conflicts;

    // By text: the keys of its lines.
    RawArray<uint64_t> keys[TextDiffMergeText_Count];

    // The changes of the base to the left side and to the right, of the part between two sync points.
    RawArray<PrTextDiffChange> leftChanges;
    RawArray<PrTextDiffChange> rightChanges;
};

// The texts of a merge during a call, and their keys.
struct MergeTexts {
    const PrTextDiffText* texts[TextDiffMergeText_Count];
    const RawArray<uint64_t>* keys;
};

bool sameLine(const MergeTexts& texts, TextDiffMergeText a, uint32_t lineA, TextDiffMergeText b, uint32_t lineB) {
    if (texts.keys[a].data[lineA] != texts.keys[b].data[lineB])
        return false;
    TextDiffLines lines{texts.texts[a], texts.texts[b]};
    return textDiffSameLines(&lines, lineA, lineB);
}

// Adds a chunk, joined to the last if both are unchanged, since unchanged lines align wherever they split.
void addChunk(TextDiffMerge* merge, const PrTextDiffMergeChunk& chunk) {
    if (chunk.baseCount == 0 && chunk.leftCount == 0 && chunk.rightCount == 0)
        return;
    if (chunk.kind == PrTextDiffMergeKind_Unchanged && merge->chunks.count != 0) {
        PrTextDiffMergeChunk& last = merge->chunks.last();
        if (last.kind == PrTextDiffMergeKind_Unchanged) {
            last.baseCount += chunk.baseCount;
            last.leftCount += chunk.leftCount;
            last.rightCount += chunk.rightCount;
            return;
        }
    }
    merge->chunks.push(chunk);
    merge->conflicts += chunk.kind == PrTextDiffMergeKind_Conflict ? 1 : 0;
}

// Adds a chunk that both sides changed, as one chunk that they changed alike, or as a conflict between the lines that
// begin and end both sides alike.
void addChanged(TextDiffMerge* merge, const MergeTexts& texts, const PrTextDiffMergeChunk& chunk) {
    uint32_t shorter = chunk.leftCount < chunk.rightCount ? chunk.leftCount : chunk.rightCount;
    uint32_t front = 0;
    while (front < shorter && sameLine(texts, TextDiffMergeText_Left, chunk.leftStart + front, TextDiffMergeText_Right,
            chunk.rightStart + front))
        ++front;
    if (front == chunk.leftCount && front == chunk.rightCount) {
        PrTextDiffMergeChunk alike = chunk;
        alike.kind = PrTextDiffMergeKind_Both;
        addChunk(merge, alike);
        return;
    }
    uint32_t back = 0;
    while (front + back < shorter && sameLine(texts, TextDiffMergeText_Left, chunk.leftStart + chunk.leftCount - 1 - back,
            TextDiffMergeText_Right, chunk.rightStart + chunk.rightCount - 1 - back))
        ++back;
    addChunk(merge, PrTextDiffMergeChunk{
        .baseStart = chunk.baseStart,
        .leftStart = chunk.leftStart,
        .leftCount = front,
        .rightStart = chunk.rightStart,
        .rightCount = front,
        .kind = PrTextDiffMergeKind_Both,
    });
    addChunk(merge, PrTextDiffMergeChunk{
        .baseStart = chunk.baseStart,
        .baseCount = chunk.baseCount,
        .leftStart = chunk.leftStart + front,
        .leftCount = chunk.leftCount - front - back,
        .rightStart = chunk.rightStart + front,
        .rightCount = chunk.rightCount - front - back,
        .kind = PrTextDiffMergeKind_Conflict,
    });
    addChunk(merge, PrTextDiffMergeChunk{
        .baseStart = chunk.baseStart + chunk.baseCount,
        .leftStart = chunk.leftStart + chunk.leftCount - back,
        .leftCount = back,
        .rightStart = chunk.rightStart + chunk.rightCount - back,
        .rightCount = back,
        .kind = PrTextDiffMergeKind_Both,
    });
}

// Where a side stands in the walk along the base: its next change, and how far its lines are past the base's.
struct SideWalk {
    const RawArray<PrTextDiffChange>* changes;
    uint32_t next;
    int64_t shift;
};

uint32_t changeEnd(const PrTextDiffChange& change) {
    return change.oldStart + change.oldCount;
}

// The lines of a side that a chunk of the base from `start` to `end` covers, given the side's changes from `first` to
// `last` that the chunk holds, and moves the side's shift past the chunk.
void sideRange(SideWalk& side, uint32_t first, uint32_t last, uint32_t start, uint32_t end, uint32_t* outStart,
        uint32_t* outCount) {
    int64_t from = int64_t(start) + side.shift;
    int64_t to = int64_t(end) + side.shift;
    if (first != last) {
        const PrTextDiffChange& head = side.changes->data[first];
        const PrTextDiffChange& tail = side.changes->data[last - 1];
        from = int64_t(head.newStart) - int64_t(head.oldStart - start);
        to = int64_t(tail.newStart + tail.newCount) + int64_t(end - changeEnd(tail));
    }
    side.shift = to - int64_t(end);
    *outStart = uint32_t(from);
    *outCount = uint32_t(to - from);
}

// Merges the part of the texts from the lines of `start` to those of `end`.
void mergePart(TextDiffMerge* merge, const MergeTexts& texts, PrTextDiffMergeSync start, PrTextDiffMergeSync end) {
    TextDiffLines leftLines{texts.texts[TextDiffMergeText_Base], texts.texts[TextDiffMergeText_Left]};
    TextDiffLines rightLines{texts.texts[TextDiffMergeText_Base], texts.texts[TextDiffMergeText_Right]};
    TextDiffSequenceDesc leftDesc{
        .oldKeys = texts.keys[TextDiffMergeText_Base].data,
        .oldCount = texts.keys[TextDiffMergeText_Base].count,
        .newKeys = texts.keys[TextDiffMergeText_Left].data,
        .newCount = texts.keys[TextDiffMergeText_Left].count,
        .equal = textDiffSameLines,
        .equalContext = &leftLines,
    };
    TextDiffSequenceDesc rightDesc{
        .oldKeys = texts.keys[TextDiffMergeText_Base].data,
        .oldCount = texts.keys[TextDiffMergeText_Base].count,
        .newKeys = texts.keys[TextDiffMergeText_Right].data,
        .newCount = texts.keys[TextDiffMergeText_Right].count,
        .equal = textDiffSameLines,
        .equalContext = &rightLines,
    };
    merge->leftChanges.clear();
    merge->rightChanges.clear();
    textDiffSequenceCompare(&merge->sequence, leftDesc,
        TextDiffPart{start.baseLine, end.baseLine, start.leftLine, end.leftLine}, merge->leftChanges);
    textDiffSequenceCompare(&merge->sequence, rightDesc,
        TextDiffPart{start.baseLine, end.baseLine, start.rightLine, end.rightLine}, merge->rightChanges);

    SideWalk left{&merge->leftChanges, 0, int64_t(start.leftLine) - int64_t(start.baseLine)};
    SideWalk right{&merge->rightChanges, 0, int64_t(start.rightLine) - int64_t(start.baseLine)};
    uint32_t at = start.baseLine;
    while (left.next < left.changes->count || right.next < right.changes->count) {
        // The chunk starts at the side's change that starts first, and takes every change of either side that starts
        // before it ends or where it ends.
        uint32_t chunkStart = UINT32_MAX;
        if (left.next < left.changes->count)
            chunkStart = left.changes->data[left.next].oldStart;
        if (right.next < right.changes->count && right.changes->data[right.next].oldStart < chunkStart)
            chunkStart = right.changes->data[right.next].oldStart;
        if (chunkStart > at) {
            uint32_t count = chunkStart - at;
            addChunk(merge, PrTextDiffMergeChunk{
                .baseStart = at,
                .baseCount = count,
                .leftStart = uint32_t(int64_t(at) + left.shift),
                .leftCount = count,
                .rightStart = uint32_t(int64_t(at) + right.shift),
                .rightCount = count,
                .kind = PrTextDiffMergeKind_Unchanged,
            });
        }
        uint32_t chunkEnd = chunkStart;
        uint32_t leftFirst = left.next;
        uint32_t rightFirst = right.next;
        for (bool took = true; took;) {
            took = false;
            if (left.next < left.changes->count && left.changes->data[left.next].oldStart <= chunkEnd) {
                uint32_t reach = changeEnd(left.changes->data[left.next++]);
                chunkEnd = reach > chunkEnd ? reach : chunkEnd;
                took = true;
            }
            if (right.next < right.changes->count && right.changes->data[right.next].oldStart <= chunkEnd) {
                uint32_t reach = changeEnd(right.changes->data[right.next++]);
                chunkEnd = reach > chunkEnd ? reach : chunkEnd;
                took = true;
            }
        }
        PrTextDiffMergeChunk chunk{.baseStart = chunkStart, .baseCount = chunkEnd - chunkStart};
        sideRange(left, leftFirst, left.next, chunkStart, chunkEnd, &chunk.leftStart, &chunk.leftCount);
        sideRange(right, rightFirst, right.next, chunkStart, chunkEnd, &chunk.rightStart, &chunk.rightCount);
        bool leftChanged = left.next != leftFirst;
        bool rightChanged = right.next != rightFirst;
        if (leftChanged && rightChanged) {
            addChanged(merge, texts, chunk);
        } else {
            chunk.kind = leftChanged ? PrTextDiffMergeKind_Left : PrTextDiffMergeKind_Right;
            addChunk(merge, chunk);
        }
        at = chunkEnd;
    }
    uint32_t count = end.baseLine - at;
    addChunk(merge, PrTextDiffMergeChunk{
        .baseStart = at,
        .baseCount = count,
        .leftStart = uint32_t(int64_t(at) + left.shift),
        .leftCount = count,
        .rightStart = uint32_t(int64_t(at) + right.shift),
        .rightCount = count,
        .kind = PrTextDiffMergeKind_Unchanged,
    });
}

// Adds the chunk of a sync point's lines, by which of them differ from the base.
void addSync(TextDiffMerge* merge, const MergeTexts& texts, PrTextDiffMergeSync sync) {
    bool leftKept = sameLine(texts, TextDiffMergeText_Base, sync.baseLine, TextDiffMergeText_Left, sync.leftLine);
    bool rightKept = sameLine(texts, TextDiffMergeText_Base, sync.baseLine, TextDiffMergeText_Right, sync.rightLine);
    PrTextDiffMergeKind kind = PrTextDiffMergeKind_Unchanged;
    if (leftKept && !rightKept)
        kind = PrTextDiffMergeKind_Right;
    else if (!leftKept && rightKept)
        kind = PrTextDiffMergeKind_Left;
    else if (!leftKept &&
            sameLine(texts, TextDiffMergeText_Left, sync.leftLine, TextDiffMergeText_Right, sync.rightLine))
        kind = PrTextDiffMergeKind_Both;
    else if (!leftKept)
        kind = PrTextDiffMergeKind_Conflict;
    PrTextDiffMergeChunk chunk{
        .baseStart = sync.baseLine,
        .baseCount = 1,
        .leftStart = sync.leftLine,
        .leftCount = 1,
        .rightStart = sync.rightLine,
        .rightCount = 1,
        .kind = kind,
    };
    // A changed sync point stays a chunk of its own, so that its lines pair.
    if (kind == PrTextDiffMergeKind_Unchanged) {
        addChunk(merge, chunk);
    } else {
        merge->chunks.push(chunk);
        merge->conflicts += kind == PrTextDiffMergeKind_Conflict ? 1 : 0;
    }
}

// Appends the marker `marker`, then a space and `label` unless it is empty, as a line of `out`.
void appendMarker(PrRawString* out, PrStringView marker, PrStringView label) {
    prRawStringAppend(out, marker);
    if (label.size != 0) {
        prRawStringAppend(out, " ");
        prRawStringAppend(out, label);
    }
}

// Appends lines of `text` to `out`, each after a line feed unless `out` is still empty of lines.
void appendLines(PrRawString* out, bool* any, const PrTextDiffText* text, uint32_t start, uint32_t count) {
    for (uint32_t line = start; line < start + count; ++line) {
        if (*any)
            prRawStringAppend(out, "\n");
        prRawStringAppend(out, text->line(text->context, line));
        *any = true;
    }
}

void appendMarkerLine(PrRawString* out, bool* any, PrStringView marker, PrStringView label) {
    if (*any)
        prRawStringAppend(out, "\n");
    appendMarker(out, marker, label);
    *any = true;
}

// Returns true if `line` is a marker of seven `marker` characters, alone or before a space.
bool isMarker(PrStringView line, char marker) {
    if (line.size < 7 || (line.size > 7 && line.data[7] != ' '))
        return false;
    for (uint32_t i = 0; i < 7; ++i) {
        if (line.data[i] != marker)
            return false;
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextDiffMerge : TextDiffMerge {};

PrTextDiffMerge* prTextDiffMergeCreate(const PrTextDiffOptions* PR_NULLABLE options) {
    PrTextDiffMerge* merge = allocZeroed<PrTextDiffMerge>();
    PrTextDiffOptions chosen = options != nullptr ? *options : prTextDiffOptionsDefault();
    merge->sequence.maxCost = chosen.maxCost != 0 ? chosen.maxCost : 1;
    return merge;
}

void prTextDiffMergeDestroy(PrTextDiffMerge* PR_NULLABLE merge) {
    if (merge == nullptr)
        return;
    textDiffSequenceReset(&merge->sequence);
    merge->chunks.reset();
    for (RawArray<uint64_t>& keys : merge->keys)
        keys.reset();
    merge->leftChanges.reset();
    merge->rightChanges.reset();
    prMemoryFree(merge);
}

void prTextDiffMergeCompare(PrTextDiffMerge* merge, const PrTextDiffText* base, const PrTextDiffText* left,
        const PrTextDiffText* right, const PrTextDiffMergeSync* PR_NULLABLE syncs, uint32_t syncCount) {
    MergeTexts texts{{base, left, right}, merge->keys};
    for (uint32_t i = 0; i < TextDiffMergeText_Count; ++i)
        textDiffLineKeys(texts.texts[i], merge->keys[i]);
    merge->chunks.clear();
    merge->conflicts = 0;
    PrTextDiffMergeSync at{};
    for (uint32_t i = 0; i <= syncCount; ++i) {
        PrTextDiffMergeSync sync{base->lineCount, left->lineCount, right->lineCount};
        if (i < syncCount) {
            sync = syncs[i];
            PR_ASSERT(sync.baseLine >= at.baseLine && sync.baseLine < base->lineCount);
            PR_ASSERT(sync.leftLine >= at.leftLine && sync.leftLine < left->lineCount);
            PR_ASSERT(sync.rightLine >= at.rightLine && sync.rightLine < right->lineCount);
        }
        mergePart(merge, texts, at, sync);
        if (i == syncCount)
            break;
        addSync(merge, texts, sync);
        at = PrTextDiffMergeSync{sync.baseLine + 1, sync.leftLine + 1, sync.rightLine + 1};
    }
}

uint32_t prTextDiffMergeChunkCount(const PrTextDiffMerge* merge) {
    return merge->chunks.count;
}

const PrTextDiffMergeChunk* PR_NULLABLE prTextDiffMergeChunks(const PrTextDiffMerge* merge) {
    return merge->chunks.data;
}

uint32_t prTextDiffMergeConflictCount(const PrTextDiffMerge* merge) {
    return merge->conflicts;
}

PrTextDiffMarkerOptions prTextDiffMarkerOptionsDefault(void) {
    return PrTextDiffMarkerOptions{
        .style = PrTextDiffMarkerStyle_Diff3,
        .leftLabel = "",
        .baseLabel = "",
        .rightLabel = "",
    };
}

PrRawString prTextDiffMergeText(const PrTextDiffMerge* merge, const PrTextDiffText* base, const PrTextDiffText* left,
        const PrTextDiffText* right, const PrTextDiffMarkerOptions* PR_NULLABLE options) {
    PrTextDiffMarkerOptions chosen = options != nullptr ? *options : prTextDiffMarkerOptionsDefault();
    PrRawString out = prRawStringEmpty();
    bool any = false;
    for (uint32_t i = 0; i < merge->chunks.count; ++i) {
        const PrTextDiffMergeChunk& chunk = merge->chunks.data[i];
        switch (chunk.kind) {
            case PrTextDiffMergeKind_Unchanged:
            case PrTextDiffMergeKind_Left:
            case PrTextDiffMergeKind_Both:
                appendLines(&out, &any, left, chunk.leftStart, chunk.leftCount);
                break;
            case PrTextDiffMergeKind_Right:
                appendLines(&out, &any, right, chunk.rightStart, chunk.rightCount);
                break;
            case PrTextDiffMergeKind_Conflict:
                appendMarkerLine(&out, &any, "<<<<<<<", chosen.leftLabel);
                appendLines(&out, &any, left, chunk.leftStart, chunk.leftCount);
                if (chosen.style == PrTextDiffMarkerStyle_Diff3) {
                    appendMarkerLine(&out, &any, "|||||||", chosen.baseLabel);
                    appendLines(&out, &any, base, chunk.baseStart, chunk.baseCount);
                }
                appendMarkerLine(&out, &any, "=======", "");
                appendLines(&out, &any, right, chunk.rightStart, chunk.rightCount);
                appendMarkerLine(&out, &any, ">>>>>>>", chosen.rightLabel);
                break;
        }
    }
    return out;
}

uint32_t prTextDiffConflicts(const PrTextDiffText* text, PrTextDiffConflict* PR_NULLABLE out, uint32_t capacity) {
    // Where the conflict that is open stands: the section whose lines follow, if a conflict is open.
    enum { Outside, InLeft, InBase, InRight } state = Outside;
    PrTextDiffConflict open{};
    uint32_t count = 0;
    for (uint32_t line = 0; line < text->lineCount; ++line) {
        PrStringView bytes = text->line(text->context, line);
        // An opening marker begins a conflict anywhere, and leaves one that did not close as text.
        if (isMarker(bytes, '<')) {
            open = PrTextDiffConflict{.start = line, .leftStart = line + 1};
            state = InLeft;
            continue;
        }
        switch (state) {
            case Outside:
                break;
            case InLeft:
                if (isMarker(bytes, '|')) {
                    open.leftCount = line - open.leftStart;
                    open.baseStart = line + 1;
                    open.hasBase = true;
                    state = InBase;
                } else if (isMarker(bytes, '=')) {
                    open.leftCount = line - open.leftStart;
                    open.baseStart = line;
                    open.rightStart = line + 1;
                    state = InRight;
                }
                break;
            case InBase:
                if (isMarker(bytes, '=')) {
                    open.baseCount = line - open.baseStart;
                    open.rightStart = line + 1;
                    state = InRight;
                }
                break;
            case InRight:
                if (isMarker(bytes, '>')) {
                    open.rightCount = line - open.rightStart;
                    open.end = line + 1;
                    if (count < capacity)
                        out[count] = open;
                    ++count;
                    state = Outside;
                }
                break;
        }
    }
    return count;
}
