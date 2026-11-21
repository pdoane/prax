// The animation model of a document: its timelines, the keyframe effects that its animations play, and the animations
// themselves, whether a script makes them or the styles of a presentation run them as CSS animations and transitions
// (pr/css_animations.h).
//
// web-animations-1 - https://drafts.csswg.org/web-animations-1/
// css-animations-2 - https://drafts.csswg.org/css-animations-2/
// css-transitions-2 - https://drafts.csswg.org/css-transitions-2/
//
// An animation, an effect, and a timeline that a script makes each hold a reference to their document. The document's
// own timeline is the document's, which forgets the document as the document is destroyed.
//
// The document lists the animations that may still apply an effect, run a task or notify something, in the order that
// they were made, and holds a reference to each. Only the animations whose timeline is active are listed: those of a
// document that a presentation shows. An animation leaves the list when it is idle, or finished with nothing left to
// do, and returns when it plays again. A CSS animation or transition is owned by the presentation whose styles run it,
// and stays listed while those styles do. As the presentation detaches, its animations are canceled and the list is
// emptied, which breaks the cycle between the document and the animations of its elements.

#pragma once

#include "pr/css_animations.h"
#include "pr/hash_index.h"
#include "pr/layout_animation.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

#include "css/src/css_animation.h"
#include "css/src/css_keyframes.h"
#include "css/src/css_selector.h"
#include "css/src/css_value.h"
#include "dom/src/dom_event.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssPresentation;

// The phase of an animation as its CSS events last saw it (css-animations-2/#events,
// css-transitions-2/#transition-events). Idle is the phase of an animation whose events have not run, or that has no
// current time.
enum {
    AnimationPhase_Idle,
    AnimationPhase_Before,
    AnimationPhase_Active,
    AnimationPhase_After,
};
typedef uint8_t AnimationPhase;

// The task that an animation waits to run once it is ready (web-animations-1/#pending-play-task).
enum {
    CssPendingTask_None,
    CssPendingTask_Play,
    CssPendingTask_Pause,
};
typedef uint8_t CssPendingTask;

// A keyframe of an effect, computed for its target: one longhand's value at an offset.
struct CssAnimateKeyframe {
    double offset;                  // in [0, 1]
    PrCssProperty property;
    CssValueSpan value;
    CssValueSpan easing;            // the keyframe's own easing, or empty for the effect's default
    PrCssComposite composite;

    // True if the keyframe takes the element's base value in place of `value`, as revert-layer and revert-rule roll the
    // animation origin back to the author origin (css-cascade-5/#revert-layer).
    bool base;
};

// A timeline of a document (web-animations-1/#the-documenttimeline-interface).
struct CssTimeline {
    uint32_t references;
    PrDomDocument* PR_NULLABLE document;            // null once the document of its own timeline is destroyed
    double originTime;                              // on the document's timeline
    bool own;                                       // the document's own, which holds no reference to the document
};

// A keyframe effect (web-animations-1/#keyframe-effects).
struct CssKeyframeEffect {
    uint32_t references;
    PrDomDocument* document;                        // with a reference
    PrCssAnimation* PR_NULLABLE animation;          // the animation that plays it, which holds a reference to it
    PrDomElement* PR_NULLABLE target;               // with a reference
    PrRawString pseudoText;                         // the pseudo-element selector, or the null string for the element

    // The pseudo-element that `pseudoText` names, CssPseudo_None for the element, or CssPseudo_Count for one that no
    // presentation generates or animates.
    CssPseudo pseudo;

    // The timing as it applies: a duration of auto is zero, and a fill of auto is none. `duration` and `fill` are as
    // given, and `easing` is the given easing, serialized.
    PrLayoutEffectTiming timing;
    double duration;                                // NaN for auto
    PrCssFillMode fill;
    PrRawString easing;

    PrCssComposite composite;
    PrCssComposite iterationComposite;

    // The keyframes as given, or for a CSS animation those of its @keyframes rule. Null for a transition, whose
    // keyframes are its endpoints, or for an effect without keyframes.
    CssKeyframes* PR_NULLABLE keyframes;
    CssValueList defaultEasing;            // a CSS animation's animation-timing-function, for each keyframe

    // The keyframes computed for the target's style, in order of offset, and of appearance among equal offsets, with
    // the values they refer to. A transition's are its endpoints, which it sets itself (cssEffectHasEndpoints).
    // `computed` is false until they are computed, and after the effect or the target's style changes, but for
    // endpoints, which stay computed.
    CssValuePool values;
    RawArray<CssAnimateKeyframe> frames;
    bool computed;
    bool inheritsReset;             // true if a keyframe takes the parent's value of a property that does not inherit
    bool indexed;                   // counted in the document's index of animated targets
};

