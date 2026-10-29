// Named character references (html/#named-character-references), which HTML and Markdown expand alike.
//
// tools/gen/gen_html.py generates the table of references from WHATWG's entities.json.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The length of the longest name, "CounterClockwiseContourIntegral;".
enum { kPrHtmlEntityMaxLength = 32 };

// The length of the longest name that may be written without its semicolon, "Aacute".
enum { kPrHtmlEntityMaxLegacyLength = 6 };

// Returns the UTF-8 that the named character reference `name` expands to, or the null view if no reference has that
// name. `name` is written without its '&', and with the ';' if it has one. A reference that is valid without its
// semicolon matches either way. The expansion is static.
PrStringView prHtmlEntityMatch(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
