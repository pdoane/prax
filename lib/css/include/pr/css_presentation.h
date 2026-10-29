// Document rendering: computed styles for each element, projected onto a layout tree.
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
// css-display-4 - https://drafts.csswg.org/css-display-4/
//
// A presentation displays one document. An update recomputes the computed styles that DOM and style sheet changes
// invalidated, applies the resulting box changes to the layout tree, and lays the tree out. Only the document's agent
// thread calls a presentation. The presentation runs background work on the agent's restyle and layout threads
// (PrDomAgent), which all presentations of the agent share.

#pragma once

#include "pr/base.h"
#include "pr/css.h"
#include "pr/dom.h"
#include "pr/draw.h"
#include "pr/geom.h"
#include "pr/layout_animation.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The style, box and layout state that displays a document.
typedef struct PrCssPresentation PrCssPresentation;

// How a presentation's document is shown, which decides its canvas (css-color-adjust-1/#color-scheme-effect). The
// canvas's color is the tree's (prLayoutTreeSetCanvasColor): the Canvas system color in the root's color scheme for a
// top-level document. A frame's canvas has that color only where its root's color scheme differs from the scheme that
// the frame's element uses, which is its document's preferred scheme, and is transparent otherwise, so that its
// embedder shows through.
enum {
    PrCssPresentationEmbedding_TopLevel,
    PrCssPresentationEmbedding_Frame,
};
typedef uint8_t PrCssPresentationEmbedding;

// Conditions on a document that decide whether a user agent style sheet applies.
enum {
    PrCssSheetCondition_Quirks = 1u << 0,           // the document is in quirks mode (html/#quirks-mode)
    PrCssSheetCondition_HtmlDocument = 1u << 1,     // the document is an HTML document (dom/#html-document)
    PrCssSheetCondition_Markdown = 1u << 2,         // the document's content type is text/markdown (RFC 7763)
};
typedef uint8_t PrCssSheetConditions;

// The elements that a user agent style sheet styles, by how each is rendered under its computed appearance
// (css-ui-4/#appearance-switching). A sheet for one appearance holds the look of that appearance. It cannot set
// `appearance` or a custom property, since the cascade decides the appearance before it applies the sheet.
enum {
    PrCssSheetAppearance_Any,
    PrCssSheetAppearance_Native,        // widgets rendered with their native appearance
    PrCssSheetAppearance_Base,          // widgets rendered with their base appearance (css-forms-1/#appearance)
};
typedef uint8_t PrCssSheetAppearance;

// A user agent style sheet. The sheet applies while the document meets all of its conditions, to the elements that its
// appearance selects. Its selectors may name the engine's private pseudo-classes.
typedef struct PrCssUserAgentSheet {
    PrStringView source;                // the sheet's CSS
    PrCssSheetConditions conditions;
    PrCssSheetAppearance appearance;
} PrCssUserAgentSheet;

// The inputs to prCssPresentationCreate. The presentation retains `document`. Its layout tree sets text that names no
// family in `font`, and shapes with `shaper` (PrLayoutTreeDesc). The presentation runs its threaded work on `agent`,
// which must be the agent that owns the document. `shaper`, `agent` and the font set of `font` must outlive the
// presentation. The rules of `userAgentSheets` apply in order, before the document's own rules. The presentation copies
// the sheets. The document's fonts (css-fonts-4/#font-face-rule) are a font set that the presentation owns, layered
// over the set of `font` and matched before it.
typedef struct PrCssPresentationDesc {
    PrDomDocument* document;
    PrTextFont* font;
    PrTextShaper* shaper;
    PrDomAgent* agent;
    const PrCssUserAgentSheet* PR_NULLABLE userAgentSheets;
    uint32_t userAgentSheetCount;
    PrCssPresentationEmbedding embedding;
} PrCssPresentationDesc;

// Creates a presentation. It has no styles and an empty layout tree until the first update.
PrCssPresentation* prCssPresentationCreate(const PrCssPresentationDesc* desc);

// Destroys the presentation, its layout tree and its computed styles.
void prCssPresentationDestroy(PrCssPresentation* PR_NULLABLE presentation);

