// Web Animations: a document's timelines, the animations that run on them, and their keyframe effects.
//
// web-animations-1 - https://drafts.csswg.org/web-animations-1/
// web-animations-2 - https://drafts.csswg.org/web-animations-2/
// css-animations-2 - https://drafts.csswg.org/css-animations-2/
// css-transitions-2 - https://drafts.csswg.org/css-transitions-2/
//
// Every animation of a document is a PrCssAnimation: one that a script makes, and each CSS animation and transition
// that the document's styles run, which a script reaches as a CSSAnimation or a CSSTransition. An animation plays an
// effect on a timeline. Its effect is a keyframe effect, which animates properties of an element or of one of its
// pseudo-elements.
//
// A document's timeline advances as its window updates its rendering, by the time that the host gives the frame
// (prCssPresentationSetTime). At each update the animations run their pending play and pause tasks, update their
// finished state, and queue their events, which the window dispatches
// (web-animations-1/#update-animations-and-send-events). Only the document's agent thread calls these functions.
//
// The functions that a script calls through the bindings follow the interfaces' algorithms. One that the interface lets
// throw returns PrStatus_InvalidState for an InvalidStateError, PrStatus_InvalidData for a TypeError and
// PrStatus_Syntax for a SyntaxError.

#pragma once

#include "pr/base.h"
#include "pr/css.h"
#include "pr/dom.h"
#include "pr/float.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Timelines (web-animations-1/#timelines)

// A timeline of a document: its own timeline, or one that a script makes with an origin time
// (web-animations-1/#the-documenttimeline-interface).
typedef struct PrCssTimeline PrCssTimeline;

// Returns the document's timeline (web-animations-1/#dom-document-timeline).
PrCssTimeline* prCssDocumentTimeline(PrDomDocument* document);

// Makes a timeline of the document whose zero is `originTime` on the document's timeline, with one reference that the
// caller holds.
PrCssTimeline* prCssTimelineCreate(PrDomDocument* document, double originTime);

// Adds or releases a reference. The last release frees the timeline.
void prCssTimelineRetain(PrCssTimeline* timeline);
void prCssTimelineRelease(PrCssTimeline* PR_NULLABLE timeline);

// Returns the timeline's current time in milliseconds, or NaN while it is inactive. A document's timelines are active
// while its window updates its rendering.
double prCssTimelineCurrentTime(const PrCssTimeline* timeline);

// ---------------------------------------------------------------------------------------------------------------------
// Keyframe effects (web-animations-1/#the-keyframeeffect-interface)

// A keyframe effect, which an animation plays. It is reference counted.
typedef struct PrCssKeyframeEffect PrCssKeyframeEffect;

// An animation, which plays a keyframe effect on a timeline. It is reference counted, and an EventTarget.
typedef struct PrCssAnimation PrCssAnimation;

// How an effect fills outside its active interval (web-animations-1/#enumdef-fillmode). Auto fills as None does for a
// keyframe effect.
enum {
    PrCssFillMode_None,
    PrCssFillMode_Forwards,
    PrCssFillMode_Backwards,
    PrCssFillMode_Both,
    PrCssFillMode_Auto,
};
typedef uint8_t PrCssFillMode;

// The direction that an effect's iterations play in (web-animations-1/#enumdef-playbackdirection).
enum {
    PrCssPlaybackDirection_Normal,
    PrCssPlaybackDirection_Reverse,
    PrCssPlaybackDirection_Alternate,
    PrCssPlaybackDirection_AlternateReverse,
};
typedef uint8_t PrCssPlaybackDirection;

// How an effect combines with the value under it (web-animations-1/#enumdef-compositeoperation), and, for a keyframe,
// Auto for the effect's own (web-animations-1/#enumdef-compositeoperationorauto).
enum {
    PrCssComposite_Replace,
    PrCssComposite_Add,
    PrCssComposite_Accumulate,
    PrCssComposite_Auto,
};
typedef uint8_t PrCssComposite;

// The timing that an effect is given (web-animations-1/#the-effecttiming-dictionaries). A duration that is NaN is
// `auto`, which is zero for a keyframe effect.
typedef struct PrCssEffectTiming {
    double delay;
    double endDelay;
    double iterationStart;
    double iterations;
    double duration;
    PrStringView easing;            // the text of an <easing-function>
    PrCssFillMode fill;
    PrCssPlaybackDirection direction;
} PrCssEffectTiming;

