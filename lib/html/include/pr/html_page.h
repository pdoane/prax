// Native hosting of HTML documents.
//
// html - https://html.spec.whatwg.org/multipage/document-sequences.html

#pragma once

#include "pr/app_display.h"
#include "pr/app_user_preferences.h"
#include "pr/audio_mixer.h"
#include "pr/byte_view.h"
#include "pr/charset.h"
#include "pr/css_presentation.h"
#include "pr/draw_renderer.h"
#include "pr/fetch.h"
#include "pr/gpu.h"
#include "pr/html.h"
#include "pr/html_history.h"
#include "pr/html_storage.h"
#include "pr/image.h"
#include "pr/media.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A page: a Window with its document, the loading of that document, and the pages of its iframes. A host makes the
// top-level pages, and a page makes the page of each of its iframes.
typedef struct PrHtmlPage PrHtmlPage;

// The kinds of script that the host runs.
enum {
    PrHtmlScriptKind_Classic,
    PrHtmlScriptKind_Module,
};
typedef uint8_t PrHtmlScriptKind;

// A script to execute. Its string views are valid for the duration of the execute call.
typedef struct PrHtmlScript {
    PrStringView source;
    PrStringView url;                   // the URL that the source came from
    PrStringView baseUrl;               // the URL that imports resolve against
    PrHtmlScriptKind kind;
    bool external;                      // true if the source came from the src attribute

    // The line and column at which the source starts, counting from one. Columns count UTF-16 code units.
    uint32_t line;
    uint32_t column;

    uint32_t module;                    // for a module script to execute, the module that the host parsed of it
} PrHtmlScript;

// Called with the URL of each module that a module imports (PrHtmlHost::parseModule).
typedef void (*PrHtmlModuleRequestFn)(void* PR_NULLABLE context, PrStringView url);

// Called once with the outcome of prHtmlPageImportModule.
typedef void (*PrHtmlImportFn)(void* PR_NULLABLE context, PrStatus status);

// The response headers that a page reads (fetch/#concept-response-header-list). Each member holds the header's value,
// or the null view if the response has none.
typedef struct PrHtmlResponseHeaders {
    PrStringView contentType;           // Content-Type: the resource's MIME type
    PrStringView contentLanguage;       // Content-Language: the languages of the resource's audience
    PrStringView referrerPolicy;        // Referrer-Policy (referrer-policy/#parse-referrer-policy-from-header), or null
} PrHtmlResponseHeaders;

// Which animation frames a page's event loop runs (html/#update-the-rendering).
enum {
    // Every frame that an animation wants. A host that shows the document presents all of them.
    PrHtmlAnimationFrames_All,
    // Only the frames that something observes: frames with frame requests, events or other work to do, and the last
    // frame before each animation event, timer or task. A frame that would change nothing but animated styles is
    // skipped. The next frame runs at the time that it would have had, so scripts and events see the same values as if
    // no frame had been skipped. This mode is for a host that shows no animation, such as a test harness that steps its
    // own clock.
    PrHtmlAnimationFrames_Observed,
};
typedef uint8_t PrHtmlAnimationFrames;