// Synchronously updates styles and the layout tree, and lays the tree out in a viewport of `extent`
// (css-display-4/#initial-containing-block). The update is incremental: an unchanged document in the same extent costs
// nothing. When a whole subtree needs restyling, the restyle runs fork/join on the agent's restyle threads, which read
// the document concurrently while the caller blocks. Blocks first until the agent's layout thread is idle. Returns true
// if anything changed and the tree was laid out. Only in that case do the tree's stats describe this update.
bool prCssPresentationUpdate(PrCssPresentation* presentation, PrLayoutVec extent);

// Updates styles and box changes in the same way as prCssPresentationUpdate, then starts a job on the agent's layout
// thread that lays out the tree and records its scene (prLayoutTreeScene). Returns once the job starts. While the job
// runs, the layout thread owns the tree, and every call on any presentation of the agent blocks until the job finishes.
// The document may change during the job, and the next update picks up those changes.
void prCssPresentationStart(PrCssPresentation* presentation, PrLayoutVec extent);

// Sets a target duration in milliseconds for each layout job that prCssPresentationStart starts. 0, the initial value,
// means no limit. With a limit, newly added content is admitted in tree order. Each job admits as much as the measured
// layout and style rates predict will fit in the budget. The rest is deferred to later jobs, as if the parser had not
// reached it yet. Changes to content already admitted are never deferred. prCssPresentationUpdate and synchronous
// queries admit all content. A host lowers the budget when it wants the next change, such as user input, on screen
// sooner.
void prCssPresentationSetJobBudget(PrCssPresentation* presentation, double milliseconds);

// Returns true if the scene of the last job includes every change to the document. This means that nothing changed
// since, the job budget defers no added content, and no update on the caller's thread, such as a script's query of the
// layout, has laid out what the scene lacks. A host with time before its next frame starts another job when this is
// false. A scroll leaves it true, because a scene holds no offsets and the host takes them separately
// (prCssPresentationTakeScrolls).
bool prCssPresentationIsCurrent(const PrCssPresentation* presentation);

// Blocks until the agent's layout thread is idle. Returns at once if no job is running.
void prCssPresentationWait(PrCssPresentation* presentation);

// Returns true if a job is running on the agent's layout thread, which means that a blocking call would block. A host
// can keep running the page's tasks in the meantime and take the scene once the job finishes.
bool prCssPresentationIsBusy(const PrCssPresentation* presentation);

// Blocks until the agent's layout thread is idle, then returns the scene recorded by the presentation's last job. The
// caller owns the returned reference and plays the scene (prLayoutScenePlay) or commits it to a compositor with the
// presentation's tracks. Returns null if no scene was recorded since the last call.
const PrLayoutScene* PR_NULLABLE prCssPresentationTakeScene(PrCssPresentation* presentation);

// Sets the presentation's timeline to `milliseconds`. The next update samples transitions and CSS animations at this
// time. A host sets it at the start of each frame (html/#update-the-rendering). The timeline starts at zero. Does not
// block on the layout thread.
void prCssPresentationSetTime(PrCssPresentation* presentation, double milliseconds);

// Returns the tracks of the composited animations as of the last update, for a compositor to run
// (pr/layout_compositor.h). An animation is composited when it animates only opacity and transform. Its box is a layer
// whose recorded effects stay at their base values, so updates do not re-record it as the animation runs. Track times
// are on the presentation's timeline. Writes up to `capacity` tracks to `out` and returns the total count. Keyframe
// pointers stay valid until the next call.
uint32_t prCssPresentationTracks(PrCssPresentation* presentation, PrLayoutTrack* PR_NULLABLE out, uint32_t capacity);

// Returns true if a transition or a running CSS animation changes as the timeline advances.
bool prCssPresentationIsAnimating(const PrCssPresentation* presentation);

// Work counts for one update.
typedef struct PrCssPresentationStats {
    uint32_t elementsVisited;         // elements the update's walk visited, restyled or not
    uint32_t elementsStyled;          // elements whose style was recomputed
    uint32_t elementsStyledAhead;     // styled elements that were restyled in parallel before the update's walk
    uint32_t elementsStyledUnchanged; // styled elements whose new style equals their previous one, as wasted work
    uint32_t elementsStyledCached;    // styles taken from those computed for elements that matched the same rules
    uint32_t elementsAnimated;        // elements whose animations were sampled
    uint32_t boxesCreated;            // layout nodes created for DOM nodes that had none
    uint32_t boxesErased;             // layout nodes erased because their DOM node no longer generates a box
    uint32_t counterSteps;            // nodes and ends of subtrees that the counter pass walked
    uint32_t svgNodesBuilt;           // nodes of the SVG trees of svg elements, built whole or as changes reach them
} PrCssPresentationStats;