// The members of an OptionalEffectTiming that are given (web-animations-1/#dictdef-optionaleffecttiming).
enum {
    PrCssTimingMember_Delay = 1u << 0,
    PrCssTimingMember_EndDelay = 1u << 1,
    PrCssTimingMember_IterationStart = 1u << 2,
    PrCssTimingMember_Iterations = 1u << 3,
    PrCssTimingMember_Duration = 1u << 4,
    PrCssTimingMember_Easing = 1u << 5,
    PrCssTimingMember_Fill = 1u << 6,
    PrCssTimingMember_Direction = 1u << 7,
    PrCssTimingMember_All = 0xff,
};
typedef uint8_t PrCssTimingMembers;

// Returns the default timing: no delays, one iteration of `auto` duration, linear easing, auto fill, normal direction.
PrCssEffectTiming prCssEffectTimingDefault(void);

// The timing of an effect at its animation's current time (web-animations-1/#dictdef-computedeffecttiming). A time or
// progress that is unresolved is NaN.
typedef struct PrCssComputedTiming {
    double endTime;
    double activeDuration;
    double localTime;
    double progress;
    double currentIteration;
} PrCssComputedTiming;

// A property of a script's keyframe, by its name in its CSS spelling, which may be a shorthand or a custom property,
// and its value as written.
typedef struct PrCssKeyframeProperty {
    PrStringView name;
    PrStringView value;
} PrCssKeyframeProperty;

// One keyframe of a script's keyframes (web-animations-1/#processing-a-keyframes-argument). `offset` is NaN if the
// keyframe gives none. `computedOffset` is the offset that the keyframes of a property-indexed form are spaced at, or
// NaN to space the keyframe between those around it. A property whose value does not parse animates nothing.
typedef struct PrCssKeyframe {
    double offset;
    double computedOffset;
    PrStringView easing;            // the text of an <easing-function>, or the null view for linear
    PrCssComposite composite;
    const PrCssKeyframeProperty* PR_NULLABLE properties;
    uint32_t propertyCount;
} PrCssKeyframe;

// Makes a keyframe effect of `target`, or of its pseudo-element `pseudo` (as `::before`, or the null view for the
// element itself), with `count` keyframes and `timing`, with one reference that the caller holds. The target may be
// null. Returns InvalidData, and writes null, if a keyframe's offsets or easing are refused, or the timing is refused
// (web-animations-1/#dom-keyframeeffect-keyframeeffect).
PrStatus prCssKeyframeEffectCreate(PrDomDocument* document, PrDomElement* PR_NULLABLE target, PrStringView pseudo,
    const PrCssKeyframe* PR_NULLABLE keyframes, uint32_t count, const PrCssEffectTiming* timing,
    PrCssKeyframeEffect* PR_NULLABLE* PR_NONNULL out);

// Makes a copy of `source`, with one reference that the caller holds
// (web-animations-1/#dom-keyframeeffect-keyframeeffect-source).
PrCssKeyframeEffect* prCssKeyframeEffectClone(const PrCssKeyframeEffect* source);

// Adds or releases a reference. The last release frees the effect, which no animation plays by then.
void prCssKeyframeEffectRetain(PrCssKeyframeEffect* effect);
void prCssKeyframeEffectRelease(PrCssKeyframeEffect* PR_NULLABLE effect);

// Returns the animation that plays the effect, or null if none does.
PrCssAnimation* PR_NULLABLE prCssKeyframeEffectAnimation(const PrCssKeyframeEffect* effect);

// The effect's target element, or null for none, and its pseudo-element, or the null view for the element itself
// (web-animations-1/#dom-keyframeeffect-target).
PrDomElement* PR_NULLABLE prCssKeyframeEffectTarget(const PrCssKeyframeEffect* effect);
PrStringView prCssKeyframeEffectPseudoElement(const PrCssKeyframeEffect* effect);

// Sets the effect's target. Returns Syntax, changing nothing, if `pseudo` is not a pseudo-element selector that an
// effect can animate (web-animations-1/#dom-keyframeeffect-pseudoelement).
void prCssKeyframeEffectSetTarget(PrCssKeyframeEffect* effect, PrDomElement* PR_NULLABLE target);
PrStatus prCssKeyframeEffectSetPseudoElement(PrCssKeyframeEffect* effect, PrStringView pseudo);

