// The model of a comparison of buffers, as the editor shows them side by side: each buffer compared with the hub, the
// first of them, the blocks of lines that lie beside each other across all of them, the gaps that align each buffer
// with the others, the colors of their changed lines, and the bytes that changed within changed lines. A merge also
// holds the chunks of its sides' changes to its base, and the conflicts that its result marks.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/raw_array.h"
#include "pr/text_diff.h"
#include "pr/text_diff_merge.h"
#include "pr/text_edit.h"
#include "pr/text_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The most buffers of a comparison.
constexpr uint32_t kTextEditorDiffMaxBuffers = 4;

// What a comparison compares: an old buffer with a new one, or the buffers of a merge.
enum {
    TextEditorDiffKind_TwoWay,
    TextEditorDiffKind_Merge,
};
typedef uint8_t TextEditorDiffKind;

// The buffers of a comparison, by kind: an old one and a new one, or a merge's base, sides and result. The first of
// each kind is the hub, which the others are compared with.
enum {
    TextEditorDiffSlot_Old = 0,
    TextEditorDiffSlot_New = 1,
    TextEditorDiffSlot_Base = 0,
    TextEditorDiffSlot_Left = 1,
    TextEditorDiffSlot_Right = 2,
    TextEditorDiffSlot_Result = 3,
};
typedef uint8_t TextEditorDiffSlot;

// What a buffer is to the others, which chooses the colors of its changes.
enum {
    TextEditorDiffRole_Old,
    TextEditorDiffRole_New,
    TextEditorDiffRole_Base,
    TextEditorDiffRole_Left,
    TextEditorDiffRole_Right,
    TextEditorDiffRole_Result,
    TextEditorDiffRole_Count,
};
typedef uint8_t TextEditorDiffRole;

// The colors of a comparison: by TextEditorDiffRole, of a changed line and of the bytes that changed in it, and of the
// lines of a merge's conflicts.
struct TextEditorDiffColors {
    PrColor16 lines[TextEditorDiffRole_Count];
    PrColor16 bytes[TextEditorDiffRole_Count];
    PrColor16 conflict;
};

// A run of lines that a buffer tints.
struct TextEditorDiffRun {
    uint32_t start;
    uint32_t count;
    PrColor16 color;
};

// What one buffer shows: its gaps, its tinted runs of lines in order, and the spans of the bytes that changed in its
// changed lines, in order.
struct TextEditorDiffShown {
    RawArray<PrTextViewGap> gaps;
    RawArray<TextEditorDiffRun> runs;
    RawArray<PrTextViewSpan> spans;
    const PrTextEditBuffer* PR_NULLABLE buffer; // during a comparison
    RawArray<char> line;                        // a line's text, copied for a comparison
};

// Lines of each buffer that lie beside each other, where one or more differ from the hub, by slot.
struct TextEditorDiffBlock {
    uint32_t starts[kTextEditorDiffMaxBuffers];
    uint32_t counts[kTextEditorDiffMaxBuffers];
};

// What a comparison of buffers shows of them, kept until any changes.
struct TextEditorDiffModel {
    TextEditorDiffKind kind;
    uint32_t count;                             // the number of buffers that the comparison compares
    TextEditorDiffShown buffers[kTextEditorDiffMaxBuffers];

    // The hub compared with each other buffer, by slot past the hub.
    PrTextDiff* PR_NULLABLE diffs[kTextEditorDiffMaxBuffers - 1];

    PrTextDiffMerge* PR_NULLABLE merge;
    RawArray<TextEditorDiffBlock> blocks;
    RawArray<PrTextDiffMergeChunk> chunks;      // of a merge's sides
    RawArray<PrTextDiffConflict> conflicts;     // that a merge's result marks

    // What the last comparison compared: the versions of the texts, the lines of the sync points, a line of each buffer
    // to a point, and the colors.
    uint64_t versions[kTextEditorDiffMaxBuffers];
    RawArray<uint32_t> syncs;
    TextEditorDiffColors colors;
    bool compared;

    // The pairs of lines of the sync points of a comparison with the hub, during a comparison.
    RawArray<PrTextDiffSync> pairs;
};

// Makes an empty model of a comparison of `kind`, or frees one.
void textEditorDiffModelInit(TextEditorDiffModel* model, TextEditorDiffKind kind);
void textEditorDiffModelReset(TextEditorDiffModel* model);

// Returns the number of buffers of a comparison of `kind`.
uint32_t textEditorDiffBufferCount(TextEditorDiffKind kind);

// Returns the role of the buffer of `slot` in a comparison of `kind`.
TextEditorDiffRole textEditorDiffRole(TextEditorDiffKind kind, TextEditorDiffSlot slot);

// Compares the buffers again if any changed since the last comparison, or the sync points or the colors did, with gaps
// of `lineHeight` px for each line that a buffer lacks beside the others. `syncs` holds a line of each buffer to each
// sync point, which ascend strictly in each buffer. Returns true if it compared.
bool textEditorDiffModelUpdate(TextEditorDiffModel* model, const PrTextEditBuffer* PR_NONNULL const* PR_NONNULL buffers,
    const uint32_t* PR_NULLABLE syncs, uint32_t syncCount, const TextEditorDiffColors& colors, float lineHeight);

// Returns the color of `line` of the buffer of `slot`, or zero alpha if it is not tinted.
PrColor16 textEditorDiffLineColor(const TextEditorDiffModel* model, TextEditorDiffSlot slot, uint32_t line);

// Writes the spans of the buffer of `slot` that intersect the bytes from `start` to `end`, as PrTextViewSpansFn does.
uint32_t textEditorDiffSpans(const TextEditorDiffModel* model, TextEditorDiffSlot slot, uint32_t start, uint32_t end,
    PrTextViewSpan* PR_NULLABLE out, uint32_t capacity);

// Returns the line of the hub that lies beside `line` of the buffer of `slot`: the same line of a block's hub lines, or
// its last if the block has fewer, as of the last comparison.
uint32_t textEditorDiffHubLine(const TextEditorDiffModel* model, TextEditorDiffSlot slot, uint32_t line);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
