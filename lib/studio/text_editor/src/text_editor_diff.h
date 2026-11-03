// The model of a comparison of two buffers, as the editor shows it side by side: the changes of lines, the gaps that
// align each side with the other, the color of each changed line, and the bytes that changed within changed lines.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/raw_array.h"
#include "pr/text_diff.h"
#include "pr/text_edit.h"
#include "pr/text_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A side of a comparison.
enum {
    TextEditorDiffSide_Old,
    TextEditorDiffSide_New,
    TextEditorDiffSide_Count,
};
typedef uint8_t TextEditorDiffSide;

// What one side shows: its gaps, and the spans of the bytes that changed in its changed lines, in order.
struct TextEditorDiffShown {
    RawArray<PrTextViewGap> gaps;
    RawArray<PrTextViewSpan> spans;
    const PrTextEditBuffer* PR_NULLABLE buffer; // during a comparison
    RawArray<char> line;                        // a line's text, copied for a comparison
};

// The colors of a comparison: of a changed line on each side, and of the bytes that changed in it.
struct TextEditorDiffColors {
    PrColor16 lines[TextEditorDiffSide_Count];
    PrColor16 bytes[TextEditorDiffSide_Count];
};

// What a comparison of two buffers shows of them, kept until either changes.
struct TextEditorDiffModel {
    PrTextDiff* diff;
    RawArray<PrTextDiffChange> changes;    // of lines
    TextEditorDiffShown sides[TextEditorDiffSide_Count];
    uint64_t versions[TextEditorDiffSide_Count];    // of the texts that the changes are of
    bool compared;
    TextEditorDiffColors colors;
};

// Makes an empty model, or frees one.
void textEditorDiffModelInit(TextEditorDiffModel* model);
void textEditorDiffModelReset(TextEditorDiffModel* model);

// Compares the buffers again if either changed since the last comparison, in `colors`, with gaps of `lineHeight` px for
// each line that one side lacks. Returns true if it compared.
bool textEditorDiffModelUpdate(TextEditorDiffModel* model, const PrTextEditBuffer* old, const PrTextEditBuffer* young,
    const TextEditorDiffColors& colors, float lineHeight);

// Returns the color of `line` of `side`, or zero alpha if the line did not change.
PrColor16 textEditorDiffLineColor(const TextEditorDiffModel* model, TextEditorDiffSide side, uint32_t line);

// Writes the spans of `side` that intersect the bytes from `start` to `end`, as PrTextViewSpansFn does.
uint32_t textEditorDiffSpans(const TextEditorDiffModel* model, TextEditorDiffSide side, uint32_t start, uint32_t end,
    PrTextViewSpan* PR_NULLABLE out, uint32_t capacity);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
