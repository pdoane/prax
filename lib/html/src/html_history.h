// Session history: the entries of a traversable's documents, and the History interface over them.
//
// html - https://html.spec.whatwg.org/multipage/browsing-the-web.html#session-history
// html - https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-history-interface

#pragma once

#include "pr/html.h"
#include "pr/html_history.h"
#include "pr/html_page.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A session history entry (html/#session-history-entry).
struct HtmlHistoryEntry {
    PrRawString url;
    PrRawString state;          // html/#she-classic-history-api-state, serialized, or null
    uint64_t document;          // the number of the document whose entry it is (HtmlSessionHistory::documents)
    PrHtmlScrollRestoration scrollRestoration;
    bool initial;               // the entry of a browsing context's initial about:blank, which its first load replaces
};

// What the next document that a browsing context of the history commits does with its entry (htmlHistoryCommit).
enum {
    HtmlHistoryCommit_Push,     // pushes an entry after the current one, dropping the entries after it
    HtmlHistoryCommit_Traverse, // takes the entry `target`, which keeps its state, as a traversal does
    HtmlHistoryCommit_Replace,  // replaces the entry `target`, as location.replace() does
};
typedef uint8_t HtmlHistoryCommit;

// A traversable's session history (html/#tn-session-history-entries), and its current entry
// (html/#tn-current-session-history-step).
struct HtmlSessionHistory {
    RawArray<HtmlHistoryEntry> entries;
    uint32_t index;
    uint64_t documents;         // the number given to the last document that committed an entry
    HtmlHistoryCommit commit;
    uint32_t target;
};

// Sets how the next commit takes its entry. A commit of the initial about:blank is not affected.
void htmlHistoryExpect(HtmlSessionHistory* history, HtmlHistoryCommit commit, uint32_t target);

// Commits a new document at `url`, which takes an entry as htmlHistoryExpect last said, or replaces the initial
// about:blank's entry if it is current, and otherwise pushes one. An `initial` document is a browsing context's initial
// about:blank, which pushes an entry that the next commit replaces. Returns the document's number.
uint64_t htmlHistoryCommit(HtmlSessionHistory* history, PrStringView url, bool initial);

// Frees the entries.
void htmlHistoryReset(HtmlSessionHistory* history);

// Fires popstate at the window, whose history state the event carries, and queues a hashchange from `oldUrl` to the
// document's URL if their fragments differ (html/#update-document-for-history-step-application).
void htmlHistoryNotify(PrHtmlWindow* window, PrStringView oldUrl);

// Adds an entry for the window's document at its URL, with `state`, which it takes, or replaces the current entry with
// it, as the URL and history update steps and a fragment navigation do (html/#url-and-history-update-steps,
// html/#navigate-fragid). The document of an initial about:blank replaces its entry. The history state becomes `state`.
void htmlHistoryUpdate(PrHtmlWindow* window, PrRawString state, PrHtmlHistoryHandling handling);

// Queues a traversal of the window's session history by `delta` (prHtmlPageTraverse).
void htmlHistoryTraverse(PrHtmlWindow* window, int32_t delta);

} // namespace pr_impl

// C opaque types
struct PrHtmlSessionHistory : pr_impl::HtmlSessionHistory {};

PR_ASSUME_NONNULL_END