// The effect's composite operation and iteration composite operation (web-animations-1/#dom-keyframeeffect-composite,
// web-animations-2/#dom-keyframeeffect-iterationcomposite). Neither is Auto.
PrCssComposite prCssKeyframeEffectComposite(const PrCssKeyframeEffect* effect);
void prCssKeyframeEffectSetComposite(PrCssKeyframeEffect* effect, PrCssComposite composite);
PrCssComposite prCssKeyframeEffectIterationComposite(const PrCssKeyframeEffect* effect);
void prCssKeyframeEffectSetIterationComposite(PrCssKeyframeEffect* effect, PrCssComposite composite);

// Writes the effect's timing, as given (web-animations-1/#dom-animationeffect-gettiming). The easing's view is valid
// until the timing changes.
void prCssKeyframeEffectTiming(const PrCssKeyframeEffect* effect, PrCssEffectTiming* out);

// Writes the effect's timing at its animation's current time, with the fill resolved and `duration` the iteration
// duration (web-animations-1/#dom-animationeffect-getcomputedtiming).
void prCssKeyframeEffectComputedTiming(const PrCssKeyframeEffect* effect, PrCssEffectTiming* timing,
    PrCssComputedTiming* out);

// Updates the members of the effect's timing that `members` names, or returns InvalidData, changing nothing, if a value
// is refused (web-animations-1/#dom-animationeffect-updatetiming).
PrStatus prCssKeyframeEffectUpdateTiming(PrCssKeyframeEffect* effect, const PrCssEffectTiming* timing,
    PrCssTimingMembers members);

// Replaces the effect's keyframes (web-animations-1/#dom-keyframeeffect-setkeyframes). Returns InvalidData, changing
// nothing, if they are refused.
PrStatus prCssKeyframeEffectSetKeyframes(PrCssKeyframeEffect* effect, const PrCssKeyframe* PR_NULLABLE keyframes,
    uint32_t count);

// The effect's keyframes, as getKeyframes reports them (web-animations-1/#dom-keyframeeffect-getkeyframes). A CSS
// animation's are those of its @keyframes rule, and a transition's are its two endpoints, with their computed values.
typedef struct PrCssComputedKeyframe {
    double offset;                  // as given, or NaN for none
    double computedOffset;
    PrRawString easing;             // serialized
    PrCssComposite composite;
    uint32_t propertyCount;
} PrCssComputedKeyframe;

// Returns the number of the effect's keyframes, and writes up to `capacity` of them to `out`. The caller resets each
// easing with prRawStringReset.
uint32_t prCssKeyframeEffectKeyframes(const PrCssKeyframeEffect* effect, PrCssComputedKeyframe* PR_NULLABLE out,
    uint32_t capacity);

// Writes the name, in its CSS spelling, and the value of the property at `index` of keyframe `frame`: the value as a
// script gave it, or the value that a keyframe rule declares, or the computed value of a transition's endpoint. Returns
// false past either end. The caller resets both strings.
bool prCssKeyframeEffectKeyframeProperty(const PrCssKeyframeEffect* effect, uint32_t frame, uint32_t index,
    PrRawString* name, PrRawString* value);

// ---------------------------------------------------------------------------------------------------------------------
// Animations (web-animations-1/#the-animation-interface)

// What made an animation.
enum {
    PrCssAnimationKind_Script,              // an Animation that a script made
    PrCssAnimationKind_Css,                 // a CSS animation (css-animations-2/#the-CSSAnimation-interface)
    PrCssAnimationKind_Transition,          // a CSS transition (css-transitions-2/#the-CSSTransition-interface)
};
typedef uint8_t PrCssAnimationKind;

// An animation's play state (web-animations-1/#play-states).
enum {
    PrCssPlayState_Idle,
    PrCssPlayState_Running,
    PrCssPlayState_Paused,
    PrCssPlayState_Finished,
};
typedef uint8_t PrCssPlayState;

// Whether an animation still applies its effect (web-animations-1/#enumdef-animationreplacestate).
enum {
    PrCssReplaceState_Active,
    PrCssReplaceState_Removed,
    PrCssReplaceState_Persisted,
};
typedef uint8_t PrCssReplaceState;

// Returns the animation as the EventTarget that its playback events are dispatched to.
PrDomEventTarget* prCssAnimationAsEventTarget(PrCssAnimation* animation);

// Returns the animation that `target` is, or null if it is not one.
PrCssAnimation* PR_NULLABLE prCssAnimationFromEventTarget(PrDomEventTarget* target);

