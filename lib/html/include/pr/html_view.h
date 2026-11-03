// HTML views: a top-level browsing context's document on a host's screen.
//
// A view is how a host presents a browsing context. The view owns the steps between the event loop and the screen. It
// sizes the viewport, which the window's media environment and the document's layout follow. It brings the rendering
// through the pipeline of the context's agent, and commits each scene that the agent's layout thread records, with the
// composited animations, to a compositor (pr/layout_compositor.h). It passes the scroll offsets that the compositor
// applied back to the document. It routes the host's input to the document's elements.
//
// A host calls a view on the agent's thread: as each of the host's frames starts, as the event loop's tasks run, and as
// the agent's layout thread finishes its jobs (prDomAgentSetLayoutDone).
//
// A host with a compositor scrolls a user's wheel there at once, on its own thread, unless the compositor says that the
// wheel waits for the document (prLayoutCompositorWheelBlocks). It passes every wheel to the view, with
// PrHtmlViewEventFlag_Scrolled if it scrolled the wheel itself.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/draw.h"
#include "pr/html_page.h"
#include "pr/layout_compositor.h"
#include "pr/layout_tree.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The presentation of a browsing context's document.
typedef struct PrHtmlView PrHtmlView;

// The result of an update.
typedef struct PrHtmlViewUpdate {
    bool committed;                 // true if a scene was committed to the compositor
    bool started;                   // true if a job started on the agent's layout thread
    bool busy;                      // true if a job is running on the agent's layout thread after the update
    bool current;                   // true if the last scene shows every change, and no job runs or needs to start
    PrVec2 extent;                  // the viewport now, which a job started by this update lays out in
    PrVec2 committedExtent;         // if committed, the viewport that the committed scene was laid out in
    double committedMillis;         // if committed, how long the committed job took on the layout thread
    double startMillis;             // if started, how long this thread took to prepare and post the job
    bool startedForView;            // if started, the viewport, the frames' sizes or the pixel ratio changed
    bool startedForPage;            // if started, the window's document has changes that the last scene lacks
    uint32_t startedForFrames;      // if started, the iframes whose documents have changes that their scenes lack
    uint32_t framesStyled;          // if started, the elements styled in the iframes' documents
    uint32_t framesAnimated;        // if started, the elements whose animations were sampled in those documents
    uint32_t framesBlocks;          // if committed, the block boxes laid out in the iframes' documents
    PrLayoutTreeStats layout;       // if committed, the statistics of the job that recorded the scene
    PrCssPresentationStats style;   // if started, the statistics of bringing the job's styles and boxes up to date
} PrHtmlViewUpdate;

// Called on the agent's thread with the result of each update that the view's agent runs (prDomAgentAddUpdate).
typedef void (*PrHtmlViewUpdatedFn)(void* PR_NULLABLE context, const PrHtmlViewUpdate* update);

// Parameters for creating a view.
typedef struct PrHtmlViewDesc {
    // The browsing context to present, which must outlive the view. Its host gives it a text font and a shaper.
    PrHtmlPage* page;

    // The compositor that presents the scenes, which must outlive the view. If null, the view commits nothing. This is
    // for a host that records the document itself (prHtmlViewPaint).
    PrLayoutCompositor* PR_NULLABLE compositor;

    // The approximate duration of each job of the layout thread, in milliseconds, or 0 for no limit. The presentation
    // interprets it as prCssPresentationSetJobBudget describes.
    double jobBudget;

    // True if the host fills what lies beneath each list that prHtmlViewPaint records with the document's canvas color,
    // as its render pass's clear does, so that the list records no fill for the canvas
    // (PrLayoutScenePlayOptions::canvasFilled). A host that presents through the compositor says so to the compositor.
    bool canvasFilled;

    // Told of each update that the agent runs, if not null. This is for a host that reports what the view's jobs did.
    PrHtmlViewUpdatedFn PR_NULLABLE updated;
    void* PR_NULLABLE updatedContext;
} PrHtmlViewDesc;