// The host's side of a page. Each callback is called synchronously with `context`.
typedef struct PrHtmlHost {
    void* PR_NULLABLE context;

    // The fetcher of the resources that the documents load (html/#fetching-resources), the documents of their iframes
    // included, and of the document that a navigation loads (prHtmlPageNavigate). The page starts each fetch on the
    // fetcher's loop, from the document's origin and with its URL as the referrer, and the agent's event loop receives
    // the whole response in a task. A navigation that a document starts, by a hyperlink or by setting its location, is
    // fetched from that document's origin. A traversal of the session history, like the user's own navigations, is
    // fetched from no origin (PrFetchRequestDesc::origin). A script, a style sheet or a font whose status is not ok
    // fails as a network error does. The element whose resource fails fires `error`, and the load continues. A data:
    // URL is answered by the page itself, during the call that asks for it. The fetcher must outlive the page and the
    // tasks that it posts to the loop: a host destroys the fetcher on the loop after the page, so that the cancels of
    // the fetches that it dropped run first. If null, an iframe's page uses its parent's fetcher, and any other fetch
    // fails as a network error.
    PrFetchNetwork* PR_NULLABLE fetcher;

    // Runs a script. A module script runs the module that `parseModule` made of it, once every module that its graph
    // imports is parsed: the host reports the first parse error in the graph, or else links and evaluates it. A failure
    // aborts the load with its status. Null disables scripting.
    PrStatus (*PR_NULLABLE execute)(void* PR_NULLABLE context, PrHtmlPage* page, const PrHtmlScript* script);

    // Parses `script`, a module script, into a module of the page's realm without evaluating it
    // (html/#creating-a-javascript-module-script), and returns the host's number for the module (PrHtmlScript::module).
    // Calls `request` with `requestContext` and the URL of each module that the module imports, resolved against the
    // script's base URL (html/#resolve-a-module-specifier). A module that does not parse, or that names a specifier
    // that does not resolve, keeps its error and imports nothing. The page fetches and parses each module of the
    // window's module map once (html/#module-map). Null runs no module scripts.
    uint32_t (*PR_NULLABLE parseModule)(void* PR_NULLABLE context, PrHtmlPage* page,
        const PrHtmlScript* script, PrHtmlModuleRequestFn request, void* PR_NULLABLE requestContext);

    // Runs after the deferred scripts and before the load event, and may run the window's timers. A failure aborts the
    // load with its status.
    PrStatus (*PR_NULLABLE settle)(void* PR_NULLABLE context, PrHtmlPage* page);

    // Compiles `body` into the callback of the event handler `handler` of `target`
    // (html/#getting-the-current-value-of-the-event-handler). `body` is the text of the event handler content attribute
    // of `element`. Writes the callback to `out` with one reference. If `body` does not compile, reports the error and
    // returns false. Null leaves every such handler null.
    bool (*PR_NULLABLE compile)(void* PR_NULLABLE context, PrHtmlPage* page,
        PrDomEventTarget* target, PrDomElement* element, PrDomEventHandler handler, PrStringView body,
        PrDomEventHandlerCallback* out);

    // Runs `source`, the script of a javascript: URL, as a classic script in the page's realm
    // (html/#evaluate-a-javascript:-url), and reports any error that it throws. Returns true and writes the value to
    // `out` if the script completes with a string, which the caller releases with prRawStringReset. Null runs no
    // javascript: URLs.
    bool (*PR_NULLABLE evaluate)(void* PR_NULLABLE context, PrHtmlPage* page, PrStringView source, PrRawString* out);

    // Queues `run`, called with `data`, as a microtask (html/#queue-a-microtask). The host runs it along with its
    // script engine's jobs. If null, no microtasks run.
    void (*PR_NULLABLE microtask)(void* PR_NULLABLE context, PrHtmlPage* page, void (*PR_NONNULL run)(void* data), void* data);

    // Sets `child` to the host of a page that is about to be created for an iframe of `parent`. `child` starts as the
    // default host. Null gives each such page the default host, which runs no scripts.
    void (*PR_NULLABLE frame)(void* PR_NULLABLE context, PrHtmlPage* parent, struct PrHtmlHost* child);

    // Asks the host to show `url` in a new top-level browsing context with no opener, because a hyperlink's target
    // names no browsing context that the page can reach (html/#the-rules-for-choosing-a-navigable). The call is made
    // from the page's event loop. It consumes the window's transient activation, and a window without one makes no call
    // (html/#popup-blocker). If null, such a hyperlink does nothing.
    void (*PR_NULLABLE openWindow)(void* PR_NULLABLE context, PrHtmlPage* page, PrStringView url);

    // Called when the page's window opens, which prHtmlPageWindow then returns: once the page is created, before
    // anything loads in it, and each time a navigation commits a document, which has a window of its own
    // (html/#initialise-the-document-object).
    void (*PR_NULLABLE windowOpened)(void* PR_NULLABLE context, PrHtmlPage* page);

    // Called when a document commits, once its window's time origin is set and before any of its content runs. A host
    // that steps a clock of its own advances it here, since a document's scripts run after its time origin.
    void (*PR_NULLABLE documentCommitted)(void* PR_NULLABLE context, PrHtmlPage* page);

    // Called before the page's window closes: before the window of a document that a navigation commits takes its
    // place, and before the page is destroyed. A closed window runs nothing, and lives while references to it remain
    // (prDomEventTargetRetain).
    void (*PR_NULLABLE windowClosed)(void* PR_NULLABLE context, PrHtmlPage* page);

    // Called when the host's page is about to be destroyed, after its window closes.
    void (*PR_NULLABLE closed)(void* PR_NULLABLE context, PrHtmlPage* page);

    // Called once the load of a document ends (html/#the-end), after its load event or after the failure that aborted
    // it. `status` is that failure's status, or Ok. A load that a new load replaces before it ends makes no call.
    void (*PR_NULLABLE loaded)(void* PR_NULLABLE context, PrHtmlPage* page, PrStatus status);

    // Returns the states among `wanted` that `element` is in. These are the states that the application knows and the
    // engine does not (PrDomHostState). If null, no states are reported. A restyle may call it from several threads at
    // once, while the document does not change.
    PrDomHostState (*PR_NULLABLE elementState)(void* PR_NULLABLE context, PrHtmlPage* page,
        const PrDomElement* element, PrDomHostState wanted);

    // The agent whose event loop runs the page, and whose threads run its rendering. It must outlive the page. If null,
    // a top-level page gets a new agent, and an iframe's page uses the agent of its parent.
    PrDomAgent* PR_NULLABLE agent;

    // The text font and shaper that the page renders its documents' text with (PrCssPresentationDesc). The shaper and
    // the font's set must outlive the page. If either is null, no document is rendered, and the elements' computed
    // styles are empty. If they are null, an iframe's page uses its parent's font and shaper.
    PrTextFont* PR_NULLABLE font;
    PrTextShaper* PR_NULLABLE shaper;

    // The codec that decodes the images that the documents show, which must outlive the page. If null, no image is
    // decoded: an image still has the natural size that its header records, and draws nothing. If null, an iframe's
    // page uses its parent's codec. The codec also encodes the bitmaps of canvases (prHtmlCanvasElementToDataUrl).
    PrImageCodec* PR_NULLABLE images;

    // The codec that reads the media resources of video and audio elements, and decodes the frames of their video,
    // which must outlive the page. If null, no media resource plays. If null, an iframe's page uses its parent's codec.
    PrMediaCodec* PR_NULLABLE media;

    // The mixer that plays the audio of video and audio elements, which must outlive the page. If null, media elements
    // play without sound, on the clock. If null, an iframe's page uses its parent's mixer.
    PrAudioMixer* PR_NULLABLE mixer;

    // The device that the renderer presenting the documents renders on, or null if that renderer renders in memory. It
    // must outlive the page. The page keeps content that the renderer draws into on that device, such as the bitmaps of
    // canvases. If null, an iframe's page uses its parent's device.
    PrGpuDevice* PR_NULLABLE device;

    // The codec that decompresses fonts that the documents load in compressed form, such as WOFF files. It must outlive
    // the page. If null, such fonts are not loaded. If null, an iframe's page uses its parent's codec.
    PrCompressCodec* PR_NULLABLE compress;

    // The providers that the documents' local storage and session storage are kept in (PrHtmlStorageProvider), which
    // must outlive the page. If null, an iframe's page uses its parent's provider of that kind, and any other
    // document's storage of that kind throws a SecurityError, as for a user who disables it.
    PrHtmlStorageProvider* PR_NULLABLE localStorage;
    PrHtmlStorageProvider* PR_NULLABLE sessionStorage;

    // The session history that the page's documents take entries in, which must outlive it. If null, the page keeps one
    // of its own, whose first entry is its initial about:blank document's, as an iframe's page does.
    PrHtmlSessionHistory* PR_NULLABLE sessionHistory;

    // The monotonic clock, in milliseconds, that the window's timers, animation frames and animations run on. It never
    // goes back. If null, the system's clock is used. A host that steps a clock of its own runs the event loop
    // deterministically.
    double (*PR_NULLABLE now)(void* PR_NULLABLE context);

    // The wall clock that the window's dates read, in milliseconds since 1970 (prHtmlWindowCurrentTime). If null, it is
    // the wall time of the window's time origin plus the time since then on `now`. A host that steps `now` gives a wall
    // clock that also moves while a script runs, so that a script that waits on dates in a loop ends.
    double (*PR_NULLABLE wallNow)(void* PR_NULLABLE context);

    // Which animation frames the event loop runs. An iframe's page uses the value from its own host.
    PrHtmlAnimationFrames animationFrames;

    // The navigator compatibility mode of the window's Navigator (PrHtmlNavigatorCompatibilityMode), which agrees with
    // the User-Agent string of the fetcher. An iframe's host starts with the mode of its parent's host.
    PrHtmlNavigatorCompatibilityMode navigatorCompatibilityMode;

    // Called at the end of each rendering update (html/#update-the-rendering). If set, the update first brings the
    // document's presentation fully up to date, including layout, so that the host can check it
    // (prCssPresentationVerify). This is the same work that a geometry read does. If null, nothing is called, and the
    // update computes only styles.
    void (*PR_NULLABLE rendered)(void* PR_NULLABLE context, PrHtmlPage* page);
} PrHtmlHost;

