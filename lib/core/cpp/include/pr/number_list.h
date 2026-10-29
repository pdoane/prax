// Number lists: the grammar of numbers and separators that SVG writes its path data and its other lists of numbers in.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Reads a number as SVG writes one (SVG2/paths.html#PathDataBNF, `number`). The number starts at `*pos` after any white
// space, and `*pos` advances past it. Returns false, leaving `*pos` unchanged, if there is no number.
bool numberListRead(PrStringView text, size_t* pos, float* out);

// Reads `count` numbers, each after an optional separator except the first. Returns false, leaving `*pos` unchanged, if
// any of them is missing.
bool numberListReadArray(PrStringView text, size_t* pos, float* out, uint32_t count);

// Skips white space and at most one comma between two items of a list.
void numberListSkipSeparator(PrStringView text, size_t* pos);

// Skips the white space at `*pos`: space, tab, line feed, carriage return and form feed.
void numberListSkipSpace(PrStringView text, size_t* pos);

} // namespace pr

PR_ASSUME_NONNULL_END