// Creates a view of the browsing context's documents, in the page's viewport (prHtmlPageSetViewport). The view follows
// the page from one document to the next. A view with a compositor adds an update to the page's agent
// (prDomAgentAddUpdate), so that each run of the agent's tasks (prDomAgentRun) brings the rendering to the compositor
// (prHtmlViewUpdate).
PrHtmlView* prHtmlViewCreate(const PrHtmlViewDesc* desc);

// Waits for the agent's layout thread, and frees the view.
void prHtmlViewDestroy(PrHtmlView* PR_NULLABLE view);

// Sets the viewport's extent in CSS pixels. The window's media queries see it, and the document is laid out in it.
void prHtmlViewSetViewport(PrHtmlView* view, PrVec2 extent);

// Sets the display that the view is shown on, and the user's preferences (prHtmlPageSetDisplay,
// prHtmlPageSetUserPreferences). Scenes are committed and painted at the display's pixel ratio.
void prHtmlViewSetDisplay(PrHtmlView* view, const PrAppDisplay* display);
void prHtmlViewSetUserPreferences(PrHtmlView* view, const PrAppUserPreferences* preferences);

// Advances the view's rendering without waiting. The scroll offsets that the compositor applied are passed to the
// document immediately. The rest of the update is skipped if the document is render-blocked or the agent's layout
// thread is running a job. Otherwise, the scene of the view's last job is committed to the compositor with the
// composited animations. A pointer move that waited for the job hovers the element under the pointer. A new job starts
// if the document, its scroll offsets or the viewport changed since the last job.
PrHtmlViewUpdate prHtmlViewUpdate(PrHtmlView* view);

// Sets the application whose clipboard a text control's cut, copy and paste use, which must outlive the view. With
// none, they do nothing. A paste's text comes back as a Clipboard event, which the host passes to the view.
void prHtmlViewSetApp(PrHtmlView* view, PrApp* PR_NULLABLE app);

// Sets whether a text control's caret is shown, which it is by default. A host that compares renders hides it, as a
// test runner does, since a screenshot of a blinking caret says nothing about the page.
void prHtmlViewSetCaretHidden(PrHtmlView* view, bool hidden);

// Sets the time that a text control's caret blinks by, in seconds of the clock that events carry. A posted frame sets
// it (prHtmlViewPostFrame). A caret that no time reaches is steady.
void prHtmlViewSetTime(PrHtmlView* view, double time);

// Waits for the agent's layout thread to finish its jobs.
void prHtmlViewWait(PrHtmlView* view);

// Records the document into `list` in CSS pixels, as the view's next frame would show it. The call brings the
// document's styles and layout up to date on the caller's thread, waits for the agent's layout thread, and plays the
// resulting scene with its composited animations sampled at the document's current time (prLayoutSceneSampleTracks), as
// a compositor's frame at that time shows them. The layout snaps to the view's pixel ratio, and the caller renders the
// list at that pixel ratio. Records nothing while the document is render-blocked.
//
// Returns the color of the document's canvas (prLayoutSceneCanvasColor), which the list fills unless the host does
// (PrHtmlViewDesc::canvasFilled), or transparent while the document is render-blocked.
PrColor16 prHtmlViewPaint(PrHtmlView* view, PrDrawList* list);

// Brings the document up to date for prHtmlViewPaint without recording it: its styles and layout, and a text control's
// caret and selection. Computing a style starts the fetches that it names. A host runs what those fetches deliver, and
// repeats this until they deliver nothing, so that it records the document once.
void prHtmlViewPrepare(PrHtmlView* view);

// Returns what the view reports to the window, in viewport px, which a host places (prAppOutputPlaced). The cursor is
// what the document under the pointer shows there (css-ui-4/#cursor). The view finds it again at each hover, and after
// a commit of a scene, at most every so often while scenes follow each other and always after the last of them. It
// reads the cursor property of the element hit, in the innermost document under the pointer. The view shows no cursor
// images, so the cursor is the keyword that ends the property's value. The value auto shows the text cursor over an
// editable element, and over text that the user can select: text that user-select does not exclude, outside any widget
// other than a text control. Auto shows the arrow anywhere else. The cursor is the arrow until the pointer has been
// over the view.
//
// The text input is where a focused text control's caret stands in the last layout.
PrAppOutput prHtmlViewOutput(const PrHtmlView* view);

