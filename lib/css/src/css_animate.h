// How a presentation runs a document's animations: it starts and cancels the CSS animations and transitions that its
// styles name, computes the keyframes of the effects that target what it styles, applies their values over each
// target's base style in the animation and transition origins (css-cascade-5/#cascade-origin-animation), hands the
// composited effects to its compositor, and moves the document's timeline as its window updates the rendering.
//
// css-transitions-1 - https://drafts.csswg.org/css-transitions-1/
// css-transitions-2 - https://drafts.csswg.org/css-transitions-2/
// css-animations-1 - https://drafts.csswg.org/css-animations-1/
// css-animations-2 - https://drafts.csswg.org/css-animations-2/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/
//
// A target is an element or a pseudo-element that the presentation generates (CssPresentationPseudo), which animates as
// an element does. The CSS events of a pseudo-element's animations go to its originating element, and name the
// pseudo-element (css-transitions-1/#interface-transitionevent).

#pragma once

#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/layout_animation.h"
#include "pr/raw_array.h"

#include "css/src/css_animations.h"
#include "css/src/css_style.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The context that a target's keyframes compute relative to. A pseudo-element's parent style is the style of what
// originates it.
struct CssAnimateContext {
    const CssComputedStyle* PR_NULLABLE parent;
    CssFontBases rootFont;          // font bases of the root element, which root-relative lengths are relative to
    CssViewport viewport;
};

// Starts, changes and cancels the target's transitions and CSS animations as its base style changes from `oldBase` to
// `base` (css-transitions-1/#starting, css-animations-1/#animations), and computes the keyframes of every effect that
// targets it. `old` is how `oldBase` presented with its animations. The old styles are null for a target styled for the
// first time, which starts no transition. Colors resolve in `environment`, as they do where the target's box presents
// them.
void cssAnimateRestyled(PrCssPresentation* presentation, const PrDomNode* target,
    const CssComputedStyle* PR_NULLABLE oldBase, const CssComputedStyle* PR_NULLABLE old, const CssComputedStyle* base,
    const CssAnimateContext& context, const CssColorEnvironment& environment);

// Returns the target's style with its effects at the document's time applied over `base`, with one reference that the
// caller holds. Returns `base` itself if no effect applies. The composited effects apply only if `composited` is true,
// because otherwise a compositor runs them and the target's box keeps its base effects. Colors that interpolate resolve
// in `environment`, so that a color for a display's headroom keeps its range.
const CssComputedStyle* cssAnimateApply(PrCssPresentation* presentation, const PrDomNode* target,
    const CssComputedStyle* base, bool composited, const CssColorEnvironment& environment);

// Returns true if the target has a composited effect that applies now or later. The target's box is a layer for such an
// effect.
bool cssAnimateComposited(const PrCssPresentation* presentation, const PrDomNode* target);

// Return the layout node that presents a target, and the style of the target without its animations.
typedef PrLayoutNode (*AnimationNodeFn)(const PrCssPresentation* presentation, const PrDomNode* target);
typedef const CssComputedStyle* PR_NULLABLE (*AnimationBaseFn)(const PrCssPresentation* presentation, const PrDomNode* target);

// Makes a track of each composited effect on the box of its target's layout node, which `layoutNode` gives. Each value
// is computed against the target's base style, which `baseOf` gives. Returns the tracks, which stay valid until the
// next call.
const RawArray<PrLayoutTrack>& cssAnimateTracks(PrCssPresentation* presentation, AnimationNodeFn layoutNode,
    AnimationBaseFn baseOf);

// Returns true if an effect that an animation plays targets the target.
bool cssAnimateHas(const PrCssPresentation* presentation, const PrDomNode* target);

// Cancels the presentation's CSS animations and transitions of the target, whose styles no longer run them, as the
// target leaves the presentation.
void cssAnimateForget(PrCssPresentation* presentation, const PrDomNode* target);

// Makes the document's timeline active, as the presentation attaches, or cancels the CSS animations and transitions
// that the presentation runs and empties the document's list as it detaches.
void cssAnimateAttach(PrCssPresentation* presentation);
void cssAnimateDetach(PrCssPresentation* presentation);

// Moves the document's timeline to `time`, and marks the targets whose effects show something different. Runs the
// animations' pending tasks, updates their finished state, removes the animations that others replace, and queues the
// events of the CSS animations and transitions (web-animations-1/#update-animations-and-send-events).
void cssAnimateUpdate(PrCssPresentation* presentation, double time);

// Records that the presentation styled the document, which can render the animations pending at the time, and so makes
// them ready at the timeline's time.
void cssAnimateRendered(PrCssPresentation* presentation);

// Queues the cancel event of a CSS animation or transition that is canceled before its effect ends
// (css-animations-2/#events, css-transitions-2/#transition-events).
void cssAnimateQueueCancelEvent(PrCssAnimation* animation);

// Returns the events that wait for dispatch, in the order to dispatch them, and leaves the queue empty.
RawArray<CssAnimateEvent> cssAnimateTakeEvents(PrDomDocument* document);

// Releases what an event holds.
void cssAnimateEventReset(CssAnimateEvent* event);

// Returns true if an animation has an event, or a change of its effect, still to come as time moves on.
bool cssAnimateHaveEventsAhead(PrDomDocument* document);

// Returns the time on the document's timeline, after its current time, at which the next event of an animation is due.
// Returns the current time if an event is due now, and -1 if no event is ahead. Until that time, the passage of time
// changes only the styles that the effects animate.
double cssAnimateNextEventTime(PrDomDocument* document);

// Returns true if an effect changes as the document's time advances.
bool cssAnimateIsAnimating(PrDomDocument* document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