// An animation (web-animations-1/#animations).
struct CssAnimation : DomCountedTarget {
    PrDomDocument* document;                        // with a reference
    PrCssAnimationKind kind;
    uint64_t sequence;                              // the order in which the document's animations were made
    PrCssKeyframeEffect* PR_NULLABLE effect;        // with a reference
    PrCssTimeline* PR_NULLABLE timeline;            // with a reference
    PrRawString id;

    // web-animations-1/#the-current-time-of-an-animation: the start time and hold time are on the timeline, unresolved
    // as NaN, and run at the playback rate. The pending playback rate applies when the pending task runs, or when the
    // animation next is not running (web-animations-1/#pending-playback-rate).
    PrLayoutPlayback playback;
    CssPendingTask pending;
    uint64_t pendingRender;                         // the document's renders when the task became pending
    bool hasPendingRate;
    double pendingRate;
    double previousCurrentTime;                     // web-animations-1/#previous-current-time
    bool finishScheduled;                           // the finish notification steps wait for a microtask
    bool listed;                                    // in the document's list, which holds a reference

    // The current ready and finished promises (web-animations-1/#current-ready-promise).
    uint32_t readyGeneration;
    uint32_t finishedGeneration;
    PrCssPromiseState ready;
    PrCssPromiseState finished;

    PrCssReplaceState replaceState;

    // A CSS animation or transition: the presentation whose styles run it, or null once they no longer do
    // (css-animations-2/#owning-element-section), its name and position in its animation-name list, or a transition's
    // property and the values of reversing (css-transitions-1/#reversing).
    CssPresentation* PR_NULLABLE owner;
    PrRawString name;
    uint32_t nameIndex;
    PrCssProperty property;
    CssValueList transitionTo;             // the transition's end value
    CssValueList reversingStart;           // css-transitions-1/#transition-reversing-adjusted-start-value
    double reversingFactor;                // css-transitions-1/#transition-reversing-shortening-factor
    bool matched;                          // scratch: an item of the owner's animation-name matched it

    // What a script changed of a CSS animation, which its style no longer changes (css-animations-2/#animations).
    PrCssTimingMembers timingOverridden;
    bool playStateOverridden;
    bool keyframesOverridden;
    bool cssPaused;                                 // as animation-play-state last gave it

    bool composited;                                // a compositor runs the effect on the target's box

    // As the CSS events last saw it.
    AnimationPhase eventPhase;
    double eventIteration;
};

// A TransitionEvent, an AnimationEvent or an AnimationPlaybackEvent that waits for the next rendering update
// (web-animations-1/#pending-animation-event-queue). It holds its target, the element that originates the animated
// pseudo-element or the animation, and the animation that queued it, which orders the events.
struct CssAnimateEvent {
    PrDomEventTarget* target;
    PrCssAnimation* animation;
    PrStringView type;              // a literal
    PrDomEventKind kind;
    CssPseudo pseudo;               // the animated pseudo-element, or CssPseudo_None for the element
    PrRawString name;               // the property, or the animation's name
    double elapsedTime;             // in seconds
    double currentTime;             // of a playback event, or NaN
    double timelineTime;            // of a playback event, or NaN
    double scheduled;               // the time on the document's timeline that orders the events, or NaN
};

// A target that effects animate, and the effects of animations that target it, in the order that they came to.
struct CssAnimatedTarget {
    const PrDomElement* element;
    CssPseudo pseudo;
    RawArray<PrCssKeyframeEffect* PR_NONNULL> effects;
};

// A document's animations.
struct CssAnimations {
    double time;                         // the document timeline's current time, in milliseconds
    bool active;                         // a presentation shows the document, and its window advances the timeline
    PrCssTimeline* PR_NULLABLE timeline; // the document's own, made on first use
    uint64_t sequence;                   // the sequence of the next animation made

    // The restyles of the document's presentation, and the timeline's time at the last. A pending animation is ready
    // once a restyle has styled it, which can render it, at the time of that restyle (web-animations-1/#ready).
    uint64_t renders;
    double renderTime;

    RawArray<PrCssAnimation* PR_NONNULL> list;
    RawArray<CssAnimateEvent> events;
    bool recordsEvents;             // a window updates the rendering and dispatches the events
    PrCssAnimationCallback callback;