// Returns the default host, with every member null. Its navigator compatibility mode is
// PrHtmlNavigatorCompatibilityMode_Gecko, which names no vendor. It runs no scripts.
static inline PrHtmlHost prHtmlHostDefault(void) {
    PrHtmlHost value = {.navigatorCompatibilityMode = PrHtmlNavigatorCompatibilityMode_Gecko};
    return value;
}

// Creates the Window and its initial Document without executing scripts. The page owns the Window, everything that the
// Window owns, and the pages of its iframes. The page holds a reference to its document, and copies `host`. A null host
// disables scripting.
PrHtmlPage* prHtmlPageCreate(const PrHtmlHost* PR_NULLABLE host);

// Destroys the page and its Window. First destroys the runtime of the Window's scripts, and every object that refers to
// the Window, such as an event targeted at it. The document lives on while references to it remain.
void prHtmlPageDestroy(PrHtmlPage* PR_NULLABLE page);

// Traverses the page's session history by `delta` entries (html/#traverse-the-history-by-a-delta), as a user's back and
// forward buttons do. An entry of the window's document becomes current in a task, which sets the document's URL and
// history state and fires popstate, and hashchange if the fragment changed. For an entry of another document, the task
// navigates the page to its URL (prHtmlPageNavigate), and the document that loads takes that entry and its state rather
// than pushing one. A delta of 0 reloads the current entry's document in the same way. Does nothing for a delta past
// either end.
void prHtmlPageTraverse(PrHtmlPage* page, int32_t delta);

