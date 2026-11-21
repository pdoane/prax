// The animation model of a document: its timelines, keyframe effects and animations, and the algorithms of their
// interfaces (web-animations-1/#animations, #keyframe-effects).
//
// web-animations-1 - https://drafts.csswg.org/web-animations-1/
// web-animations-2 - https://drafts.csswg.org/web-animations-2/
// css-animations-2 - https://drafts.csswg.org/css-animations-2/

#include "css/src/css_animations.h"

#include "pr/alloc.h"
#include "pr/css_presentation.h"
#include "pr/float.h"
#include "pr/hash_index.h"
#include "pr/sort.h"
#include "pr/text_buffer.h"

#include "css/src/css_animate.h"
#include "css/src/css_property.h"
#include "css/src/css_root.h"
#include "css/src/css_selector.h"
#include "css/src/css_selector_match.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Promises

void settle(PrCssAnimation* animation, PrCssAnimationPromise promise, PrCssPromiseState state) {
    PrCssPromiseState& at = promise == PrCssAnimationPromise_Ready ? animation->ready : animation->finished;
    if (at != PrCssPromiseState_Pending)
        return;
    at = state;
    uint32_t generation =
        promise == PrCssAnimationPromise_Ready ? animation->readyGeneration : animation->finishedGeneration;
    const PrCssAnimationCallback& callback = cssAnimations(animation->document).callback;
    if (callback.promiseSettled != nullptr)
        callback.promiseSettled(callback.context, animation, promise, generation, state);
}