// Makes an idle animation of the document that plays `effect`, which leaves any animation that played it, on `timeline`
// (web-animations-1/#dom-animation-animation). Either may be null. The caller holds one reference.
PrCssAnimation* prCssAnimationCreate(PrDomDocument* document, PrCssKeyframeEffect* PR_NULLABLE effect,
    PrCssTimeline* PR_NULLABLE timeline);

// Adds or releases a reference. The last release frees the animation, which its document no longer runs by then.
void prCssAnimationRetain(PrCssAnimation* animation);
void prCssAnimationRelease(PrCssAnimation* PR_NULLABLE animation);

// Returns what made the animation.
PrCssAnimationKind prCssAnimationKind(const PrCssAnimation* animation);

// The animation-name of a CSS animation, or the transition-property of a transition, in its CSS spelling. The null view
// for an animation that a script made.
PrStringView prCssAnimationName(const PrCssAnimation* animation);

// The animation's identifier (web-animations-1/#dom-animation-id). The view is valid until it changes.
PrStringView prCssAnimationId(const PrCssAnimation* animation);
void prCssAnimationSetId(PrCssAnimation* animation, PrStringView id);

// The animation's effect and timeline, or null for none. Setting the effect takes it from any animation that played it
// (web-animations-1/#setting-the-associated-effect, #setting-the-timeline).
PrCssKeyframeEffect* PR_NULLABLE prCssAnimationEffect(const PrCssAnimation* animation);
void prCssAnimationSetEffect(PrCssAnimation* animation, PrCssKeyframeEffect* PR_NULLABLE effect);
PrCssTimeline* PR_NULLABLE prCssAnimationTimeline(const PrCssAnimation* animation);
void prCssAnimationSetTimeline(PrCssAnimation* animation, PrCssTimeline* PR_NULLABLE timeline);

// The animation's start time and current time in milliseconds, or NaN for unresolved
// (web-animations-1/#dom-animation-starttime, #dom-animation-currenttime).
double prCssAnimationStartTime(const PrCssAnimation* animation);
PrStatus prCssAnimationSetStartTime(PrCssAnimation* animation, double time);
double prCssAnimationCurrentTime(const PrCssAnimation* animation);
PrStatus prCssAnimationSetCurrentTime(PrCssAnimation* animation, double time);

// The animation's playback rate (web-animations-1/#dom-animation-playbackrate). Setting it keeps the current time.
double prCssAnimationPlaybackRate(const PrCssAnimation* animation);
void prCssAnimationSetPlaybackRate(PrCssAnimation* animation, double rate);

// The animation's play state and replace state (web-animations-1/#dom-animation-playstate,
// #dom-animation-replacestate).
PrCssPlayState prCssAnimationPlayState(const PrCssAnimation* animation);
PrCssReplaceState prCssAnimationReplaceState(const PrCssAnimation* animation);

// Returns true while the animation has a play or pause task pending (web-animations-1/#dom-animation-pending).
bool prCssAnimationPending(const PrCssAnimation* animation);

// The playback control methods (web-animations-1/#the-animation-interface). They return InvalidState where the methods
// throw it.
PrStatus prCssAnimationPlay(PrCssAnimation* animation);
PrStatus prCssAnimationPause(PrCssAnimation* animation);
PrStatus prCssAnimationFinish(PrCssAnimation* animation);
void prCssAnimationCancel(PrCssAnimation* animation);
PrStatus prCssAnimationReverse(PrCssAnimation* animation);
void prCssAnimationUpdatePlaybackRate(PrCssAnimation* animation, double rate);
void prCssAnimationPersist(PrCssAnimation* animation);

// Writes the computed values of the properties that the effect animates to its target's style attribute
// (web-animations-1/#dom-animation-commitstyles). Returns InvalidState if the target is a pseudo-element, is not an
// element whose styles are computed, or has no style attribute to write to.
PrStatus prCssAnimationCommitStyles(PrCssAnimation* animation);

// The promises of an animation (web-animations-1/#dom-animation-ready, #dom-animation-finished).
enum {
    PrCssAnimationPromise_Ready,
    PrCssAnimationPromise_Finished,
};
typedef uint8_t PrCssAnimationPromise;

// The state of a promise of an animation. Each time the animation replaces a promise, its generation increases.
enum {
    PrCssPromiseState_Pending,
    PrCssPromiseState_Fulfilled,
    PrCssPromiseState_Rejected,             // with an AbortError
};
typedef uint8_t PrCssPromiseState;

// Writes the promise's generation, and returns its state.
PrCssPromiseState prCssAnimationPromiseState(const PrCssAnimation* animation, PrCssAnimationPromise promise,
    uint32_t* generation);