// Returns the page's Window, which the page owns.
PrHtmlWindow* prHtmlPageWindow(PrHtmlPage* page);

// Returns the page whose Window is `window`, or null once the window's document is no longer the page's current
// document.
PrHtmlPage* PR_NULLABLE prHtmlWindowPage(PrHtmlWindow* window);

// Returns the agent whose event loop runs the page's tasks (prDomAgentRunTask). This is the host's agent, or the one
// that the page created.
PrDomAgent* prHtmlPageAgent(PrHtmlPage* page);

// Parses `source` into a new Document at `url`, runs its scripts, and fires the Window's load event. `headers` are the
// response's headers, or null for none. The XML parser parses an XML MIME type (mimesniff/#xml-mime-type). Another
// image type, or a video or audio type, makes a media document (html/#read-media). Its one element fetches the resource
// at `url`, and `source` is not read. Markdown (text/markdown) makes an HTML document whose body holds what
// prHtmlSetMarkdown gives, with lib/markdown's default options, and the document runs no script. The HTML parser parses
// any other type, or a response without a Content-Type. The first language of the Content-Language header is the
// document's default language if no pragma sets one (html/#pragma-set-default-language). If an XML document is not
// well-formed, its content is replaced with a parsererror element that describes the error, in the same way as for
// DOMParser.
//
// The parse waits in tasks for each script that blocks it and is fetched, and the end of the load waits for whatever
// delays it, as for a streamed load. A document that fetches nothing loads within this call. If the URL parser rejects
// `url`, returns its status and leaves the current Document in place. Returns InvalidState during another load.
PrStatus prHtmlPageLoad(PrHtmlPage* page, PrStringView source, PrStringView url,
    const PrHtmlResponseHeaders* PR_NULLABLE headers);

