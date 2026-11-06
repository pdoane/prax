// Finds links in a terminal's text: URLs, and references to files. A file reference may carry the line and column that
// compilers, test runners and other tools print after a path.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/vterm.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The kind of target that a link in the text names.
enum {
    TerminalTextLinkKind_Url,
    TerminalTextLinkKind_File,
};
typedef uint8_t TerminalTextLinkKind;

// A link in a line of text. Offsets are in bytes of the text, and each range is exclusive at its end. `start` to `end`
// is the part that shows as the link. `targetStart` to `targetEnd` is the URL or path within it. The line and column of
// a file reference count from 1, and are 0 if the text names none.
struct TerminalTextLink {
    TerminalTextLinkKind kind;
    uint32_t start;
    uint32_t end;
    uint32_t targetStart;
    uint32_t targetEnd;
    uint32_t line;
    uint32_t column;
};

// Appends the links in `text` in order. Links do not overlap.
//
// A URL begins with http:// or https:// at the start of a word, after any opening brackets and quotes. It ends before
// any closing brackets, quotes and punctuation.
//
// A file reference is one of these:
// - a path followed by a line and an optional column, as in `path:line`, `path:line:column`, `path(line)` and
//   `path(line,column)`
// - the path in a Python traceback line, `File "path", line N`
// - any other run of path characters that contains a separator or an extension, with no line
//
// The caller checks whether the file exists.
void terminalTextLinks(PrStringView text, RawArray<TerminalTextLink>* out);

// The text of a terminal line, and the column of each of its bytes.
struct TerminalLineText {
    RawArray<char> text;
    RawArray<uint32_t> columns;     // by byte of `text`
};

// Appends the text of line `index` of `vterm` to `out` as prVtermLineAppendText writes it, and records the column of
// each byte.
void terminalLineText(const PrVterm* vterm, uint32_t index, TerminalLineText* out);

// Frees the storage of `text`.
void terminalLineTextReset(TerminalLineText* text);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
