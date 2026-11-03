// Grid templates in CSS: the strings of grid-template-areas, read as rows of cell tokens.
//
// css-grid-1 - https://drafts.csswg.org/css-grid-1/

#pragma once

#include "pr/ascii.h"
#include "pr/string_view.h"

#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if `c` may be part of the name of a named cell token: a name code point, which is a letter, a digit,
// `-`, `_`, or not ASCII (css-grid-1/#grid-template-areas-property).
inline bool cssGridIsNameCharacter(char c) {
    return prAsciiIsLetter(c) || prAsciiIsDigit(c) || c == '-' || c == '_' || uint8_t(c) >= 0x80;
}

// Calls `cell(name)` with each cell token of `row`, a string of grid-template-areas, in order: the name of a named cell
// token, or an empty view for a null cell token, a run of `.` (css-grid-1/#grid-template-areas-property). Returns false
// if the row holds a trash token, any other character that is not white space.
template <typename Cell>
bool cssGridAreaCells(PrStringView row, Cell cell) {
    for (size_t k = 0; k < row.size;) {
        char c = row.data[k];
        if (prAsciiIsSpace(c)) {
            ++k;
            continue;
        }
        size_t end = k;
        if (c == '.') {
            while (end < row.size && row.data[end] == '.')
                ++end;
            cell(PrStringView{});
        } else if (cssGridIsNameCharacter(c)) {
            while (end < row.size && cssGridIsNameCharacter(row.data[end]))
                ++end;
            cell(PrStringView{row.data + k, end - k});
        } else {
            return false;
        }
        k = end;
    }
    return true;
}

// Checks the strings of a value of grid-template-areas, the `count` values from `first` in `pool`, and rewrites each in
// its canonical form (css-grid-1/#serialize-template): its cells separated by single spaces, each null cell token a
// single `.`. Returns false, and rewrites nothing, if the strings do not form a template: a string holds a trash token,
// the strings hold different numbers of cells, or the cells of a name do not form one filled rectangle
// (css-grid-1/#grid-template-areas-property).
bool cssGridAreasFold(CssValuePool* pool, uint32_t first, uint32_t count);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