// What the host did with an event before passing it to the view.
enum {
    PrHtmlViewEventFlag_Scrolled = 1u << 0,    // the host scrolled the compositor by this Scroll already
};
typedef uint8_t PrHtmlViewEventFlags;

// What the document did with an event.
enum {
    PrHtmlViewEventResult_Ignored,      // the document had no use for it
    PrHtmlViewEventResult_Canceled,     // a listener canceled it, and its default action did not run
    PrHtmlViewEventResult_Handled,      // the document dispatched it, and its default action ran if it has one
    PrHtmlViewEventResult_FocusLeft,    // the focus moved out of the document, past its last focusable area
};
typedef uint8_t PrHtmlViewEventResult;

// Routes a host's input to the document. A position is in the viewport, in CSS pixels, as the document was last laid
// out. Text that `event` holds is read during the call only.
//
// The pointer and the wheel reach into the document of a frame whose content box is under them, and its events carry
// positions in that document's viewport. An element that the pointer hovers or activates in a frame's document makes
// the frame match :hover or :active in the document around it. Keys go to the focused element of the innermost focused
// frame. What keys and the wheel scroll that a frame's document cannot take passes on to the containers around the
// frame.
//
// The pointer follows pointerevents4/#mouse-event-order. A move hovers the element under the pointer no later than the
// next update, and an update hovers once for all the moves since the previous one. A scroll by the compositor also
// hovers the element that comes under a pointer that has not moved. A press makes the element under the pointer active
// and fires mousedown at it. A release fires mouseup, and then, if the primary button was pressed, fires click at the
// nearest element that contained both the press and the release.
//
// Keys follow uievents/#events-keyboard-event-order, at the focused element, or else the body or the document element.
// A keydown that no listener cancels runs the key's default action: Enter activates a focused link or button, Space
// activates a focused button as it is released, and the keys that scroll move the nearest scroll container that can
// move their way. The text of a key fires a keypress for each character, unless the key's keydown was canceled or the
// key is held with Control or Meta. An input method's composition fires the composition events
// (uievents/#events-compositionevents), and the keys it takes report Process.
//
// A Scroll fires wheel (pointerevents4/#interface-wheelevent) with its deltas in CSS pixels, and a line of the device's
// is kPrAppScrollLineHeight pixels. A sequence of wheels (kPrLayoutWheelSequenceGap) is one gesture, which keeps the
// target of its first wheel while that target keeps its box. The first wheel can be canceled if a listener on its path
// is not passive, and the rest can be canceled only if a listener canceled the first. A wheel that no listener cancels
// scrolls from its target's box outward, as a user's scroll does, unless the host scrolled it already
// (PrHtmlViewEventFlag_Scrolled). Once the first wheel is not canceled, the view releases the rest of the sequence in
// its compositor (prLayoutCompositorReleaseWheel).
//
// The events that the view fires are trusted.
PrHtmlViewEventResult prHtmlViewEvent(PrHtmlView* view, const PrAppEvent* event, PrHtmlViewEventFlags flags);

// Pass what a host's window gives the view from the window's thread, which is the compositor's, to the agent's thread.
// Each is applied in a task of the agent: an event as prHtmlViewEvent dispatches it, with a copy of its text; a frame's
// extent, display and preferences as prHtmlViewSetViewport, prHtmlViewSetDisplay and prHtmlViewSetUserPreferences set
// them; and a traversal of the session history by `delta` as prHtmlPageTraverse runs it. A wheel scrolls the compositor
// at once, unless the compositor says that it waits for the document (prLayoutCompositorWheelBlocks), and its event
// then goes to the document with PrHtmlViewEventFlag_Scrolled. What is posted after the view is destroyed is dropped.
void prHtmlViewPostEvent(PrHtmlView* view, const PrAppEvent* event);
void prHtmlViewPostFrame(PrHtmlView* view, const PrAppFrame* frame);
void prHtmlViewPostTraverse(PrHtmlView* view, int32_t delta);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
