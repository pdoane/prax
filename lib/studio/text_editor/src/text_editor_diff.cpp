// The model of a comparison (text_editor_diff.h): the buffers' lines given to a text diff, and its changes turned into
// what each side shows.

#include "studio/text_editor/src/text_editor_diff.h"

#include "pr/mem.h"
#include "pr/search.h"

namespace pr_impl {

namespace {

// Copies line `index` of the side's buffer, which the text diff reads until it asks for the side's next line.
PrStringView lineOf(void* PR_NULLABLE context, uint32_t index) {
    TextEditorDiffShown* side = static_cast<TextEditorDiffShown*>(context);
    uint32_t start = prTextEditBufferLineStart(side->buffer, index);
    uint32_t end = prTextEditBufferLineEnd(side->buffer, index);
    side->line.resize(end - start);
    prTextEditBufferCopy(side->buffer, start, end, side->line.data);
    return PrStringView{side->line.data, side->line.count};
}

} // namespace

void textEditorDiffModelInit(TextEditorDiffModel* model) {
    *model = TextEditorDiffModel{};
    model->diff = prTextDiffCreate(nullptr);
}

void textEditorDiffModelReset(TextEditorDiffModel* model) {
    prTextDiffDestroy(model->diff);
    model->changes.reset();
    for (TextEditorDiffShown& side : model->sides) {
        side.gaps.reset();
        side.spans.reset();
        side.line.reset();
    }
}

bool textEditorDiffModelUpdate(TextEditorDiffModel* model, const PrTextEditBuffer* old, const PrTextEditBuffer* young,
        const TextEditorDiffColors& colors, float lineHeight) {
    uint64_t versions[] = {prTextEditBufferVersion(old), prTextEditBufferVersion(young)};
    if (model->compared && versions[0] == model->versions[0] && versions[1] == model->versions[1] &&
            memEquals(&colors, &model->colors, sizeof colors))
        return false;
    model->compared = true;
    model->versions[0] = versions[0];
    model->versions[1] = versions[1];
    model->colors = colors;
    const PrTextEditBuffer* buffers[] = {old, young};
    PrTextDiffText texts[TextEditorDiffSide_Count];
    for (uint32_t side = 0; side < TextEditorDiffSide_Count; ++side) {
        model->sides[side].buffer = buffers[side];
        texts[side] = PrTextDiffText{
            .lineCount = prTextEditBufferLineCount(buffers[side]),
            .line = lineOf,
            .context = &model->sides[side],
        };
    }
    prTextDiffCompare(model->diff, &texts[TextEditorDiffSide_Old], &texts[TextEditorDiffSide_New]);
    model->changes.clear();
    if (prTextDiffChangeCount(model->diff) != 0)
        model->changes.append(prTextDiffChanges(model->diff), prTextDiffChangeCount(model->diff));
    for (TextEditorDiffShown& side : model->sides) {
        side.gaps.clear();
        side.spans.clear();
        side.buffer = nullptr;
    }
    for (uint32_t i = 0; i < model->changes.count; ++i) {
        const PrTextDiffChange& change = model->changes.data[i];
        // The side with fewer lines leaves space for the other's, after its own.
        if (change.oldCount > change.newCount) {
            float size = float(change.oldCount - change.newCount) * lineHeight;
            model->sides[TextEditorDiffSide_New].gaps.push(PrTextViewGap{change.newStart + change.newCount, size});
        } else if (change.newCount > change.oldCount) {
            float size = float(change.newCount - change.oldCount) * lineHeight;
            model->sides[TextEditorDiffSide_Old].gaps.push(PrTextViewGap{change.oldStart + change.oldCount, size});
        }
    }
    const PrTextDiffLineChange* within = prTextDiffLineChanges(model->diff);
    for (uint32_t i = 0; i < prTextDiffLineChangeCount(model->diff); ++i) {
        uint32_t lines[] = {within[i].oldLine, within[i].newLine};
        uint32_t offsets[] = {within[i].bytes.oldStart, within[i].bytes.newStart};
        uint32_t counts[] = {within[i].bytes.oldCount, within[i].bytes.newCount};
        for (uint32_t side = 0; side < TextEditorDiffSide_Count; ++side) {
            if (counts[side] == 0)
                continue;
            uint32_t start = prTextEditBufferLineStart(buffers[side], lines[side]) + offsets[side];
            model->sides[side].spans.push(PrTextViewSpan{
                .start = start,
                .end = start + counts[side],
                .background = model->colors.bytes[side],
            });
        }
    }
    return true;
}

PrColor16 textEditorDiffLineColor(const TextEditorDiffModel* model, TextEditorDiffSide side, uint32_t line) {
    const Array<PrTextDiffChange>& changes = model->changes;
    auto startOf = [&](uint32_t i) {
        return side == TextEditorDiffSide_Old ? changes.data[i].oldStart : changes.data[i].newStart;
    };
    uint32_t after = partitionPoint(0, changes.count, [&](uint32_t i) { return startOf(i) <= line; });
    if (after == 0)
        return PrColor16{};
    const PrTextDiffChange& change = changes.data[after - 1];
    uint32_t count = side == TextEditorDiffSide_Old ? change.oldCount : change.newCount;
    return line < startOf(after - 1) + count ? model->colors.lines[side] : PrColor16{};
}

uint32_t textEditorDiffSpans(const TextEditorDiffModel* model, TextEditorDiffSide side, uint32_t start, uint32_t end,
        PrTextViewSpan* PR_NULLABLE out, uint32_t capacity) {
    const Array<PrTextViewSpan>& spans = model->sides[side].spans;
    uint32_t first = partitionPoint(0, spans.count, [&](uint32_t i) { return spans.data[i].end <= start; });
    uint32_t count = 0;
    for (uint32_t i = first; i < spans.count && spans.data[i].start < end; ++i) {
        if (count < capacity)
            out[count] = spans.data[i];
        ++count;
    }
    return count;
}

} // namespace pr_impl
