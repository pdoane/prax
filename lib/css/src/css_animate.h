// The animations that a presentation runs. These are the transitions that changes to an element's style start, and the
// CSS animations that its style names. They apply over the element's base style as the animation and transition origins
// (css-cascade-5/#cascade-origin-animation).
//
// An animation's target is an element or a pseudo-element that the presentation generates (CssPresentationPseudo),
// which animates as an element does. Its events go to the originating element, and name the pseudo-element
// (css-transitions-1/#interface-transitionevent).
//
// css-transitions-1 - https://drafts.csswg.org/css-transitions-1/
// css-animations-1 - https://drafts.csswg.org/css-animations-1/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/
//
// Animations run on the presentation's timeline, whose time the host sets each frame. An animation keeps copies of the
// values that it interpolates, so nothing that it holds refers to a style that may be freed.

#pragma once

#include "pr/array.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/layout_animation.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include "css/src/css_animation.h"
#include "css/src/css_selector.h"
#include "css/src/css_style.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssPresentation;

// The phase of an animation's effect (web-animations-1/#animation-effect-phases-and-states), as its events last saw it.
// The phase is idle before the first rendering update of the animation.
enum {
    AnimationPhase_Idle,
    AnimationPhase_Before,
    AnimationPhase_Active,
    AnimationPhase_After,
};
typedef uint8_t AnimationPhase;

// A transition of one property of an element or a pseudo-element (css-transitions-1/#running-transition).
struct CssRunningTransition {
    const PrDomNode* target;
    PrCssProperty property;
    double start;                   // time at which the delay starts, on the presentation's timeline
    double delay;                   // milliseconds, which may be below zero
    double duration;
    CssValueList easing;
    CssValueList from;
    CssValueList to;
    bool composited;                // true if the transition is of opacity or transform, which a compositor runs
    AnimationPhase phase;           // phase as the transition's events last saw it
};

// The value of one property in a keyframe (css-animations-1/#keyframes), stored in the animation's pool.
struct CssAnimateKeyframe {
    double offset;                  // in [0, 1]
    PrCssProperty property;
    CssValueSpan value;
    CssValueSpan easing;            // the keyframe's own easing, or empty to use the animation's easing
    CssComposite composite;         // the keyframe's own animation-composition, or else the animation's

    // True if the keyframe takes the element's base value in place of `value`, as revert-layer and revert-rule roll the
    // animation origin back to the author origin (css-cascade-5/#revert-layer).
    bool base;
};

// A CSS animation of an element or a pseudo-element (css-animations-1/#animations). It holds one item of the target's
// animation-name list, and the keyframes of the @keyframes rule that the item names, computed for the target.
struct CssRunningAnimation {
    const PrDomNode* target;
    PrString name;
    PrLayoutTiming timing;          // on the presentation's timeline
    bool matched;                   // scratch: true if an item of the target's list names the animation
    bool composited;                // true if the keyframes set only opacity and transform, which a compositor runs
    bool inheritsReset;             // true if a keyframe takes the parent's value of a property that does not inherit
    AnimationPhase phase;           // phase as the animation's events last saw it
    double iteration;               // iteration that the events last saw, in the active phase
    CssValueList easing;
    CssValuePool values;                 // values of the keyframes
    Array<CssAnimateKeyframe> keyframes; // in order of offset, and of appearance among equal offsets
};

// A TransitionEvent or an AnimationEvent that is due at the next rendering update. It holds its element (domNodeHold),
// which is the originating element of an animated pseudo-element.
struct CssAnimateEvent {
    const PrDomElement* element;
    CssPseudo pseudo;               // the animated pseudo-element, or CssPseudo_None for the element
    PrStringView type;              // a literal
    PrString name;                  // the property, or the animation's name
    double elapsedTime;             // in seconds
    double scheduled;               // time of the event on the presentation's timeline, which orders the events
    bool transition;
};

// A presentation's animations, by target.
struct CssAnimateState {
    double time;                    // current time of the presentation's timeline, in milliseconds
    Array<CssRunningTransition> transitions;
    Array<CssRunningAnimation> animations;
    CssValuePool scratch;            // values that an application overrides
    Array<PrCssProperty> overridden; // property of each override; its value is the span at the same index
    Array<CssValueSpan> overrides;
    Array<PrLayoutTrack> tracks;    // tracks that cssAnimateTracks made last
    Array<PrLayoutKeyframe> keyframes;
    Array<CssAnimateEvent> events; // in the order they were found
    bool recordsEvents;            // true if a window updates the presentation's rendering and fires its events
};

