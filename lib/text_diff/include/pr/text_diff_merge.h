// Three-way merges: two texts that each changed a common base, the chunks where they agree and conflict, and the text
// of the merge with conflict markers.
//
// A merge compares the base with each side as a diff does (pr/text_diff.h), and walks both edit scripts along the base
// (Khanna, Kunal and Pierce, "A Formal Investigation of Diff3", 2007). Changes of the two sides that overlap or meet in
// the base are one chunk. A chunk that both sides changed alike is no conflict. A conflict leaves out the lines that
// begin and end both sides alike, which are chunks that both changed alike, so that it holds only what differs.
//
// A merge may be given sync points, a line of each text that it aligns, as a diff's are. The texts between sync points
// merge apart, so no chunk spans a sync point.
//
// One thread uses a merge at a time.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text_diff.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Merges

// The results of merging two texts with their base, which a merge keeps until it merges again.
typedef struct PrTextDiffMerge PrTextDiffMerge;

// What the two sides did to a chunk of the base.
enum {
    PrTextDiffMergeKind_Unchanged,              // the three texts agree
    PrTextDiffMergeKind_Left,                   // the left side changed the base, and the right kept it
    PrTextDiffMergeKind_Right,                  // the right side changed the base, and the left kept it
    PrTextDiffMergeKind_Both,                   // both sides changed the base alike
    PrTextDiffMergeKind_Conflict,               // both sides changed the base, differently
};
typedef uint8_t PrTextDiffMergeKind;

// A run of lines of each text that a merge takes together. A chunk that both sides changed alike, beside a conflict,
// may hold no lines of the base.
typedef struct PrTextDiffMergeChunk {
    uint32_t baseStart;
    uint32_t baseCount;
    uint32_t leftStart;
    uint32_t leftCount;
    uint32_t rightStart;
    uint32_t rightCount;
    PrTextDiffMergeKind kind;
} PrTextDiffMergeChunk;

// A line of each text that a merge aligns.
typedef struct PrTextDiffMergeSync {
    uint32_t baseLine;
    uint32_t leftLine;
    uint32_t rightLine;
} PrTextDiffMergeSync;

// Creates a merge that holds no chunks, or frees one. A null `options` means the defaults of a diff.
PrTextDiffMerge* prTextDiffMergeCreate(const PrTextDiffOptions* PR_NULLABLE options);
void prTextDiffMergeDestroy(PrTextDiffMerge* PR_NULLABLE merge);

// Merges `left` and `right`, which each changed `base`, and replaces the merge's chunks with theirs. The lines of each
// of `syncs` are one chunk of a line of each text. The sync points ascend strictly in each text, and name lines that
// the texts hold. The texts are read only during the call.
void prTextDiffMergeCompare(PrTextDiffMerge* merge, const PrTextDiffText* base, const PrTextDiffText* left,
    const PrTextDiffText* right, const PrTextDiffMergeSync* PR_NULLABLE syncs, uint32_t syncCount);

// The chunks of the last merge, in order, which cover each text whole. The pointer is valid until the merge compares
// again.
uint32_t prTextDiffMergeChunkCount(const PrTextDiffMerge* merge);
const PrTextDiffMergeChunk* PR_NULLABLE prTextDiffMergeChunks(const PrTextDiffMerge* merge);

// The number of chunks of the last merge that conflict.
uint32_t prTextDiffMergeConflictCount(const PrTextDiffMerge* merge);

// ---------------------------------------------------------------------------------------------------------------------
// Conflict markers

// Which texts a conflict's markers hold.
enum {
    PrTextDiffMarkerStyle_Diff3,                // the left side, the base and the right side
    PrTextDiffMarkerStyle_Merge,                // the left side and the right side
};
typedef uint8_t PrTextDiffMarkerStyle;

// How the text of a merge marks its conflicts. A label follows its marker after a space, and an empty label leaves the
// marker alone on its line.
typedef struct PrTextDiffMarkerOptions {
    PrTextDiffMarkerStyle style;
    PrStringView leftLabel;
    PrStringView baseLabel;
    PrStringView rightLabel;
} PrTextDiffMarkerOptions;

// The diff3 style, with no labels.
PrTextDiffMarkerOptions prTextDiffMarkerOptionsDefault(void);

// The text of the last merge of these texts: the lines of each chunk that agrees, and each conflict between markers,
// with a line feed between lines. A null `options` means the defaults. The caller releases the text with
// prRawStringReset.
PrRawString prTextDiffMergeText(const PrTextDiffMerge* merge, const PrTextDiffText* base, const PrTextDiffText* left,
    const PrTextDiffText* right, const PrTextDiffMarkerOptions* PR_NULLABLE options);

// A conflict that a text marks, in lines: from its opening marker to its closing one, and the lines of each side
// between them. A conflict of the merge style holds no base.
typedef struct PrTextDiffConflict {
    uint32_t start;                             // the line of the opening marker
    uint32_t end;                               // the line after the closing marker
    uint32_t leftStart;
    uint32_t leftCount;
    uint32_t baseStart;
    uint32_t baseCount;
    uint32_t rightStart;
    uint32_t rightCount;
    bool hasBase;
} PrTextDiffConflict;

// Writes up to `capacity` of the conflicts that `text` marks to `out`, in order, and returns the number of conflicts. A
// marker is seven of its character at the start of a line, alone or before a space. Markers that do not close a
// conflict are text.
uint32_t prTextDiffConflicts(const PrTextDiffText* text, PrTextDiffConflict* PR_NULLABLE out, uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
