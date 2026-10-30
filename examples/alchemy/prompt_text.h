// Prompt text: the edits that alchemy makes to a prompt besides typing, which are attention weights, lines taken from
// other images' records, and embedding tokens.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

#include <stdint.h>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

// ---------------------------------------------------------------------------------------------------------------------
// Attention weights
//
// ComfyUI reads `(text:1.15)` as text sampled at 1.15 times the attention, bare text at 1, and parentheses that name no
// weight at 1.1. An inner weight replaces the weight around it rather than compounding with it, so nesting adds
// nothing. Adjusting a weight is therefore an edit of the run that the caret stands in: bare text gains a group, a
// group's number changes, and a group that returns to 1 loses its parentheses. Only explicit weights are written, since
// they read the same at every depth.
//
// The run that is adjusted follows the caret. A selection is the run. A caret without a selection takes the group that
// encloses it, or else the word that it stands in. A selection of exactly the weighted text of the group around it
// adjusts that group rather than nesting a group inside it. Groups span lines, and a parenthesis after a backslash is
// text rather than a delimiter.

// The change in attention of one press of the weight chords.
constexpr double kPromptWeightStep = 0.05;

// An adjustment of a weight: `text` replaces the bytes from `begin` to `end`, and what it writes stays selected.
struct WeightEdit {
    std::string text;
    uint32_t begin = 0;
    uint32_t end = 0;
};

// Writes to `out` the edit that adding `delta` to the weight of the run between `selectionStart` and `selectionEnd`
// makes. Returns false if there is nothing to weight: an empty run, or bare text whose weight would stay at 1.
bool promptWeightAdjust(PrStringView text, uint32_t selectionStart, uint32_t selectionEnd, double delta, WeightEdit* out);

// ---------------------------------------------------------------------------------------------------------------------
// Lines
//
// A prompt written over several lines reads as the pieces that it was composed from. A piece taken from a record joins
// the prompt as a line of its own rather than replacing it.

// Returns the lines of `prompt`, which view its bytes, without the whitespace around the whole. A blank prompt has no
// lines.
std::vector<PrStringView> promptLines(PrStringView prompt);

// Returns `prompt` with `line` appended as a line of its own, or `line` alone if the prompt is empty.
std::string promptAppendLine(PrStringView prompt, PrStringView line);

// ---------------------------------------------------------------------------------------------------------------------
// Embeddings
//
// A textual inversion embedding enters a prompt as `embedding:<name>`, where the name is the file that the server
// reports without its model extension.

// Returns the name that `filename` carries: the file without its model extension, or the whole name if it has none.
PrStringView embeddingName(PrStringView filename);

// Returns the text that inserting the embedding over the selection writes: its token, with a space on each side that
// meets text rather than whitespace or an end of the prompt.
std::string embeddingInsertText(PrStringView text, uint32_t selectionStart, uint32_t selectionEnd, PrStringView filename);

} // namespace alchemy

PR_ASSUME_NONNULL_END
