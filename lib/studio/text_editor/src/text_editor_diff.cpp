// The model of a comparison (text_editor_diff.h): the buffers' lines given to text diffs and to a merge, and their
// changes turned into blocks, and into what each buffer shows.

#include "studio/text_editor/src/text_editor_diff.h"

#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/search.h"

namespace pr_impl {

namespace {

// Copies line `index` of the buffer, which a comparison reads until it asks for the buffer's next line.
PrStringView lineOf(void* PR_NULLABLE context, uint32_t index) {
    TextEditorDiffShown* shown = static_cast<TextEditorDiffShown*>(context);
    uint32_t start = prTextEditBufferLineStart(shown->buffer, index);
    uint32_t end = prTextEditBufferLineEnd(shown->buffer, index);
    shown->line.resize(end - start);
    prTextEditBufferCopy(shown->buffer, start, end, shown->line.data);
    return PrStringView{shown->line.data, shown->line.count};
}

uint32_t changeEnd(const PrTextDiffChange& change) {
    return change.oldStart + change.oldCount;
}

// Where a buffer stands in the walk along the hub: its next change, and how far its lines are past the hub's.
struct BufferWalk {
    const PrTextDiffChange* changes;
    uint32_t count;
    uint32_t next;
    int64_t shift;
};

// Returns true if the buffer's next change ends in the hub at or before `limit`.
bool nextWithin(const BufferWalk& walk, uint32_t limit) {
    return walk.next < walk.count && changeEnd(walk.changes[walk.next]) <= limit;
}

// Groups the changes of every buffer past the hub that end in the hub at or before `limit` into blocks. Changes that
// overlap or meet in the hub are one block.
void groupBlocks(TextEditorDiffModel* model, BufferWalk* walks, uint32_t limit) {
    for (;;) {
        uint32_t blockStart = UINT32_MAX;
        for (uint32_t i = 1; i < model->count; ++i) {
            const BufferWalk& walk = walks[i];
            if (nextWithin(walk, limit) && walk.changes[walk.next].oldStart < blockStart)
                blockStart = walk.changes[walk.next].oldStart;
        }
        if (blockStart == UINT32_MAX)
            return;
        uint32_t blockEnd = blockStart;
        uint32_t firsts[kTextEditorDiffMaxBuffers] = {};
        for (uint32_t i = 1; i < model->count; ++i)
            firsts[i] = walks[i].next;
        for (bool took = true; took;) {
            took = false;
            for (uint32_t i = 1; i < model->count; ++i) {
                BufferWalk& walk = walks[i];
                if (nextWithin(walk, limit) && walk.changes[walk.next].oldStart <= blockEnd) {
                    uint32_t reach = changeEnd(walk.changes[walk.next++]);
                    blockEnd = reach > blockEnd ? reach : blockEnd;
                    took = true;
                }
            }
        }
        TextEditorDiffBlock block{};
        block.starts[0] = blockStart;
        block.counts[0] = blockEnd - blockStart;
        for (uint32_t i = 1; i < model->count; ++i) {
            BufferWalk& walk = walks[i];
            int64_t from = int64_t(blockStart) + walk.shift;
            int64_t to = int64_t(blockEnd) + walk.shift;
            if (walk.next != firsts[i]) {
                const PrTextDiffChange& head = walk.changes[firsts[i]];
                const PrTextDiffChange& tail = walk.changes[walk.next - 1];
                from = int64_t(head.newStart) - int64_t(head.oldStart - blockStart);
                to = int64_t(tail.newStart + tail.newCount) + int64_t(blockEnd - changeEnd(tail));
            }
            walk.shift = to - int64_t(blockEnd);
            block.starts[i] = uint32_t(from);
            block.counts[i] = uint32_t(to - from);
        }
        model->blocks.push(block);
    }
}

// Turns the changes of the comparisons with the hub into blocks, which never span a sync point: the changes between two
// sync points group apart, and a sync point whose lines differ is a block of its own.
void buildBlocks(TextEditorDiffModel* model, const uint32_t* PR_NULLABLE syncs, uint32_t syncCount) {
    model->blocks.clear();
    BufferWalk walks[kTextEditorDiffMaxBuffers] = {};
    for (uint32_t i = 1; i < model->count; ++i) {
        walks[i].changes = prTextDiffChanges(model->diffs[i - 1]);
        walks[i].count = prTextDiffChangeCount(model->diffs[i - 1]);
    }
    for (uint32_t k = 0; k <= syncCount; ++k) {
        if (k == syncCount) {
            groupBlocks(model, walks, UINT32_MAX);
            break;
        }
        // A change that ends at the sync point's line, as one that adds lines before it does, belongs before it.
        const uint32_t* sync = syncs + k * model->count;
        groupBlocks(model, walks, sync[0]);
        bool changed = false;
        for (uint32_t i = 1; i < model->count; ++i) {
            BufferWalk& walk = walks[i];
            if (walk.next < walk.count && walk.changes[walk.next].oldStart == sync[0] &&
                    walk.changes[walk.next].oldCount == 1 && walk.changes[walk.next].newStart == sync[i]) {
                ++walk.next;
                changed = true;
            }
            walk.shift = int64_t(sync[i]) - int64_t(sync[0]);
        }
        if (changed) {
            TextEditorDiffBlock block{};
            for (uint32_t i = 0; i < model->count; ++i) {
                block.starts[i] = sync[i];
                block.counts[i] = 1;
            }
            model->blocks.push(block);
        }
    }
}

void addRun(TextEditorDiffShown& shown, uint32_t start, uint32_t count, PrColor16 color) {
    if (count != 0)
        shown.runs.push(TextEditorDiffRun{start, count, color});
}

// Adds the spans of the bytes that changed in the lines that a comparison with the hub paired, to the hub if `hub` and
// to the other buffer otherwise.
void addSpans(TextEditorDiffShown& shown, const PrTextDiff* diff, bool hub, PrColor16 color) {
    const PrTextDiffLineChange* within = prTextDiffLineChanges(diff);
    for (uint32_t i = 0; i < prTextDiffLineChangeCount(diff); ++i) {
        uint32_t line = hub ? within[i].oldLine : within[i].newLine;
        uint32_t offset = hub ? within[i].bytes.oldStart : within[i].bytes.newStart;
        uint32_t count = hub ? within[i].bytes.oldCount : within[i].bytes.newCount;
        if (count == 0)
            continue;
        uint32_t start = prTextEditBufferLineStart(shown.buffer, line) + offset;
        shown.spans.push(PrTextViewSpan{.start = start, .end = start + count, .background = color});
    }
}

// Returns true if `count` lines of `a` from `startA` hold the bytes of those of `b` from `startB`. The lines are copied
// to `scratch`.
bool sameLines(const PrTextEditBuffer* a, uint32_t startA, const PrTextEditBuffer* b, uint32_t startB, uint32_t count,
        RawArray<char>& scratch) {
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t aStart = prTextEditBufferLineStart(a, startA + i);
        uint32_t aSize = prTextEditBufferLineEnd(a, startA + i) - aStart;
        uint32_t bStart = prTextEditBufferLineStart(b, startB + i);
        uint32_t bSize = prTextEditBufferLineEnd(b, startB + i) - bStart;
        if (aSize != bSize)
            return false;
        scratch.resize(2 * aSize);
        prTextEditBufferCopy(a, aStart, aStart + aSize, scratch.data);
        prTextEditBufferCopy(b, bStart, bStart + bSize, scratch.data + aSize);
        if (!prMemEquals(scratch.data, scratch.data + aSize, aSize))
            return false;
    }
    return true;
}

// Returns the color of a chunk of `kind` in every buffer of a merge but the result.
PrColor16 chunkColor(const TextEditorDiffColors& colors, PrTextDiffMergeKind kind) {
    switch (kind) {
        case PrTextDiffMergeKind_Left: return colors.lines[TextEditorDiffRole_Left];
        case PrTextDiffMergeKind_Right: return colors.lines[TextEditorDiffRole_Right];
        case PrTextDiffMergeKind_Both: return colors.lines[TextEditorDiffRole_Base];
        case PrTextDiffMergeKind_Conflict: return colors.conflict;
        default: return PrColor16{};
    }
}

// Puts `runs` in order of their lines.
void sortRuns(RawArray<TextEditorDiffRun>& runs) {
    for (uint32_t i = 1; i < runs.count; ++i) {
        for (uint32_t k = i; k > 0 && runs.data[k - 1].start > runs.data[k].start; --k) {
            TextEditorDiffRun swapped = runs.data[k];
            runs.data[k] = runs.data[k - 1];
            runs.data[k - 1] = swapped;
        }
    }
}

// Adds a run of the result from `start` to `end` in `color`, except where the sorted runs of `covered` lie.
void addUncovered(TextEditorDiffShown& result, const RawArray<TextEditorDiffRun>& covered, uint32_t start, uint32_t end,
        PrColor16 color) {
    uint32_t next = 0;
    while (start < end) {
        while (next < covered.count && covered.data[next].start + covered.data[next].count <= start)
            ++next;
        if (next < covered.count && covered.data[next].start <= start) {
            start = covered.data[next].start + covered.data[next].count;
            continue;
        }
        uint32_t stop = end;
        if (next < covered.count && covered.data[next].start < stop)
            stop = covered.data[next].start;
        addRun(result, start, stop - start, color);
        start = stop;
    }
}

// Tints the runs of a merge. Each chunk that a side changed has its kind's color in the sides and the base, so that a
// change shows in every pane that holds it. The result's lines beside a run of changed chunks show where they came
// from: the conflict's color while they mark a conflict, a side's color while they hold that side's lines, and the
// result's color otherwise, as they do where the result changed lines that the sides did not.
void tintMerge(TextEditorDiffModel* model) {
    const TextEditorDiffColors& colors = model->colors;
    TextEditorDiffShown* buffers = model->buffers;
    const PrTextDiff* resultDiff = model->diffs[TextEditorDiffSlot_Result - 1];
    const PrTextDiffChange* changes = prTextDiffChanges(resultDiff);
    uint32_t changeCount = prTextDiffChangeCount(resultDiff);
    uint32_t next = 0;
    int64_t shift = 0;
    RawArray<TextEditorDiffRun> groups;
    RawArray<char> scratch;
    for (uint32_t i = 0; i < model->chunks.count;) {
        const PrTextDiffMergeChunk& first = model->chunks.data[i];
        if (first.kind == PrTextDiffMergeKind_Unchanged) {
            ++i;
            continue;
        }
        // A run of changed chunks, which meet in every text.
        uint32_t end = i;
        while (end < model->chunks.count && model->chunks.data[end].kind != PrTextDiffMergeKind_Unchanged) {
            const PrTextDiffMergeChunk& chunk = model->chunks.data[end];
            PrColor16 color = chunkColor(colors, chunk.kind);
            addRun(buffers[TextEditorDiffSlot_Left], chunk.leftStart, chunk.leftCount, color);
            addRun(buffers[TextEditorDiffSlot_Right], chunk.rightStart, chunk.rightCount, color);
            addRun(buffers[TextEditorDiffSlot_Base], chunk.baseStart, chunk.baseCount, color);
            ++end;
        }
        const PrTextDiffMergeChunk& last = model->chunks.data[end - 1];
        uint32_t baseStart = first.baseStart;
        uint32_t baseEnd = last.baseStart + last.baseCount;
        uint32_t leftCount = last.leftStart + last.leftCount - first.leftStart;
        uint32_t rightCount = last.rightStart + last.rightCount - first.rightStart;
        i = end;

        // The result's lines beside the run: the base's, moved by the result's changes before them, and widened by
        // those that overlap or meet them.
        while (next < changeCount && changeEnd(changes[next]) < baseStart) {
            shift = int64_t(changes[next].newStart + changes[next].newCount) - int64_t(changeEnd(changes[next]));
            ++next;
        }
        int64_t from = int64_t(baseStart) + shift;
        int64_t to = int64_t(baseEnd) + shift;
        uint32_t taken = next;
        while (taken < changeCount && changes[taken].oldStart <= baseEnd) {
            const PrTextDiffChange& change = changes[taken];
            uint32_t before = change.oldStart > baseStart ? change.oldStart - baseStart : 0;
            uint32_t after = baseEnd > changeEnd(change) ? baseEnd - changeEnd(change) : 0;
            if (taken == next)
                from = int64_t(change.newStart) - int64_t(before);
            to = int64_t(change.newStart + change.newCount) + int64_t(after);
            ++taken;
        }
        uint32_t start = uint32_t(from);
        uint32_t count = uint32_t(to - from);
        PrColor16 color = colors.lines[TextEditorDiffRole_Result];
        bool marked = false;
        for (uint32_t k = 0; k < model->conflicts.count && !marked; ++k) {
            const PrTextDiffConflict& conflict = model->conflicts.data[k];
            marked = conflict.start < start + count && conflict.end > start;
        }
        const PrTextEditBuffer* resultBuffer = buffers[TextEditorDiffSlot_Result].buffer;
        const PrTextEditBuffer* leftBuffer = buffers[TextEditorDiffSlot_Left].buffer;
        const PrTextEditBuffer* rightBuffer = buffers[TextEditorDiffSlot_Right].buffer;
        bool alike = first.kind == PrTextDiffMergeKind_Both && last.kind == PrTextDiffMergeKind_Both;
        if (marked)
            color = colors.conflict;
        else if (count == leftCount && sameLines(resultBuffer, start, leftBuffer, first.leftStart, count, scratch))
            color = alike ? colors.lines[TextEditorDiffRole_Base] : colors.lines[TextEditorDiffRole_Left];
        else if (count == rightCount && sameLines(resultBuffer, start, rightBuffer, first.rightStart, count, scratch))
            color = colors.lines[TextEditorDiffRole_Right];
        // A change of the result that reaches past an unchanged chunk widens the runs on both sides of it.
        uint32_t previous = groups.count != 0 ? groups.last().start + groups.last().count : 0;
        if (start < previous) {
            count = start + count > previous ? start + count - previous : 0;
            start = previous;
        }
        if (count != 0)
            groups.push(TextEditorDiffRun{start, count, color});
    }
    scratch.reset();
    // The result's conflicts where no run of changed chunks lies, and then its own changes where neither does.
    TextEditorDiffShown& result = buffers[TextEditorDiffSlot_Result];
    for (uint32_t i = 0; i < groups.count; ++i)
        result.runs.push(groups.data[i]);
    for (uint32_t i = 0; i < model->conflicts.count; ++i) {
        const PrTextDiffConflict& conflict = model->conflicts.data[i];
        addUncovered(result, groups, conflict.start, conflict.end, colors.conflict);
    }
    sortRuns(result.runs);
    groups.clear();
    groups.append(result.runs.data, result.runs.count);
    for (uint32_t i = 0; i < changeCount; ++i) {
        addUncovered(result, groups, changes[i].newStart, changes[i].newStart + changes[i].newCount,
            colors.lines[TextEditorDiffRole_Result]);
    }
    groups.reset();
    sortRuns(result.runs);
}

} // namespace