// What a document's animations tell the script binding. `promiseSettled` is called when a promise is fulfilled or
// rejected, so that the binding settles the object that stands for that generation, if it made one. `scheduleFinish`
// asks for a microtask that calls prCssAnimationNotifyFinished (web-animations-1/#finish-notification-steps). `listed`
// is called as an animation starts and stops running in the document, so that the binding keeps the animation's object
// alive while it runs, with its promises and its event handlers.
typedef struct PrCssAnimationCallback {
    void* PR_NULLABLE context;
    void (*PR_NULLABLE promiseSettled)(void* PR_NULLABLE context, PrCssAnimation* animation,
        PrCssAnimationPromise promise, uint32_t generation, PrCssPromiseState state);
    void (*PR_NULLABLE scheduleFinish)(void* PR_NULLABLE context, PrCssAnimation* animation);
    void (*PR_NULLABLE listed)(void* PR_NULLABLE context, PrCssAnimation* animation, bool listed);
} PrCssAnimationCallback;

// Returns true while the animation runs in its document, which then holds it.
bool prCssAnimationRunning(const PrCssAnimation* animation);

// Sets what the document's animations tell the script binding, or nothing for null. The window clears it as its realm
// lets the document go.
void prCssDocumentSetAnimationCallback(PrDomDocument* document, const PrCssAnimationCallback* PR_NULLABLE callback);

// Runs the finish notification steps that the callback scheduled, unless they were canceled since.
void prCssAnimationNotifyFinished(PrCssAnimation* animation);

// Returns true if `name`, in its CSS spelling, names what a keyframe animates: a longhand that animates, a shorthand of
// one, or a custom property (web-animations-1/#animating-properties).
bool prCssPropertyAnimatable(PrStringView name);

// Returns true if `text` is an <easing-function> (css-easing-2/#typedef-easing-function).
bool prCssEasingValid(PrStringView text);

// ---------------------------------------------------------------------------------------------------------------------
// Getting animations (web-animations-1/#extensions-to-the-documentorshadowroot-interface-mixin)

// Returns the number of the relevant animations of the document whose effects target the subtree of `root`, its
// shadow-including descendants or the elements of the document if `root` is null, and writes up to `capacity` of them
// to `out` in composite order, each with a reference that the caller releases. With `subtree` false only those of
// `root` and its pseudo-elements are written (web-animations-1/#dom-animatable-getanimations).
uint32_t prCssDocumentAnimations(PrDomDocument* document, PrDomElement* PR_NULLABLE root, bool subtree,
    PrCssAnimation* PR_NONNULL* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Playback events (web-animations-1/#the-animationplaybackevent-interface)

// An AnimationPlaybackEvent, which an animation fires as it finishes, is canceled, or is removed.
typedef struct PrCssAnimationPlaybackEvent PrCssAnimationPlaybackEvent;

// The members of AnimationPlaybackEventInit (web-animations-1/#dictdef-animationplaybackeventinit).
typedef struct PrCssAnimationPlaybackEventInit {
    PrDomEventInit event;
    double currentTime;             // NaN for null
    double timelineTime;            // NaN for null
} PrCssAnimationPlaybackEventInit;

// Returns an init with the event's members false and both times null.
static inline PrCssAnimationPlaybackEventInit prCssAnimationPlaybackEventInitDefault(void) {
    PrCssAnimationPlaybackEventInit init = {prDomEventInitDefault(), kPrF64Nan, kPrF64Nan};
    return init;
}

// Creates an event of `type`, with one reference that the caller holds.
PrCssAnimationPlaybackEvent* prCssAnimationPlaybackEventCreate(PrStringView type, const PrCssAnimationPlaybackEventInit* init);

// Converts the event to the interface that it inherits from, and back, which gives null for another kind of event.
PrDomEvent* prCssAnimationPlaybackEventAsEvent(PrCssAnimationPlaybackEvent* event);
PrCssAnimationPlaybackEvent* PR_NULLABLE prCssAnimationPlaybackEventFromEvent(PrDomEvent* event);

// The event's times in milliseconds, or NaN for null (web-animations-1/#dom-animationplaybackevent-currenttime).
double prCssAnimationPlaybackEventCurrentTime(const PrCssAnimationPlaybackEvent* event);
double prCssAnimationPlaybackEventTimelineTime(const PrCssAnimationPlaybackEvent* event);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