    // The targets of the effects that animations play, so that styling a target finds them at once.
    RawArray<CssAnimatedTarget> targets;
    HashIndex targetIndex;

    // Scratch of sampling.
    CssValuePool scratch;               // values that a target's effects override
    RawArray<PrCssProperty> overridden; // property of each override; its value is the span at the same index
    RawArray<CssValueSpan> overrides;
    RawArray<PrLayoutTrack> tracks;     // tracks that cssAnimateTracks made last
    RawArray<PrLayoutKeyframe> keyframes;
};

// Returns the document's animations.
CssAnimations& cssAnimations(PrDomDocument* document);

// Frees the document's animations as the document is destroyed. The list is empty by then.
void cssAnimationsReset(CssAnimations* animations);

// Returns the end time of the effect (web-animations-1/#animation-effect-end-time).
double cssEffectEndTime(const PrCssKeyframeEffect* effect);

// Returns the animation's current time, or NaN.
double cssAnimationCurrentTime(const PrCssAnimation* animation);

// Returns the timing of the animation's effect at its current time. The animation has an effect.
PrLayoutComputedTiming cssAnimationComputedTiming(const PrCssAnimation* animation);

// Forgets the effect's computed keyframes, unless they are a transition's endpoints (cssEffectHasEndpoints), and
// restyles its target in the document's presentation.
void cssAnimationEffectChanged(PrCssKeyframeEffect* effect);

// Returns true if the effect's keyframes are a transition's endpoints, which the transition sets as it starts. They
// stay computed: no change to the effect or to its target's style computes them again (PrCssKeyframeEffect::computed).
bool cssEffectHasEndpoints(const PrCssKeyframeEffect* effect);

// Lists the effect with its target in the document's index while the effect has an animation and a target, or takes it
// out.
void cssAnimationIndexEffect(PrCssKeyframeEffect* effect);

// Returns the index of the target in the document's index of animated targets, or kHashMissing.
uint32_t cssAnimatedTarget(const CssAnimations& animations, const PrDomElement* element, CssPseudo pseudo);

// Lists the animation if it runs, and keeps it listed. cssAnimationUnlistIdle drops those that no longer run.
void cssAnimationRelist(PrCssAnimation* animation);
void cssAnimationUnlistIdle(CssAnimations& animations);

// Makes an animation of the document, idle, with no effect and no timeline, with one reference that the caller holds.
PrCssAnimation* cssAnimationMake(PrDomDocument* document, PrCssAnimationKind kind);

// The procedures of web-animations-1/#playing-an-animation-section and the sections that follow it, which the methods
// and the styles of CSS animations run.
void cssAnimationResetPendingTasks(PrCssAnimation* animation);
void cssAnimationUpdateFinishedState(PrCssAnimation* animation, bool didSeek, bool sync);
void cssAnimationFinishNotification(PrCssAnimation* animation);
PrStatus cssAnimationPlay(PrCssAnimation* animation, bool autoRewind);
PrStatus cssAnimationPause(PrCssAnimation* animation);
void cssAnimationCancel(PrCssAnimation* animation);
PrStatus cssAnimationSilentlySetCurrentTime(PrCssAnimation* animation, double time);

// Runs the animation's pending task at `readyTime` on its timeline (web-animations-1/#pending-play-task).
void cssAnimationRunPendingTask(PrCssAnimation* animation, double readyTime);

// Returns the time on the animation's timeline at which its pending task is ready to run, as the document's timeline
// moves to `time`: the time of the frame that rendered it while it was pending, or else `time`.
double cssAnimationReadyTime(const PrCssAnimation* animation, double time);

// Queues an AnimationPlaybackEvent of the animation, if a window dispatches the document's events.
void cssAnimationQueuePlaybackEvent(PrCssAnimation* animation, PrStringView type, double currentTime, double scheduled);

// Returns true if `a` comes before `b` in composite order (web-animations-1/#animation-composite-order): transitions,
// then CSS animations, then the rest, each in the order that their classes define.
bool cssAnimationBefore(const PrCssAnimation* animation, const PrCssAnimation* other);

} // namespace pr_impl

// C opaque types
struct PrCssTimeline : pr_impl::CssTimeline {};
struct PrCssKeyframeEffect : pr_impl::CssKeyframeEffect {};
struct PrCssAnimation : pr_impl::CssAnimation {};

PR_ASSUME_NONNULL_END