void textEditorDiffModelInit(TextEditorDiffModel* model, TextEditorDiffKind kind) {
    *model = TextEditorDiffModel{};
    model->kind = kind;
    model->count = textEditorDiffBufferCount(kind);
    for (uint32_t i = 0; i + 1 < model->count; ++i)
        model->diffs[i] = prTextDiffCreate(nullptr);
    if (kind == TextEditorDiffKind_Merge)
        model->merge = prTextDiffMergeCreate(nullptr);
}

void textEditorDiffModelReset(TextEditorDiffModel* model) {
    for (uint32_t i = 0; i + 1 < model->count; ++i)
        prTextDiffDestroy(model->diffs[i]);
    prTextDiffMergeDestroy(model->merge);
    model->blocks.reset();
    model->chunks.reset();
    model->conflicts.reset();
    model->syncs.reset();
    model->pairs.reset();
    for (TextEditorDiffShown& shown : model->buffers) {
        shown.gaps.reset();
        shown.runs.reset();
        shown.spans.reset();
        shown.line.reset();
    }
}

uint32_t textEditorDiffBufferCount(TextEditorDiffKind kind) {
    return kind == TextEditorDiffKind_Merge ? 4 : 2;
}

TextEditorDiffRole textEditorDiffRole(TextEditorDiffKind kind, TextEditorDiffSlot slot) {
    if (kind == TextEditorDiffKind_TwoWay)
        return slot == TextEditorDiffSlot_Old ? TextEditorDiffRole_Old : TextEditorDiffRole_New;
    switch (slot) {
        case TextEditorDiffSlot_Base: return TextEditorDiffRole_Base;
        case TextEditorDiffSlot_Left: return TextEditorDiffRole_Left;
        case TextEditorDiffSlot_Right: return TextEditorDiffRole_Right;
        default: return TextEditorDiffRole_Result;
    }
}

