// Browsing contexts: what a document's dynamic markup insertion asks of the browsing context that runs it, and what a
// view asks of the browsing contexts of its iframes.
//
// html - https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html

#pragma once

#include "pr/atomic.h"
#include "pr/dom.h"
#include "pr/fetch.h"
#include "pr/html_page.h"
#include "pr/markdown.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/vec.h"

#include "dom/src/dom_xml.h"
#include "html/src/html_charset.h"
#include "html/src/html_parser.h"
#include "html/src/html_performance.h"
#include "html/src/html_window.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct HtmlMarkdownBuilder;
struct PreparedScript;
struct SvgPresentation;

// A header of a request or a response that a fetch keeps.
struct HtmlFetchHeader {
    PrRawString name;
    PrRawString value;
};

// A fetch that a browsing context runs through its host's fetcher. The agent's thread makes it and uses its response.
// The fetcher's loop runs it, reads the response whole, and posts it back to the agent.
//
// `lock` guards `dropped` and `posted`. The loop posts the fetch only while it is not dropped, and holds the lock while
// it posts. So once the browsing context drops a fetch, either the post is complete or none follows. A dropped fetch
// that was not posted is cancelled by a task on the loop. The fetch has a reference for each side, and the side that
// releases the last one frees it.
//
// A document's fetch passes its result to `done`. A script's fetch (destination Empty) passes its response to
// `scriptDone`, with every header that it exposes, and sends the method, headers and body that the script gave.
struct HtmlFetch {
    // The agent's side, which the loop does not reach.
    PrHtmlPage* PR_NULLABLE page;   // null once the context no longer waits for the fetch
    HtmlFetch* PR_NULLABLE next;    // the next fetch in the context's list
    DomFetchDestination destination;
    DomFetchFn PR_NULLABLE done;
    PrFetchResponseFn PR_NULLABLE scriptDone;
    void* PR_NULLABLE data;
    void (*PR_NULLABLE drop)(void* PR_NULLABLE data);
    bool delaysLoad;            // true if its document's load event waits for it (html/#delay-the-load-event)
    bool answered;              // true once the response arrived, and a task of the window waits to use it
    bool cancelled;             // true once the script's fetch was cancelled, whose callback is then never called
    PrFetchInitiator initiator;
    HtmlFetchAnchor anchor;

    // The request, which the agent's thread writes before it posts the fetch to the loop.
    PrDomAgent* agent;
    PrFetchNetwork* PR_NULLABLE fetcher; // null for a fetch that the browsing context answers itself
    PrRawString url;
    PrRawString origin;
    PrRawString referrer;
    PrFetchMode mode;
    PrFetchCredentials credentials;
    PrFetchRedirect redirect;
    PrFetchReferrerPolicy referrerPolicy;
    PrRawString method;            // the null string for GET
    RawArray<HtmlFetchHeader> requestHeaders;
    RawArray<uint8_t> requestBody;

    // The loop's side: the fetch while it runs, and the response, which the agent's thread reads once it is posted.
    PrFetch* PR_NULLABLE fetch;
    bool loopDone;              // true once the fetch ended on the loop, or was cancelled there
    PrStatus status;            // Ok for a response, or the status of a network error
    uint16_t responseStatus;    // the response's status, or 0 for a network error
    PrFetchResponseType responseType;
    RawArray<uint8_t> body;
    PrRawString contentType;
    PrRawString contentLanguage;
    PrRawString responseUrl;                   // the response's URL, after the redirects that the fetch followed
    RawArray<HtmlFetchHeader> responseHeaders; // a script's fetch's, unfiltered
    PrRawString contentEncoding;
    PrRawString serverTiming;
    PrRawString sentReferrer;
    PrRawString responseReferrerPolicy;        // the Referrer-Policy headers joined by commas
    PrFetchTiming timing;
    PrNetError error;

    PrMutex lock;
    bool dropped;               // true once the browsing context no longer waits for the fetch
    bool posted;                // true once the loop posted the fetch to the agent
    Atomic<uint32_t> references;
};