// Starts loading a new Document at `url` whose source arrives in pieces, as a network delivers it
// (html/#navigate-html). prHtmlPageAppendLoad delivers each piece, and prHtmlPageEndLoad marks the end of the source.
// Tasks of the event loop parse what has arrived, one slice at a time. Rendering opportunities come between those
// tasks, so the document renders as it arrives. The parse stops (html/#the-end) in the task that parses the end of the
// source. A media document is made at once, and ignores the pieces. Returns the same statuses as prHtmlPageLoad.
PrStatus prHtmlPageBeginLoad(PrHtmlPage* page, PrStringView url, const PrHtmlResponseHeaders* PR_NULLABLE headers);
void prHtmlPageAppendLoad(PrHtmlPage* page, PrStringView bytes);
void prHtmlPageEndLoad(PrHtmlPage* page);

// Navigates the page to the document at `url` (html/#navigate), an absolute URL that may have a fragment, as a user
// does by entering an address: fetches it through the host's fetcher, from no document and so with no origin and no
// referrer, and loads the response as it arrives (prHtmlPageBeginLoad), at the response's URL with `url`'s fragment,
// with the response's headers. The current document stays until the response arrives. A network error, or a URL that
// the fetcher does not take, loads a document that says why in its place, and the end of that load reports Io
// (PrHtmlHost::loaded). Another navigation or load replaces a navigation in flight, and prHtmlPageIsFetching is true
// until its response has arrived whole. A document that is still loading stops (html/#abort-a-document). Returns the
// URL parser's status if it rejects `url`, and leaves that document loading. Returns InvalidState if the host has no
// fetcher, or the call comes from a script of the page or from within a load of the page.
PrStatus prHtmlPageNavigate(PrHtmlPage* page, PrStringView url);

// Fetches the module graph at `url`, an absolute URL, as a script's import() does (html/#hostloadimportedmodule): each
// module that the window's module map lacks is fetched and parsed with the host's parseModule, which the host must
// have. Calls `done` with `context` in a task, with Ok once every module of the graph is parsed, or with the status of
// a fetch that failed. If the window's document is replaced or the page destroyed first, `done` is called with Lost
// during that call.
void prHtmlPageImportModule(PrHtmlPage* page, PrStringView url, PrHtmlImportFn done, void* PR_NULLABLE context);

// Fetches what `request` asks for a script of the window, through the host's fetcher. The window's document is the
// request's client: the request has its origin, its URL as the referrer, and its base URL. Calls `done` with `context`
// once: in a task with the response or a network error, or with Lost if the window's document is replaced or the page
// destroyed first. The body is read whole before the response is passed. The fetch does not delay the document's load
// event. Returns InvalidData, and never calls `done`, for a URL that does not parse or that has a username or a
// password (fetch/#dom-request).
PrStatus prHtmlPageFetch(PrHtmlPage* page, const PrFetchClientRequest* request, PrFetchResponseFn done,
    void* PR_NULLABLE context);

// Cancels the fetch that prHtmlPageFetch started with `context` and has not answered (fetch/#fetch-controller-abort).
// The fetch stops if it still runs, and its `done` is never called. Does nothing if no such fetch waits.
void prHtmlPageCancelFetch(PrHtmlPage* page, void* PR_NULLABLE context);

// Drops the tasks that the page's window has queued and its performance observers. A script engine calls this for each
// of its pages before it frees its runtime, because they refer to the runtime's objects.
void prHtmlPageDropScriptWork(PrHtmlPage* page);

// Returns true while the page's navigation (prHtmlPageNavigate) waits for its response, which the current document
// stays in place for.
bool prHtmlPageIsNavigating(const PrHtmlPage* page);

// Returns true while the page waits for the response to a fetch that it started (PrHtmlHost::fetcher), or for the whole
// of its navigation's document (prHtmlPageNavigate).
bool prHtmlPageIsFetching(const PrHtmlPage* page);

// Returns true if the Window's document is render-blocked (html/#render-blocked). Its first rendering waits until the
// parser reaches its body, past any sheets that its head adds, and until the sheets that its parser added there arrive.
// Its rendering is not updated in the meantime, and a host paints nothing of it.
bool prHtmlPageIsRenderBlocked(PrHtmlPage* page);