bool textEditorDiffModelUpdate(TextEditorDiffModel* model, const PrTextEditBuffer* PR_NONNULL const* PR_NONNULL buffers,
        const uint32_t* PR_NULLABLE syncs, uint32_t syncCount, const TextEditorDiffColors& colors, float lineHeight) {
    uint32_t count = model->count;
    uint64_t versions[kTextEditorDiffMaxBuffers] = {};
    bool same = model->compared && prMemEquals(&colors, &model->colors, sizeof colors);
    for (uint32_t i = 0; i < count; ++i) {
        versions[i] = prTextEditBufferVersion(buffers[i]);
        same = same && versions[i] == model->versions[i];
    }
    uint32_t syncLines = syncCount * count;
    same = same && model->syncs.count == syncLines &&
        (syncLines == 0 || prMemEquals(syncs, model->syncs.data, syncLines * sizeof(uint32_t)));
    if (same)
        return false;
    model->compared = true;
    prMemCopy(model->versions, versions, sizeof versions);
    model->syncs.clear();
    if (syncLines != 0)
        model->syncs.append(syncs, syncLines);
    model->colors = colors;
    PrTextDiffText texts[kTextEditorDiffMaxBuffers];
    for (uint32_t i = 0; i < count; ++i) {
        TextEditorDiffShown& shown = model->buffers[i];
        shown.buffer = buffers[i];
        shown.gaps.clear();
        shown.runs.clear();
        shown.spans.clear();
        texts[i] = PrTextDiffText{.lineCount = prTextEditBufferLineCount(buffers[i]), .line = lineOf, .context = &shown};
    }

    // Each buffer past the hub compares with the hub, with the sync points' pairs of their lines.
    for (uint32_t i = 1; i < count; ++i) {
        model->pairs.clear();
        for (uint32_t k = 0; k < syncCount; ++k)
            model->pairs.push(PrTextDiffSync{syncs[k * count], syncs[k * count + i]});
        prTextDiffCompare(model->diffs[i - 1], &texts[0], &texts[i], model->pairs.data, model->pairs.count);
    }
    model->chunks.clear();
    model->conflicts.clear();
    if (model->kind == TextEditorDiffKind_Merge) {
        RawArray<PrTextDiffMergeSync> merged;
        for (uint32_t k = 0; k < syncCount; ++k) {
            const uint32_t* sync = syncs + k * count;
            merged.push(PrTextDiffMergeSync{
                sync[TextEditorDiffSlot_Base], sync[TextEditorDiffSlot_Left], sync[TextEditorDiffSlot_Right],
            });
        }
        prTextDiffMergeCompare(model->merge, &texts[TextEditorDiffSlot_Base], &texts[TextEditorDiffSlot_Left],
            &texts[TextEditorDiffSlot_Right], merged.data, merged.count);
        merged.reset();
        if (prTextDiffMergeChunkCount(model->merge) != 0)
            model->chunks.append(prTextDiffMergeChunks(model->merge), prTextDiffMergeChunkCount(model->merge));
        model->conflicts.resize(prTextDiffConflicts(&texts[TextEditorDiffSlot_Result], nullptr, 0));
        prTextDiffConflicts(&texts[TextEditorDiffSlot_Result], model->conflicts.data, model->conflicts.count);
    }
    buildBlocks(model, syncs, syncCount);

    // The buffers with fewer lines in a block leave space for the others', after their own.
    for (uint32_t b = 0; b < model->blocks.count; ++b) {
        const TextEditorDiffBlock& block = model->blocks.data[b];
        uint32_t most = 0;
        for (uint32_t i = 0; i < count; ++i)
            most = block.counts[i] > most ? block.counts[i] : most;
        for (uint32_t i = 0; i < count; ++i) {
            if (block.counts[i] < most) {
                float size = float(most - block.counts[i]) * lineHeight;
                model->buffers[i].gaps.push(PrTextViewGap{block.starts[i] + block.counts[i], size});
            }
        }
    }

    // The runs and spans of each buffer.
    if (model->kind == TextEditorDiffKind_TwoWay) {
        const PrTextDiff* diff = model->diffs[0];
        const PrTextDiffChange* changes = prTextDiffChanges(diff);
        for (uint32_t i = 0; i < prTextDiffChangeCount(diff); ++i) {
            addRun(model->buffers[TextEditorDiffSlot_Old], changes[i].oldStart, changes[i].oldCount,
                colors.lines[TextEditorDiffRole_Old]);
            addRun(model->buffers[TextEditorDiffSlot_New], changes[i].newStart, changes[i].newCount,
                colors.lines[TextEditorDiffRole_New]);
        }
    } else {
        tintMerge(model);
    }
    addSpans(model->buffers[0], model->diffs[0], true, colors.bytes[textEditorDiffRole(model->kind, 0)]);
    for (uint32_t i = 1; i < count; ++i) {
        TextEditorDiffRole role = textEditorDiffRole(model->kind, TextEditorDiffSlot(i));
        addSpans(model->buffers[i], model->diffs[i - 1], false, colors.bytes[role]);
    }
    for (TextEditorDiffShown& shown : model->buffers)
        shown.buffer = nullptr;
    return true;
}