// The fetch of the document that a navigation loads into a page (prHtmlPageNavigate). The fetcher's loop runs it and
// posts the response to the agent in pieces as they arrive: the response's head, each part of the body, and the end or
// the network error. The page loads each piece in the task that receives it.
//
// `lock` guards `dropped`. The loop posts a piece only while the load is not dropped, and holds the lock while it
// posts. So once the page drops a load, the pieces already posted find no page, and none follows. A dropped load whose
// fetch still runs is cancelled by a task on the loop. The page, the loop, and each piece in flight hold a reference,
// and the last one frees the load.
struct HtmlDocumentLoad {
    // The measurements of the fetch that the agent's thread has received, and what the navigation's entry reports.
    HtmlFetchAnchor anchor;
    PrFetchTiming timing;
    uint16_t responseStatus;
    PrRawString contentType;
    PrRawString contentEncoding;
    PrRawString serverTiming;

    // The referrer that the navigation sends, and its policy, and what the fetch sent once its policy applied.
    PrRawString referrer;
    PrFetchReferrerPolicy referrerPolicy;
    PrRawString sentReferrer;

    PrHtmlPage* PR_NULLABLE page;           // the agent's side: null once the page no longer waits for the load
    PrDomAgent* agent;
    PrFetchNetwork* fetcher;
    PrRawString url;                           // as the navigation asked for it, with its fragment

    // The serialized origin of the document that started the navigation, which the fetch is from, or the null string
    // for a navigation that the user started, whose fetch has no client.
    PrRawString origin;

    PrFetch* PR_NULLABLE fetch;             // the loop's side, while the fetch runs
    bool loopDone;                          // true once the fetch ended on the loop, or was cancelled there

    bool responded;                         // the agent's side: true once the response's head has arrived
    PrMutex lock;
    bool dropped;
    Atomic<uint32_t> references;
};

// An image that the document fetched and decoded (html/#list-of-available-images): the URL that it was fetched at,
// without its fragment, and its bytes.
struct HtmlAvailableImage {
    PrRawString url;
    PrRawString bytes;
};

// A module of a window's module map (html/#module-map), by its index in the browsing context's modules.
typedef uint32_t HtmlModuleIndex;

// Where the fetch and parse of a module stand.
enum {
    HtmlModuleState_Fetching,
    HtmlModuleState_Parsed,     // parsed by the host, with an error or without
    HtmlModuleState_Failed,     // its fetch failed, or its response is not JavaScript
};
typedef uint8_t HtmlModuleState;

// A module of the module map, or an inline module script's module, which the map does not key. Once parsed, the module
// names the modules that it imports, each of which the map holds.
struct HtmlModule {
    PrRawString url;                       // the key, or the null string for an inline module script's
    HtmlModuleState state;
    PrFetchCredentials credentials;     // the credentials mode of its fetch, which its imports' fetches take
    uint32_t record;                    // the host's number for the parsed module (PrHtmlScript::module)

    // The modules that it imports, a range of the browsing context's module requests.
    uint32_t firstRequest;
    uint32_t requestCount;
};

// A module graph that a module script or an import() waits to have fetched.
struct HtmlModuleWait {
    HtmlModuleIndex root;
    PreparedScript* PR_NULLABLE script;     // the script that becomes ready, or null for an import()
    PrHtmlImportFn PR_NULLABLE done;        // an import()'s callback, and its context
    void* PR_NULLABLE context;
};

// The load or error event of an element's content. The event holds the element (domNodeHold). It is dropped if a later
// update of that content supersedes the update that queued it.
struct HtmlElementEvent {
    PrDomElement* element;
    uint32_t generation;
    bool error;
};

// A browsing context (html/#browsing-context), which hosts a window.
struct HtmlPage {
    PrHtmlWindow* window;       // the window of its active document (html/#nav-window)
    HtmlParser parser;          // the parse that an HTML load runs
    DomXmlBuilder xml;          // the parse that an XML load runs, whose parser is null outside one
    PrHtmlHost host;
    DomDocumentHost documentHost;                 // the host through which the window's document fetches and queues
                                                  // tasks
    HtmlFetch* PR_NULLABLE fetches;               // the fetches that it waits for, newest first
    const HtmlFetch* PR_NULLABLE completingFetch; // the fetch whose callback is running
    PrRawString nextReferrer;                     // what the next startNavigation sends, or the null string for none
    PrFetchReferrerPolicy nextReferrerPolicy;
    HtmlDocumentLoad* PR_NULLABLE documentLoad;   // the navigation's fetch of its next document (prHtmlPageNavigate)

    // The failure of the navigation whose error document is loading, which the end of the load reports in its place
    // (PrHtmlHost::loaded), or Ok.
    PrStatus navigationFailure;
    uint32_t delayingFetches;         // the number of those fetches that its document's load event waits for

    // html/#list-of-scripts-that-will-execute-when-the-document-has-finished-parsing
    RawArray<PreparedScript* PR_NONNULL> deferred;