// Sets a media feature of the environment that the Window's media queries are evaluated against
// (mediaqueries-5/#media). The feature is one of @media's descriptors, and `value` is written as in a query, such as
// `1024px` for PrCssDescriptor_MediaWidth. The feature is not computed from others. Returns false, and leaves the
// environment unchanged, if the feature does not accept `value`.
bool prHtmlPageSetMediaFeature(PrHtmlPage* page, PrCssDescriptor feature, PrStringView value);

// Sets the media type of that environment (mediaqueries-5/#media-types).
void prHtmlPageSetMediaType(PrHtmlPage* page, PrCssMediaType type);

// Sets the charset of an HTML document whose bytes, response and parent document give it none
// (html/#encoding-sniffing-algorithm). It is windows-1252 until a host sets it, and the page's frames take it. The spec
// suggests a default that depends on the user's locale.
void prHtmlPageSetDefaultCharset(PrHtmlPage* page, PrCharset charset);

// Sets the extent of the viewport in CSS pixels, which is 800 by 600 until a host sets it. The window's media queries
// see it, and the document is laid out in it, including the layout that a script's query brings up to date. A frame's
// viewport is its iframe's content box, which its parent sets.
void prHtmlPageSetViewport(PrHtmlPage* page, PrVec2 extent);

// Sets the display that the document is shown on, which is prAppDisplayDefault until a host sets it. The media features
// that describe a display follow it: resolution, color-gamut and dynamic-range. The layout snaps to its pixel ratio,
// and colors resolve for its headroom. Only the styles that read the headroom are projected again when the headroom
// alone changes. The page's frames are shown on the same display.
void prHtmlPageSetDisplay(PrHtmlPage* page, const PrAppDisplay* display);

// Sets the user's preferences, which are neutral until a host sets them. The user preference media features follow
// them, below any override that script makes. The page's frames take the same preferences, except the preferred color
// scheme, which is the scheme that a frame's iframe element uses (css-color-adjust-1/#color-scheme-page).
void prHtmlPageSetUserPreferences(PrHtmlPage* page, const PrAppUserPreferences* preferences);

// Sets whether the page is shown to the user (html/#system-visibility-state), which it is until a host sets it. The
// page's frames take the same visibility. A change updates the visibility state of each window's document, which fires
// visibilitychange at it in a task (html/#update-the-visibility-state).
void prHtmlPageSetVisibility(PrHtmlPage* page, PrHtmlVisibility visibility);

// Restyles the elements that can be in `states`, which are states that the host reports (PrHtmlHost::elementState), in
// the window's document and in the documents of the page's frames. A host calls it once what it knows changes, as when
// the user visits a URL that links point to, so that the elements match their selectors again.
void prHtmlPageInvalidateHostStates(PrHtmlPage* page, PrDomHostState states);

// Returns the presentation of the window's document. For each document that the page loads, it creates a presentation
// with the user agent style sheet of html/#rendering, if its host gives a text font and a shaper. Returns null if the
// host gives none. A new document replaces the presentation.
PrCssPresentation* PR_NULLABLE prHtmlPagePresentation(PrHtmlPage* page);

// Brings a presentation up to date, as prCssPresentationUpdate does, together with what the browsing context draws in
// its boxes: the content of the outermost svg elements of a page's presentation. Returns true if anything was laid out.
bool prHtmlPresentationUpdate(PrCssPresentation* presentation, PrLayoutVec extent);

// Starts a job of the agent's layout thread for a presentation, as prCssPresentationStart does, with what the browsing
// context draws in its boxes brought up to date first.
void prHtmlPresentationStart(PrCssPresentation* presentation, PrLayoutVec extent);

// Returns the time on the host's clock at which the Window's timeline starts (hr-time-3/#sec-time-origin). The
// timeline's time, which the presentation's animations run on, is the clock's time minus this value.
double prHtmlPageTimeOrigin(PrHtmlPage* page);

// Returns the URL of the icon that represents the window's document (html/#rel-icon), which a host fetches and shows
// beside the page's title. It is the last link in tree order whose rel holds icon, skipping a link whose type the
// host's image codec cannot decode. A document at an http: or https: URL with no such link has the icon at
// /favicon.ico. Returns the null string for any other document. The caller releases the string with prRawStringReset.
PrRawString prHtmlPageIconUrl(PrHtmlPage* page);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
