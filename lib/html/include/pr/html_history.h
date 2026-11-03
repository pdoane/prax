// Session history and the History interface: the entries of a tab's documents, and what script does with them.
//
// html - https://html.spec.whatwg.org/multipage/browsing-the-web.html#session-history
// html - https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-history-interface

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A top-level traversable's session history (html/#tn-session-history-entries): an entry for each document that its
// browsing contexts loaded and for each same-document navigation within them, and which entry is current. A host keeps
// one for a tab across the browsing contexts that its navigations make (PrHtmlHost::sessionHistory). It is used from
// the thread of those browsing contexts.
typedef struct PrHtmlSessionHistory PrHtmlSessionHistory;

// The events that a traversal fires (html/#popstateevent, html/#hashchangeevent).
typedef struct PrHtmlPopStateEvent PrHtmlPopStateEvent;
typedef struct PrHtmlHashChangeEvent PrHtmlHashChangeEvent;

// ---------------------------------------------------------------------------------------------------------------------
// Session history (html/#tn-session-history-entries)

// Creates an empty session history, and destroys one. The browsing contexts that use it must be destroyed first.
PrHtmlSessionHistory* prHtmlSessionHistoryCreate(void);
void prHtmlSessionHistoryDestroy(PrHtmlSessionHistory* PR_NULLABLE history);

// Returns the number of entries, the index of the current one, and the URL of the entry at `index`, or the null view
// past the end.
uint32_t prHtmlSessionHistoryLength(const PrHtmlSessionHistory* history);
uint32_t prHtmlSessionHistoryIndex(const PrHtmlSessionHistory* history);
PrStringView prHtmlSessionHistoryUrl(const PrHtmlSessionHistory* history, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// History (html/#the-history-interface), over the session history of the window's browsing context
// (PrHtmlHost::sessionHistory). Each function that returns a status returns NotAllowed, the attribute's SecurityError,
// if the window's document is not fully active (html/#fully-active).

// html/#dom-history-scroll-restoration
enum {
    PrHtmlScrollRestoration_Auto,
    PrHtmlScrollRestoration_Manual,
};
typedef uint8_t PrHtmlScrollRestoration;

// html/#dom-history-length: the number of entries.
PrStatus prHtmlHistoryLength(const PrHtmlWindow* window, uint32_t* out);

// html/#dom-history-state: the current entry's state, as the bytes that a script serialized it to
// (html/#structuredserializeforstorage), or the null view for null. `generation` changes each time the history state is
// set (html/#restore-the-history-object-state), so that a binding deserializes it once for each time.
PrStatus prHtmlHistoryState(const PrHtmlWindow* window, PrStringView* out, uint32_t* generation);

// html/#dom-history-pushstate and html/#dom-history-replacestate: `serialized` is the state, or the null view for null,
// and `url` the URL to change to, or the null view or the empty string for the document's. Returns Syntax for a URL
// that does not parse, and NotAllowed for one that the document cannot have (html/#can-have-its-url-rewritten).
PrStatus prHtmlHistoryPushState(PrHtmlWindow* window, PrStringView serialized, PrStringView url, PrHtmlHistoryHandling handling);

// html/#dom-history-go: traverses the session history by `delta` in a task, as prHtmlPageTraverse does.
PrStatus prHtmlHistoryGo(PrHtmlWindow* window, int32_t delta);

// html/#dom-history-scroll-restoration: the scroll restoration mode of the current entry.
PrStatus prHtmlHistoryScrollRestoration(const PrHtmlWindow* window, PrHtmlScrollRestoration* out);
PrStatus prHtmlHistorySetScrollRestoration(PrHtmlWindow* window, PrHtmlScrollRestoration mode);

// ---------------------------------------------------------------------------------------------------------------------
// PopStateEvent : Event (html)
//
// A popstate event that the engine fires carries the history state of the window that it is fired at, which a binding
// reads from the window's history (prHtmlHistoryState). The state of an event that script creates is a value of the
// script's language, which the binding holds.

// A PopStateEvent as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlPopStateEvent, DomEvent, Event)

// Returns `object` as a PopStateEvent, or null if it is not one.
PrHtmlPopStateEvent* PR_NULLABLE prHtmlPopStateEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a PopStateEvent of `type`, whose state the caller's binding holds. The caller holds the one reference.
PrHtmlPopStateEvent* prHtmlPopStateEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
    bool hasUAVisualTransition);

// Returns true if the engine fired the event, which then carries its window's history state.
bool prHtmlPopStateEventCarriesHistoryState(const PrHtmlPopStateEvent* event);

// html/#dom-popstateevent-hasuavisualtransition
bool prHtmlPopStateEventHasUAVisualTransition(const PrHtmlPopStateEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// HashChangeEvent : Event (html)

// A HashChangeEvent as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlHashChangeEvent, DomEvent, Event)

// Returns `object` as a HashChangeEvent, or null if it is not one.
PrHtmlHashChangeEvent* PR_NULLABLE prHtmlHashChangeEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a HashChangeEvent of `type` from `oldUrl` to `newUrl`. The caller holds the one reference.
PrHtmlHashChangeEvent* prHtmlHashChangeEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
    PrStringView oldUrl, PrStringView newUrl);

// html/#dom-hashchangeevent-oldurl and html/#dom-hashchangeevent-newurl
PrStringView prHtmlHashChangeEventOldUrl(const PrHtmlHashChangeEvent* event);
PrStringView prHtmlHashChangeEventNewUrl(const PrHtmlHashChangeEvent* event);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