// Replaces the promise with a new pending one.
void renew(PrCssAnimation* animation, PrCssAnimationPromise promise) {
    if (promise == PrCssAnimationPromise_Ready) {
        ++animation->readyGeneration;
        animation->ready = PrCssPromiseState_Pending;
    } else {
        ++animation->finishedGeneration;
        animation->finished = PrCssPromiseState_Pending;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Times

double timelineTime(const PrCssAnimation* animation) {
    return animation->timeline != nullptr ? prCssTimelineCurrentTime(animation->timeline) : kPrF64Nan;
}

bool timelineActive(const PrCssAnimation* animation) {
    return !prF64IsNan(timelineTime(animation));
}

double effectiveRate(const PrCssAnimation* animation) {
    return animation->hasPendingRate ? animation->pendingRate : animation->playback.rate;
}

void applyPendingRate(PrCssAnimation* animation) {
    if (!animation->hasPendingRate)
        return;
    animation->playback.rate = animation->pendingRate;
    animation->hasPendingRate = false;
}

// The current time with the hold time taken as unresolved (web-animations-1/#update-an-animations-finished-state).
double unheldTime(const PrCssAnimation* animation) {
    PrLayoutPlayback playback = animation->playback;
    playback.holdTime = kPrF64Nan;
    return prLayoutPlaybackCurrentTime(&playback, timelineTime(animation));
}

double effectEnd(const PrCssAnimation* animation) {
    return animation->effect != nullptr ? cssEffectEndTime(animation->effect) : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The document's list

void list(PrCssAnimation* animation) {
    if (animation->listed || !cssAnimations(animation->document).active)
        return;
    animation->listed = true;
    prCssAnimationRetain(animation);
    CssAnimations& animations = cssAnimations(animation->document);
    animations.list.push(animation);
    if (animations.callback.listed != nullptr)
        animations.callback.listed(animations.callback.context, animation, true);
}

// Returns true if the animation may still apply its effect, run a task, or notify something, so that the document runs
// it. An animation that a style owns runs while the style does.
bool runs(const PrCssAnimation* animation) {
    if (animation->owner != nullptr)
        return true;
    // An animation off an active timeline never becomes ready, and its time does not move.
    if (!timelineActive(animation))
        return false;
    PrCssPlayState state = prCssAnimationPlayState(animation);
    if (state == PrCssPlayState_Idle)
        return false;
    if (state != PrCssPlayState_Finished || animation->finishScheduled)
        return true;
    // A finished animation runs while its effect fills, or until its finished promise settles.
    return animation->finished == PrCssPromiseState_Pending ||
        (animation->effect != nullptr && !prF64IsNan(cssAnimationComputedTiming(animation).activeTime));
}

uint32_t targetHash(const PrDomElement* element, CssPseudo pseudo) {
    uint64_t key = uint64_t(uintptr_t(element)) ^ (uint64_t(pseudo) << 1);
    return KeyIndex::hashOf(key);
}

// The class of an animation in composite order: transitions, then CSS animations, then the rest
// (css-transitions-2/#animation-composite-order, css-animations-2/#animation-composite-order). An animation that its
// style no longer owns sorts with the rest.
int compositeClass(const PrCssAnimation* animation) {
    if (animation->owner == nullptr)
        return 2;
    return animation->kind == PrCssAnimationKind_Transition ? 0 : 1;
}

// Writes the pseudo-element that a selector names (web-animations-1/#dom-keyframeeffect-pseudoelement), which is
// CssPseudo_Count for one that no presentation animates, and its text, with a legacy single colon made double. Returns
// false if `text` is not a <pseudo-element-selector>.
bool parsePseudo(PrStringView text, CssPseudo* pseudo, PrRawString* name) {
    if (text.size < 2 || text.data[0] != ':')
        return false;
    PrCssSelectors* selectors = cssParseDomSelectors(text);
    if (selectors == nullptr)
        return false;
    bool valid = false;
    const CssComplexSelector* complex = &selectors->complex[selectors->list.first];
    if (selectors->list.count == 1 && (complex->flags & CssComplexFlag_Invalid) == 0 && complex->compoundCount == 1) {
        const CssCompoundSelector* compound = &selectors->compounds[complex->compound];
        valid = compound->simpleCount == 1 && selectors->simple[compound->simple].kind == CssSimpleKind_PseudoElement;
    }
    if (valid) {
        CssPseudo found = cssSelectorPseudo(selectors, selectors->list.first);
        bool animated = found == CssPseudo_Before || found == CssPseudo_After || found == CssPseudo_Marker;
        *pseudo = animated ? found : CssPseudo_Count;
        if (*pseudo != CssPseudo_Count) {
            *name = prRawStringCopy(cssPseudoName(*pseudo));
        } else if (text.size > 1 && text.data[1] != ':') {
            PrTextBuffer doubled{};
            prTextBufferAppend(&doubled, ":");
            prTextBufferAppend(&doubled, text);
            *name = prRawStringCopy(prTextBufferView(&doubled));
            prTextBufferReset(&doubled);
        } else {
            *name = prRawStringCopy(text);
        }
    }
    cssSelectorsDestroy(selectors);
    return valid;
}

// Returns true if the members of `timing` that `members` names are values that an effect takes
// (web-animations-1/#updating-animationeffect-timing). The easing is checked as it is parsed.
bool timingValid(const PrCssEffectTiming& timing, PrCssTimingMembers members) {
    if ((members & PrCssTimingMember_IterationStart) != 0 && !(timing.iterationStart >= 0))
        return false;
    if ((members & PrCssTimingMember_Iterations) != 0 && !(timing.iterations >= 0))
        return false;
    if ((members & PrCssTimingMember_Duration) != 0 && !prF64IsNan(timing.duration) && !(timing.duration >= 0))
        return false;
    if ((members & PrCssTimingMember_Delay) != 0 && !prF64IsFinite(timing.delay))
        return false;
    if ((members & PrCssTimingMember_EndDelay) != 0 && !prF64IsFinite(timing.endDelay))
        return false;
    return true;
}

PrLayoutFill layoutFill(PrCssFillMode fill) {
    switch (fill) {
        case PrCssFillMode_Forwards: return PrLayoutFill_Forwards;
        case PrCssFillMode_Backwards: return PrLayoutFill_Backwards;
        case PrCssFillMode_Both: return PrLayoutFill_Both;
        default: return PrLayoutFill_None;
    }
}

// Applies the members of `timing` that `members` names, whose easing, if named, is already parsed into `easing`.
void applyTiming(PrCssKeyframeEffect* effect, const PrCssEffectTiming& timing, PrCssTimingMembers members,
        const CssValueList* PR_NULLABLE easing) {
    PrLayoutEffectTiming& t = effect->timing;
    if ((members & PrCssTimingMember_Delay) != 0)
        t.delay = timing.delay;
    if ((members & PrCssTimingMember_EndDelay) != 0)
        t.endDelay = timing.endDelay;
    if ((members & PrCssTimingMember_IterationStart) != 0)
        t.iterationStart = timing.iterationStart;
    if ((members & PrCssTimingMember_Iterations) != 0)
        t.iterations = timing.iterations;
    if ((members & PrCssTimingMember_Duration) != 0) {
        effect->duration = timing.duration;
        t.duration = prF64IsNan(timing.duration) ? 0 : timing.duration;
    }
    if ((members & PrCssTimingMember_Fill) != 0) {
        effect->fill = timing.fill;
        t.fill = layoutFill(timing.fill);
    }
    if ((members & PrCssTimingMember_Direction) != 0)
        t.direction = PrLayoutPlaybackDirection(timing.direction);
    if ((members & PrCssTimingMember_Easing) != 0 && easing != nullptr) {
        t.easing = cssEasing(cssValueListRun(easing));
        PrTextBuffer text{};
        cssSerializeValues(&text, cssValueListRun(easing));
        prRawStringReplace(&effect->easing, prTextBufferView(&text));
        prTextBufferReset(&text);
    }
}

// Parses the easing of `timing`, if `members` names it. Returns false if it is not an <easing-function>.
bool parseTimingEasing(const PrCssEffectTiming& timing, PrCssTimingMembers members, CssValueList* out) {
    *out = CssValueList{};
    if ((members & PrCssTimingMember_Easing) == 0)
        return true;
    return cssParseEasing(timing.easing, out);
}

// Makes keyframes from a script's. Writes null for none.
PrStatus makeKeyframes(const PrCssKeyframe* PR_NULLABLE keyframes, uint32_t count, CssKeyframes* PR_NULLABLE* out) {
    *out = nullptr;
    RawArray<CssKeyframeInput> inputs;
    for (uint32_t i = 0; i < count; ++i) {
        const PrCssKeyframe& k = keyframes[i];
        inputs.push(CssKeyframeInput{k.offset, k.computedOffset, k.easing, k.composite, k.properties, k.propertyCount});
    }
    CssKeyframesError error = cssKeyframesFromInput(inputs.data, inputs.count, out);
    inputs.reset();
    return error == CssKeyframesError_None ? PrStatus_Ok : PrStatus_InvalidData;
}

// The pseudo-elements of an element in the order that composite order sorts them
// (css-animations-2/#animation-composite-order).
int pseudoRank(CssPseudo pseudo) {
    switch (pseudo) {
        case CssPseudo_None: return 0;
        case CssPseudo_Marker: return 1;
        case CssPseudo_Before: return 2;
        case CssPseudo_After: return 3;
        default: return 4;
    }
}

// Returns true if the animation is relevant: its effect is current or in effect, and nothing replaced it
// (web-animations-1/#relevant-animations).
bool relevant(const PrCssAnimation* animation) {
    if (animation->effect == nullptr || animation->replaceState == PrCssReplaceState_Removed)
        return false;
    PrLayoutComputedTiming timing = cssAnimationComputedTiming(animation);
    if (!prF64IsNan(timing.activeTime))
        return true;
    if (timing.phase == PrLayoutEffectPhase_Before || timing.phase == PrLayoutEffectPhase_Active)
        return true;
    return timing.phase != PrLayoutEffectPhase_None && prCssAnimationPlayState(animation) != PrCssPlayState_Finished;
}

struct PlaybackEventData : DomEventExtra {
    double currentTime;
    double timelineTime;
};

// Changes the effect's target, taking it out of the document's index of animated targets and putting it back.
template <typename Change>
void retarget(PrCssKeyframeEffect* effect, Change change) {
    cssAnimationEffectChanged(effect);
    PrCssAnimation* animation = effect->animation;
    effect->animation = nullptr;
    cssAnimationIndexEffect(effect);
    change();
    effect->animation = animation;
    cssAnimationIndexEffect(effect);
    cssAnimationEffectChanged(effect);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Internal

CssAnimations& cssAnimations(PrDomDocument* document) {
    return cssDocumentRoot(document)->animations;
}

void cssAnimationsReset(CssAnimations* animations) {
    PR_ASSERT(animations->list.count == 0);
    animations->list.reset();
    for (uint32_t i = 0; i < animations->events.count; ++i)
        cssAnimateEventReset(&animations->events.data[i]);
    animations->events.reset();
    if (animations->timeline != nullptr) {
        animations->timeline->document = nullptr;
        prCssTimelineRelease(animations->timeline);
    }
    for (uint32_t i = 0; i < animations->targets.count; ++i)
        animations->targets.data[i].effects.reset();
    animations->targets.reset();
    animations->targetIndex.reset();
    cssValuePoolReset(&animations->scratch);
    animations->overridden.reset();
    animations->overrides.reset();
    animations->tracks.reset();
    animations->keyframes.reset();
}

double cssEffectEndTime(const PrCssKeyframeEffect* effect) {
    PrLayoutComputedTiming computed = prLayoutEffectTimingCompute(&effect->timing, kPrF64Nan, 1);
    return computed.endTime;
}

double cssAnimationCurrentTime(const PrCssAnimation* animation) {
    return prLayoutPlaybackCurrentTime(&animation->playback, timelineTime(animation));
}

PrLayoutComputedTiming cssAnimationComputedTiming(const PrCssAnimation* animation) {
    PR_ASSERT(animation->effect != nullptr);
    return prLayoutEffectTimingCompute(&animation->effect->timing, cssAnimationCurrentTime(animation),
        animation->playback.rate);
}

void cssAnimationEffectChanged(PrCssKeyframeEffect* effect) {
    effect->computed = cssEffectHasEndpoints(effect);
    if (effect->target == nullptr)
        return;
    CssRoot* root = cssDocumentRoot(effect->document);
    if (root->presentationHooks != nullptr)
        root->presentationHooks->effectChanged(root->presentation, effect->target);
}

bool cssEffectHasEndpoints(const PrCssKeyframeEffect* effect) {
    return effect->animation != nullptr && effect->animation->kind == PrCssAnimationKind_Transition &&
        effect->keyframes == nullptr;
}

void cssAnimationIndexEffect(PrCssKeyframeEffect* effect) {
    bool indexed = effect->animation != nullptr && effect->target != nullptr;
    if (indexed == effect->indexed)
        return;
    CssAnimations& animations = cssAnimations(effect->document);
    uint32_t hash = targetHash(effect->target, effect->pseudo);
    uint32_t at = cssAnimatedTarget(animations, effect->target, effect->pseudo);
    if (indexed) {
        if (at == kHashMissing) {
            at = animations.targets.count;
            animations.targets.push(CssAnimatedTarget{effect->target, effect->pseudo, {}});
            animations.targetIndex.insert(hash, at);
        }
        animations.targets.data[at].effects.push(effect);
    } else {
        PR_ASSERT(at != kHashMissing);
        RawArray<PrCssKeyframeEffect*>& effects = animations.targets.data[at].effects;
        effects.removeFirst(effect);
        if (effects.count == 0) {
            effects.reset();
            // The last target moves into the place of the one that goes.
            uint32_t last = animations.targets.count - 1;
            animations.targetIndex.erase(hash, at);
            if (at != last) {
                const CssAnimatedTarget& moved = animations.targets.data[last];
                animations.targetIndex.relocate(targetHash(moved.element, moved.pseudo), last, at);
                animations.targets.data[at] = moved;
            }
            animations.targets.pop();
        }
    }
    effect->indexed = indexed;
}

uint32_t cssAnimatedTarget(const CssAnimations& animations, const PrDomElement* element, CssPseudo pseudo) {
    return animations.targetIndex.find(targetHash(element, pseudo), [&](uint32_t at) {
        const CssAnimatedTarget& target = animations.targets.data[at];
        return target.element == element && target.pseudo == pseudo;
    });
}

void cssAnimationRelist(PrCssAnimation* animation) {
    if (runs(animation))
        list(animation);
}

void cssAnimationUnlistIdle(CssAnimations& animations) {
    for (uint32_t i = 0; i < animations.list.count;) {
        PrCssAnimation* a = animations.list.data[i];
        if (runs(a)) {
            ++i;
            continue;
        }
        animations.list.erase(i);
        a->listed = false;
        if (animations.callback.listed != nullptr)
            animations.callback.listed(animations.callback.context, a, false);
        prCssAnimationRelease(a);
    }
}

PrCssAnimation* cssAnimationMake(PrDomDocument* document, PrCssAnimationKind kind) {
    PrCssAnimation* animation = allocZeroed<PrCssAnimation>();
    animation->type = DomTargetType_Animation;
    animation->references = 1;
    animation->destroy = [](DomCountedTarget* target) {
        PrCssAnimation* animation = static_cast<PrCssAnimation*>(target);
        PR_ASSERT(!animation->listed);
        if (animation->effect != nullptr) {
            animation->effect->animation = nullptr;
            cssAnimationIndexEffect(animation->effect);
        }
        prCssKeyframeEffectRelease(animation->effect);
        prCssTimelineRelease(animation->timeline);
        prRawStringReset(&animation->id);
        prRawStringReset(&animation->name);
        cssValueListReset(&animation->transitionTo);
        cssValueListReset(&animation->reversingStart);
        domListenersDestroy(animation->listeners);
        PrDomDocument* document = animation->document;
        prMemoryFree(animation);
        domNodeRelease(document);
    };
    domNodeRetain(document);
    animation->document = document;
    animation->kind = kind;
    CssAnimations& animations = cssAnimations(document);
    animation->sequence = animations.sequence++;
    animation->id = prRawStringEmpty();
    animation->playback = prLayoutPlaybackDefault();
    animation->previousCurrentTime = kPrF64Nan;
    animation->ready = PrCssPromiseState_Fulfilled;
    animation->finished = PrCssPromiseState_Pending;
    animation->replaceState = PrCssReplaceState_Active;
    animation->reversingFactor = 1;
    animation->eventIteration = kPrF64Nan;
    return animation;
}

// web-animations-1/#animation-reset-an-animations-pending-tasks
void cssAnimationResetPendingTasks(PrCssAnimation* animation) {
    if (animation->pending == CssPendingTask_None)
        return;
    animation->pending = CssPendingTask_None;
    applyPendingRate(animation);
    settle(animation, PrCssAnimationPromise_Ready, PrCssPromiseState_Rejected);
    renew(animation, PrCssAnimationPromise_Ready);
    animation->ready = PrCssPromiseState_Fulfilled;
}

// web-animations-1/#update-an-animations-finished-state
void cssAnimationUpdateFinishedState(PrCssAnimation* animation, bool didSeek, bool sync) {
    double unconstrained = didSeek ? cssAnimationCurrentTime(animation) : unheldTime(animation);
    double rate = animation->playback.rate;
    if (!prF64IsNan(unconstrained) && !prF64IsNan(animation->playback.startTime) &&
            animation->pending == CssPendingTask_None) {
        double end = effectEnd(animation);
        double previous = animation->previousCurrentTime;
        if (rate > 0 && unconstrained >= end) {
            animation->playback.holdTime = didSeek ? unconstrained : prF64IsNan(previous) ? end : prF64Max(previous, end);
        } else if (rate < 0 && unconstrained <= 0) {
            animation->playback.holdTime = didSeek ? unconstrained : prF64IsNan(previous) ? 0 : prF64Min(previous, 0);
        } else if (rate != 0 && timelineActive(animation)) {
            if (didSeek && !prF64IsNan(animation->playback.holdTime))
                animation->playback.startTime = timelineTime(animation) - animation->playback.holdTime / rate;
            animation->playback.holdTime = kPrF64Nan;
        }
    }
    animation->previousCurrentTime = cssAnimationCurrentTime(animation);
    bool finished = prCssAnimationPlayState(animation) == PrCssPlayState_Finished;
    if (finished && animation->finished == PrCssPromiseState_Pending) {
        if (sync) {
            animation->finishScheduled = false;
            cssAnimationFinishNotification(animation);
        } else if (!animation->finishScheduled) {
            animation->finishScheduled = true;
            const PrCssAnimationCallback& callback = cssAnimations(animation->document).callback;
            if (callback.scheduleFinish != nullptr) {
                callback.scheduleFinish(callback.context, animation);
            } else {
                animation->finishScheduled = false;
                cssAnimationFinishNotification(animation);
            }
        }
    } else if (!finished && animation->finished == PrCssPromiseState_Fulfilled) {
        renew(animation, PrCssAnimationPromise_Finished);
    }
    if (!finished)
        animation->finishScheduled = false;
    cssAnimationRelist(animation);
}

// web-animations-1/#finish-notification-steps
void cssAnimationFinishNotification(PrCssAnimation* animation) {
    if (prCssAnimationPlayState(animation) != PrCssPlayState_Finished)
        return;
    settle(animation, PrCssAnimationPromise_Finished, PrCssPromiseState_Fulfilled);
    // The event's scheduled time is when the effect reached the end that it plays toward.
    double scheduled = kPrF64Nan;
    if (!prF64IsNan(animation->playback.startTime) && animation->playback.rate != 0) {
        double end = animation->playback.rate > 0 ? effectEnd(animation) : 0;
        scheduled = prF64Min(animation->playback.startTime + end / animation->playback.rate, timelineTime(animation));
        if (animation->timeline != nullptr)
            scheduled += animation->timeline->originTime;
    }
    cssAnimationQueuePlaybackEvent(animation, "finish", cssAnimationCurrentTime(animation), scheduled);
}

// web-animations-1/#playing-an-animation-section
PrStatus cssAnimationPlay(PrCssAnimation* animation, bool autoRewind) {
    bool abortedPause = animation->pending == CssPendingTask_Pause;
    bool pendingReady = false;
    double seek = kPrF64Nan;
    double rate = effectiveRate(animation);
    double current = cssAnimationCurrentTime(animation);
    double end = effectEnd(animation);
    if (autoRewind) {
        if (rate >= 0 && (prF64IsNan(current) || current < 0 || current >= end)) {
            seek = 0;
        } else if (rate < 0 && (prF64IsNan(current) || current <= 0 || current > end)) {
            if (!prF64IsFinite(end))
                return PrStatus_InvalidState;
            seek = end;
        }
    }
    if (prF64IsNan(seek) && prF64IsNan(animation->playback.startTime) && prF64IsNan(current))
        seek = 0;
    if (!prF64IsNan(seek))
        animation->playback.holdTime = seek;
    if (!prF64IsNan(animation->playback.holdTime))
        animation->playback.startTime = kPrF64Nan;
    if (animation->pending != CssPendingTask_None) {
        animation->pending = CssPendingTask_None;
        pendingReady = true;
    }
    if (prF64IsNan(animation->playback.holdTime) && prF64IsNan(seek) && !abortedPause && !animation->hasPendingRate)
        return PrStatus_Ok;
    if (!pendingReady)
        renew(animation, PrCssAnimationPromise_Ready);
    animation->pending = CssPendingTask_Play;
    animation->pendingRender = cssAnimations(animation->document).renders;
    cssAnimationUpdateFinishedState(animation, false, false);
    return PrStatus_Ok;
}

// web-animations-1/#pausing-an-animation-section
PrStatus cssAnimationPause(PrCssAnimation* animation) {
    if (animation->pending == CssPendingTask_Pause || prCssAnimationPlayState(animation) == PrCssPlayState_Paused)
        return PrStatus_Ok;
    double seek = kPrF64Nan;
    if (prF64IsNan(cssAnimationCurrentTime(animation))) {
        if (animation->playback.rate >= 0) {
            seek = 0;
        } else {
            double end = effectEnd(animation);
            if (!prF64IsFinite(end))
                return PrStatus_InvalidState;
            seek = end;
        }
    }
    if (!prF64IsNan(seek))
        animation->playback.holdTime = seek;
    bool pendingReady = false;
    if (animation->pending == CssPendingTask_Play) {
        animation->pending = CssPendingTask_None;
        pendingReady = true;
    }
    if (!pendingReady)
        renew(animation, PrCssAnimationPromise_Ready);
    animation->pending = CssPendingTask_Pause;
    animation->pendingRender = cssAnimations(animation->document).renders;
    cssAnimationUpdateFinishedState(animation, false, false);
    return PrStatus_Ok;
}

// web-animations-1/#canceling-an-animation-section. A CSS animation or transition that is canceled also queues its
// cancel event, which `cancelEvent` does.
void cssAnimationCancel(PrCssAnimation* animation) {
    if (prCssAnimationPlayState(animation) != PrCssPlayState_Idle) {
        cssAnimateQueueCancelEvent(animation);
        cssAnimationResetPendingTasks(animation);
        settle(animation, PrCssAnimationPromise_Finished, PrCssPromiseState_Rejected);
        renew(animation, PrCssAnimationPromise_Finished);
        double time = timelineTime(animation);
        double scheduled =
            prF64IsNan(time) || animation->timeline == nullptr ? kPrF64Nan : time + animation->timeline->originTime;
        cssAnimationQueuePlaybackEvent(animation, "cancel", kPrF64Nan, scheduled);
    }
    animation->playback.holdTime = kPrF64Nan;
    animation->playback.startTime = kPrF64Nan;
    animation->finishScheduled = false;
}

// web-animations-1/#animation-silently-set-the-current-time
PrStatus cssAnimationSilentlySetCurrentTime(PrCssAnimation* animation, double time) {
    if (prF64IsNan(time))
        return prF64IsNan(cssAnimationCurrentTime(animation)) ? PrStatus_Ok : PrStatus_InvalidData;
    if (!prF64IsNan(animation->playback.holdTime) || prF64IsNan(animation->playback.startTime) ||
            !timelineActive(animation) || animation->playback.rate == 0)
        animation->playback.holdTime = time;
    else
        animation->playback.startTime = timelineTime(animation) - time / animation->playback.rate;
    if (!timelineActive(animation))
        animation->playback.startTime = kPrF64Nan;
    animation->previousCurrentTime = kPrF64Nan;
    return PrStatus_Ok;
}

// web-animations-1/#pending-play-task and #pending-pause-task
void cssAnimationRunPendingTask(PrCssAnimation* animation, double readyTime) {
    if (animation->pending == CssPendingTask_Play) {
        if (!prF64IsNan(animation->playback.holdTime)) {
            applyPendingRate(animation);
            double rate = animation->playback.rate;
            animation->playback.startTime = rate != 0 ? readyTime - animation->playback.holdTime / rate : readyTime;
            if (rate != 0)
                animation->playback.holdTime = kPrF64Nan;
        } else if (!prF64IsNan(animation->playback.startTime) && animation->hasPendingRate) {
            double match = (readyTime - animation->playback.startTime) * animation->playback.rate;
            applyPendingRate(animation);
            double rate = animation->playback.rate;
            if (rate == 0)
                animation->playback.holdTime = match;
            animation->playback.startTime = rate != 0 ? readyTime - match / rate : readyTime;
        }
    } else if (animation->pending == CssPendingTask_Pause) {
        if (!prF64IsNan(animation->playback.startTime) && prF64IsNan(animation->playback.holdTime))
            animation->playback.holdTime = (readyTime - animation->playback.startTime) * animation->playback.rate;
        applyPendingRate(animation);
        animation->playback.startTime = kPrF64Nan;
    } else {
        return;
    }
    animation->pending = CssPendingTask_None;
    settle(animation, PrCssAnimationPromise_Ready, PrCssPromiseState_Fulfilled);
    cssAnimationUpdateFinishedState(animation, false, false);
}

double cssAnimationReadyTime(const PrCssAnimation* animation, double time) {
    const CssAnimations& animations = cssAnimations(animation->document);
    double origin = animation->timeline != nullptr ? animation->timeline->originTime : 0;
    double at = animations.renders > animation->pendingRender ? animations.renderTime : time;
    return at - origin;
}

void cssAnimationQueuePlaybackEvent(PrCssAnimation* animation, PrStringView type, double currentTime, double scheduled) {
    CssAnimations& animations = cssAnimations(animation->document);
    if (!animations.recordsEvents)
        return;
    // The event holds the animation twice, as its target and as what orders it.
    prCssAnimationRetain(animation);
    prCssAnimationRetain(animation);
    animations.events.push(CssAnimateEvent{
        .target = animation,
        .animation = animation,
        .type = type,
        .kind = PrDomEventKind_AnimationPlayback,
        .pseudo = CssPseudo_None,
        .name = prRawStringNull(),
        .elapsedTime = 0,
        .currentTime = currentTime,
        .timelineTime = timelineTime(animation),
        .scheduled = scheduled,
    });
}

bool cssAnimationBefore(const PrCssAnimation* animation, const PrCssAnimation* other) {
    int x = compositeClass(animation);
    int y = compositeClass(other);
    if (x != y)
        return x < y;
    if (x != 2 && animation->effect != nullptr && other->effect != nullptr &&
            animation->effect->target != other->effect->target &&
            animation->effect->target != nullptr && other->effect->target != nullptr) {
        // Animations of owning elements sort in tree order of those elements.
        PrDomNodePosition position = prDomNodeCompareDocumentPosition(animation->effect->target, other->effect->target);
        return (position & PrDomNodePosition_Following) != 0;
    }
    if (x == 1 && animation->nameIndex != other->nameIndex)
        return animation->nameIndex < other->nameIndex;
    return animation->sequence < other->sequence;
}

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Timelines

PrCssTimeline* prCssDocumentTimeline(PrDomDocument* document) {
    CssAnimations& animations = cssAnimations(document);
    if (animations.timeline == nullptr) {
        PrCssTimeline* timeline = allocZeroed<PrCssTimeline>();
        timeline->references = 1;
        timeline->document = document;
        timeline->own = true;
        animations.timeline = timeline;
    }
    return animations.timeline;
}

PrCssTimeline* prCssTimelineCreate(PrDomDocument* document, double originTime) {
    PrCssTimeline* timeline = allocZeroed<PrCssTimeline>();
    timeline->references = 1;
    domNodeRetain(document);
    timeline->document = document;
    timeline->originTime = originTime;
    return timeline;
}

void prCssTimelineRetain(PrCssTimeline* timeline) {
    ++timeline->references;
}

void prCssTimelineRelease(PrCssTimeline* PR_NULLABLE timeline) {
    if (timeline == nullptr || --timeline->references != 0)
        return;
    PrDomDocument* document = timeline->own ? nullptr : timeline->document;
    prMemoryFree(timeline);
    domNodeRelease(document);
}

double prCssTimelineCurrentTime(const PrCssTimeline* timeline) {
    if (timeline->document == nullptr)
        return kPrF64Nan;
    const CssAnimations& animations = cssAnimations(timeline->document);
    if (!animations.active)
        return kPrF64Nan;
    return animations.time - timeline->originTime;
}

PrCssEffectTiming prCssEffectTimingDefault(void) {
    return PrCssEffectTiming{
        .delay = 0,
        .endDelay = 0,
        .iterationStart = 0,
        .iterations = 1,
        .duration = kPrF64Nan,
        .easing = "linear",
        .fill = PrCssFillMode_Auto,
        .direction = PrCssPlaybackDirection_Normal,
    };
}

PrStatus prCssKeyframeEffectCreate(PrDomDocument* document, PrDomElement* PR_NULLABLE target, PrStringView pseudo,
        const PrCssKeyframe* PR_NULLABLE keyframes, uint32_t count, const PrCssEffectTiming* timing,
        PrCssKeyframeEffect* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // web-animations-1/#dom-keyframeeffect-keyframeeffect: the timing is checked before the keyframes.
    CssValueList easing{};
    if (!timingValid(*timing, PrCssTimingMember_All) || !parseTimingEasing(*timing, PrCssTimingMember_All, &easing))
        return PrStatus_InvalidData;
    CssPseudo parsed = CssPseudo_None;
    PrRawString pseudoText = prRawStringNull();
    if (pseudo.data != nullptr && !parsePseudo(pseudo, &parsed, &pseudoText)) {
        cssValueListReset(&easing);
        return PrStatus_Syntax;
    }
    CssKeyframes* frames = nullptr;
    PrStatus status = makeKeyframes(keyframes, count, &frames);
    if (status != PrStatus_Ok) {
        cssValueListReset(&easing);
        prRawStringReset(&pseudoText);
        return status;
    }
    PrCssKeyframeEffect* effect = allocZeroed<PrCssKeyframeEffect>();
    effect->references = 1;
    domNodeRetain(document);
    effect->document = document;
    if (target != nullptr)
        domNodeRetain(target);
    effect->target = target;
    effect->pseudoText = pseudoText;
    effect->pseudo = parsed;
    effect->timing = prLayoutEffectTimingDefault();
    effect->duration = kPrF64Nan;
    effect->fill = PrCssFillMode_Auto;
    effect->easing = prRawStringCopy("linear");
    applyTiming(effect, *timing, PrCssTimingMember_All, &easing);
    cssValueListReset(&easing);
    effect->keyframes = frames;
    *out = effect;
    return PrStatus_Ok;
}

PrCssKeyframeEffect* prCssKeyframeEffectClone(const PrCssKeyframeEffect* source) {
    PrCssKeyframeEffect* effect = allocZeroed<PrCssKeyframeEffect>();
    effect->references = 1;
    domNodeRetain(source->document);
    effect->document = source->document;
    if (source->target != nullptr)
        domNodeRetain(source->target);
    effect->target = source->target;
    effect->pseudoText = prRawStringClone(&source->pseudoText);
    effect->pseudo = source->pseudo;
    effect->timing = source->timing;
    effect->duration = source->duration;
    effect->fill = source->fill;
    effect->easing = prRawStringClone(&source->easing);
    effect->composite = source->composite;
    effect->iterationComposite = source->iterationComposite;
    if (source->keyframes != nullptr)
        cssKeyframesRetain(source->keyframes);
    effect->keyframes = source->keyframes;
    effect->defaultEasing = cssValueListCopy(cssValueListRun(&source->defaultEasing));
    return effect;
}

void prCssKeyframeEffectRetain(PrCssKeyframeEffect* effect) {
    ++effect->references;
}

void prCssKeyframeEffectRelease(PrCssKeyframeEffect* PR_NULLABLE effect) {
    if (effect == nullptr || --effect->references != 0)
        return;
    PR_ASSERT(effect->animation == nullptr && !effect->indexed);
    PrDomDocument* document = effect->document;
    domNodeRelease(effect->target);
    prRawStringReset(&effect->pseudoText);
    prRawStringReset(&effect->easing);
    cssKeyframesRelease(effect->keyframes);
    cssValueListReset(&effect->defaultEasing);
    cssValuePoolReset(&effect->values);
    effect->frames.reset();
    prMemoryFree(effect);
    domNodeRelease(document);
}

PrCssAnimation* PR_NULLABLE prCssKeyframeEffectAnimation(const PrCssKeyframeEffect* effect) {
    return effect->animation;
}

PrDomElement* PR_NULLABLE prCssKeyframeEffectTarget(const PrCssKeyframeEffect* effect) {
    return effect->target;
}

PrStringView prCssKeyframeEffectPseudoElement(const PrCssKeyframeEffect* effect) {
    return prRawStringView(&effect->pseudoText);
}

void prCssKeyframeEffectSetTarget(PrCssKeyframeEffect* effect, PrDomElement* PR_NULLABLE target) {
    if (effect->target == target)
        return;
    retarget(effect, [&] {
        if (target != nullptr)
            domNodeRetain(target);
        domNodeRelease(effect->target);
        effect->target = target;
    });
}

PrStatus prCssKeyframeEffectSetPseudoElement(PrCssKeyframeEffect* effect, PrStringView pseudo) {
    CssPseudo parsed = CssPseudo_None;
    PrRawString text = prRawStringNull();
    if (pseudo.data != nullptr && !parsePseudo(pseudo, &parsed, &text))
        return PrStatus_Syntax;
    retarget(effect, [&] {
        prRawStringReset(&effect->pseudoText);
        effect->pseudoText = text;
        effect->pseudo = parsed;
    });
    return PrStatus_Ok;
}

PrCssComposite prCssKeyframeEffectComposite(const PrCssKeyframeEffect* effect) {
    return PrCssComposite(effect->composite);
}

void prCssKeyframeEffectSetComposite(PrCssKeyframeEffect* effect, PrCssComposite composite) {
    effect->composite = composite == PrCssComposite_Auto ? PrCssComposite_Replace : PrCssComposite(composite);
    cssAnimationEffectChanged(effect);
}

PrCssComposite prCssKeyframeEffectIterationComposite(const PrCssKeyframeEffect* effect) {
    return PrCssComposite(effect->iterationComposite);
}

void prCssKeyframeEffectSetIterationComposite(PrCssKeyframeEffect* effect, PrCssComposite composite) {
    // Only replace and accumulate apply between iterations (web-animations-2/#effect-accumulation-section).
    bool accumulates = composite == PrCssComposite_Accumulate;
    effect->iterationComposite = accumulates ? PrCssComposite_Accumulate : PrCssComposite_Replace;
    cssAnimationEffectChanged(effect);
}

void prCssKeyframeEffectTiming(const PrCssKeyframeEffect* effect, PrCssEffectTiming* out) {
    const PrLayoutEffectTiming& t = effect->timing;
    *out = PrCssEffectTiming{
        .delay = t.delay,
        .endDelay = t.endDelay,
        .iterationStart = t.iterationStart,
        .iterations = t.iterations,
        .duration = effect->duration,
        .easing = prRawStringView(&effect->easing),
        .fill = effect->fill,
        .direction = PrCssPlaybackDirection(t.direction),
    };
}

void prCssKeyframeEffectComputedTiming(const PrCssKeyframeEffect* effect, PrCssEffectTiming* timing,
        PrCssComputedTiming* out) {
    prCssKeyframeEffectTiming(effect, timing);
    timing->duration = effect->timing.duration;
    timing->fill = PrCssFillMode(effect->timing.fill);
    double local = effect->animation != nullptr ? cssAnimationCurrentTime(effect->animation) : kPrF64Nan;
    double rate = effect->animation != nullptr ? effect->animation->playback.rate : 1;
    PrLayoutComputedTiming c = prLayoutEffectTimingCompute(&effect->timing, local, rate);
    *out = PrCssComputedTiming{c.endTime, c.activeDuration, c.localTime, c.progress, c.currentIteration};
}

PrStatus prCssKeyframeEffectUpdateTiming(PrCssKeyframeEffect* effect, const PrCssEffectTiming* timing,
        PrCssTimingMembers members) {
    CssValueList easing{};
    if (!timingValid(*timing, members) || !parseTimingEasing(*timing, members, &easing))
        return PrStatus_InvalidData;
    applyTiming(effect, *timing, members, &easing);
    cssValueListReset(&easing);
    if (PrCssAnimation* animation = effect->animation) {
        // What a script sets of a CSS animation's timing, its style no longer changes (css-animations-2/#animations).
        animation->timingOverridden |= members;
        cssAnimationUpdateFinishedState(animation, false, false);
    }
    cssAnimationEffectChanged(effect);
    return PrStatus_Ok;
}

PrStatus prCssKeyframeEffectSetKeyframes(PrCssKeyframeEffect* effect, const PrCssKeyframe* PR_NULLABLE keyframes,
        uint32_t count) {
    CssKeyframes* frames = nullptr;
    PrStatus status = makeKeyframes(keyframes, count, &frames);
    if (status != PrStatus_Ok)
        return status;
    cssKeyframesRelease(effect->keyframes);
    effect->keyframes = frames;
    cssValueListReset(&effect->defaultEasing);
    // A CSS animation's keyframes are the script's from now on (css-animations-2/#animations).
    if (effect->animation != nullptr)
        effect->animation->keyframesOverridden = true;
    cssAnimationEffectChanged(effect);
    return PrStatus_Ok;
}

uint32_t prCssKeyframeEffectKeyframes(const PrCssKeyframeEffect* effect, PrCssComputedKeyframe* PR_NULLABLE out,
        uint32_t capacity) {
    const PrCssAnimation* animation = effect->animation;
    if (animation != nullptr && animation->kind == PrCssAnimationKind_Transition && effect->keyframes == nullptr) {
        // A transition's keyframes are its endpoints, with its property (css-transitions-2/#csstransition).
        for (uint32_t i = 0; i < 2 && i < capacity; ++i)
            out[i] = PrCssComputedKeyframe{double(i), double(i), prRawStringCopy("linear"), PrCssComposite_Auto, 1};
        return 2;
    }
    if (effect->keyframes == nullptr)
        return 0;
    const RawArray<CssKeyframeRecord>& frames = effect->keyframes->frames;
    for (uint32_t i = 0; i < frames.count && i < capacity; ++i) {
        const CssKeyframeRecord& frame = frames.data[i];
        PrTextBuffer easing{};
        if (frame.easing.count != 0)
            cssSerializeValues(&easing, cssValueListRun(&frame.easing));
        else if (effect->defaultEasing.count != 0)
            cssSerializeValues(&easing, cssValueListRun(&effect->defaultEasing));
        else
            prTextBufferAppend(&easing, "linear");
        uint32_t properties = 0;
        if (frame.rule == nullptr) {
            properties = frame.given.count;
        } else {
            const CssDeclaration* declarations = cssDeclarations(frame.style);
            for (uint32_t d = 0; d < frame.style->count; ++d) {
                PrCssProperty property = declarations[d].property;
                properties += property != PrCssProperty_AnimationTimingFunction &&
                    property != PrCssProperty_AnimationComposition && !declarations[d].important;
            }
        }
        out[i] = PrCssComputedKeyframe{
            frame.offset, frame.computedOffset, prRawStringCopy(prTextBufferView(&easing)),
            frame.composite, properties,
        };
        prTextBufferReset(&easing);
    }
    return frames.count;
}

bool prCssKeyframeEffectKeyframeProperty(const PrCssKeyframeEffect* effect, uint32_t frame, uint32_t index,
        PrRawString* name, PrRawString* value) {
    *name = prRawStringNull();
    *value = prRawStringNull();
    const PrCssAnimation* animation = effect->animation;
    if (animation != nullptr && animation->kind == PrCssAnimationKind_Transition && effect->keyframes == nullptr) {
        if (frame >= 2 || index != 0)
            return false;
        *name = prRawStringCopy(cssPropertyName(animation->property));
        PrTextBuffer text{};
        for (uint32_t i = 0; i < effect->frames.count; ++i) {
            const CssAnimateKeyframe& k = effect->frames.data[i];
            if (k.offset == double(frame))
                cssSerializeValues(&text, cssValuePoolRun(&effect->values, k.value));
        }
        *value = prRawStringCopy(prTextBufferView(&text));
        prTextBufferReset(&text);
        return true;
    }
    if (effect->keyframes == nullptr || frame >= effect->keyframes->frames.count)
        return false;
    const CssKeyframeRecord& k = effect->keyframes->frames.data[frame];
    if (k.rule == nullptr) {
        if (index >= k.given.count)
            return false;
        *name = prRawStringClone(&k.given.data[index].name);
        *value = prRawStringClone(&k.given.data[index].value);
        return true;
    }
    const CssDeclaration* declarations = cssDeclarations(k.style);
    for (uint32_t d = 0; d < k.style->count; ++d) {
        PrCssProperty property = declarations[d].property;
        if (property == PrCssProperty_AnimationTimingFunction || property == PrCssProperty_AnimationComposition ||
                declarations[d].important)
            continue;
        if (index-- != 0)
            continue;
        *name = prRawStringCopy(cssDeclarationName(k.style, &declarations[d]));
        PrTextBuffer text{};
        cssSerializeValues(&text, cssDeclarationValue(k.style, &declarations[d]));
        *value = prRawStringCopy(prTextBufferView(&text));
        prTextBufferReset(&text);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Animations

PrDomEventTarget* prCssAnimationAsEventTarget(PrCssAnimation* animation) {
    return animation;
}

PrCssAnimation* PR_NULLABLE prCssAnimationFromEventTarget(PrDomEventTarget* target) {
    return target->type == DomTargetType_Animation ? static_cast<PrCssAnimation*>(target) : nullptr;
}

PrCssAnimation* prCssAnimationCreate(PrDomDocument* document, PrCssKeyframeEffect* PR_NULLABLE effect,
        PrCssTimeline* PR_NULLABLE timeline) {
    PrCssAnimation* a = cssAnimationMake(document, PrCssAnimationKind_Script);
    prCssAnimationSetTimeline(a, timeline);
    prCssAnimationSetEffect(a, effect);
    return a;
}

void prCssAnimationRetain(PrCssAnimation* animation) {
    ++animation->references;
}

void prCssAnimationRelease(PrCssAnimation* PR_NULLABLE animation) {
    domEventTargetRelease(animation);
}

PrCssAnimationKind prCssAnimationKind(const PrCssAnimation* animation) {
    return animation->kind;
}

PrStringView prCssAnimationName(const PrCssAnimation* animation) {
    if (animation->kind == PrCssAnimationKind_Transition)
        return cssPropertyName(animation->property);
    return prRawStringView(&animation->name);
}

PrStringView prCssAnimationId(const PrCssAnimation* animation) {
    return prRawStringView(&animation->id);
}

void prCssAnimationSetId(PrCssAnimation* animation, PrStringView id) {
    prRawStringReplace(&animation->id, id);
}

PrCssKeyframeEffect* PR_NULLABLE prCssAnimationEffect(const PrCssAnimation* animation) {
    return animation->effect;
}

void prCssAnimationSetEffect(PrCssAnimation* animation, PrCssKeyframeEffect* PR_NULLABLE effect) {
    // web-animations-1/#setting-the-associated-effect
    if (animation->effect == effect)
        return;
    // A CSS animation's effect is the script's from now on (css-animations-2/#animations).
    if (effect != nullptr && effect->animation != nullptr)
        prCssAnimationSetEffect(effect->animation, nullptr);
    PrCssKeyframeEffect* old = animation->effect;
    if (old != nullptr) {
        cssAnimationEffectChanged(old);
        old->animation = nullptr;
        cssAnimationIndexEffect(old);
    }
    if (effect != nullptr) {
        prCssKeyframeEffectRetain(effect);
        effect->animation = animation;
        cssAnimationIndexEffect(effect);
        cssAnimationEffectChanged(effect);
    }
    animation->effect = effect;
    prCssKeyframeEffectRelease(old);
    cssAnimationUpdateFinishedState(animation, false, false);
}

PrCssTimeline* PR_NULLABLE prCssAnimationTimeline(const PrCssAnimation* animation) {
    return animation->timeline;
}

void prCssAnimationSetTimeline(PrCssAnimation* animation, PrCssTimeline* PR_NULLABLE timeline) {
    // web-animations-1/#setting-the-timeline
    if (animation->timeline == timeline)
        return;
    if (timeline != nullptr)
        prCssTimelineRetain(timeline);
    prCssTimelineRelease(animation->timeline);
    animation->timeline = timeline;
    if (!prF64IsNan(animation->playback.startTime))
        animation->playback.holdTime = kPrF64Nan;
    cssAnimationUpdateFinishedState(animation, false, false);
    if (animation->effect != nullptr)
        cssAnimationEffectChanged(animation->effect);
}

double prCssAnimationStartTime(const PrCssAnimation* animation) {
    return animation->playback.startTime;
}

PrStatus prCssAnimationSetStartTime(PrCssAnimation* animation, double time) {
    // web-animations-1/#setting-the-start-time-of-an-animation
    double timeline = timelineTime(animation);
    if (prF64IsNan(timeline) && !prF64IsNan(time))
        animation->playback.holdTime = kPrF64Nan;
    double previous = cssAnimationCurrentTime(animation);
    applyPendingRate(animation);
    animation->playback.startTime = time;
    if (!prF64IsNan(time)) {
        if (animation->playback.rate != 0)
            animation->playback.holdTime = kPrF64Nan;
    } else {
        animation->playback.holdTime = previous;
    }
    if (animation->pending != CssPendingTask_None) {
        animation->pending = CssPendingTask_None;
        settle(animation, PrCssAnimationPromise_Ready, PrCssPromiseState_Fulfilled);
    }
    animation->playStateOverridden = true;
    cssAnimationUpdateFinishedState(animation, true, false);
    if (animation->effect != nullptr)
        cssAnimationEffectChanged(animation->effect);
    return PrStatus_Ok;
}

double prCssAnimationCurrentTime(const PrCssAnimation* animation) {
    return cssAnimationCurrentTime(animation);
}

PrStatus prCssAnimationSetCurrentTime(PrCssAnimation* animation, double time) {
    // web-animations-1/#setting-the-current-time-of-an-animation
    PrStatus status = cssAnimationSilentlySetCurrentTime(animation, time);
    if (status != PrStatus_Ok || prF64IsNan(time))
        return status;
    if (animation->pending == CssPendingTask_Pause) {
        animation->playback.holdTime = time;
        applyPendingRate(animation);
        animation->playback.startTime = kPrF64Nan;
        animation->pending = CssPendingTask_None;
        settle(animation, PrCssAnimationPromise_Ready, PrCssPromiseState_Fulfilled);
    }
    cssAnimationUpdateFinishedState(animation, true, false);
    if (animation->effect != nullptr)
        cssAnimationEffectChanged(animation->effect);
    return PrStatus_Ok;
}

double prCssAnimationPlaybackRate(const PrCssAnimation* animation) {
    return animation->playback.rate;
}

void prCssAnimationSetPlaybackRate(PrCssAnimation* animation, double rate) {
    // web-animations-1/#setting-the-playback-rate-of-an-animation
    animation->hasPendingRate = false;
    double previous = cssAnimationCurrentTime(animation);
    animation->playback.rate = rate;
    if (!prF64IsNan(previous))
        prCssAnimationSetCurrentTime(animation, previous);
    if (animation->effect != nullptr)
        cssAnimationEffectChanged(animation->effect);
}

PrCssPlayState prCssAnimationPlayState(const PrCssAnimation* animation) {
    // web-animations-1/#play-states
    double current = cssAnimationCurrentTime(animation);
    if (prF64IsNan(current) && prF64IsNan(animation->playback.startTime) && animation->pending == CssPendingTask_None)
        return PrCssPlayState_Idle;
    if (animation->pending == CssPendingTask_Pause ||
            (prF64IsNan(animation->playback.startTime) && animation->pending != CssPendingTask_Play))
        return PrCssPlayState_Paused;
    double rate = effectiveRate(animation);
    if (!prF64IsNan(current) && ((rate > 0 && current >= effectEnd(animation)) || (rate < 0 && current <= 0)))
        return PrCssPlayState_Finished;
    return PrCssPlayState_Running;
}

PrCssReplaceState prCssAnimationReplaceState(const PrCssAnimation* animation) {
    return animation->replaceState;
}

bool prCssAnimationPending(const PrCssAnimation* animation) {
    return animation->pending != CssPendingTask_None;
}

PrStatus prCssAnimationPlay(PrCssAnimation* animation) {
    PrStatus status = cssAnimationPlay(animation, true);
    if (status == PrStatus_Ok) {
        animation->playStateOverridden = true;
        if (animation->effect != nullptr)
            cssAnimationEffectChanged(animation->effect);
    }
    return status;
}

PrStatus prCssAnimationPause(PrCssAnimation* animation) {
    PrStatus status = cssAnimationPause(animation);
    if (status == PrStatus_Ok) {
        animation->playStateOverridden = true;
        if (animation->effect != nullptr)
            cssAnimationEffectChanged(animation->effect);
    }
    return status;
}

PrStatus prCssAnimationFinish(PrCssAnimation* animation) {
    // web-animations-1/#finishing-an-animation-section
    double rate = effectiveRate(animation);
    double end = effectEnd(animation);
    if (rate == 0 || (rate > 0 && !prF64IsFinite(end)))
        return PrStatus_InvalidState;
    applyPendingRate(animation);
    double limit = animation->playback.rate > 0 ? end : 0;
    cssAnimationSilentlySetCurrentTime(animation, limit);
    if (prF64IsNan(animation->playback.startTime) && timelineActive(animation))
        animation->playback.startTime = timelineTime(animation) - limit / animation->playback.rate;
    if (animation->pending == CssPendingTask_Pause && !prF64IsNan(animation->playback.startTime)) {
        animation->playback.holdTime = kPrF64Nan;
        animation->pending = CssPendingTask_None;
        settle(animation, PrCssAnimationPromise_Ready, PrCssPromiseState_Fulfilled);
    }
    if (animation->pending == CssPendingTask_Play && !prF64IsNan(animation->playback.startTime)) {
        animation->pending = CssPendingTask_None;
        settle(animation, PrCssAnimationPromise_Ready, PrCssPromiseState_Fulfilled);
    }
    cssAnimationUpdateFinishedState(animation, true, true);
    if (animation->effect != nullptr)
        cssAnimationEffectChanged(animation->effect);
    return PrStatus_Ok;
}

void prCssAnimationCancel(PrCssAnimation* animation) {
    cssAnimationCancel(animation);
    cssAnimationRelist(animation);
    if (animation->effect != nullptr)
        cssAnimationEffectChanged(animation->effect);
}

PrStatus prCssAnimationReverse(PrCssAnimation* animation) {
    // web-animations-1/#reversing-an-animation-section
    if (!timelineActive(animation))
        return PrStatus_InvalidState;
    bool hadPending = animation->hasPendingRate;
    double original = animation->pendingRate;
    animation->pendingRate = -effectiveRate(animation);
    animation->hasPendingRate = true;
    PrStatus status = cssAnimationPlay(animation, true);
    if (status != PrStatus_Ok) {
        animation->hasPendingRate = hadPending;
        animation->pendingRate = original;
        return status;
    }
    animation->playStateOverridden = true;
    if (animation->effect != nullptr)
        cssAnimationEffectChanged(animation->effect);
    return PrStatus_Ok;
}

void prCssAnimationUpdatePlaybackRate(PrCssAnimation* animation, double rate) {
    // web-animations-1/#seamlessly-updating-the-playback-rate-of-an-animation
    PrCssPlayState previous = prCssAnimationPlayState(animation);
    animation->pendingRate = rate;
    animation->hasPendingRate = true;
    if (animation->pending != CssPendingTask_None) {
        // The task applies the rate.
    } else if (previous == PrCssPlayState_Idle || previous == PrCssPlayState_Paused ||
            prF64IsNan(cssAnimationCurrentTime(animation))) {
        applyPendingRate(animation);
    } else if (previous == PrCssPlayState_Finished) {
        double unconstrained = unheldTime(animation);
        double time = timelineTime(animation);
        animation->playback.startTime = rate != 0 ? time - unconstrained / rate : time;
        applyPendingRate(animation);
        cssAnimationUpdateFinishedState(animation, false, false);
    } else {
        cssAnimationPlay(animation, false);
    }
    if (animation->effect != nullptr)
        cssAnimationEffectChanged(animation->effect);
}

void prCssAnimationPersist(PrCssAnimation* animation) {
    animation->replaceState = PrCssReplaceState_Persisted;
}

PrStatus prCssAnimationCommitStyles(PrCssAnimation* animation) {
    PrCssKeyframeEffect* effect = animation->effect;
    if (effect == nullptr)
        return PrStatus_Ok;
    PrDomElement* target = effect->target;
    if (target == nullptr || effect->pseudo != CssPseudo_None || !domIsConnected(target))
        return PrStatus_InvalidState;
    // The computed values, with the effects at the animation's current time.
    PrCssStyleDeclaration* computed =
        prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(target, PrStringView{}));
    PrCssStyleDeclaration* inlineStyle = prCssStylePropertiesAsCssStyleDeclaration(prCssElementStyle(target));
    RawArray<PrCssProperty> done;
    for (uint32_t i = 0; i < effect->frames.count; ++i) {
        PrCssProperty property = effect->frames.data[i].property;
        if (done.find(property) != kArrayMissing)
            continue;
        done.push(property);
        PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
        PrRawString value = prCssStyleDeclarationGetPropertyValue(computed, &name);
        prCssStyleDeclarationSetProperty(inlineStyle, &name, prRawStringView(&value), false);
        prRawStringReset(&value);
    }
    done.reset();
    return PrStatus_Ok;
}

PrCssPromiseState prCssAnimationPromiseState(const PrCssAnimation* animation, PrCssAnimationPromise promise,
        uint32_t* generation) {
    if (promise == PrCssAnimationPromise_Ready) {
        *generation = animation->readyGeneration;
        return animation->ready;
    }
    *generation = animation->finishedGeneration;
    return animation->finished;
}

bool prCssAnimationRunning(const PrCssAnimation* animation) {
    return animation->listed;
}

void prCssDocumentSetAnimationCallback(PrDomDocument* document, const PrCssAnimationCallback* PR_NULLABLE callback) {
    cssAnimations(document).callback = callback != nullptr ? *callback : PrCssAnimationCallback{};
}

void prCssAnimationNotifyFinished(PrCssAnimation* animation) {
    if (!animation->finishScheduled)
        return;
    animation->finishScheduled = false;
    cssAnimationFinishNotification(animation);
    cssAnimationRelist(animation);
}

bool prCssPropertyAnimatable(PrStringView name) {
    if (name.size >= 2 && name.data[0] == '-' && name.data[1] == '-')
        return true;
    PrCssProperty property = cssPropertyFromText(name);
    if (property >= PrCssProperty_Count)
        return false;
    CssPropertySpan longhands = kCssPropertyLonghands[property];
    if (longhands.count == 0)
        return bitsetTest(kCssPropertyAnimatable, property);
    for (uint32_t i = 0; i < longhands.count; ++i) {
        if (bitsetTest(kCssPropertyAnimatable, kCssPropertyRuns[longhands.start + i]))
            return true;
    }
    return false;
}

bool prCssEasingValid(PrStringView text) {
    CssValueList easing{};
    bool valid = cssParseEasing(text, &easing);
    cssValueListReset(&easing);
    return valid;
}

uint32_t prCssDocumentAnimations(PrDomDocument* document, PrDomElement* PR_NULLABLE root, bool subtree,
        PrCssAnimation* PR_NONNULL* PR_NULLABLE out, uint32_t capacity) {
    // The styles are brought up to date first, which starts and cancels CSS animations and transitions
    // (web-animations-1/#dom-animatable-getanimations).
    CssRoot* style = cssDocumentRoot(document);
    const PrDomElement* flushed = root != nullptr ? root : domDocumentElement(document);
    if (style->presentationHooks != nullptr && flushed != nullptr)
        style->presentationHooks->styleCurrent(style->presentation, flushed);
    CssAnimations& animations = cssAnimations(document);
    RawArray<PrCssAnimation*> found;
    for (uint32_t i = 0; i < animations.list.count; ++i) {
        PrCssAnimation* a = animations.list.data[i];
        if (!relevant(a))
            continue;
        const PrDomElement* target = a->effect->target;
        if (target == nullptr || target->document != document || !domIsConnected(target))
            continue;
        bool own = target == root && a->effect->pseudo == CssPseudo_None;
        bool within = root == nullptr || (subtree ? domShadowIncludingContains(root, target) : own);
        if (within)
            found.push(a);
    }
    sortItems(found.span(), [](PrCssAnimation* x, PrCssAnimation* y) {
        if (x->effect->target == y->effect->target && x->effect->pseudo != y->effect->pseudo &&
                x->owner != nullptr && y->owner != nullptr && x->kind == y->kind)
            return pseudoRank(x->effect->pseudo) < pseudoRank(y->effect->pseudo);
        return cssAnimationBefore(x, y);
    });
    for (uint32_t i = 0; i < found.count && i < capacity; ++i) {
        prCssAnimationRetain(found.data[i]);
        out[i] = found.data[i];
    }
    uint32_t count = found.count;
    found.reset();
    return count;
}

PrCssAnimationPlaybackEvent* prCssAnimationPlaybackEventCreate(PrStringView type, const PrCssAnimationPlaybackEventInit* init) {
    PrCssAnimationPlaybackEvent* event =
        domEventCreate<PrCssAnimationPlaybackEvent>(type, init->event, PrDomEventKind_AnimationPlayback);
    PlaybackEventData* data = allocZeroed<PlaybackEventData>();
    data->destroy = [](DomEventExtra* extra) { prMemoryFree(extra); };
    data->currentTime = init->currentTime;
    data->timelineTime = init->timelineTime;
    event->extra = data;
    return event;
}

PrDomEvent* prCssAnimationPlaybackEventAsEvent(PrCssAnimationPlaybackEvent* event) {
    return event;
}

PrCssAnimationPlaybackEvent* PR_NULLABLE prCssAnimationPlaybackEventFromEvent(PrDomEvent* event) {
    return event->kind == PrDomEventKind_AnimationPlayback ? static_cast<PrCssAnimationPlaybackEvent*>(event) : nullptr;
}

double prCssAnimationPlaybackEventCurrentTime(const PrCssAnimationPlaybackEvent* event) {
    return event->extra != nullptr ? static_cast<const PlaybackEventData*>(event->extra)->currentTime : kPrF64Nan;
}

double prCssAnimationPlaybackEventTimelineTime(const PrCssAnimationPlaybackEvent* event) {
    return event->extra != nullptr ? static_cast<const PlaybackEventData*>(event->extra)->timelineTime : kPrF64Nan;
}