    RawArray<PreparedScript* PR_NONNULL> soon;   // html/#set-of-scripts-that-will-execute-as-soon-as-possible

    // html/#list-of-scripts-that-will-execute-in-order-as-soon-as-possible
    RawArray<PreparedScript* PR_NONNULL> inOrder;
    PreparedScript* PR_NULLABLE blocking;     // html/#pending-parsing-blocking-script
    bool advanceQueued;                       // true if a task to run what has become ready is queued

    // The window's module map, the modules that each module imports, and the graphs that wait to be fetched. A task
    // settles the waits whose graphs a fetch completed (modulesQueued), so that no import() completes during the call
    // that asked for it.
    RawArray<HtmlModule> modules;
    RawArray<HtmlModuleIndex> moduleRequests;
    RawArray<HtmlModuleWait> moduleWaits;
    bool modulesQueued;

    // True if the parse has stopped and the end (html/#the-end) is waiting for deferred scripts or for whatever delays
    // the load event.
    bool ending;

    bool contentLoaded;         // true once the end has fired DOMContentLoaded
    bool loadEnded;             // true once the end of its document's load has run, whether or not it fired load

    // True for a frame's initial about:blank (html/#is-initial-about:blank). Its load fires no event at the iframe,
    // which fires load when the iframe is inserted, or when the document that replaces it loads.
    bool initialBlank;

    // The context's ongoing navigation (html/#ongoing-navigation): the source of its iframe, which its container's
    // document fetches while this initial about:blank stands, or a navigation that a task waits to run. The task runs
    // only while its id is still the pending one, so a later navigation or a stop cancels it.
    bool awaitingSource;
    uint32_t pendingNavigation;   // the id of the navigation that waits for its task, or zero
    uint32_t insertionNavigation; // the javascript: navigation that the iframe's insertion began, or zero
    uint32_t navigations;         // the ids that navigations have taken

    // The target name (html/#nav-target). An iframe's page takes it from the iframe's name attribute.
    PrRawString targetName;

    bool settleAtEnd;           // true if the end calls the host's settle hook
    PrStatus endStatus;         // the failure of a script, which ends the load
    uint32_t locationAt;        // the input offset that `line` and `column` count up to
    uint32_t line;
    uint32_t column;
    bool loaded;                // true once a load has run, so that the next load starts a new document
    bool loading;
    bool parsing;
    bool settling;                    // true while the settle hook is on the stack
    bool continuing;                  // true while continueEnd is on the stack
    bool runningScript;               // true while a parser-blocking script is executing
    uint32_t ignoreDestructiveWrites; // html/#ignore-destructive-writes-counter

    // True if the parser is script-created (html/#script-created-parser). Its input is open for writes until
    // document.close().
    bool scriptCreated;

    // A load whose input arrives in pieces (prHtmlPageBeginLoad), and which tasks parse one slice at a time. A load of
    // a whole source that waits for a script continues as one whose input has all arrived.
    PrTextBuffer streamInput = {}; // the bytes that have arrived
    uint32_t streamRead;           // the number of them that the decoder has taken
    bool streamEnded;              // true once the last piece has arrived
    bool streamQueued;             // true if a task to parse more is queued
    bool streamSniffed;            // true once the charset of the input is known
    bool streamFlushed;            // true once the decoder has read the end of the input

    // The decoding of the bytes of a load (html/#the-input-byte-stream), and how certain the parse is of their charset.
    // The document's charset is the decoder's.
    PrCharsetDecoder decoder;
    HtmlConfidence confidence;

    // The charset that a meta element of the load named, and in which the load parses its bytes again, or
    // PrCharset_None (html/#change-the-encoding).
    PrCharset reparse;

    // The bytes of a load of a whole source while it parses them, or the null view.
    PrStringView loadSource;

    // The charset of the Content-Type of the current load, or PrCharset_None.
    PrCharset transportCharset;

    // The charset of a document whose bytes declare none (html/#encoding-sniffing-algorithm).
    PrCharset defaultCharset;

    // The builder of a Markdown document whose load streams, which the context owns, or null.
    HtmlMarkdownBuilder* PR_NULLABLE markdown;

    // The options that a Markdown document is parsed with, which its content type's variant chooses.
    PrMarkdownOptions markdownOptions;

    // The browsing contexts of its iframes, current and discarded. The context owns them.
    RawArray<PrHtmlPage* PR_NONNULL> children;

