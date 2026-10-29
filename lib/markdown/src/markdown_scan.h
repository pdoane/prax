// Markdown's lexical rules, which the block and inline parsers share: its classes of characters, raw HTML, the parts of
// links, and character references.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The most bytes in a link label between its brackets (commonmark/#link-label).
constexpr size_t kMarkdownMaxLabelBytes = 999;

// Returns true if `c` is an ASCII punctuation character, which a backslash escapes.
bool markdownIsAsciiPunctuation(char c);

// Returns true if `codePoint` is a Unicode punctuation character, which is one of General_Category P or S
// (commonmark/#unicode-punctuation-character).
bool markdownIsPunctuation(uint32_t codePoint);

// Returns true if `codePoint` is Unicode whitespace: General_Category Zs, tab, line feed, form feed or carriage return
// (commonmark/#unicode-whitespace-character).
bool markdownIsWhitespace(uint32_t codePoint);

// Returns true if `c` is a space or a tab.
inline bool markdownIsSpaceOrTab(char c) {
    return c == ' ' || c == '\t';
}

// The searches that failed while scanning one text for HTML tags. A search that found no end after one offset finds
// none after a later offset either, so a scan skips it. This keeps a text of many unclosed tags linear.
struct MarkdownTagScan {
    size_t missing[6];              // by MarkdownTagEnd: the earliest offset after which the end was not found
};

// Makes a scan that has searched for nothing.
MarkdownTagScan markdownTagScanStart();

// Returns the length of the HTML tag at `start` in `text`, which is an open tag, a closing tag, a comment, a processing
// instruction, a declaration or a CDATA section (commonmark/#html-tag). Returns 0 if no tag starts there. `scan` holds
// the searches of earlier calls for the same text.
size_t markdownScanHtmlTag(PrStringView text, size_t start, MarkdownTagScan* scan);

// Returns the kind of HTML block, 1 to 7, whose start condition the line `line` meets at `start`, or 0 if it meets none
// (commonmark/#html-blocks). A block of kind 7 cannot interrupt a paragraph, so `interruptsParagraph` rules it out.
uint8_t markdownHtmlBlockStart(PrStringView line, size_t start, bool interruptsParagraph);

// Returns true if the line `line` meets the end condition of an HTML block of `kind` 1 to 5. Blocks of kinds 6 and 7
// end at a blank line, which is not a condition on the line.
bool markdownHtmlBlockEnds(uint8_t kind, PrStringView line);

// Returns the length of the URI autolink or the email autolink at `start` in `text`, from its '<' through its '>', or 0
// if none starts there (commonmark/#autolinks).
size_t markdownScanUriAutolink(PrStringView text, size_t start);
size_t markdownScanEmailAutolink(PrStringView text, size_t start);

// A part of a link, as a range of the text that was scanned.
struct MarkdownSpan {
    size_t start;
    size_t size;
};

// Scans the link destination at `start` in `text` (commonmark/#link-destination). Returns false if no destination
// starts there. Otherwise writes the length that it takes to `*outLength`, and its characters to `out`: within its
// angle brackets if it has them. A destination without angle brackets may be empty if it is followed by ')', which a
// caller that needs a destination rules out by its length. A destination must end before the text does.
bool markdownScanLinkDestination(PrStringView text, size_t start, size_t* outLength, MarkdownSpan* out);

// Scans the link title at `start` in `text` (commonmark/#link-title). Returns the length that it takes, or 0 if there
// is none, and writes its characters within its delimiters to `out`.
size_t markdownScanLinkTitle(PrStringView text, size_t start, MarkdownSpan* out);

// Appends the raw HTML `html` to `out` with the '<' of each tag that GFM disallows written as "&lt;", so that the tag
// reads as text (gfm/#disallowed-raw-html-extension).
void markdownAppendFilteredHtml(PrStringView html, Array<char>* out);

// Returns true if the destination `destination` has a scheme that can run code or read local files: javascript:,
// vbscript:, file:, or data: other than a PNG, GIF, JPEG or WebP image's (PrMarkdownDestinations_Safe).
bool markdownIsUnsafeDestination(PrStringView destination);

// Returns the number of spaces, tabs and line endings at `start` in `text`, which may include at most one line ending.
size_t markdownSkipSpaceAndLineEnding(PrStringView text, size_t start);

// Decodes the character reference at `start` in `text`, whose '&' is at `start`, and appends its UTF-8 to `out`
// (commonmark/#entity-and-numeric-character-references). Returns its length, or 0 if no valid reference starts there.
size_t markdownDecodeReference(PrStringView text, size_t start, Array<char>* out);

// Appends `text` to `out` with its backslash escapes and character references decoded.
void markdownAppendDecoded(PrStringView text, Array<char>* out);

// Appends the normalized form of the link label `label` to `out`: case folded, with each run of whitespace made one
// space, and with no whitespace at either end (commonmark/#matches). Returns false and appends nothing if the label is
// only whitespace, which no reference matches.
bool markdownAppendNormalizedLabel(PrStringView label, Array<char>* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