PrColor16 textEditorDiffLineColor(const TextEditorDiffModel* model, TextEditorDiffSlot slot, uint32_t line) {
    const RawArray<TextEditorDiffRun>& runs = model->buffers[slot].runs;
    uint32_t after = partitionPoint(0, runs.count, [&](uint32_t i) { return runs.data[i].start <= line; });
    if (after == 0)
        return PrColor16{};
    const TextEditorDiffRun& run = runs.data[after - 1];
    return line < run.start + run.count ? run.color : PrColor16{};
}

uint32_t textEditorDiffSpans(const TextEditorDiffModel* model, TextEditorDiffSlot slot, uint32_t start, uint32_t end,
        PrTextViewSpan* PR_NULLABLE out, uint32_t capacity) {
    const RawArray<PrTextViewSpan>& spans = model->buffers[slot].spans;
    uint32_t first = partitionPoint(0, spans.count, [&](uint32_t i) { return spans.data[i].end <= start; });
    uint32_t count = 0;
    for (uint32_t i = first; i < spans.count && spans.data[i].start < end; ++i) {
        if (count < capacity)
            out[count] = spans.data[i];
        ++count;
    }
    return count;
}

uint32_t textEditorDiffHubLine(const TextEditorDiffModel* model, TextEditorDiffSlot slot, uint32_t line) {
    const RawArray<TextEditorDiffBlock>& blocks = model->blocks;
    uint32_t after = partitionPoint(0, blocks.count, [&](uint32_t i) { return blocks.data[i].starts[slot] <= line; });
    if (after == 0)
        return line;
    const TextEditorDiffBlock& block = blocks.data[after - 1];
    uint32_t into = line - block.starts[slot];
    if (into < block.counts[slot]) {
        uint32_t last = block.counts[0] != 0 ? block.counts[0] - 1 : 0;
        return block.starts[0] + (into < last ? into : last);
    }
    return block.starts[0] + block.counts[0] + (into - block.counts[slot]);
}

} // namespace pr_impl
