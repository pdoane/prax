// Character encodings of HTML documents (html/#determining-the-character-encoding).

#pragma once

#include "pr/base.h"
#include "pr/charset.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// How certain a parse is of the charset that decodes its input (html/#concept-encoding-confidence).
enum {
    HtmlConfidence_Tentative,   // a meta element may still change the charset
    HtmlConfidence_Certain,
    HtmlConfidence_Irrelevant,  // the input is text, which no charset decodes
};
typedef uint8_t HtmlConfidence;

// The most bytes at the start of a document that the prescan reads, as html/#encoding-sniffing-algorithm suggests.
constexpr uint32_t kHtmlPrescanBytes = 1024;

// Returns the charset that a declaration in a document's bytes means when it names `charset`. A UTF-16 document cannot
// declare its charset in ASCII, so a declaration of UTF-16 means UTF-8. x-user-defined means windows-1252.
PrCharset htmlDeclaredCharset(PrCharset charset);

// Returns the charset that a meta element's content attribute names, or PrCharset_None
// (html/#algorithm-for-extracting-a-character-encoding-from-a-meta-element).
PrCharset htmlCharsetFromMetaContent(PrStringView content);

// Finds the charset that the first kHtmlPrescanBytes of a document declare, and writes it to `*out`, or PrCharset_None
// if they declare none (html/#prescan-a-byte-stream-to-determine-its-encoding). The prescan reads the content of the
// elements whose content the tokenizer reads as text, such as title and script, as text, so that a meta element in it
// declares nothing, as whatwg/html#6962 proposes. Returns false, and writes PrCharset_None, if `bytes` are not
// `complete` and end within the first kilobyte before the prescan has decided.
bool htmlPrescan(PrStringView bytes, bool complete, PrCharset* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