// Returns the work counts of the last update.
PrCssPresentationStats prCssPresentationStats(const PrCssPresentation* presentation);

// Returns true if the presentation's incremental work was correct and bounded. The work is correct if each element's
// computed style, which nodes generate boxes and in what order, the boxes' style records, the content of svg elements,
// the layout and the cached recordings all match a full recomputation. The work is bounded as prLayoutTreeVerify checks
// it. On failure, writes a description of the first difference to `report` if it is not null, replacing its contents.
// This is expensive, and is meant for tests and for debugging invalidation.
bool prCssPresentationVerify(PrCssPresentation* presentation, PrString* PR_NULLABLE report);

// Returns the presentation's layout tree, as of the last update. The presentation owns the tree and its node keys.
PrLayoutTree* prCssPresentationTree(PrCssPresentation* presentation);

// Returns the element or text node that generated layout `node`, or null for an anonymous box. The result is valid
// until the document next changes.
PrDomNode* PR_NULLABLE prCssPresentationNode(const PrCssPresentation* presentation, PrLayoutNode node);

// Returns the layout node generated by `node`, an element or text node, as of the last update. Returns
// kPrLayoutNodeNone if `node` generates no box.
PrLayoutNode prCssPresentationLayoutNode(const PrCssPresentation* presentation, const PrDomNode* node);

// Scrolls the scroll container of layout `node`, or the viewport for kPrLayoutNodeNone, to `offset` clamped to its
// range, in the same way as a user scroll. The scroll and scrollend events fire at the next rendering update. A host
// uses this to apply offsets that its compositor scrolled to (prLayoutCompositorTakeScrolls). Does nothing if `node` is
// not a scroll container. Does not block on the layout thread. A scroll made during a job applies when the next call
// blocks on the job.
void prCssPresentationSetScroll(PrCssPresentation* presentation, PrLayoutNode node, PrLayoutVec offset);

// Scrolls in the same way as a user's scroll by `delta` from layout `node`, or from the viewport for kPrLayoutNodeNone.
// The scroll container that is `node`, or else the one that scrolls its box (prLayoutNodeScrollContainer), takes what
// it can along the axes that a user can scroll. It passes the rest to the containers around it, out to the viewport
// (css-overscroll-1/#scroll-chaining), unless a container's overscroll behavior keeps it (PrLayoutScrollInfo::
// chainAxes). The scroll and scrollend events fire at the next rendering update, and the host takes the offsets for its
// compositor (prCssPresentationTakeScrolls). Returns the part of `delta` that passes on past the viewport, which the
// containers around a frame's document take next.
PrLayoutVec prCssPresentationScrollFrom(PrCssPresentation* presentation, PrLayoutNode node, PrLayoutVec delta);

// Writes to `out` up to `capacity` of the offsets that the document's own scrolls set since the last call, one for each
// container, for the host to give its compositor (prLayoutCompositorSetScrolls). Returns the number written. The host
// calls it until it writes none. Offsets that the host set (prCssPresentationSetScroll) are not included.
uint32_t prCssPresentationTakeScrolls(PrCssPresentation* presentation, PrLayoutSceneScroll* PR_NULLABLE out,
                                      uint32_t capacity);

// Hit tests `point` in viewport coordinates against the last layout, in paint order. Returns the deepest element or
// text node whose box is topmost at the point, or null if there is none. A point on an anonymous box hits the node of
// the nearest box that holds it, such as the element whose ::before generates the box. In the content of an outermost
// svg element, the element hit is the topmost that pointer-events makes a target there
// (SVG2/interact.html#PointerEventsProp), or the svg element if none is.
PrDomNode* PR_NULLABLE prCssPresentationHitTest(PrCssPresentation* presentation, PrLayoutVec point);

// Returns the computed style of the element, or of the pseudo-element that `pseudoElement` names, such as "::before"
// (cssom-1/#dom-window-getcomputedstyle), which the element owns. It holds read-only declarations of the resolved value
// of each property that the engine renders, brought up to date as each is read. It is empty for an element whose
// document has no presentation, and for a `pseudoElement` that starts with a colon but is not a pseudo-element
// selector. A `pseudoElement` that does not start with a colon names the element.
PrCssStyleProperties* prCssElementComputedStyle(PrDomElement* self, PrStringView pseudoElement);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
