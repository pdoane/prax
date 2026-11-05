// Addresses: the URL that the text of the address bar means, and what the chrome shows of a URL.

#pragma once

#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace browser {

using namespace pr;

// The search that the address bar runs for text that names no URL. The text follows it as the query, percent-encoded.
// The search's HTML results need no script.
constexpr const char* kAddressSearch = "https://html.duckduckgo.com/html/?q=";

// The URL of a tab that shows no document, which the start page stands in for.
constexpr const char* kAddressBlank = "about:blank";

// The address of the history page. The browser answers a navigation that the user starts to it with an empty document,
// which the chrome draws the page over.
constexpr const char* kAddressHistory = "prax:history";

// Returns the URL that the user means by `text`, typed in the address bar or given on the command line, or the empty
// string if the text is only whitespace.
//
// Text that begins with a scheme and `//`, or with a scheme that has no authority, such as about: or data:, is a URL as
// it is written. A path to a file that exists is the file's file: URL. Text without spaces that names a host, by a dot,
// a port or the name localhost, is the host's https: URL, or its http: URL for the local host. Any other text is a
// search for the text.
String addressResolve(PrStringView text);

// Returns true if `url` is the URL of a blank tab, or empty.
bool addressIsBlank(PrStringView url);

// Returns true if `url` names one of the browser's own pages, whose scheme is prax.
bool addressIsBrowserPage(PrStringView url);

// Returns what the address bar shows for `url` while the user is not editing it, which is `url` itself or, for a blank
// tab, nothing.
PrStringView addressShown(PrStringView url);

// Returns the host of `url`, a part of it, without a leading "www.", or the empty view if the URL has no host.
PrStringView addressHost(PrStringView url);

// Returns true if `text` holds `query`, ignoring ASCII case. Every text holds the empty query.
bool addressMatches(PrStringView text, PrStringView query);

} // namespace browser

PR_ASSUME_NONNULL_END