    PrCssPresentation* PR_NULLABLE presentation;    // the window document's presentation, if the host gives fonts
    SvgPresentation* PR_NULLABLE svg;               // what lib/svg keeps for the presentation

    // The display that the window is shown on and the user's preferences, which its media features describe and its
    // frames take. A frame's preferred color scheme is its iframe element's instead (htmlPageFollowFrameScheme).
    PrAppDisplay display;
    PrAppUserPreferences preferences;

    PrHtmlVisibility visibility;    // whether the page is shown to the user (html/#system-visibility-state)

    // The viewport in CSS pixels (prHtmlPageSetViewport), which the window's media features describe and its document
    // is laid out in. A frame's is its iframe's content box once the parent lays the iframe out.
    PrVec2 viewport;

    RawArray<HtmlElementEvent> elementEvents;     // events for its document's embedded content, in the order queued
    RawArray<HtmlAvailableImage> availableImages; // the images of its document that an img shows at once
    bool ownsAgent;                               // true if the context owns its window's agent and destroys it with
                                                  // itself
    bool ownsHistory;                             // true if the context owns its window's session history
};

// The dynamic markup insertion methods of a document (html/#dynamic-markup-insertion), which its browsing context
// carries out. Each returns InvalidState for an XML document, and does nothing for a document that is not a browsing
// context's current document (html/#fully-active).
PrStatus htmlDocumentOpen(PrDomDocument* document);
PrStatus htmlDocumentWrite(PrDomDocument* document, PrStringView text);
PrStatus htmlDocumentClose(PrDomDocument* document);

// Appends to `out` the browsing contexts of the iframes of the context's window that show their content, in the order
// in which the window holds them.
void htmlPageFrames(PrHtmlPage* page, RawArray<PrHtmlPage*>* out);

// Returns the browsing context whose current document is `document`, or null for any other document.
PrHtmlPage* PR_NULLABLE htmlDocumentPage(const PrDomDocument* document);

// Returns the id by which the context's scenes fill its iframe's slot in the scenes of the enclosing context
// (PrLayoutContentKind_Embed). The id is never zero.
uint64_t htmlPageEmbed(const PrHtmlPage* page);

// Navigates the browsing context whose current window is `window` to the document at `url` (html/#navigate), through
// its host, from a task. `source` is the window that starts the navigation, whose origin fetches the document, or null
// for a traversal of the session history. A javascript: URL runs only if `source` has the origin of `window`. Any call
// but a traversal replaces the ongoing navigation of the browsing context. Returns false, and does nothing, if the
// window is not its browsing context's current window or the host does not navigate.
bool htmlPageNavigate(PrHtmlWindow* window, const PrHtmlWindow* PR_NULLABLE source, PrStringView url,
    HtmlHistoryCommit commit = HtmlHistoryCommit_Push, uint32_t target = 0);

// Returns the window of the browsing context that `name` chooses for a hyperlink of `window`
// (html/#the-rules-for-choosing-a-navigable), or null if the link needs a new top-level browsing context.
PrHtmlWindow* PR_NULLABLE htmlPageChooseTarget(PrHtmlWindow* window, PrStringView name);

// Asks the host of the window's page to show `url` in a new top-level browsing context (PrHtmlHost::openWindow).
void htmlPageOpenWindow(PrHtmlWindow* window, PrStringView url);

// Sets the preferred color scheme of the browsing context of an iframe of `parent` to the scheme that the iframe uses
// in the presentation of `parent`'s document, as of its last update (css-color-adjust-1/#color-scheme-page). Changes
// nothing if the preference is already that scheme.
void htmlPageFollowFrameScheme(PrHtmlPage* parent, PrHtmlPage* page);

// The first half of prHtmlPresentationStart: brings the presentation's styles and boxes up to date, as
// cssPresentationPrepareJob does, and then what the browsing context draws in the boxes. cssPresentationPostJob is the
// second half.
void htmlPresentationPrepareJob(PrCssPresentation* presentation, PrLayoutVec extent);

// Starts a load whose input arrives in pieces, as prHtmlPageBeginLoad does, without dropping the page's document load
// (prHtmlPageNavigate), which calls it as its response arrives.
PrStatus htmlPageBeginStreamedLoad(PrHtmlPage* page, PrStringView url, const PrHtmlResponseHeaders* PR_NULLABLE headers);

} // namespace pr_impl

// C opaque types
struct PrHtmlPage : pr_impl::HtmlPage {};

PR_ASSUME_NONNULL_END