// The context that a target's animations compute relative to. A pseudo-element's parent style is the style of what
// originates it.
struct CssAnimateContext {
    const CssComputedStyle* PR_NULLABLE parent;
    CssFontBases rootFont;          // font bases of the root element, which root-relative lengths are relative to
    CssViewport viewport;
};

// Starts, changes and cancels the target's transitions and CSS animations as its base style changes from `oldBase` to
// `base` (css-transitions-1/#starting, css-animations-1/#animations). `old` is how `oldBase` presented with its
// animations. The old styles are null for a target styled for the first time, which starts no transition. Colors
// resolve in `environment`, as they do where the target's box presents them.
void cssAnimateRestyled(PrCssPresentation* presentation, const PrDomNode* target,
                        const CssComputedStyle* PR_NULLABLE oldBase, const CssComputedStyle* PR_NULLABLE old,
                        const CssComputedStyle* base,
                        const CssAnimateContext& context, const CssColorEnvironment& environment);

// Returns the target's style with its animations at the presentation's time applied over `base`, with one reference
// that the caller holds. Returns `base` itself if no animation applies. The composited animations apply only if
// `composited` is true, because otherwise a compositor runs them and the target's box keeps its base effects. Drops the
// transitions that have ended. Colors that interpolate resolve in `environment`, so that a color for a display's
// headroom keeps its range.
const CssComputedStyle* cssAnimateApply(PrCssPresentation* presentation, const PrDomNode* target,
                                        const CssComputedStyle* base, bool composited,
                                        const CssColorEnvironment& environment);

// Returns true if the target has a composited animation that has an effect now or later. The target's box is a layer
// for such an animation.
bool cssAnimateComposited(const PrCssPresentation* presentation, const PrDomNode* target);

// Return the layout node that presents a target, and the style of the target without its animations.
typedef PrLayoutNode (*AnimationNodeFn)(const PrCssPresentation* presentation, const PrDomNode* target);
typedef const CssComputedStyle* PR_NULLABLE (*AnimationBaseFn)(const PrCssPresentation* presentation,
                                                               const PrDomNode* target);

// Makes a track of the effect of each composited animation on the box of its target's layout node, which `layoutNode`
// gives. Each value is computed against the target's base style, which `baseOf` gives. Returns the tracks, which stay
// valid until the next call.
const Array<PrLayoutTrack>& cssAnimateTracks(PrCssPresentation* presentation, AnimationNodeFn layoutNode,
                                             AnimationBaseFn baseOf);

// Returns true if the target has a transition or a CSS animation.
bool cssAnimateHas(const PrCssPresentation* presentation, const PrDomNode* target);

// Drops the target's animations, and cancels those that have not ended.
void cssAnimateForget(PrCssPresentation* presentation, const PrDomNode* target);

// Finds the events of the animations whose phase or iteration changed since the last call
// (css-transitions-1/#transition-events, css-animations-1/#events). Queues them together with the events of the
// animations canceled since then.
void cssAnimateFindEvents(PrCssPresentation* presentation);

// Returns true if an animation has an event still to come as time moves on. Such an animation is a transition, which
// ends, a CSS animation that has not started, or a CSS animation that runs and ends. The iterations of an infinite
// animation do not count, so a page with one can be idle once it has started.
bool cssAnimateHaveEventsAhead(const CssAnimateState* animations);

// Returns the time on the presentation's timeline, after its current time, at which the next event of an animation is
// due. An event is the start, iteration or end of a transition or a CSS animation, as cssAnimateFindEvents finds them.
// Returns the current time if an event is due that the animation's events have not seen, and -1 if no event is ahead.
// Until that time, the passage of time changes only the styles that the animations animate.
double cssAnimateNextEventTime(const CssAnimateState* animations);

// Returns true if the animation changes with time, which means that it is neither paused nor finished.
bool cssAnimationRunning(const CssAnimateState* animations, const CssRunningAnimation* animation);

// Frees what the animations hold.
void cssAnimateReset(CssAnimateState* animations);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
